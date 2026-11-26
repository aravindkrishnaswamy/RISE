//////////////////////////////////////////////////////////////////////
//
//  CancellableProgressCallback.cpp
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "CancellableProgressCallback.h"
#include <chrono>
#include <thread>

using namespace RISE;

void PreparedRenderProgress::Reset()
{
	// The UI may also have replaced the entire scene/controller between polls.
	static std::atomic<std::uint64_t> nextGeneration{2};
	generation.store(nextGeneration.fetch_add(2, std::memory_order_relaxed));
	paused.store(false);
	fraction.store(0.0);
}

bool PreparedRenderProgress::IsCancelled() const
{
	while( paused.load() && !CancelRequested() )
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	return CancelRequested();
}

bool PreparedRenderProgress::Progress(double done, double total)
{
	if( total > 0 ) fraction.store(done / total);
	return !IsCancelled();
}

CancellableProgressCallback::CancellableProgressCallback( IProgressCallback* inner )
: mInner( inner )
, mCancelled( false )
, mInnerCancelled( false )
{
}

CancellableProgressCallback::~CancellableProgressCallback()
{
}

void CancellableProgressCallback::SetInner( IProgressCallback* inner )
{
	mInner.store( inner, std::memory_order_release );
}

void CancellableProgressCallback::RequestCancel()
{
	prepared.RequestCancel();
	mCancelled.store( true, std::memory_order_release );
}

void CancellableProgressCallback::Reset()
{
	prepared.Reset();
	mCancelled.store( false, std::memory_order_release );
	mInnerCancelled.store( false, std::memory_order_release );
}

bool CancellableProgressCallback::IsCancelRequested() const
{
	return mCancelled.load( std::memory_order_acquire );
}

bool CancellableProgressCallback::WasCancellationObserved() const
{
	return mCancelled.load( std::memory_order_acquire )
		|| mInnerCancelled.load( std::memory_order_acquire ) || prepared.CancelRequested();
}

bool CancellableProgressCallback::Progress( const double progress, const double total )
{
	if( mCancelled.load( std::memory_order_acquire ) )
	{
		return false;
	}
	IProgressCallback* inner = mInner.load( std::memory_order_acquire );
	if( inner )
	{
		const bool accepted = inner->Progress( progress, total );
		if( !accepted )
		{
			mInnerCancelled.store( true, std::memory_order_release );
		}
		return accepted;
	}
	return true;
}

void CancellableProgressCallback::SetTitle( const char* title )
{
	IProgressCallback* inner = mInner.load( std::memory_order_acquire );
	if( inner )
	{
		inner->SetTitle( title );
	}
}
