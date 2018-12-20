//////////////////////////////////////////////////////////////////////
//
//  SubmitterServerConnection.cpp - Implements the submitter server
//    connection
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 27, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "SubmitterServerConnection.h"
#include "../Library/Interfaces/ILog.h"
#include <string>

using namespace RISE;

SubmitterServerConnection::SubmitterServerConnection( ICommunicator* pCommunicator_ ) : 
  ServerConnection( pCommunicator_ )
{
}

SubmitterServerConnection::~SubmitterServerConnection()
{
}

bool SubmitterServerConnection::SubmitJob( const char * szFileName, unsigned int x, unsigned int y, const char* szOutputName, unsigned int xgran, unsigned int ygran )
{
	//
	// Pack up the message and send it away
	//
	pSendBuffer->Resize( 1024 + sizeof( unsigned int ) * 4 + 1024, true );
	pSendBuffer->seek( IBuffer::START, 0 );

	char file[1024] = {0};
	char output[1024] = {0};

	strncpy( file, szFileName, 1024 );
	strncpy( output, szOutputName, 1024 );

	pSendBuffer->setBytes( file, 1024 );
	pSendBuffer->setUInt( x );
	pSendBuffer->setUInt( y );
	pSendBuffer->setBytes( output, 1024 );
	pSendBuffer->setUInt( xgran );
	pSendBuffer->setUInt( ygran );

	if( !TrySendMessage( eMessage_SubmitJobBasic, true ) ) {
		pCommunicator->CloseConnection();	
		return false;
	}

	if( !TryReceiveSpecificMessage( eMessage_SubmitOK ) ) {
		pCommunicator->CloseConnection();	
		GlobalLog()->PrintEasyError( "Server didn't send back job ok" );
		return false;
	}

	pCommunicator->CloseConnection();	
	return true;
}

bool SubmitterServerConnection::SubmitAnimationJob( const char * szFileName, unsigned int x, unsigned int y, const char* szOutputName, const unsigned int frames )
{
	//
	// Pack up the message and send it away
	//
	pSendBuffer->Resize( 1024 + sizeof( unsigned int ) * 4 + 1024, true );
	pSendBuffer->seek( IBuffer::START, 0 );

	char file[1024] = {0};
	char output[1024] = {0};

	strncpy( file, szFileName, 1024 );
	strncpy( output, szOutputName, 1024 );

	pSendBuffer->setBytes( file, 1024 );
	pSendBuffer->setUInt( x );
	pSendBuffer->setUInt( y );
	pSendBuffer->setBytes( output, 1024 );
	pSendBuffer->setUInt( frames );

	if( !TrySendMessage( eMessage_SubmitJobAnim, true ) ) {
		pCommunicator->CloseConnection();	
		return false;
	}

	if( !TryReceiveSpecificMessage( eMessage_SubmitOK ) ) {
		pCommunicator->CloseConnection();	
		GlobalLog()->PrintEasyError( "Server didn't send back job ok" );
		return false;
	}

	pCommunicator->CloseConnection();	
	return true;
}

bool SubmitterServerConnection::ProcessServerRequest()
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

	case eMessage_GetClientType:
		{
			pSendBuffer->Resize( 1, true );
			pSendBuffer->seek( IBuffer::START, 0 );
			pSendBuffer->setChar( char( eClient_Submitter ) );
			if( !TrySendMessage( eMessage_ClientType, true ) ) {
				return false;
			}
		}
		break;

	default:
		GlobalLog()->PrintEasyWarning( "Unknown request from server" );
		break;
	}

	return true;
}

