//////////////////////////////////////////////////////////////////////
//
//  ServerConnection.cpp - Implements the server connection object
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
#include "ServerConnection.h"
#include "../Library/Interfaces/ILog.h"
#include "../Library/Version.h"

using namespace RISE;

ServerConnection::ServerConnection( ICommunicator* pCommunicator_ ) : 
  Connection( pCommunicator_ )
{
}
 
ServerConnection::~ServerConnection()
{
}

bool ServerConnection::PerformHandshaking( const char * szSecretCode )
{
	//
	// This means we are to do handshaking with a server
	//

	pSendBuffer->Resize( strlen( szSecretCode ) + 1 );
	pSendBuffer->setBytes( szSecretCode, strlen( szSecretCode ) );
	pSendBuffer->setChar( 0 );

	if( !TrySendMessage( eMessage_Handshake, true ) ) {
		return false;
	}

	pSendBuffer->Resize( sizeof( int ) * 4, true );
	pSendBuffer->seek( IBuffer::START, 0 );

	pSendBuffer->setUInt( RISE_VER_MAJOR_VERSION );
	pSendBuffer->setUInt( RISE_VER_MINOR_VERSION );
	pSendBuffer->setUInt( RISE_VER_REVISION_VERSION );
	pSendBuffer->setUInt( RISE_VER_BUILD_VERSION );

	if( !TrySendMessage( eMessage_Version, true ) ) {
		return false;
	}

	// If everything is ok, we should get a message from the server
	if( !TryReceiveSpecificMessage( eMessage_EverythingOK ) ) {
		return false;
	}

	return true;
}

bool ServerConnection::ProcessServerRequest()
{
	if( !TryReceiveMessage() ) {
		return false;
	}

	// Process the message
	switch( mtRecvType )
	{
	case eMessage_Disconnect:
		pCommunicator->CloseConnection();
		break;

	default:
		GlobalLog()->PrintEasyWarning( "Unknown request from server" );
		break;
	}

	return true;
}

