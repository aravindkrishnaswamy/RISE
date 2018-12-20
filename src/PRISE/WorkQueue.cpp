//////////////////////////////////////////////////////////////////////
//
//  WorkQueue.cpp - Implementation of the work queue
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: December 7, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "WorkQueue.h"

WorkQueue::WorkQueue()
{
	mutQueueAccess = Threads::ifxCreateMutex();
}

WorkQueue::~WorkQueue()
{
	Threads::ifxDestroyMutex( mutQueueAccess );
}


void WorkQueue::AddToEnd( const Ray& r, const unsigned int x, const unsigned int y )
{
	Threads::ifxMutexLock( mutQueueAccess );
	{
		WORK_RAY	wr;
		wr.type = eCell;
		wr.r = r;
		wr.x = x;
		wr.y = y;
		wr.mb = 0;
		worklist.push_back( wr );
	}
	Threads::ifxMutexUnlock( mutQueueAccess );
}

void WorkQueue::AddIncompleteToEnd( const Ray& r, const unsigned int x, const unsigned int y, IMemoryBuffer* mb )
{
	Threads::ifxMutexLock( mutQueueAccess );
	{
		WORK_RAY	wr;
		wr.type = ePrimary;
		wr.r = r;
		wr.x = x;
		wr.y = y;
		wr.mb = mb;
		mb->AddRef();
		worklist.push_back( wr );
	}
	Threads::ifxMutexUnlock( mutQueueAccess );
}

bool WorkQueue::GetFront( Ray& r, unsigned int& x, unsigned int& y, IMemoryBuffer*& mb )
{
	bool bSuccess = false;

	Threads::ifxMutexLock( mutQueueAccess );
	{
		if( worklist.size() > 0 ) {
			WORK_RAY wr = worklist.front();
			r = wr.r;
			x = wr.x;
			y = wr.y;
			mb = wr.mb;
			worklist.pop_front();
			bSuccess = true;
		}
	}
	Threads::ifxMutexUnlock( mutQueueAccess );

	return bSuccess;
}

unsigned int WorkQueue::Size()
{
	unsigned int ret = 0;

	Threads::ifxMutexLock( mutQueueAccess );
	{
		ret = worklist.size();
	}
	Threads::ifxMutexUnlock( mutQueueAccess );

	return ret;
}

