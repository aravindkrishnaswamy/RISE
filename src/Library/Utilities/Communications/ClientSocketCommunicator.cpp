//////////////////////////////////////////////////////////////////////
//
//  ClientSocketCommunicator.h - Implements the client socket comm.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 24, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "ClientSocketCommunicator.h"
#include "../../Interfaces/ILog.h"

using namespace RISE;

ClientSocketCommunicator::ClientSocketCommunicator( const char * szAddress, const unsigned short nPort, unsigned int nSocketType )
{
	GlobalLog()->PrintEx( eLog_Info, "ClientSocketCommunicator:: Calling [%s] on port %d", szAddress, nPort );
	
	conn = SocketComm::CallSocket( szAddress, nPort, nSocketType );

	if( conn == BAD_SOCKET ) {
		GlobalLog()->PrintEx( eLog_Error, "Failed to connect to [%s] on port %d, maybe the server is down.", szAddress, nPort );
	}
}

ClientSocketCommunicator::~ClientSocketCommunicator()
{
}
