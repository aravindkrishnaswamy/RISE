//////////////////////////////////////////////////////////////////////
//
//  WorkerServerConnection.h - Defines a worker client that is 
//    talking to a server
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 27, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef WORKER_SERVER_CONNECTION_
#define WORKER_SERVER_CONNECTION_

#include "ServerConnection.h"

namespace RISE
{
	class WorkerServerConnection : public ServerConnection
	{
	protected:
		virtual ~WorkerServerConnection();

	public:
		WorkerServerConnection( ICommunicator* pCommunicator );

		// Sits here and processes requests, looking for a job
		int ProcessServerRequest( 
			unsigned int& taskid,
			unsigned int& taskactionid,
			IMemoryBuffer*& buffer, 
			unsigned int comptaskid,
			unsigned int comptaskactionid,
			IMemoryBuffer* compbuffer
			);
	};
}

#endif

