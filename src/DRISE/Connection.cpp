//////////////////////////////////////////////////////////////////////
//
//  Connection.cpp - Implements the connection class
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
#include "Connection.h"
#include "../Library/Interfaces/ILog.h"

using namespace RISE;

Connection::Connection( ICommunicator* pCommunicator_ ) : 
  pCommunicator( pCommunicator_ ),
  pSendBuffer( 0 ),
  pRecvBuffer( 0 )
{
	pSendBuffer = new Implementation::MemoryBuffer();
	GlobalLog()->PrintNew( pSendBuffer, __FILE__, __LINE__, "send buffer" );

	if( pCommunicator ) {
		pCommunicator->addref();
	} else {
		GlobalLog()->PrintSourceError( "Bad communicator", __FILE__, __LINE__ );
	}
}

Connection::~Connection()
{
	safe_release( pSendBuffer );
	safe_release( pRecvBuffer );
	safe_release( pCommunicator );
}

bool Connection::TryReceiveMessage()
{
	mtRecvType = eMessage_None;

	if( pRecvBuffer ) {
		pRecvBuffer->release();
		pRecvBuffer = 0;
	}

	if( !pCommunicator->CommRecvMessage( mtRecvType, pRecvBuffer ) ) {
		GlobalLog()->PrintEasyError( "Failed to receive message" );
		return false;
	}

	return true;
}

bool Connection::TryReceiveSpecificMessage( MESSAGE_TYPE mtExpected )
{
	TryReceiveMessage();

	if( mtRecvType != mtExpected ) {
		GlobalLog()->PrintEx( eLog_Error, "Tried to get message %d, got %d instead", mtExpected, mtRecvType );
		return false;
	}

	return true;
}

bool Connection::TrySendMessage( MESSAGE_TYPE mtSendType, bool bSendBuffer )
{
	if( !pCommunicator->CommSendMessage( mtSendType, bSendBuffer ? pSendBuffer : 0 ) ) {
		GlobalLog()->PrintEasyError( "Failed to send message" );
		return false;
	}

	return true;
}

bool Connection::TrySendMessage( MESSAGE_TYPE mtSendType, const IMemoryBuffer* pBuffer )
{
	if( !pCommunicator->CommSendMessage( mtSendType, pBuffer ) ) {
		GlobalLog()->PrintEasyError( "Failed to send message" );
		return false;
	}

	return true;
}

