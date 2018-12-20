//////////////////////////////////////////////////////////////////////
//
//  SubmitterClientConnection .cpp - Implements the submitter client 
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
#include "IJobEngine.h"
#include "SubmitterClientConnection.h"
#include "Task.h"
#include "AnimationTask.h"

using namespace RISE;

SubmitterClientConnection ::SubmitterClientConnection ( ICommunicator* pCommunicator_ ) : 
  ClientConnection( pCommunicator_ )
{
}

SubmitterClientConnection ::~SubmitterClientConnection ()
{
}

void SubmitterClientConnection::PerformClientTasks()
{
	// !@@ Add secure authentication!

	// We expect a submitter client to simply want to submit a single job
	if( !TryReceiveMessage() ) {
		Disconnect();
		return;
	}

	// Now that we have the job, check kind of task and add it to the main engine
	switch( mtRecvType ) 
	{
	default:
		GlobalLog()->PrintEx( eLog_Error, "SubmitterClientConnection::PerformClientTasks:: was expecting job type, got %d instead",  mtRecvType );
		break;

	case eMessage_SubmitJobBasic:
		{
			char				szFileName[1024] = {0};
			unsigned int		x, y, granx, grany;
			char				szOutFile[1024] = {0};

			pRecvBuffer->getBytes( szFileName, 1024 );
			x = pRecvBuffer->getUInt();
			y = pRecvBuffer->getUInt();
			pRecvBuffer->getBytes( szOutFile, 1024 );
			granx = pRecvBuffer->getUInt();
			grany = pRecvBuffer->getUInt();

			// We have a basic task, so create it and add it
			ITask* pTask = new Implementation::Task( szFileName, x, y, szOutFile, granx, grany );
			MasterJobEngine().AddTask( pTask );
			pTask->release();

			GlobalLog()->PrintEx( eLog_Event, "SubmitterClientConnection:: new job [%s] %dx%d output: [%s], granularity [%dx%d]", szFileName, x, y, szOutFile, granx, grany );

			// Send ok message
			TrySendMessage( eMessage_SubmitOK, false );
		}
		break;

	case eMessage_SubmitJobAnim:
		{
			char				szFileName[1024] = {0};
			unsigned int		x, y;
			char				szOutFile[1024] = {0};
			unsigned int		frames;

			pRecvBuffer->getBytes( szFileName, 1024 );
			x = pRecvBuffer->getUInt();
			y = pRecvBuffer->getUInt();
			pRecvBuffer->getBytes( szOutFile, 1024 );
			frames = pRecvBuffer->getUInt();

            // We have an animation task, so create it and add it
			ITask* pTask = new Implementation::AnimationTask( szFileName, x, y, szOutFile, frames );
			MasterJobEngine().AddTask( pTask );
			pTask->release();

			GlobalLog()->PrintEx( eLog_Event, "SubmitterClientConnection:: new animation job [%s] %dx%d output: [%s], number of frames [%d]", szFileName, x, y, szOutFile, frames );

			// Send ok message
			TrySendMessage( eMessage_SubmitOK, false );
		}
	};

	Disconnect();
}

