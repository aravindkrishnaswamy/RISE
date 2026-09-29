//////////////////////////////////////////////////////////////////////
//
//  ThreadsPTHREADs.cpp - Implements the functions in Threads.h specifically
//  for any platform with posix style threads
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

#ifndef WIN32

#include "Threads.h"
#include "../../Interfaces/ILog.h"
#include "../../Interfaces/IOptions.h"
#include "../RTime.h"
#ifndef NO_PTHREAD_SUPPORT
	#include <pthread.h>
	#include <semaphore.h>
#endif
#ifdef __APPLE__
	#include <pthread/qos.h>
#endif
#ifdef __linux__
	#include <sched.h>
	#include <sys/resource.h>	// setpriority / PRIO_PROCESS
#endif
#include <unistd.h>
#include <cstdint>
#include <limits.h>

using namespace RISE;

void Threading::riseSetThreadRenderPriority( RISETHREADID threadid )
{
	// Render workers need to stay on the fast cores at full clock.
	// On macOS, QOS_CLASS_USER_INITIATED tells the scheduler:
	//   - Prefer the P-cores for this thread.
	//   - Run the P-core at its full clock, not the throttled UTILITY
	//     frequency.
	//   - Yield only to USER_INTERACTIVE work (UI animations).
	// This is the right setting for long-running parallel work that
	// should co-exist with the user's foreground app without
	// paying the E-core / throttle tax of UTILITY.
	(void)threadid;
#ifndef NO_PTHREAD_SUPPORT
#ifdef __APPLE__
	pthread_set_qos_class_self_np( QOS_CLASS_USER_INITIATED, 0 );
#else
	// Linux / other POSIX: inherit the parent's nice value.  No-op;
	// affinity handles the placement.
#endif
#endif
}

void Threading::riseSetThreadAffinity( const std::vector<unsigned int>& cpuIds )
{
	// Pin the calling thread to the given CPU set.  No-op on macOS
	// (no thread-affinity API on Apple Silicon).
#ifdef __linux__
	if( cpuIds.empty() ) return;
	cpu_set_t set;
	CPU_ZERO( &set );
	for( unsigned int id : cpuIds ) {
		if( id < CPU_SETSIZE ) {
			CPU_SET( id, &set );
		}
	}
	sched_setaffinity( 0, sizeof( set ), &set );
#else
	(void)cpuIds;
#endif
}

void Threading::riseSetThreadLowPriority( RISETHREADID threadid )
{
	// Lowered priority for "good citizen" background rendering.
	// On macOS this is a HARD throttle: QOS_CLASS_UTILITY prefers E-cores
	// and caps P-core frequency, reducing real per-thread throughput by
	// roughly 2–4× under load.
	//
	// Most render threads MUST stay at normal priority — otherwise a 10-
	// core machine delivers only 2–3× aggregate throughput.  The
	// production policy (see ThreadPool / CPUTopology) is
	// topology-aware: every P-core + (E-cores − 1) gets a worker at
	// normal priority, and 1 E-core is left to the OS.  Benchmarks
	// override via render_thread_reserve_count 0.  The ONLY code path
	// that takes this low-priority branch is legacy
	// force_all_threads_low_priority mode, which the user opts into
	// explicitly when they want to render while using the machine.
	(void)threadid;
#ifndef NO_PTHREAD_SUPPORT
#ifdef __APPLE__
	// macOS: QoS classes are the scheduling mechanism.  UTILITY is the
	// harshest "keep out of the user's way" class the scheduler offers
	// without going strictly background.  Calling repeatedly with
	// the same class is a no-op (QoS is sticky, lower-only).
	pthread_set_qos_class_self_np( QOS_CLASS_UTILITY, 0 );
#elif defined( __linux__ )
	// Linux: set the calling thread's nice value to an ABSOLUTE 10.
	// Previously used nice(10) which is additive — calling it once
	// per progressive SP pass would drift the caller from nice 0 →
	// 10 → 19 (clamped) over successive passes instead of staying
	// at 10.  setpriority( PRIO_PROCESS, 0, … ) with tid 0 operates
	// on the calling thread and sets an absolute value, which is
	// idempotent across calls.
	setpriority( PRIO_PROCESS, 0, 10 );
#else
	// Other POSIX: fall back to nice().  Not idempotent, but these
	// platforms aren't a priority target and only hit this path
	// under force_all_threads_low_priority anyway.
	nice( 10 );
#endif
#endif
}

