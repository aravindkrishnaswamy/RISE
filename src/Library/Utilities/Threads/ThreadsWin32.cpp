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
#define _WIN32_WINNT 0x0400		// require NT4 or greater for TryEnterCriticalSection
#include <windows.h>

using namespace RISE;

unsigned int Threading::riseCreateThread( THREAD_FUNC pFunc, void* pParam, unsigned int initial_stack_size, void* thread_attributes, RISETHREADID* threadid )
{
	HANDLE hThread = CreateThread( 0, initial_stack_size, (LPTHREAD_START_ROUTINE)pFunc, pParam, (DWORD)thread_attributes, 0 );

	if( threadid ) {
		*threadid = (RISETHREADID)hThread;
	}

	// Makes MP rendering not choke the poor system...  I know this is a hack
	if( GlobalOptions().ReadBool( "force_all_threads_low_priority", true ) ) {
		SetThreadPriority( hThread, THREAD_PRIORITY_BELOW_NORMAL );
	}

	if( hThread ) {
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

