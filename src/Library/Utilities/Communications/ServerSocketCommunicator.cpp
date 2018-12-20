//////////////////////////////////////////////////////////////////////
//
//  ServerSocketCommunicator.h - Implements the server socket comm.
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
#include "ServerSocketCommunicator.h"
#include "../../Interfaces/ILog.h"

using namespace RISE;

ServerSocketCommunicator::ServerSocketCommunicator( const SOCKET sock )
{
	if( sock == BAD_SOCKET ) {
		GlobalLog()->PrintSourceError( "Passed in bad socket", __FILE__, __LINE__ );
	} else {

		// Listen for a connection
		conn = SocketComm::GetConnection( sock );

		if( conn == BAD_SOCKET ) {
			GlobalLog()->PrintSourceError( "Bad socket returned on listen", __FILE__, __LINE__ );
		}
	}
}

ServerSocketCommunicator::~ServerSocketCommunicator( )
{
}

