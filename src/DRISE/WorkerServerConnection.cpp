//////////////////////////////////////////////////////////////////////
//
//  WorkerServerConnection.cpp - Implements the worker server
//    connection
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 27, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "WorkerServerConnection.h"
#include "../Library/Interfaces/ILog.h"

using namespace RISE;

WorkerServerConnection::WorkerServerConnection( ICommunicator* pCommunicator_ ) : 
  ServerConnection( pCommunicator_ )
{
}

WorkerServerConnection::~WorkerServerConnection()
{
}

int WorkerServerConnection::ProcessServerRequest( unsigned int& taskid, unsigned int& taskactionid, IMemoryBuffer*& buffer,
												 unsigned int comptaskid, unsigned int comptaskactionid, IMemoryBuffer* compbuffer )
{
	if( !TryReceiveMessage() ) {
		return -1;
	}

	// Process the message
	switch( mtRecvType )
	{
	case eMessage_Disconnect:
		pCommunicator->CloseConnection();
		break;

	case eMessage_GetClientType:
		{
			pSendBuffer->Resize( 1, true );
			pSendBuffer->seek( IBuffer::START, 0 );
			pSendBuffer->setChar( char( eClient_Worker ) );
			if( !TrySendMessage( eMessage_ClientType, true ) ) {
				return -1;
			}
		}
		break;

	case eMessage_GetCompJobs:
		{
			bool bCompletedJobsToSend = compbuffer ? true : false;

			// We should send our list of completed jobs
			pSendBuffer->Resize( 4, true );
			pSendBuffer->seek( IBuffer::START, 0 );
			pSendBuffer->setUInt( bCompletedJobsToSend ? 1 : 0 );			// no jobs to send for now
			if( !TrySendMessage( eMessage_CompletedJobs, true ) ) {
				return -1;
			}

			if( bCompletedJobsToSend ) {

				pSendBuffer->Resize( sizeof( unsigned int ) * 2, true );
				pSendBuffer->seek( IBuffer::START, 0 );
				pSendBuffer->setUInt( comptaskid );
				pSendBuffer->setUInt( comptaskactionid );
				if( !TrySendMessage( eMessage_TaskIDs, true ) ) {
					return -1;
				}

				// Send the completed job
				safe_release( pSendBuffer );
				pSendBuffer = new Implementation::MemoryBuffer( *compbuffer );

				if( !TrySendMessage( eMessage_CompTaskAction, true ) ) {
					return -1;
				}
			}
		}
		break;

	case eMessage_HowMuchAction:
		{
			// Server wants to know how many task actions we want
			pSendBuffer->Resize( 4, true );
			pSendBuffer->seek( IBuffer::START, 0 );
			pSendBuffer->setUInt( 1 );						// just one for now
			if( !TrySendMessage( eMessage_ActionCount, true ) ) {
				return -1;
			}
		}
		break;

	case eMessage_TaskIDs:
		{
			// The server is sending us a task action to complete

			//
			// Note for now we are just a dumb worker, and we only 
			// accept one task action, then we bail on the connection, 
			// go off and work on it
			//
			
			taskid = pRecvBuffer->getUInt();
			taskactionid = pRecvBuffer->getUInt();

			if( !TryReceiveSpecificMessage( eMessage_TaskAction ) ) {
				GlobalLog()->PrintEasyError( "Server gave us a task id, but no task message" );
				return -1;
			}

			buffer = pRecvBuffer;
			pRecvBuffer->addref();
			return 1;

		}
		break;

		/*
	case eMessage_GetClientID:
		{
			// We should send our client id
			pSendBuffer->Resize( 4, true );
			pSendBuffer->seek( IBuffer::START, 0 );
			pSendBuffer->setUInt( 0 );						// just say we have no ID for now
			if( !TrySendMessage( eMessage_ClientID, true ) ) {
				return false;
			}
		}
		break;
		*/

	default:
		GlobalLog()->PrintEasyWarning( "Unknown request from server" );
		break;
	}

	return 0;
}

