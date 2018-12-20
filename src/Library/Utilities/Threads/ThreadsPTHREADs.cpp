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
#include "../RTime.h"
#ifndef NO_PTHREAD_SUPPORT
	#include <pthread.h>
	#include <semaphore.h>
#endif

using namespace RISE;

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

	pthread_attr_init( &attr );
	pthread_create( &tid, &attr, pFunc, pParam );

	if( threadid ) {
		*threadid = (RISETHREADID)tid;
	}

//	GlobalLog()->PrintEx( eLog_Info, "Starting Thread ID:%d", tid );
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
	sem_t* sem = new sem_t;
	sem_init( sem, 0, value );
	return (void*)sem;
#endif
	return 0;
}

void Threading::riseDestroySemaphore(
	RISESEMAPHORE sem
	)
{
#ifndef NO_PTHREAD_SUPPORT
	sem_t* pSem = (sem_t*)sem;
	sem_destroy( pSem );
	delete pSem;
#endif
}

void Threading::riseSemaphoreAcquire(
	RISESEMAPHORE sem
	)
{
#ifndef NO_PTHREAD_SUPPORT
	sem_t* pSem = (sem_t*)sem;
	sem_wait( pSem );
#endif
}

void Threading::riseSemaphoreRelease(
	RISESEMAPHORE sem,
	int count
	)
{
#ifndef NO_PTHREAD_SUPPORT
	sem_t* pSem = (sem_t*)sem;
	sem_post( pSem );
#endif
}

#endif

