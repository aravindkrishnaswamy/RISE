//////////////////////////////////////////////////////////////////////
//
//  ThreadPoolExceptionSafetyTest.cpp
//
//  Regression test for ThreadPool::ParallelFor exception safety.
//
//  Root cause it locks down (2026-07): under memory pressure a render
//  worker's std::bad_alloc used to escape the pool worker thread's task
//  invocation.  An exception unwinding out of a pthread entry point is
//  std::terminate — the whole process crashes.  Symmetrically, a throw
//  out of the caller's own ParallelFor participation left the other pool
//  workers running against the caller's tearing-down render frame (a
//  use-after-free).  The fix captures the first task exception, waits for
//  the latch barrier (every worker has stopped touching the caller's
//  closure), and only THEN re-throws to the caller.
//
//  These tests assert the observable contract:
//    1. A throwing body surfaces AS A CATCHABLE EXCEPTION at the caller
//       (no std::terminate), and EVERY non-throwing index still ran
//       (the latch drained; no lost work, no hang).
//    2. The pool is fully usable AFTER a ParallelFor that threw.
//    3. Many simultaneous throws still yield exactly one caught exception
//       and a drained latch.
//
//  Standalone executable (no framework), matching the repo's test style.
//
//  Tabs: 4
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "../src/Library/Utilities/ThreadPool.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

using namespace RISE::Implementation;

// Deterministic allocation-failure injection for T5.  The global
// operator new consults a thread_local countdown that is armed ONLY on the
// calling thread around one ParallelFor submission: the k-th allocation that
// thread makes while armed throws std::bad_alloc.  Every other thread (pool
// workers) and every unarmed allocation is untouched.
static thread_local long t_allocCountdown = 0;     // 0 = disarmed
static thread_local bool t_allocFired = false;

void* operator new( std::size_t bytes )
{
	if( t_allocCountdown > 0 && --t_allocCountdown == 0 ) {
		t_allocFired = true;
		throw std::bad_alloc();
	}
	if( void* p = std::malloc( bytes ? bytes : 1 ) ) {
		return p;
	}
	throw std::bad_alloc();
}
void* operator new[]( std::size_t bytes ) { return ::operator new( bytes ); }
void operator delete( void* p ) noexcept { std::free( p ); }
void operator delete[]( void* p ) noexcept { std::free( p ); }
void operator delete( void* p, std::size_t ) noexcept { std::free( p ); }
void operator delete[]( void* p, std::size_t ) noexcept { std::free( p ); }

namespace
{
	struct AllocFailureArm
	{
		explicit AllocFailureArm( long k ) { t_allocFired = false; t_allocCountdown = k; }
		~AllocFailureArm() { t_allocCountdown = 0; }
		AllocFailureArm( const AllocFailureArm& ) = delete;
		AllocFailureArm& operator=( const AllocFailureArm& ) = delete;
	};
}

static int g_pass = 0;
static int g_fail = 0;

static void Check( bool cond, const std::string& what )
{
	if( cond ) {
		++g_pass;
	} else {
		++g_fail;
		std::printf( "  FAIL: %s\n", what.c_str() );
	}
}

// A distinctive exception type so we can assert the SAME exception the
// body threw is what reaches the caller.
struct MarkerException : public std::runtime_error
{
	explicit MarkerException( int idx ) : std::runtime_error( "marker" ), which( idx ) {}
	int which;
};

//! One body throws; every other index must still execute, and the caller
//! must receive a catchable exception (never std::terminate / hang).
static void TestSingleThrowPropagatesAndDrains()
{
	std::printf( "T1: a single throwing task propagates + all others run...\n" );
	ThreadPool pool( 4, {} );

	const unsigned int n = 64;
	std::vector<std::atomic<int>> ran( n );
	for( unsigned int i = 0; i < n; ++i ) ran[i].store( 0 );

	const unsigned int throwIdx = 37;
	bool caught = false;
	int  caughtWhich = -1;

	try {
		pool.ParallelFor( n, [&]( unsigned int i ) {
			ran[i].fetch_add( 1 );
			if( i == throwIdx ) {
				throw MarkerException( static_cast<int>( i ) );
			}
		} );
	}
	catch( const MarkerException& e ) {
		caught = true;
		caughtWhich = e.which;
	}
	catch( ... ) {
		caught = true;      // wrong type, but at least catchable
		caughtWhich = -2;
	}

	Check( caught, "the throwing body surfaced as a catchable exception at the caller" );
	Check( caughtWhich == static_cast<int>( throwIdx ),
		"the exact exception the body threw is the one the caller received" );

	// Every task ran exactly once — the latch drained (no hang) and no
	// index was skipped because a sibling threw.
	unsigned int runCount = 0;
	bool eachOnce = true;
	for( unsigned int i = 0; i < n; ++i ) {
		const int c = ran[i].load();
		if( c != 1 ) eachOnce = false;
		runCount += static_cast<unsigned int>( c );
	}
	Check( eachOnce && runCount == n,
		"every one of the n bodies ran exactly once despite one throwing" );
}

