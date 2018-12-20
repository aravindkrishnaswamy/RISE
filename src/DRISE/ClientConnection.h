//////////////////////////////////////////////////////////////////////
//
//  ClientConnection.h - This object is supposed to represent a client
//    connection for the lifetime of a client being connected to the
//    server
//
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 25, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef CLIENT_CONNECTION_
#define CLIENT_CONNECTION_

#include "Connection.h"

namespace RISE
{
	class ClientConnection : public Connection
	{
	protected:
		virtual ~ClientConnection();

	public:
		ClientConnection( ICommunicator* pCommunicator_ );

		virtual bool PerformHandshaking( const char * szSecretCode );
		virtual CLIENT_TYPE GetClientType();
		virtual void Disconnect();

		// For each of the sub types this is what actually does the work
		// associated with that client type
		virtual void PerformClientTasks();
	};
}

#endif

