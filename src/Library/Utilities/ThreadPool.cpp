//////////////////////////////////////////////////////////////////////
//
//  ThreadPool.cpp
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "ThreadPool.h"
#include "CPU.h"
#include "CPUTopology.h"
#include "../Interfaces/IOptions.h"
#include <algorithm>
#include <chrono>

using namespace RISE;
using namespace RISE::Implementation;

ThreadPool::ThreadPool( unsigned int numWorkers,
                        const std::vector<unsigned int>& affinityMask ) :
	shuttingDown( false ),
	affinity( affinityMask )
{
	if( numWorkers == 0 ) {
		numWorkers = 1;
	}
	workers.reserve( numWorkers );
	for( unsigned int i = 0; i < numWorkers; i++ ) {
		RISETHREADID tid = 0;
		Threading::riseCreateThread( WorkerProc, this, 0, 0, &tid );
		workers.push_back( tid );
	}
}

ThreadPool::~ThreadPool()
{
	{
		std::lock_guard<std::mutex> lk( tasksMut );
		shuttingDown.store( true, std::memory_order_release );
	}
	tasksCv.notify_all();
	for( RISETHREADID tid : workers ) {
		Threading::riseWaitUntilThreadFinishes( tid, 0 );
	}
}

void* ThreadPool::WorkerProc( void* arg )
{
	static_cast<ThreadPool*>( arg )->WorkerLoop();
	return 0;
}

void ThreadPool::WorkerLoop()
{
	// Apply platform-specific placement policy to self:
	//   - macOS: QOS_CLASS_USER_INITIATED — tells the scheduler to
	//     prefer P-cores and run at full clock.  No thread-affinity
	//     API on Apple Silicon; QoS is the only lever.
	//   - Linux: sched_setaffinity to the P + (E - reserved) mask.
	//   - Windows: SetThreadSelectedCpuSetMasks with the same mask.
	//
	// CRITICAL INVARIANT: if the user has explicitly opted in to the
	// legacy "render in the background" mode via
	// force_all_threads_low_priority, riseCreateThread's wrapper has
	// ALREADY lowered this thread's priority.  We MUST NOT overwrite
	// that — otherwise the opt-in is silently undone on macOS
	// (UTILITY → USER_INITIATED) and Windows (BELOW_NORMAL → NORMAL).
	// Only apply the render priority when the user has NOT opted
	// into low priority.  Linux is unaffected here: nice(10) persists
	// because riseSetThreadRenderPriority is a no-op on Linux.
	//
	// `thread_apply_qos` remains available as an A/B testing knob
	// (default true) so measurement scripts can isolate the QoS
	// call's cost.
	const bool forceLow = GlobalOptions().ReadBool( "force_all_threads_low_priority", false );
	if( !forceLow && GlobalOptions().ReadBool( "thread_apply_qos", true ) ) {
		Threading::riseSetThreadRenderPriority( 0 );
	}
	if( !affinity.empty() ) {
		Threading::riseSetThreadAffinity( affinity );
	}

	for( ;; ) {
		std::function<void()> task;
		{
			std::unique_lock<std::mutex> lk( tasksMut );
			tasksCv.wait( lk, [this] {
				return shuttingDown.load( std::memory_order_acquire ) || !tasks.empty();
			} );
			if( shuttingDown.load( std::memory_order_acquire ) && tasks.empty() ) {
				return;
			}
			task = std::move( tasks.front() );
			tasks.pop_front();
		}
		task();
	}
}

bool ThreadPool::ExecuteOnePendingTask()
{
	std::function<void()> task;
	{
		std::unique_lock<std::mutex> lk( tasksMut );
		if( tasks.empty() ) {
			return shuttingDown.load( std::memory_order_acquire ) ? false : false;
		}
		task = std::move( tasks.front() );
		tasks.pop_front();
	}
	task();
	return true;
}