//! After a ParallelFor that threw, the pool must still schedule work.
static void TestPoolUsableAfterThrow()
{
	std::printf( "T2: the pool is still usable after a ParallelFor threw...\n" );
	ThreadPool pool( 4, {} );

	try {
		pool.ParallelFor( 16, []( unsigned int i ) {
			if( i == 0 ) throw std::runtime_error( "boom" );
		} );
	}
	catch( ... ) {
		// expected
	}

	std::atomic<int> sum( 0 );
	pool.ParallelFor( 100, [&]( unsigned int i ) {
		sum.fetch_add( static_cast<int>( i ) );
	} );

	// 0 + 1 + ... + 99 = 4950
	Check( sum.load() == 4950,
		"a clean ParallelFor after a throwing one ran every task (pool not wedged)" );
}

//! Many tasks throw at once: exactly one exception reaches the caller and
//! the latch still drains (no double-free of the latch, no hang).
static void TestManyThrowsOneCaught()
{
	std::printf( "T3: many simultaneous throws -> one caught, latch drains...\n" );
	ThreadPool pool( 8, {} );

	const unsigned int n = 200;
	std::atomic<int> ranCount( 0 );
	int caught = 0;

	try {
		pool.ParallelFor( n, [&]( unsigned int i ) {
			ranCount.fetch_add( 1 );
			// every even index throws
			if( ( i % 2 ) == 0 ) throw std::runtime_error( "even" );
		} );
	}
	catch( const std::exception& ) {
		caught = 1;
	}
	catch( ... ) {
		caught = 1;
	}

	Check( caught == 1, "at least one (and a well-formed) exception reached the caller" );
	Check( ranCount.load() == static_cast<int>( n ),
		"all tasks ran even when half of them threw (latch fully drained)" );
}

//! Mirrors the ONLY real ThreadPool::Submit-with-latch user in tree,
//! LightVertexStore::BuildKDTreeParallel: a caller Submits a task and
//! blocks on cv.wait until an `outstanding` counter drains to zero, and
//! the task decrements that counter.  If the task body THROWS, two
//! things must both hold or the caller hangs / the process dies:
//!   1. The task must drain its latch unit on the throwing path too
//!      (here via an RAII guard, exactly as the VCM code now does), so
//!      `outstanding` still reaches 0 and the waiter wakes.
//!   2. ThreadPool::WorkerLoop must SWALLOW the exception that then
//!      escapes the task, so the pool worker thread (and the process)
//!      survives instead of std::terminate-ing.
//! The wait is BOUNDED so a regression (a hang) FAILS this test instead
//! of hanging CI forever.
static void TestSubmitThrowDoesNotHangLatchWaiter()
{
	std::printf( "T4: a throwing Submit'd task drains its latch (no hang, no terminate)...\n" );
	ThreadPool pool( 4, {} );

	std::mutex              mut;
	std::condition_variable cv;
	std::atomic<unsigned int> outstanding( 1 );

	pool.Submit( [&] {
		// RAII drain: decrement + notify on EVERY exit, including the
		// exception unwinding out of this task body just below.  This is
		// the exact discipline VCMLightVertexStore's Submit lambdas use.
		struct DrainGuard {
			std::atomic<unsigned int>& out;
			std::mutex&                m;
			std::condition_variable&   c;
			~DrainGuard() {
				if( out.fetch_sub( 1, std::memory_order_acq_rel ) == 1 ) {
					{ std::lock_guard<std::mutex> lk( m ); }
					c.notify_all();
				}
			}
		} guard{ outstanding, mut, cv };

		// Let the exception ESCAPE the task (do not catch it here) so the
		// test also exercises WorkerLoop's swallow: the guard runs during
		// unwind, then the exception hits the pool worker's task() call.
		throw std::runtime_error( "submit boom" );
	} );

	bool drained = false;
	{
		std::unique_lock<std::mutex> lk( mut );
		drained = cv.wait_for( lk, std::chrono::seconds( 5 ), [&] {
			return outstanding.load( std::memory_order_acquire ) == 0;
		} );
	}
	Check( drained,
		"the latch waiter woke within the timeout (task drained its unit despite throwing -- no hang)" );

	// Pool must still work after a Submit'd task threw.
	std::atomic<int> sum( 0 );
	pool.ParallelFor( 50, [&]( unsigned int i ) { sum.fetch_add( static_cast<int>( i ) ); } );
	Check( sum.load() == 1225, "pool still schedules work after a throwing Submit (not wedged)" );
}

