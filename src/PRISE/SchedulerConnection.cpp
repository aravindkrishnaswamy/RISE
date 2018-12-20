//////////////////////////////////////////////////////////////////////
//
//  SchedulerConnection.cpp - Implements the worker's connection to
//    the master scheduler
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: December 5, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include <string>
#include "SchedulerConnection.h"
#include "../Interfaces/ILog.h"
#include "../Version.h"

SchedulerConnection::SchedulerConnection( ICommunicator* pCommunicator_ ) : 
  ServerConnection( pCommunicator_ )
{
}

SchedulerConnection::~SchedulerConnection()
{
}

bool SchedulerConnection::GetSceneFilename( String& filename )
{
	// We try to get the name of the scene file from the server
	if( !TryReceiveSpecificMessage( eMessage_SceneFile ) ) {
		return false;
	}

	filename = pRecvBuffer->Pointer();

	return true;
}

bool SchedulerConnection::GetSceneModels( std::deque<MODEL_INFO>& models )
{
	// We first try to get the model count
	if( !TryReceiveSpecificMessage( eMessage_ModelCount ) ) {
		return false;
	}

	unsigned int model_count = pRecvBuffer->getUInt();
	for( int i=0; i<model_count; i++ ) {
		if( !TryReceiveSpecificMessage( eMessage_Model ) ) {
			return false;
		}

		MODEL_INFO	mi;
		mi.ptPosition.x = pRecvBuffer->getDouble();
		mi.ptPosition.y = pRecvBuffer->getDouble();
		mi.ptPosition.z = pRecvBuffer->getDouble();
		mi.vOrientation.x = pRecvBuffer->getDouble();
		mi.vOrientation.y = pRecvBuffer->getDouble();
		mi.vOrientation.z = pRecvBuffer->getDouble();
		mi.material = pRecvBuffer->PointerAtCursor();
		pRecvBuffer->seek( IBuffer::CUR, strlen(mi.material.c_str())+1 );
		mi.szFilename = pRecvBuffer->PointerAtCursor();
		models.push_back( mi );
	}

	return true;
}

int SchedulerConnection::ProcessSchedulerRequest( WorkerRenderer* pRenderer, WorkQueue& workqueue )
{
	if( !TryReceiveMessage() ) {
		GlobalLog()->PrintEasyError( "SchedulerConnection::ProcessSchedulerRequest:: failed to receive message" );
		return -1;
	}

	switch( mtRecvType )
	{
	default:
		return -1;
		break;

	case eMessage_UnresolvedRay:
		{
			// Read the unresolved ray from the buffer
			Ray	r;
			r.origin.x = pRecvBuffer->getDouble();
			r.origin.y = pRecvBuffer->getDouble();
			r.origin.z = pRecvBuffer->getDouble();

			r.dir.x = pRecvBuffer->getDouble();
			r.dir.y = pRecvBuffer->getDouble();
			r.dir.z = pRecvBuffer->getDouble();

			// get the pixel
			unsigned int x = pRecvBuffer->getUInt();
			unsigned int y = pRecvBuffer->getUInt();

			// Get the octree call stack so that we can
			// find our place again
			IMemoryBuffer*	pCallStack = new Implementation::MemoryBuffer( pRecvBuffer->Size() );
			pCallStack->setBytes( pRecvBuffer->PointerAtCursor(), pRecvBuffer->Size()-pRecvBuffer->getCurPos() );
			workqueue.AddIncompleteToEnd( r, x, y, pCallStack );
			pCallStack->RemoveRef();
		}
		return 1;
		break;

	case eMessage_NewCell:
		{
			// Retreive the new cell and add it to the work queue
			Rect	rc;
			rc.top = pRecvBuffer->getUInt();
			rc.left = pRecvBuffer->getUInt();
			rc.bottom = pRecvBuffer->getUInt();
			rc.right = pRecvBuffer->getUInt();

			GlobalLog()->PrintEx( eLog_Event, TYPICAL_PRIORITY, "Received new cell: top=%d, left=%d, bottom=%d, right=%d", rc.top, rc.left, rc.bottom, rc.right );

			// Convert the Rect into the series of rays
			ICamera* pCamera = pRenderer->pRise->GetJob()->pScene->GetCamera();

			for( int y=rc.top; y<=rc.bottom; y++ ) {
				for( int x=rc.left; x<=rc.right; x++ ) {
					Ray r = pCamera->GenerateRay( Point2D( x, y ) );
					r.dir.Normalize();

					workqueue.AddToEnd( r, x, y );
				}
			}
		}
		return 1; 
		break;

	case eMessage_GetQueueSize:
		{
			// The scheduler wants to know the size of our queue, so return it
			pSendBuffer->Resize( sizeof( unsigned int ), true );
			pSendBuffer->seek( IBuffer::START, 0 );
			pSendBuffer->setUInt( workqueue.Size() );

			TrySendMessage( eMessage_QueueSize, true );
		}
		return 1;
		break;

	case eMessage_DoneRendering:
		GlobalLog()->PrintEasyEvent( "Scheduler says we're done with all the cells" );
		return 2;
		break;
	};

	return -1;
}

bool SchedulerConnection::SendWorkerType( int workertype )
{
	pSendBuffer->Resize( sizeof( int ), true );
	pSendBuffer->seek( IBuffer::START, 0 );

	pSendBuffer->setInt( workertype );

	return TrySendMessage( eMessage_WorkerType, true );
}

bool SchedulerConnection::SendResult( const unsigned int x, const unsigned int y, const IFXColor& c )
{
	pSendBuffer->Resize( sizeof( IFXColor ) + sizeof( unsigned int ) * 2, true );
	pSendBuffer->seek( IBuffer::START, 0 );

	pSendBuffer->setUInt( x );
	pSendBuffer->setUInt( y );

	pSendBuffer->setDouble( c.base.r );
	pSendBuffer->setDouble( c.base.g );
	pSendBuffer->setDouble( c.base.b );
	pSendBuffer->setDouble( c.a );

	return TrySendMessage( eMessage_WorkerResult, true );
}

bool SchedulerConnection::SendRenderComplete()
{
	return TrySendMessage( eMessage_DoneRendering, false );
}

bool SchedulerConnection::SendUnresolvedRay( const Ray& ray, const unsigned int x, const unsigned int y, IMemoryBuffer* pTraversalBuf )
{
	unsigned int travbufmempos = pTraversalBuf->getCurPos()+1;
	pSendBuffer->Resize( sizeof( Ray ) + sizeof( unsigned int ) * 2 + travbufmempos );
	pSendBuffer->seek( IBuffer::START, 0 );

	pSendBuffer->setDouble( ray.origin.x );
	pSendBuffer->setDouble( ray.origin.y );
	pSendBuffer->setDouble( ray.origin.z );

	pSendBuffer->setDouble( ray.dir.x );
	pSendBuffer->setDouble( ray.dir.y );
	pSendBuffer->setDouble( ray.dir.z );

	pSendBuffer->setUInt( x );
	pSendBuffer->setUInt( y );

	pSendBuffer->setBytes( pTraversalBuf->Pointer(), travbufmempos );

	return TrySendMessage( eMessage_UnresolvedRay, true );
}
