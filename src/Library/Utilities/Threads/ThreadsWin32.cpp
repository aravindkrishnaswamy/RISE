//////////////////////////////////////////////////////////////////////
//
//  ThreadsWin32.h - Implements the functions in Threads.h specifically
//  for the Windows platform
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 07, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "Threads.h"
#include "../../Interfaces/ILog.h"
#include "../../Interfaces/IOptions.h"

#ifdef WIN32

#define WIN32_LEAN_AND_MEAN		// Exclude rarely-used stuff from Windows headers
#include <windows.h>

using namespace RISE;

void Threading::riseSetThreadRenderPriority( RISETHREADID threadid )
{
	// On Windows thread priority defaults to NORMAL which is what
	// we want for render workers.  We rely on CPU affinity to keep
	// workers on P-cores (see riseSetThreadAffinity).
	HANDLE hThread = threadid ? (HANDLE)threadid : GetCurrentThread();
	SetThreadPriority( hThread, THREAD_PRIORITY_NORMAL );
}

void Threading::riseSetThreadAffinity( const std::vector<unsigned int>& cpuIds )
{
	// Pin calling thread to the given CPU set.
	if( cpuIds.empty() ) return;
	DWORD_PTR mask = 0;
	for( unsigned int id : cpuIds ) {
		if( id < sizeof( DWORD_PTR ) * 8 ) {
			mask |= ( static_cast<DWORD_PTR>( 1 ) << id );
		}
	}
	if( mask != 0 ) {
		SetThreadAffinityMask( GetCurrentThread(), mask );
	}
}

void Threading::riseSetThreadLowPriority( RISETHREADID threadid )
{
	// See ThreadsPTHREADs.cpp for the severity warning.  Kept
	// BELOW_NORMAL rather than LOWEST — on Windows LOWEST yields
	// aggressively enough to reduce per-thread throughput 2–3× under
	// load.  Used in legacy background-render mode
	// (force_all_threads_low_priority) for every render worker AND
	// the calling thread of SP fallbacks.
	//
	// threadid == 0 means "calling thread", matching the convention
	// in riseSetThreadRenderPriority.  Without this remap the call
	// passes NULL to SetThreadPriority, which silently fails.
	HANDLE hThread = threadid ? (HANDLE)threadid : GetCurrentThread();
	SetThreadPriority( hThread, THREAD_PRIORITY_BELOW_NORMAL );
}

unsigned int Threading::riseCreateThread( THREAD_FUNC pFunc, void* pParam, unsigned int initial_stack_size, void* thread_attributes, RISETHREADID* threadid )
{
	HANDLE hThread = CreateThread( 0, initial_stack_size, (LPTHREAD_START_ROUTINE)pFunc, pParam, static_cast<DWORD>(reinterpret_cast<uintptr_t>(thread_attributes)), 0 );

	if( threadid ) {
		*threadid = (RISETHREADID)hThread;
	}

	// Legacy "render in the background" override.  Default flipped to
	// false — most render threads need full priority to saturate the
	// machine.  GlobalThreadPool picks per-worker priority explicitly.
	if( GlobalOptions().ReadBool( "force_all_threads_low_priority", false ) ) {
		riseSetThreadLowPriority( (RISETHREADID)hThread );
	}

	if( hThread ) {
		return 1;
	}

	return 0;
}

unsigned int Threading::riseCreateLowPriorityThread( THREAD_FUNC pFunc, void* pParam, unsigned int initial_stack_size, void* thread_attributes, RISETHREADID* threadid )
{
	HANDLE hThread = CreateThread( 0, initial_stack_size, (LPTHREAD_START_ROUTINE)pFunc, pParam, static_cast<DWORD>(reinterpret_cast<uintptr_t>(thread_attributes)), 0 );

	if( threadid ) {
		*threadid = (RISETHREADID)hThread;
	}

	if( hThread ) {
		riseSetThreadLowPriority( (RISETHREADID)hThread );
		return 1;
	}

	return 0;
}

unsigned int Threading::riseWaitUntilThreadFinishes( RISETHREADID threadid, void* )
{
	HANDLE hThread = (HANDLE)threadid;

	if( hThread ) {
		WaitForSingleObject( hThread, INFINITE );
		return 1;
	}

	return 0;
}

RISETHREADID Threading::riseGetCurrentThread( )
{
	return (RISETHREADID)GetCurrentThread();
}

void Threading::riseSuspendThread( RISETHREADID threadid )
{
	SuspendThread( (HANDLE)threadid );
}

void Threading::riseResumeThread( RISETHREADID threadid )
{
	ResumeThread( (HANDLE)threadid );
}

RISEMUTEX Threading::riseCreateMutex( )
{
	CRITICAL_SECTION*	pCs = new CRITICAL_SECTION;
//	GlobalLog()->PrintNew( pCs, __FILE__, __LINE__, "critical section" );
	::InitializeCriticalSection( pCs );
	return pCs;
}

void Threading::riseDestroyMutex( RISEMUTEX mutex )
{
	CRITICAL_SECTION*	pCs = (CRITICAL_SECTION*)mutex;
	DeleteCriticalSection( pCs );
//	GlobalLog()->PrintDelete( pCs, __FILE__, __LINE__ );
	delete pCs;
}

void Threading::riseMutexLock( RISEMUTEX mutex )
{
	CRITICAL_SECTION*	pCs = (CRITICAL_SECTION*)mutex;
	::EnterCriticalSection( pCs );
}

bool Threading::riseMutexTryLock( RISEMUTEX mutex )
{
	CRITICAL_SECTION*	pCs = (CRITICAL_SECTION*)mutex;
	return !!::TryEnterCriticalSection( pCs );
}

void Threading::riseMutexUnlock( RISEMUTEX mutex )
{
	CRITICAL_SECTION*	pCs = (CRITICAL_SECTION*)mutex;
	::LeaveCriticalSection( pCs );
}

RISESLEEP Threading::riseCreateSleep( )
{
	HANDLE hEvent = CreateEvent( 0, TRUE, FALSE, "RISESLEEP Event that never gets signalled" );
	return (void*)hEvent;
}

void Threading::riseSleep( RISESLEEP sleep, unsigned int duration )
{
	HANDLE hEvent = (HANDLE)sleep;
	WaitForSingleObject( hEvent, duration );
}

void Threading::riseDestroySleep( RISESLEEP sleep )
{
	HANDLE hEvent = (HANDLE)sleep;
	CloseHandle( hEvent );
}

RISESEMAPHORE Threading::riseCreateSemaphore(
	int value
	)
{
	HANDLE hSem = CreateSemaphore( NULL, value, 1, NULL );
	return (void*)hSem;
}

void Threading::riseDestroySemaphore(
	RISESEMAPHORE sem
	)
{
	HANDLE hSem = (HANDLE)sem;
	CloseHandle( hSem );
}

void Threading::riseSemaphoreAcquire(
	RISESEMAPHORE sem
	)
{
	HANDLE hSem = (HANDLE)sem;
	WaitForSingleObject( hSem, INFINITE );
}

void Threading::riseSemaphoreRelease(
	RISESEMAPHORE sem,
	int count
	)
{
	HANDLE hSem = (HANDLE)sem;
	ReleaseSemaphore( hSem, 1, NULL );
}


#else 

#error "Trying to compile ThreadsWin32.cpp in a non Win32 environment"

#endif

