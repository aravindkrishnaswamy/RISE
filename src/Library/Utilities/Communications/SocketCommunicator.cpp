//////////////////////////////////////////////////////////////////////
//
//  SocketCommunicator.cpp - Implements communications via socket
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
#include "SocketCommunicator.h"
#include "../../Interfaces/ILog.h"
#include "../../Utilities/MemoryBuffer.h"

using namespace RISE;

SocketCommunicator::SocketCommunicator( )
{
}

SocketCommunicator::~SocketCommunicator( )
{
	// release the connection
	if( conn && conn != BAD_SOCKET ) {
		SocketComm::CloseSocket( conn );
		conn = 0;
	}
}


int SocketCommunicator::ReadData( char *buf, int n )
{
	int bcount = 0; // counts bytes read
	int br = 0;     // bytes read this pass

	while( bcount < n ) {
		br = recv( conn, buf, n-bcount, 0 );
		if( br > 0 ) {
			bcount += br;
			buf += br;
		} else if( br < 0 ) {
			return -1;
		}
	}

	return bcount;
}

bool SocketCommunicator::CommSendMessage( const MESSAGE_TYPE type, const IMemoryBuffer* buffer )
{
	if( conn == BAD_SOCKET || !conn ) {
		GlobalLog()->PrintSourceWarning( "Tried to send a message on bad socket", __FILE__, __LINE__ );
		return false;
	}

	// Create the header
	MESSAGE_HEADER	header;
	Implementation::MemoryBuffer*	buf = new Implementation::MemoryBuffer( &header.bufferSize[0], 4, false );
	GlobalLog()->PrintNew( buf, __FILE__, __LINE__, "mem buffer" );

	if( buffer ) {
		buf->setUInt( buffer->Size() );
	} else {
		buf->setUInt( 0 );
	}

	// Set the type
	header.chType = char(type);

	safe_release( buf );

	// Send the header
	int nBytesSent = send( conn, (const char*)&header, sizeof( MESSAGE_HEADER ), 0 );

	if( nBytesSent == -1 ) {
		// Connection broken
		GlobalLog()->PrintEasyError( "Connection broken" );
		CloseConnection();
	}

	if( nBytesSent != sizeof( MESSAGE_HEADER ) ) {
		GlobalLog()->PrintEx( eLog_Error, "HEADER sent %d bytes rather than %d", nBytesSent, sizeof( MESSAGE_HEADER ) );
		return false;
	}

	if( buffer ) {
		// Send the buffer
		nBytesSent = send( conn, buffer->Pointer(), buffer->Size(), 0 );

		if( nBytesSent != int(buffer->Size()) ) {
			GlobalLog()->PrintEx( eLog_Error, "MessageBuffer sent %d bytes rather than %d", nBytesSent, buffer->Size() );
			return false;
		}
	}

	// Otherwise all good!
	return true;
}

bool SocketCommunicator::CommRecvMessage( MESSAGE_TYPE& type, IMemoryBuffer*& buffer )
{
	if( conn == BAD_SOCKET || !conn ) {
		GlobalLog()->PrintSourceWarning( "Tried to receive a message on bad socket", __FILE__, __LINE__ );
		return false;
	}

	// Try to recieve the header
	MESSAGE_HEADER	header;

	int nBytesRead = ReadData( (char*)&header, sizeof( MESSAGE_HEADER ) );

	if( nBytesRead == -1 ) {
		// Connection broken
		GlobalLog()->PrintEasyError( "Connection broken" );
		CloseConnection();
	}

	if( nBytesRead != sizeof( MESSAGE_HEADER ) ) {
		GlobalLog()->PrintEx( eLog_Error, "HEADER received %d bytes rather than %d", nBytesRead, sizeof( MESSAGE_HEADER ) );
		return false;
	}

	// Find out how many bytes are in the buffer
	Implementation::MemoryBuffer*	buf = new Implementation::MemoryBuffer( &header.bufferSize[0], 4, false );
	GlobalLog()->PrintNew( buf, __FILE__, __LINE__, "mem buffer" );
	unsigned int nBufferSize = buf->getUInt();
	safe_release( buf );

	if( nBufferSize > 0 ) {

		// Now get the buffer
		buffer = new Implementation::MemoryBuffer( nBufferSize );
		GlobalLog()->PrintNew( buffer, __FILE__, __LINE__, "mem buffer" );

		nBytesRead = ReadData( buffer->Pointer(), nBufferSize );

		if( nBytesRead != int(nBufferSize) ) {
			GlobalLog()->PrintEx( eLog_Error, "MessageBuffer received %d bytes rather than %d", nBytesRead, nBufferSize );
			safe_release( buffer );
			return false;
		}
	}

	type = (MESSAGE_TYPE)header.chType;

	// Everything ok
	return true;
}

bool SocketCommunicator::IsConnectionOpen( )
{
	if( conn && conn != BAD_SOCKET ) {
		return true;
	}

	return false;
}

void SocketCommunicator::CloseConnection( )
{
	if( conn && conn != BAD_SOCKET ) {
		SocketComm::CloseSocket( conn );
		conn = BAD_SOCKET;
	}
}

