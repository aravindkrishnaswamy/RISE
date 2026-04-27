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

using namespace RISE;

CancellableProgressCallback::CancellableProgressCallback( IProgressCallback* inner )
: mInner( inner )
, mCancelled( false )
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
	mCancelled.store( true, std::memory_order_release );
}

void CancellableProgressCallback::Reset()
{
	mCancelled.store( false, std::memory_order_release );
}

bool CancellableProgressCallback::IsCancelRequested() const
{
	return mCancelled.load( std::memory_order_acquire );
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
		return inner->Progress( progress, total );
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
