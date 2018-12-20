//////////////////////////////////////////////////////////////////////
//
//  ServerSocketCommunicator.h - Defines a socket communicator that
//    is constructed as a "server" based socket, ie, the cons. waits
//    for a connection.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 24, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SERVER_SOCKET_COMMUNICATOR_
#define SERVER_SOCKET_COMMUNICATOR_

#include "SocketCommunicator.h"
#include "../Reference.h"

namespace RISE
{
	class ServerSocketCommunicator : public virtual SocketCommunicator, public virtual Implementation::Reference
	{
	protected:
		virtual ~ServerSocketCommunicator();

	public:
		//
		// Since a server "waits" for a client, the constructor blocks and waits
		// for a connection on a particular socket
		//
		ServerSocketCommunicator( const SOCKET sock );
	};
}

#endif

