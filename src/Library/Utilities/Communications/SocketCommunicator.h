//////////////////////////////////////////////////////////////////////
//
//  SocketCommunicator.h - Defines a communicator that communicates
//    through sockets
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 24, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SOCKET_COMMUNICATOR_
#define SOCKET_COMMUNICATOR_

#include "../../Interfaces/ICommunicator.h"
#include "SocketCommunications.h"

namespace RISE
{
	class SocketCommunicator : public virtual ICommunicator
	{
	protected:
		SocketCommunicator();
		virtual ~SocketCommunicator();

		SOCKET		conn;

		int ReadData( char * buf, int n );

	public:
		virtual bool CommSendMessage( const MESSAGE_TYPE type, const IMemoryBuffer* buffer );
		virtual bool CommRecvMessage( MESSAGE_TYPE& type, IMemoryBuffer*& buffer );
		virtual bool IsConnectionOpen( );
		virtual void CloseConnection( );
	};
}

#endif