//! ParallelFor's enqueue must be transactional: if the k-th allocation of
//! the submission throws (std::function heap storage or deque block growth),
//! NO task of that call may stay queued (each captures the caller's `body`
//! by reference -- a leaked one runs a dangling closure).  Sweep k over every
//! allocation the submission makes, from the first push through mid-loop
//! pushes and deque block growth, until a call completes unfailed.
static void TestSubmissionAllocationFailureIsTransactional()
{
	std::printf( "T5: allocation failure mid-submission leaves no task queued...\n" );

	const unsigned int n = 300;
	unsigned int throws = 0;
	unsigned int completes = 0;
	bool allZeroOnThrow = true;
	bool allFullOnComplete = true;
	bool healthyOk = true;
	bool unrelatedOk = true;
	bool wrongException = false;
	long lastK = 0;

	for( long k = 1; k < 4000; ++k ) {
		lastK = k;
		// A fresh pool per k: its task deque starts with no spare blocks, so
		// the sweep also crosses every deque block / map growth allocation
		// (a long-lived pool's deque keeps spare blocks and never grows).
		ThreadPool pool( 4, {} );
		// An unrelated task queued before the failing call must still run.
		std::atomic<int> unrelatedRan( 0 );
		pool.Submit( [&unrelatedRan] { unrelatedRan.fetch_add( 1 ); } );

		std::atomic<unsigned int> bodyRuns( 0 );
		bool threw = false;
		bool fired = false;
		{
			// `body` is constructed before arming so only the submission's
			// own allocations are counted.
			std::function<void( unsigned int )> body = [&bodyRuns]( unsigned int ) {
				// Disarm on first execution: the enqueue loop has finished
				// once any task runs on this thread, so a leftover countdown
				// must not fire inside task execution instead.
				t_allocCountdown = 0;
				bodyRuns.fetch_add( 1 );
			};
			try {
				AllocFailureArm arm( k );
				pool.ParallelFor( n, std::move( body ) );
				fired = t_allocFired;
			}
			catch( const std::bad_alloc& ) {
				threw = true;
				fired = t_allocFired;
			}
			catch( ... ) {
				threw = true;
				wrongException = true;
			}
		}

		if( threw ) {
			++throws;
			// Short settle: any leaked task would run on a worker now.
			std::this_thread::sleep_for( std::chrono::milliseconds( 2 ) );
			if( bodyRuns.load() != 0 ) {
				allZeroOnThrow = false;
			}
		} else {
			++completes;
			if( bodyRuns.load() != n ) {
				allFullOnComplete = false;
			}
		}

		// A following healthy call on the same pool runs every index
		// exactly once (and does not hang on a leftover latch).
		std::vector<std::atomic<int>> ran( n );
		for( unsigned int i = 0; i < n; ++i ) ran[i].store( 0 );
		pool.ParallelFor( n, [&ran]( unsigned int i ) { ran[i].fetch_add( 1 ); } );
		for( unsigned int i = 0; i < n; ++i ) {
			if( ran[i].load() != 1 ) {
				healthyOk = false;
			}
		}
		if( threw && bodyRuns.load() != 0 ) {
			allZeroOnThrow = false;
		}

		// The unrelated task must have run (bounded wait).
		for( int w = 0; w < 5000 && unrelatedRan.load() == 0; ++w ) {
			std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
		}
		if( unrelatedRan.load() != 1 ) {
			unrelatedOk = false;
		}

		// Done once the countdown outlived the whole submission.
		if( !threw && !fired ) {
			break;
		}
	}

	std::printf( "  swept k=1..%ld: %u submissions threw, %u completed\n",
		lastK, throws, completes );
	Check( !wrongException, "a failed submission surfaces std::bad_alloc, not another type" );
	// make_shared latch (1) + one heap std::function per task (n) + at least
	// one deque block growth: more than n + 1 distinct failure points.
	Check( throws > n + 1, "the sweep reached failures at the first push, every mid-loop push and deque growth" );
	Check( completes >= 1, "the sweep terminated with an unfailed submission" );
	Check( allZeroOnThrow, "no task of a failed submission ever ran (after settle and after a healthy call)" );
	Check( allFullOnComplete, "an unfailed submission ran all n bodies" );
	Check( healthyOk, "a healthy ParallelFor after each failure ran every index exactly once" );
	Check( unrelatedOk, "an unrelated task queued before the failing call still ran exactly once" );
}

int main()
{
	std::printf( "=== ThreadPoolExceptionSafetyTest ===\n" );

	TestSingleThrowPropagatesAndDrains();
	TestPoolUsableAfterThrow();
	TestManyThrowsOneCaught();
	TestSubmitThrowDoesNotHangLatchWaiter();
	TestSubmissionAllocationFailureIsTransactional();

	std::printf( "=== ThreadPoolExceptionSafetyTest: %d passed, %d failed ===\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