void ThreadPool::Submit( std::function<void()> task )
{
	{
		std::lock_guard<std::mutex> lk( tasksMut );
		tasks.push_back( std::move( task ) );
	}
	tasksCv.notify_one();
}

void ThreadPool::ParallelFor( unsigned int n, std::function<void( unsigned int )> body )
{
	if( n == 0 ) {
		return;
	}

	// Read the legacy low-priority opt-in BEFORE the n == 1 fast
	// path, because that fast path would otherwise run body(0) on the
	// caller thread at normal priority — defeating the user's intent
	// to keep every render participant at reduced priority.  When
	// forceLow is set we always submit to the pool, even for n == 1,
	// so the work runs on a low-priority worker.
	const bool forceLow = GlobalOptions().ReadBool(
		"force_all_threads_low_priority", false );

	if( n == 1 && !forceLow ) {
		body( 0 );
		return;
	}

	// Latch — atomic counter + mutex/cv for completion signalling.
	std::atomic<unsigned int> remaining( n );
	std::mutex doneMut;
	std::condition_variable doneCv;

	{
		std::lock_guard<std::mutex> lk( tasksMut );
		for( unsigned int i = 0; i < n; i++ ) {
			tasks.push_back( [i, &body, &remaining, &doneMut, &doneCv] {
				body( i );
				if( remaining.fetch_sub( 1, std::memory_order_acq_rel ) == 1 ) {
					{
						std::lock_guard<std::mutex> lk2( doneMut );
					}
					doneCv.notify_all();
				}
			} );
		}
	}
	tasksCv.notify_all();

	// Calling thread participates: drain available tasks while we wait,
	// preventing deadlock if n > pool size and avoiding idle time on
	// the caller while workers churn.
	//
	// EXCEPTION: when the user has opted into legacy "render in the
	// background" mode (force_all_threads_low_priority), every render
	// participant is supposed to be at reduced priority.  The pool's
	// worker threads go through riseCreateThread's wrapper and hit
	// that path on start-up, but the CALLING thread (typically a
	// user-initiated thread like the CLI main thread) did not.
	// Letting it execute render tasks would leave one participant at
	// normal priority, silently defeating the user's intent.
	//
	// In that mode we wait on the latch without stealing tasks.  The
	// pool's own workers finish all tasks; the caller just blocks.
	// This costs us one worker's worth of parallelism for the lifetime
	// of this ParallelFor call, which is already the user's intent
	// (they traded throughput for system responsiveness).
	if( forceLow ) {
		std::unique_lock<std::mutex> lk( doneMut );
		doneCv.wait( lk, [&remaining] {
			return remaining.load( std::memory_order_acquire ) == 0;
		} );
	} else {
		while( remaining.load( std::memory_order_acquire ) > 0 ) {
			if( !ExecuteOnePendingTask() ) {
				// Queue is empty but tasks still running — wait.
				std::unique_lock<std::mutex> lk( doneMut );
				doneCv.wait_for( lk, std::chrono::milliseconds( 1 ), [&remaining] {
					return remaining.load( std::memory_order_acquire ) == 0;
				} );
			}
		}
	}
}

ThreadPool& RISE::Implementation::GlobalThreadPool()
{
	// Meyers' singleton — thread-safe init since C++11.
	// Worker count and affinity are derived from the machine's CPU
	// topology.  Policy (see CPUTopology::ComputeRenderPoolSize):
	//   - All P-cores get a worker.
	//   - All but `render_thread_reserve_count` (default 1) E-cores
	//     also get a worker.
	//   - 1 E-core is always left to the OS for UI / daemons.
	//   - Benchmark override: set option to 0 to use every core.
	// On homogeneous systems, 1 core is reserved from the total.
	static const unsigned int numWorkers = ComputeRenderPoolSize();
	static const std::vector<unsigned int> affinity = GetRenderAffinityMask();
	static ThreadPool instance( numWorkers, affinity );
	return instance;
}
