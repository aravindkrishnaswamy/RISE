//////////////////////////////////////////////////////////////////////
//
//  SocketCommunications.h - Definition of some common communications
//    routines
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 15, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SOCKET_COMMUNICATIONS_
#define SOCKET_COMMUNICATIONS_

#ifdef WIN32
	#include <winsock.h>
#else
	#include <unistd.h>
	#include <sys/types.h>
	#include <sys/socket.h>
	#include <netinet/in.h>
	#include <netdb.h>

#ifndef SOCKET
	typedef int SOCKET;
#endif

#endif

namespace RISE
{
	static const SOCKET BAD_SOCKET = SOCKET(-1);

	// These are platform independent functions
	struct SocketComm
	{
		//! Initializes socket communications
		/// \return TRUE if successful, FALSE otherwise
		static bool InitializeSocketCommunications( );

		//! Called when all socket communications is done
		static void CloseSocketCommunications( );

		//! Closes a socket
		static void CloseSocket(
			SOCKET sock									///< [in] Socket to close
			);

		//! Establishes a new connection to a socket to listen on
		/// \return The opened Socket
		static SOCKET EstablishConnection( 
			unsigned short portnum,						///< [in] Port to open
			unsigned int protocol_type,					///< [in] Protocol to open with
			unsigned int socket_type					///< [in] The type of socket
			);

		//! Opens a socket on another machine
		/// \return The opened Socket
		static SOCKET CallSocket( 
			const char* szHostName,						///< [in] String with the host name of the other computer (or IP address)
			unsigned short portnum,						///< [in] Port number
			unsigned int socket_type					///< [in] The type of socket
			);

		//! Waits for a connection on a socket
		/// \return A socket with the established connection
		static SOCKET GetConnection( 
			SOCKET s									///< [in] The socket to listen on
			);
	};
}

#endif

