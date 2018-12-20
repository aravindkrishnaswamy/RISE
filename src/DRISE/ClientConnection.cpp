//////////////////////////////////////////////////////////////////////
//
//  ClientConnection.cpp - Implements the client connection object
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 25, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include <string>
#include "ClientConnection.h"
#include "../Library/Interfaces/ILog.h"
#include "../Library/Version.h"

using namespace RISE;

ClientConnection::ClientConnection( ICommunicator* pCommunicator_ ) : 
  Connection( pCommunicator_ )
{
}

ClientConnection::~ClientConnection()
{
}

bool ClientConnection::PerformHandshaking( const char * szSecretCode )
{
	//
	// This means we are to do handshaking with a client
	//

	// 
	// Handshaking protocol - First we wait for the secret code from the client
	// then we wait for client version info then we send our version info
	// Once that is all good, then we can continue talking
	//

	if( !TryReceiveSpecificMessage( eMessage_Handshake ) ) {
		return false;
	}

	// Now actually check the hand shake
	if( strcmp( pRecvBuffer->Pointer(), szSecretCode ) != 0 ) {
		GlobalLog()->PrintEasyError( "Client send wrong secret code" );
		return false;
	}

	// Get client version
	if( !TryReceiveSpecificMessage( eMessage_Version ) ) {
		return false;
	}

	// Now check the version information
	unsigned int uClientMajorVersion = pRecvBuffer->getUInt();
	unsigned int uClientMinorVersion = pRecvBuffer->getUInt();
	unsigned int uClientRevisionVersion = pRecvBuffer->getUInt();
	unsigned int uClientBuildVersion = pRecvBuffer->getUInt();

	if( uClientMajorVersion != RISE_VER_MAJOR_VERSION || 
		uClientMinorVersion != RISE_VER_MINOR_VERSION ||
		uClientRevisionVersion != RISE_VER_REVISION_VERSION ||
		uClientBuildVersion != RISE_VER_BUILD_VERSION ) {
		GlobalLog()->PrintEx( eLog_Error, "Client is version, %d.%d.%d build %d, we are version %d.%d.%d build %d, it just wouldn't work out",
			uClientMajorVersion, uClientMinorVersion, uClientRevisionVersion, uClientBuildVersion,
			RISE_VER_MAJOR_VERSION, RISE_VER_MINOR_VERSION, RISE_VER_REVISION_VERSION, RISE_VER_BUILD_VERSION );
		return false;
	}

	// Now send the client the OK message
	if( !TrySendMessage( eMessage_EverythingOK, false )	) {
		return false;
	}

	return true;
}

CLIENT_TYPE ClientConnection::GetClientType()
{
	// Ask for client type
	if( !TrySendMessage( eMessage_GetClientType, false ) ) {
		return eClient_Unknown;
	}

	if( !TryReceiveSpecificMessage( eMessage_ClientType ) ) {
		return eClient_Unknown;
	}

	CLIENT_TYPE type = (CLIENT_TYPE)pRecvBuffer->getChar();
	return type;
}

void ClientConnection::Disconnect()
{
	// Send the disconnection message
	TrySendMessage( eMessage_Disconnect, false );
}

void ClientConnection::PerformClientTasks()
{
	// The basic type does nothing, disconnect
	Disconnect();
}

