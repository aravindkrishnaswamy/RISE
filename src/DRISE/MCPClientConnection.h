//////////////////////////////////////////////////////////////////////
//
//  MCPClientConnection.h - This object is supposed to represent
//    a MCP client for the lifetime that it is connected to the 
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

#ifndef MCP_CLIENT_CONNECTION_
#define MCP_CLIENT_CONNECTION_

#include "ClientConnection.h"

namespace RISE
{
	class MCPClientConnection : public ClientConnection
	{
	protected:
		virtual ~MCPClientConnection();

	public:
		MCPClientConnection( ICommunicator* pCommunicator_ );

		virtual void PerformClientTasks();
	};
}

#endif

