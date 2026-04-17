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
#include <algorithm>
#include <chrono>

using namespace RISE;
using namespace RISE::Implementation;

ThreadPool::ThreadPool( unsigned int numWorkers ) :
	shuttingDown( false )
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
	if( n == 1 ) {
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

namespace
{
	int ResolveDefaultPoolSize()
	{
		int logical = 0, physical = 0;
		GetCPUCount( logical, physical );
		int total = logical * physical;
		if( total < 1 ) {
			total = 1;
		}
		return total;
	}
}

ThreadPool& RISE::Implementation::GlobalThreadPool()
{
	// Meyers' singleton — thread-safe init since C++11.
	static ThreadPool instance( static_cast<unsigned int>( ResolveDefaultPoolSize() ) );
	return instance;
}
