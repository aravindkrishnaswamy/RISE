//////////////////////////////////////////////////////////////////////
//
//  SocketCommunications.cpp - Implementation of some common
//    communications routines
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 15, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "SocketCommunications.h"
#include "../../Interfaces/ILog.h"
#include <memory>

using namespace RISE;

#ifdef WIN32

//
// Windows socket code initialization
//

bool SocketComm::InitializeSocketCommunications( )
{
	WSADATA               d_winsockData;
	unsigned short        d_winsockVersion;

	static const unsigned int WINSOCK_MAJOR_VERSION = 2;
	static const unsigned int WINSOCK_MINOR_VERSION = 0;

	char buffer[128];
	int error;

	d_winsockVersion = MAKEWORD( WINSOCK_MAJOR_VERSION, WINSOCK_MINOR_VERSION );

	error = WSAStartup( d_winsockVersion, &d_winsockData );
	if( error == SOCKET_ERROR )
	{
		if( error == WSAVERNOTSUPPORTED ) {
			sprintf( buffer, "WSAStartup error.\nRequested Winsock v%d.%d, found v%d.%d.",
			WINSOCK_MAJOR_VERSION, WINSOCK_MINOR_VERSION,
			LOBYTE( d_winsockData.wVersion ), HIBYTE( d_winsockData.wVersion ) );
			WSACleanup();
		} else {
			sprintf( buffer, "WSAStartup error (%d)", WSAGetLastError() );
		}

		GlobalLog()->PrintSourceError( buffer, __FILE__, __LINE__ );
		return false;
	}

	return true;  // no problems!
}

void SocketComm::CloseSocketCommunications( )
{
	WSACleanup( );
}

void SocketComm::CloseSocket( SOCKET sock )
{
	closesocket( sock );
}

#else

//
// Unix sockets
//

bool SocketComm::InitializeSocketCommunications( )
{
	return true;
}

void SocketComm::CloseSocketCommunications( )
{
}

void SocketComm::CloseSocket( SOCKET sock )
{
	shutdown( sock, 0 );
}

#endif


//
// Code for all platforms
//

SOCKET SocketComm::EstablishConnection( unsigned short portnum, unsigned int protocol_type, unsigned int socket_type )
{
	/** Another way of doing things
	static const unsigned int MAXHOSTNAME = 255;
	char			myname[MAXHOSTNAME+1] = {0};
	SOCKET			s = BAD_SOCKET;

	sockaddr_in		sa;
	hostent			*hp = 0;

	memset(&sa, 0, sizeof(sockaddr_in));			// clear our address
	gethostname(myname, MAXHOSTNAME);				// who are we?
	hp = gethostbyname(myname);						// get our address info

	if (hp == NULL) {							    // we don't exist !?
		return(BAD_SOCKET);
	}

	sa.sin_family = hp->h_addrtype;					// this is our host address
	sa.sin_port = htons(portnum);				    // this is our port number
	if ((s = socket(AF_INET, socket_type, protocol_type)) < 0){	// create socket
		return(BAD_SOCKET);
	}

	if (bind(s,(sockaddr *)&sa,sizeof(sockaddr_in)) < 0) {
		CloseSocket(s);
		return(BAD_SOCKET);							// bind address to socket
	}

	listen(s, 3);									// max # of queued connects
	return s;
	*/

	/** Another way of doing things */
	// Variables for the server component of the application.
	struct	sockaddr_in	server_address;				// Really only contains the port we want to listen on.

	SOCKET s = socket(AF_INET, socket_type, protocol_type);	// Allocate a socket for the, type is passed in.

#ifdef WIN32
	if( s == INVALID_SOCKET ) {								// Check to see if there was a failure in allocation.
#else
	if( s < 0 ) {											// Check to see if there was a failure in allocation.
#endif
		return(BAD_SOCKET);
	}

	memset( (void*)&server_address, 0, sizeof(server_address) );					// Clear the structure used to store the port and address that we want to listen on.
	server_address.sin_port = htons(portnum);										// Put port we want to listen on here. Take care to convert from host to network endian!!!
	server_address.sin_family = AF_INET;
	server_address.sin_addr.s_addr = htonl(INADDR_ANY);								// Address of the local server goes here. We put INADDR_ANY constant here to say we want to listen on all addresses on hosts with multiple ip addresses. Take care to convert from host endian to network endian!!!!
	if( bind(s, (struct sockaddr*)&server_address, sizeof(server_address))<0 ) {	// Ok. We setup our structure. Now we tell the OS to do it.
		return(BAD_SOCKET);
	}

	if( socket_type==SOCK_STREAM ||
		socket_type==SOCK_SEQPACKET ) {

		// Only listen on stream sockets
		// datagram sockets are unconnected

		if( listen(s, 10) < 0 ) {						// Tell the OS to start listening on our port and local address for clients that want to connect.
			CloseSocket( s );
			return(BAD_SOCKET);
		}
	}

	return s;
}

SOCKET SocketComm::CallSocket( const char* szHostName, unsigned short portnum, unsigned int socket_type )
{
	sockaddr_in		sa;
	SOCKET			s = BAD_SOCKET;

	const hostent	*hp = gethostbyname(szHostName);
	if( hp == NULL ) {				// do we know the host's address?
		return(BAD_SOCKET);										// no
	}

	memset(&sa,0,sizeof(sa));
	memcpy((char *)&sa.sin_addr,hp->h_addr,hp->h_length);		// set address
	sa.sin_family= hp->h_addrtype;
	sa.sin_port= htons((unsigned short)portnum);

	s = socket(hp->h_addrtype,socket_type,0);

#ifdef WIN32
	if( s == INVALID_SOCKET ) {									// get socket
#else
	if( s < 0) {												// get socket
#endif
		return(BAD_SOCKET);
	}

	if (connect(s,(sockaddr *)&sa,sizeof sa) < 0) {				// connect
		CloseSocket(s);
		return(BAD_SOCKET);
	}

	return(s);
}

SOCKET SocketComm::GetConnection( SOCKET s )
{
	SOCKET t = BAD_SOCKET;
	
	t = accept(s,NULL,NULL);

#ifdef WIN32
	if( t == INVALID_SOCKET ) {						// accept a connection if there is one
#else
	if( t < 0 ) {									// accept a connection if there is one
#endif
		return BAD_SOCKET;
	}

	return t;
}

