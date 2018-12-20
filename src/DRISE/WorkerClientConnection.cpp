//////////////////////////////////////////////////////////////////////
//
//  WorkerClientConnection.cpp - Implements the worker client 
//    connection
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 26, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "WorkerClientConnection.h"
#include "../Library/Interfaces/ILog.h"
#include "IJobEngine.h"
#include "../Library/Utilities/MemoryBuffer.h"

using namespace RISE;

WorkerClientConnection::WorkerClientConnection( ICommunicator* pCommunicator_ ) : 
  ClientConnection( pCommunicator_ )
{
}

WorkerClientConnection::~WorkerClientConnection()
{
}

void WorkerClientConnection::PerformClientTasks()
{
	unsigned int i=0;
	char c=0;
	unsigned int nNumJobs=0;
	char nActionCount=0;

	// We have a client who is a worker, first check to see if they have any completed jobs
	if( !TrySendMessage( eMessage_GetCompJobs, false ) ) {
		goto end;
	}

	// Get the list of completed jobs
	if( !TryReceiveSpecificMessage( eMessage_CompletedJobs ) ) {
		goto end;
	}

	nNumJobs = pRecvBuffer->getUInt();
//	if( nNumJobs == 0 ) {
//		GlobalLog()->PrintEasyInfo( "Client had no jobs to submit" );
//	}

	for( i=0; i<nNumJobs; i++ ) {
		// Get each of the results
		
		// First receive the task id and the task action ID
		if( !TryReceiveSpecificMessage( eMessage_TaskIDs ) ) {
			GlobalLog()->PrintEx( eLog_Error,  "Client said it had %d jobs to submit, but failed to get IDs on job %d", nNumJobs, i );
			goto end;
		}

		IJobEngine::TaskID tid = pRecvBuffer->getUInt();
		ITask::TaskActionID actionid = pRecvBuffer->getUInt();

		// Now get the actual result
		if( !TryReceiveSpecificMessage( eMessage_CompTaskAction ) ) {
			GlobalLog()->PrintEx( eLog_Error,  "Client said it had %d jobs to submit, but failed to get completed buffer on job %d", nNumJobs, i );
			goto end;
		}

		// Tell the master job engine we have a completed job
		MasterJobEngine().FinishedTaskAction( tid, actionid, *pRecvBuffer );
	}

	// Now all we do is give the client however many task actions it wants
	if( !TrySendMessage( eMessage_HowMuchAction, false ) ) {
		GlobalLog()->PrintEasyError( "Couldn't ask client how much action it wanted" );
		goto end;
	}

	if( !TryReceiveSpecificMessage( eMessage_ActionCount ) ) {
		GlobalLog()->PrintEasyError( "Client wouldn't tell me how much action it wanted" );
		goto end;
	}

	nActionCount = pRecvBuffer->getChar();

	for( c=0; c<nActionCount; c++ ) {

		// Request a job from the master job engine
		IJobEngine::TaskID	taskid;
		ITask::TaskActionID taskactionid;
		pSendBuffer->seek( IBuffer::START, 0 );
		
		Implementation::MemoryBuffer*	pBuffer = new Implementation::MemoryBuffer();

		if( MasterJobEngine().GetNewTaskAction( taskid, taskactionid, *pBuffer ) ) {

			pSendBuffer->Resize( sizeof( unsigned int ) * 2, true );
			pSendBuffer->seek( IBuffer::START, 0 );
			pSendBuffer->setUInt( taskid );
			pSendBuffer->setUInt( taskactionid );

			if( !TrySendMessage( eMessage_TaskIDs, true ) ) {
				GlobalLog()->PrintEasyError( "Failed to send client the task IDs for a task action" );
				goto end;
			}

			pSendBuffer->release();
			pSendBuffer = new Implementation::MemoryBuffer( *pBuffer );
			safe_release( pBuffer );

			if( !TrySendMessage( eMessage_TaskAction, true ) ) {
				GlobalLog()->PrintEasyError( "Failed to send client a task action" );
				goto end;
			}

		} else {
			GlobalLog()->PrintEasyInfo( "Master job engine has no jobs for this client" );
			break;
		}

	}

	// And we're done!


	/*
	// Ask the client for its ID, so we know what jobs it current has it can
	// work on
	if( !TrySendMessage( eMessage_GetClientID, false ) ) {
		Disconnect();
		return;
	}

	// Get the identity of client
	if( !TryReceiveSpecificMessage( eMessage_ClientID ) ) {
		Disconnect();
		return;
	}

	unsigned int nClientID = pRecvBuffer->getUInt();

	if( nClientID == 0 ) {
		// Request a new ID for this client
		GlobalLog()->PrintEasyInfo( "New client has joined the system" );
	}
	*/

end:
	Disconnect();
}

