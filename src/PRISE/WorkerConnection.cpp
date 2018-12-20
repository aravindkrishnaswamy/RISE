//////////////////////////////////////////////////////////////////////
//
//  WorkerConnection.cpp - Implements the worker connection 
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: December 4, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "WorkerConnection.h"
#include "ISchedulerEngine.h"
#include "../Interfaces/ILog.h"
#include <string>

WorkerConnection::WorkerConnection( ICommunicator* pCommunicator_ ) : 
  ClientConnection( pCommunicator_ )
{
	mutComm = Threads::ifxCreateMutex();
}

WorkerConnection::~WorkerConnection()
{
	Threads::ifxDestroyMutex( mutComm );
}

bool WorkerConnection::SendSceneFile( const char * szFilename )
{
	bool ret = false;
	// Send the scene file
	Threads::ifxMutexLock( mutComm );
	{
		unsigned int length = strlen(szFilename); 
		pSendBuffer->Resize( length + 1, true );
		pSendBuffer->seek( IBuffer::START, 0 );

		pSendBuffer->setBytes( szFilename, length );
		pSendBuffer->setChar( 0 );

		ret = TrySendMessage( eMessage_SceneFile, true );
	}
	Threads::ifxMutexUnlock( mutComm );

	return ret;
}

bool WorkerConnection::SendSceneModels( const std::deque<MODEL_INFO>&	szModels )
{
	bool ret = false;
	Threads::ifxMutexLock( mutComm );
	{
		// First send the count
		pSendBuffer->Resize( sizeof( unsigned int ), true );
		pSendBuffer->seek( IBuffer::START, 0 );

		unsigned int model_count = szModels.size();
		pSendBuffer->setUInt( model_count );

		ret = TrySendMessage( eMessage_ModelCount, true );

		std::deque<MODEL_INFO>::const_iterator	it;
		for( it=szModels.begin(); it!=szModels.end(); it++ ) {
			char szModelFileName[1024] = {0};
			sprintf( szModelFileName, "%s_%d.prisemesh", it->szFilename.c_str(), nWorkerID );

			unsigned int length = strlen(szModelFileName); 
			unsigned int matlength = strlen(it->material.c_str());
			pSendBuffer->Resize( sizeof( Point3D ) + sizeof( Vector3D ) + length + matlength + 2, true );
			pSendBuffer->seek( IBuffer::START, 0 );

			pSendBuffer->setDouble( it->ptPosition.x );
			pSendBuffer->setDouble( it->ptPosition.y );
			pSendBuffer->setDouble( it->ptPosition.z );
			pSendBuffer->setDouble( it->vOrientation.x );
			pSendBuffer->setDouble( it->vOrientation.y );
			pSendBuffer->setDouble( it->vOrientation.z );
			pSendBuffer->setBytes( it->material.c_str(), matlength );
			pSendBuffer->setChar( 0 );
			pSendBuffer->setBytes( szModelFileName, length );
			pSendBuffer->setChar( 0 );

			ret = TrySendMessage( eMessage_Model, true );
		}
	}
	Threads::ifxMutexUnlock( mutComm );

	return ret;
}

bool WorkerConnection::SendCell( const Rect& rc )
{
	bool ret = false;
	Threads::ifxMutexLock( mutComm );
	{
		// Send the given cell to the client
		pSendBuffer->Resize( sizeof( unsigned int ) * 4, true );
		pSendBuffer->seek( IBuffer::START, 0 );

		pSendBuffer->setUInt( rc.top );
		pSendBuffer->setUInt( rc.left );
		pSendBuffer->setUInt( rc.bottom );
		pSendBuffer->setUInt( rc.right );

		ret = TrySendMessage( eMessage_NewCell, true );
	}
	Threads::ifxMutexUnlock( mutComm );

	return ret;
}

bool WorkerConnection::SendIncompleteRay( IMemoryBuffer& buffer )
{
	bool ret = false;
	Threads::ifxMutexLock( mutComm );
	{
		ret = TrySendMessage( eMessage_UnresolvedRay, &buffer );
	}
	Threads::ifxMutexUnlock( mutComm );

	return ret;
}

bool WorkerConnection::SendRenderComplete()
{
	bool ret = false;
	Threads::ifxMutexLock( mutComm );
	{
		// Sends the signal that all job units are done being handed out
		ret = TrySendMessage( eMessage_DoneRendering, false );
	}
	Threads::ifxMutexUnlock( mutComm );

	return ret;
}

bool WorkerConnection::WaitForRenderCompiete()
{
	return TryReceiveSpecificMessage( eMessage_DoneRendering );
}

bool WorkerConnection::GetWorkerType( int& type )
{
	bool ret = false;
	Threads::ifxMutexLock( mutComm );
	{
		if( !TryReceiveSpecificMessage( eMessage_WorkerType ) ) {
			ret = false;
		} else {
			ret = true;
			type = pRecvBuffer->getInt();
		}
	}
	Threads::ifxMutexUnlock( mutComm );
	return ret;
}

bool WorkerConnection::ProcessWorkerRequest()
{
	// sits back and waits for a message
	if( !TryReceiveMessage() ) {
		return false;
	}

	switch( mtRecvType ) {
	default:
		GlobalLog()->PrintEasyWarning( "Unknown worker request" );
		break;
	case eMessage_WorkerResult:
		{
			// Worker is sending us a result
			unsigned int x = pRecvBuffer->getUInt();
			unsigned int y = pRecvBuffer->getUInt();

			IFXColor	c;
			c.base.r = pRecvBuffer->getDouble();
			c.base.g = pRecvBuffer->getDouble();
			c.base.b = pRecvBuffer->getDouble();
			c.a = pRecvBuffer->getDouble();

			MasterSchedulerEngine().WorkerResult( x, y, c );
		}
		break;
	case eMessage_UnresolvedRay:
		// Get the details and tell the master scheduler engine
		MasterSchedulerEngine().UnresolvedRay( *pRecvBuffer );
		break;

	case eMessage_DoneRendering:
		return false;
	}

	return true;
}

unsigned int WorkerConnection::GetQueueSize( )
{
	unsigned int queue_size = 0;

	Threads::ifxMutexLock( mutComm );
	{
		if( !TrySendMessage( eMessage_GetQueueSize, false ) ) {
			GlobalLog()->PrintEasyError( "WorkerConnection::GetQueueSize:: Failed to talk to worker" );
		}

		if( !TryReceiveSpecificMessage( eMessage_QueueSize ) ) {
			GlobalLog()->PrintEasyError( "WorkerConnection::GetQueueSize:: Failed to receive queue size" );
		}

		queue_size = pRecvBuffer->getUInt();
	}
	Threads::ifxMutexUnlock( mutComm );

	return queue_size;
}
