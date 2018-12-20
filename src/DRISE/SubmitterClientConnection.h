//////////////////////////////////////////////////////////////////////
//
//  SubmitterClientConnection.h - This object is supposed to represent
//    a submission client for the lifetime that it is connected to the 
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

#ifndef SUBMITTER_CLIENT_CONNECTION_
#define SUBMITTER_CLIENT_CONNECTION_

#include "ClientConnection.h"

namespace RISE
{
	class SubmitterClientConnection : public ClientConnection
	{
	protected:
		virtual ~SubmitterClientConnection ();

	public:
		SubmitterClientConnection( ICommunicator* pCommunicator_ );

		virtual void PerformClientTasks();
	};
}

#endif

