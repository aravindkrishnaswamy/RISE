//////////////////////////////////////////////////////////////////////
//
//  Threads.h - Provides a platform independent interface to 
//  threading and multiple thread protection systems.  This interface
//  only a set of declarations for functions, the other files in this
//  folder will implement platform specific code for handling all this
//
//  All our functions are prefixed with rise, so that we don't have
//  namespace collisions
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 07, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef UTIL_THREADS_
#define UTIL_THREADS_

namespace RISE
{
	typedef unsigned int RISETHREADID;
	typedef void* RISEMUTEX;
	typedef void* RISESLEEP;
	typedef void* RISESEMAPHORE;

	// When a thread is created, it needs an execution point, 
	// This callback function represents the execution point.
	typedef void* (* THREAD_FUNC)(void *threadData);

	//! Utility thread functions
	struct Threading
	{
		//! Creates and starts a thread running
		/// \return 1 if successful, 0 otherwise
		static unsigned int riseCreateThread( 
			THREAD_FUNC pFunc, 
			void* pParam, 
			unsigned int initial_stack_size, 
			void* thread_attributes, 
			RISETHREADID* threadid 
			);

		//! Waits for a thread to finish execution
		/// \return 1 is successful, 0 otherwise
		static unsigned int riseWaitUntilThreadFinishes( 
			RISETHREADID threadid, 
			void* threadreturncode 
			);

		/// \return Handle to the current thread
		static RISETHREADID riseGetCurrentThread( );

		//! Initializes a structure which threads can use to put themselves to sleep for 
		//! some period of time
		/// \return Newly created sleep object
		static RISESLEEP riseCreateSleep( );

		//! Puts the calling thread to sleep for the given period of time
		//! duration is given in milliseconds
		static void riseSleep( 
			RISESLEEP sleep, 
			unsigned int duration 
			);

		//! Destroys the sleep structure
		static void riseDestroySleep( 
			RISESLEEP sleep 
			);

		//! Suspends a thread
		static void riseSuspendThread( 
			RISETHREADID threadid 
			);

		//! Resumes a suspended thread
		static void riseResumeThread( 
			RISETHREADID threadid 
			);

		//! Creates a mutex
		/// \return Newly created mutex object
		static RISEMUTEX riseCreateMutex( );

		//! Destroys the given mutex
		static void riseDestroyMutex( 
			RISEMUTEX mutex 
			);

		//! Gets a lock on the mutex, waits for as long as necessary
		static void riseMutexLock( 
			RISEMUTEX mutex 
			);

		//! Trys to get a lock, if it fails, it returns immediately
		/// \return TRUE if lock was successful, FALSE otherwise
		static bool riseMutexTryLock( 
			RISEMUTEX mutex 
			);

		//! Releases a locked mutex
		static void riseMutexUnlock( 
			RISEMUTEX mutex 
			);

		//! Creates a semaphore
		/// \return Newly created semaphore object
		static RISESEMAPHORE riseCreateSemaphore(
			int value
			);

		//! Destroys the given semaphore
		static void riseDestroySemaphore(
			RISESEMAPHORE sem
			);

		//! Acquires the semaphore P
		static void riseSemaphoreAcquire(
			RISESEMAPHORE sem
			);

		//! Releases the semaphore V
		static void riseSemaphoreRelease(
			RISESEMAPHORE sem,
			int count
			);
	};

	// Wrapper class around Mutex, this makes it easier to use the mutex
	class RMutex
	{
	private:
		RISEMUTEX mutex;

	public:
		RMutex()
		{
			mutex = Threading::riseCreateMutex();
		}

		~RMutex()
		{
			Threading::riseDestroyMutex( mutex );
		}

		void lock() const
		{
			Threading::riseMutexLock( mutex );
		}

		bool try_lock() const
		{
			return Threading::riseMutexTryLock( mutex );
		}

		void unlock() const
		{
			return Threading::riseMutexUnlock( mutex );
		}
	};

	// Class that implements a read/write mutex using semaphores
	class RMutexReadWrite
	{
	private:
		RISESEMAPHORE semReaders;
		RISESEMAPHORE semWriters;
		int nReaders;

	public:
		RMutexReadWrite() :
		  nReaders( 0 )
		{
			semReaders = Threading::riseCreateSemaphore(1);
			semWriters = Threading::riseCreateSemaphore(1);
		}

	    ~RMutexReadWrite()
		{
			Threading::riseDestroySemaphore(semReaders);
			Threading::riseDestroySemaphore(semWriters);
		}

		void read_lock()
		{
			// P( semReaders )
			Threading::riseSemaphoreAcquire( semReaders );

			nReaders++;

			if( nReaders == 1 ) {
				// P( semWriters )
				Threading::riseSemaphoreAcquire( semWriters );
			}

			// V( semReaders )
			Threading::riseSemaphoreRelease( semReaders, 1 );
		};

		void read_unlock()
		{
			// P( semReaders )
			Threading::riseSemaphoreAcquire( semReaders );

			nReaders--;

			if( nReaders == 0 ) {
				// V( semWriters )
				Threading::riseSemaphoreRelease( semWriters, 1 );
			}

			// V( semReaders )
			Threading::riseSemaphoreRelease( semReaders, 1 );
		};

		void write_lock()
		{
			// P( semWriters )
			Threading::riseSemaphoreAcquire( semWriters );
		}

		inline void write_unlock()
		{
			// V( semWriters )
			Threading::riseSemaphoreRelease( semWriters, 1 );
		}
	};
}

#endif

