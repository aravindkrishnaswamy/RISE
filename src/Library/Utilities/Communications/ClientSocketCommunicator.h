//////////////////////////////////////////////////////////////////////
//
//  ClientSocketCommunicator.h - Defines a communicator, the reason
//    it is in it's own class is because they way they are created
//    is different
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 24, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef CLIENT_SOCKET_COMMUNICATOR_
#define CLIENT_SOCKET_COMMUNICATOR_

#include "SocketCommunicator.h"
#include "../Reference.h"

namespace RISE
{
	class ClientSocketCommunicator : public virtual SocketCommunicator, public virtual Implementation::Reference
	{
	protected:
		virtual ~ClientSocketCommunicator();

	public:
		//
		// A client socket communicator is build by calling a "server"
		// The server is described by the address, which can be an IP
		// address or a qualified windows computer name and a port
		//
		ClientSocketCommunicator( const char * szAddress, const unsigned short nPort, unsigned int nSocketType );
	};
}

#endif