#ifndef NO_PTHREAD_SUPPORT
namespace RISE {
	// Two-argument thread-start wrapper.  The new `lowPriority` flag
	// lets callers choose per-thread whether to drop priority.  Default
	// behaviour (flag = false) keeps the thread at the inherited
	// priority, so render workers run full speed unless the caller
	// explicitly asks for the reduced class.
	struct ThreadStartData
	{
		THREAD_FUNC realFunc;
		void* realParam;
		bool         lowPriority;
	};

	static void* ThreadStartProc( void* arg )
	{
		ThreadStartData* data = static_cast<ThreadStartData*>( arg );
		THREAD_FUNC func = data->realFunc;
		void* param = data->realParam;
		const bool lowPriority = data->lowPriority;
		delete data;

		// Legacy global override — deprecated, defaults to false.  Kept
		// for users who opted into "render in the background" mode.
		// New code should pass `lowPriority` through the Threading API
		// (and use GlobalThreadPool which honours the production policy).
		const bool forceLow = GlobalOptions().ReadBool(
			"force_all_threads_low_priority", false );

		if( lowPriority || forceLow ) {
			Threading::riseSetThreadLowPriority( 0 );
		}

		return func( param );
	}
}
#endif

#ifndef NO_PTHREAD_SUPPORT
namespace
{
	//! DL-315: honour riseCreateThread's `initial_stack_size` (0 = the
	//! platform default).  Rounded up to the page size and to
	//! PTHREAD_STACK_MIN, as pthread_attr_setstacksize requires.
	void ApplyStackSize( pthread_attr_t& attr, const unsigned int bytes )
	{
		if( bytes == 0 ) {
			return;
		}
		size_t size = bytes;
		const long page = sysconf( _SC_PAGESIZE );
		if( page > 0 ) {
			const size_t p = static_cast<size_t>( page );
			size = ( size + p - 1 ) / p * p;
		}
		if( size < static_cast<size_t>( PTHREAD_STACK_MIN ) ) {
			size = static_cast<size_t>( PTHREAD_STACK_MIN );
		}
		if( pthread_attr_setstacksize( &attr, size ) != 0 ) {
			GlobalLog()->PrintEx( eLog_Warning, "riseCreateThread:: could not set a %lu-byte stack; using the platform default", static_cast<unsigned long>( size ) );
		}
	}
}
#endif

unsigned int Threading::riseCreateThread( THREAD_FUNC pFunc, void* pParam, unsigned int initial_stack_size, void* thread_attributes, RISETHREADID* threadid )
{
#ifdef NO_PTHREAD_SUPPORT
	// Just execute the code and forge it
	GlobalLog()->PrintEasyInfo( "riseCreateThread:: NO PTHREAD support was compiled, simply executing code" );
	(*pFunc)( pParam );
	return 1;
#else
	pthread_t			tid;
	pthread_attr_t		attr;

	ThreadStartData* startData = new ThreadStartData;
	startData->realFunc = pFunc;
	startData->realParam = pParam;
	startData->lowPriority = false;

	pthread_attr_init( &attr );
	ApplyStackSize( attr, initial_stack_size );
	pthread_create( &tid, &attr, ThreadStartProc, startData );
	pthread_attr_destroy( &attr );

	if( threadid ) {
		*threadid = (RISETHREADID)tid;
	}

//	GlobalLog()->PrintEx( eLog_Info, "Starting Thread ID:%d", tid );
	return 1;
#endif
}

unsigned int Threading::riseCreateLowPriorityThread( THREAD_FUNC pFunc, void* pParam, unsigned int initial_stack_size, void* thread_attributes, RISETHREADID* threadid )
{
#ifdef NO_PTHREAD_SUPPORT
	GlobalLog()->PrintEasyInfo( "riseCreateLowPriorityThread:: NO PTHREAD support was compiled, simply executing code" );
	(*pFunc)( pParam );
	return 1;
#else
	pthread_t			tid;
	pthread_attr_t		attr;

	ThreadStartData* startData = new ThreadStartData;
	startData->realFunc = pFunc;
	startData->realParam = pParam;
	startData->lowPriority = true;

	pthread_attr_init( &attr );
	ApplyStackSize( attr, initial_stack_size );
	pthread_create( &tid, &attr, ThreadStartProc, startData );
	pthread_attr_destroy( &attr );

	if( threadid ) {
		*threadid = (RISETHREADID)tid;
	}
	return 1;
#endif
}

