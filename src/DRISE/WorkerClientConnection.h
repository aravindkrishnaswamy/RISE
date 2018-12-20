//////////////////////////////////////////////////////////////////////
//
//  WorkerClientConnection.h - This object is supposed to represent
//    a worker client for the lifetime that it is connected to the 
//    server
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 26, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef WORKER_CLIENT_CONNECTION_
#define WORKER_CLIENT_CONNECTION_

#include "ClientConnection.h"

namespace RISE
{
	class WorkerClientConnection : public ClientConnection
	{
	protected:
		virtual ~WorkerClientConnection();

	public:
		WorkerClientConnection( ICommunicator* pCommunicator_ );

		virtual void PerformClientTasks();
	};
}

#endif

