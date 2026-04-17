//////////////////////////////////////////////////////////////////////
//
//  ThreadPool.h - Persistent worker pool that replaces the
//    per-pass thread spawning pattern in the rasterizer.  Workers are
//    created once, park on a condition variable between tasks, and
//    are reused across the entire render lifetime.
//
//    This exists primarily to eliminate the thread-creation overhead
//    paid by VCM on every progressive pass (64+ passes × 10 workers
//    = 640 pthread_create/join pairs per render).  It also enables
//    upcoming parallel-KD-tree-build and parallel-for primitives.
//
//    Worker behaviour:
//      - All workers wait on `tasksCv` when the queue is empty.
//      - Submit() enqueues and notifies one worker.
//      - ParallelFor() submits N tasks and blocks on a latch.
//      - Destructor signals shutdown, notifies all, joins each worker.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 16, 2026
//  Tabs: 4
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_THREAD_POOL_
#define RISE_THREAD_POOL_

#include "Threads/Threads.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		class ThreadPool
		{
		public:
			ThreadPool( unsigned int numWorkers );
			~ThreadPool();

			//! Enqueue a single task.  Workers will pick it up as soon as
			//! they're free.  Thread-safe to call from any thread.
			void Submit( std::function<void()> task );

			//! Run body(i) for i in [0, n) across the worker pool.
			//! Blocks until every task completes.  Safe to call
			//! recursively from within a pool task — the calling thread
			//! participates in executing queued work while waiting to
			//! avoid deadlock on bounded pools.
			void ParallelFor( unsigned int n, std::function<void( unsigned int )> body );

			//! Number of worker threads in the pool.
			unsigned int NumWorkers() const { return static_cast<unsigned int>( workers.size() ); }

		private:
			std::vector<RISETHREADID>		workers;
			std::deque<std::function<void()>>	tasks;
			std::mutex				tasksMut;
			std::condition_variable		tasksCv;
			std::atomic<bool>			shuttingDown;

			static void* WorkerProc( void* arg );
			void WorkerLoop();

			// Drains a single task if available.  Returns false if the
			// pool is shutting down AND the queue is drained.  Used by
			// ParallelFor to let the caller participate.
			bool ExecuteOnePendingTask();
		};

		//! Process-wide pool.  Created lazily on first access; lives until
		//! process exit.  Sized to HowManyThreadsToSpawn() at creation.
		ThreadPool& GlobalThreadPool();
	}
}

#endif
