//////////////////////////////////////////////////////////////////////
//
//  Connection.h - Represents a generic connection object
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 25, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef CONNECTION_
#define CONNECTION_

#include "../Library/Interfaces/ICommunicator.h"
#include "../Library/Utilities/MemoryBuffer.h"
#include "../Library/Utilities/Reference.h"

namespace RISE
{
	enum CLIENT_TYPE {
		eClient_Unknown		= 0,
		eClient_Worker		= 1,
		eClient_Submitter	= 2,
		eClient_MCP			= 3
	};


	class Connection : public Implementation::Reference
	{
	protected:
		ICommunicator*		pCommunicator;

		IMemoryBuffer*		pSendBuffer;
		IMemoryBuffer*		pRecvBuffer;

		MESSAGE_TYPE		mtRecvType;

		Connection( ICommunicator* pCommunicator_ );

		virtual ~Connection();

		// Trys to receive a message, if it suceeds, it puts it in pRecvBuffer
		bool TryReceiveMessage();

		// Trys to receive a specific type of message, fails if the message isn't
		// this type
		bool TryReceiveSpecificMessage( MESSAGE_TYPE mtExpected );

		// Trys to send a message, bSendBuffer tells it whether to include the class level
		// memory buffer
		bool TrySendMessage( MESSAGE_TYPE mtSendType, bool bSendBuffer );

		// Trys to send a message, assumes pSendBuffer contains the message buffer
		// to send the type is passed in
		bool TrySendMessage( MESSAGE_TYPE mtSendType, const IMemoryBuffer* pBuffer );

	public:

		virtual bool PerformHandshaking( const char * szSecretCode ) = 0;
	};
}

#endif