unsigned int Threading::riseWaitUntilThreadFinishes( RISETHREADID threadid, void* threadreturncode )
{
#ifdef NO_PTHREAD_SUPPORT
	GlobalLog()->PrintEasyInfo( "riseWaitUntilThreadFinishes:: NO PTHREAD support was compiled, nothing to do" );
	return 1;
#else
	pthread_t			tid = (pthread_t)threadid;

//	GlobalLog()->PrintEx( eLog_Info, "Waiting Thread ID:%d", tid );

	pthread_join( tid, &threadreturncode );
	return 1;
#endif
}

RISETHREADID Threading::riseGetCurrentThread( )
{
#ifdef NO_PTHREAD_SUPPORT
	return 0;
#else
	return (RISETHREADID)pthread_self();
#endif
}

void Threading::riseSuspendThread( RISETHREADID threadid )
{
#ifndef NO_PTHREAD_SUPPORT
//	pthread_suspend( (pthread_t)threadid );
	GlobalLog()->PrintEasyWarning( "POSIX threads do not support suspend, don't use it" );
#endif
}

void Threading::riseResumeThread( RISETHREADID threadid )
{
#ifndef NO_PTHREAD_SUPPORT
//	pthread_wakeup( (pthread_t)threadid );
	GlobalLog()->PrintEasyWarning( "POSIX threads do not support resume, don't use it" );
#endif
}

RISEMUTEX Threading::riseCreateMutex( )
{
#ifndef NO_PTHREAD_SUPPORT
	pthread_mutex_t*	mutex = new pthread_mutex_t;
//	GlobalLog()->PrintNew( mutex, __FILE__, __LINE__, "mutex" );
	pthread_mutex_init( mutex, 0 );
	return mutex;
#else
	return 0;
#endif
}

void Threading::riseDestroyMutex( RISEMUTEX mut )
{
#ifndef NO_PTHREAD_SUPPORT
	pthread_mutex_t*	mutex = (pthread_mutex_t*)mut;
	pthread_mutex_destroy( mutex );
//	GlobalLog()->PrintDelete( mutex, __FILE__, __LINE__ );
	delete mutex;
#endif
}

void Threading::riseMutexLock( RISEMUTEX mut )
{
#ifndef NO_PTHREAD_SUPPORT
	pthread_mutex_t*	mutex = (pthread_mutex_t*)mut;
	pthread_mutex_lock( mutex );
#endif
}

bool Threading::riseMutexTryLock( RISEMUTEX mut )
{
#ifndef NO_PTHREAD_SUPPORT
	pthread_mutex_t*	mutex = (pthread_mutex_t*)mut;
	return !pthread_mutex_trylock( mutex );
#else
	return false;
#endif
}

void Threading::riseMutexUnlock( RISEMUTEX mut )
{
#ifndef NO_PTHREAD_SUPPORT
	pthread_mutex_t*	mutex = (pthread_mutex_t*)mut;
	pthread_mutex_unlock( mutex );
#endif
}

#ifndef NO_PTHREAD_SUPPORT
// The pthread version of sleep is done by creating a 
// condition object and a mutex
namespace RISE {
	struct PTHREAD_SLEEP
	{
		pthread_cond_t* cond;
		pthread_mutex_t* mutex;
	};

	// Use a condition variable and a mutex to simulate a semaphore
	struct PTHREAD_SEMAPHORE
	{
		pthread_cond_t cond;
		pthread_mutex_t mutex;
		int value;
	};
}
#endif

RISESLEEP Threading::riseCreateSleep( )
{
#ifndef NO_PTHREAD_SUPPORT
	PTHREAD_SLEEP*	sleep = new PTHREAD_SLEEP;
	sleep->mutex = new pthread_mutex_t;
	sleep->cond = new pthread_cond_t;
	pthread_mutex_init( sleep->mutex, 0 );
	pthread_cond_init( sleep->cond, 0 );
	return (void*)sleep;
#endif
	return 0;
}

