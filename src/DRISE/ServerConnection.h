//////////////////////////////////////////////////////////////////////
//
//  ServerConnection.h - This object is supposed to represent a
//    connection to the server for the lifetime that a client
//    is connected to the server
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 25, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SERVER_CONNECTION_
#define SERVER_CONNECTION_

#include "Connection.h"

namespace RISE
{
	class ServerConnection : public Connection
	{
	protected:
		virtual ~ServerConnection();

	public:
		ServerConnection( ICommunicator* pCommunicator_ );

		virtual bool PerformHandshaking( const char * szSecretCode );
		virtual bool ProcessServerRequest();
	};
}

#endif

