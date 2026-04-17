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
			//! Construct a pool.  `affinityMask` is the list of CPU IDs
			//! the workers should be pinned to (Linux / Windows only;
			//! macOS ignores this and relies on QoS class).  Pass an
			//! empty mask to let the scheduler choose freely.
			ThreadPool( unsigned int numWorkers,
			            const std::vector<unsigned int>& affinityMask );
			~ThreadPool();

			//! Enqueue a single task.  Workers will pick it up as soon as
			//! they're free.  Thread-safe to call from any thread.
			void Submit( std::function<void()> task );

			//! Run body(i) for i in [0, n) across the worker pool.
			//! Blocks until every task completes.
			//!
			//! Recursion safety in the DEFAULT mode (no
			//! force_all_threads_low_priority): safe.  The calling
			//! thread drains queued tasks while it waits, so a pool
			//! worker calling ParallelFor cannot deadlock even on a
			//! one-worker pool.
			//!
			//! Recursion in LEGACY low-priority mode
			//! (force_all_threads_low_priority true): NOT SAFE on
			//! bounded pools.  To uphold the "every render thread at
			//! reduced priority" contract, the caller does NOT steal
			//! tasks and the n == 1 fast path goes through the queue.
			//! If a pool worker calls ParallelFor while the pool is
			//! already saturated (e.g. a single-worker pool, or n
			//! bigger than the idle-worker count plus 1), it will
			//! block waiting for tasks no worker is available to
			//! execute.  Render call sites today do NOT recurse, so
			//! this is a contract restriction rather than a live bug;
			//! any future caller that wants to recurse must first
			//! check the legacy-mode option and route around the pool.
			void ParallelFor( unsigned int n, std::function<void( unsigned int )> body );

			//! Number of worker threads in the pool.
			unsigned int NumWorkers() const { return static_cast<unsigned int>( workers.size() ); }

		private:
			std::vector<RISETHREADID>		workers;
			std::deque<std::function<void()>>	tasks;
			std::mutex				tasksMut;
			std::condition_variable		tasksCv;
			std::atomic<bool>			shuttingDown;
			std::vector<unsigned int>		affinity;   ///< CPU IDs to pin workers to (Linux/Windows)

		public:
			//! Affinity mask workers are pinned to (may be empty).
			//! Exposed so worker startup code can apply it.
			const std::vector<unsigned int>& GetAffinityMask() const { return affinity; }

		private:

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