void Threading::riseSleep( RISESLEEP risesleep, unsigned int duration )
{
#ifndef NO_PTHREAD_SUPPORT
	PTHREAD_SLEEP*	sleep = (PTHREAD_SLEEP*)risesleep;
	riseMutexLock( sleep->mutex );
	// Create the time structure
	unsigned int ms = GetMilliseconds();
	ms += duration;
	timespec	ts;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (ms % 1000) * 1000000;
	pthread_cond_timedwait( sleep->cond, sleep->mutex, &ts );
	riseMutexUnlock( sleep->mutex );
#endif
}

void Threading::riseDestroySleep( RISESLEEP risesleep )
{
#ifndef NO_PTHREAD_SUPPORT
	PTHREAD_SLEEP*	sleep = (PTHREAD_SLEEP*)risesleep;
	pthread_mutex_destroy( sleep->mutex );
	pthread_cond_destroy( sleep->cond );
	delete sleep->mutex;
	delete sleep->cond;
	delete sleep;
#endif
}

RISESEMAPHORE Threading::riseCreateSemaphore(
	int value
	)
{
#ifndef NO_PTHREAD_SUPPORT
	PTHREAD_SEMAPHORE* sem = new PTHREAD_SEMAPHORE;
	pthread_mutex_init( &sem->mutex, 0 );
	pthread_cond_init( &sem->cond, 0 );
	sem->value = value;
	return (void*)sem;
#endif
	return 0;
}

void Threading::riseDestroySemaphore(
	RISESEMAPHORE sem
	)
{
#ifndef NO_PTHREAD_SUPPORT
	PTHREAD_SEMAPHORE* pSem = (PTHREAD_SEMAPHORE*)sem;
	pthread_mutex_destroy( &pSem->mutex );
	pthread_cond_destroy( &pSem->cond );
	delete pSem;
#endif
}

void Threading::riseSemaphoreAcquire(
	RISESEMAPHORE sem
	)
{
#ifndef NO_PTHREAD_SUPPORT
	PTHREAD_SEMAPHORE* pSem = (PTHREAD_SEMAPHORE*)sem;
	pthread_mutex_lock( &pSem->mutex );
	while( pSem->value <= 0 ) {
		pthread_cond_wait( &pSem->cond, &pSem->mutex );
	}
	pSem->value--;
	pthread_mutex_unlock( &pSem->mutex );
#endif
}

void Threading::riseSemaphoreRelease(
	RISESEMAPHORE sem,
	int count
	)
{
#ifndef NO_PTHREAD_SUPPORT
	PTHREAD_SEMAPHORE* pSem = (PTHREAD_SEMAPHORE*)sem;
	pthread_mutex_lock( &pSem->mutex );
	pSem->value += count;
	pthread_cond_broadcast( &pSem->cond );
	pthread_mutex_unlock( &pSem->mutex );
#endif
}


size_t Threading::riseRemainingStackBytes( )
{
#ifdef NO_PTHREAD_SUPPORT
	return SIZE_MAX;
#else
	// DL-315.  The low bound of this thread's stack, looked up once.
	static thread_local uintptr_t tlStackLow = 0;
	static thread_local bool tlLookedUp = false;
	if( !tlLookedUp ) {
		tlLookedUp = true;
#if defined( __APPLE__ )
		const pthread_t self = pthread_self();
		const uintptr_t high = reinterpret_cast<uintptr_t>( pthread_get_stackaddr_np( self ) );
		const size_t size = pthread_get_stacksize_np( self );
		if( high > size ) {
			tlStackLow = high - size;
		}
#elif defined( __linux__ )
		pthread_attr_t attr;
		if( pthread_getattr_np( pthread_self(), &attr ) == 0 ) {
			void* addr = 0;
			size_t size = 0;
			if( pthread_attr_getstack( &attr, &addr, &size ) == 0 ) {
				tlStackLow = reinterpret_cast<uintptr_t>( addr );
			}
			pthread_attr_destroy( &attr );
		}
#endif
	}
	if( !tlStackLow ) {
		return SIZE_MAX;
	}
	const uintptr_t sp = reinterpret_cast<uintptr_t>( __builtin_frame_address( 0 ) );
	return sp > tlStackLow ? static_cast<size_t>( sp - tlStackLow ) : 0;
#endif
}

#endif
