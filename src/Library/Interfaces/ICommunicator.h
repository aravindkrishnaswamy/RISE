//////////////////////////////////////////////////////////////////////
//
//  ICommunicator.h - Definition for the communicator interface
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 24, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ICOMMUNICATOR_
#define ICOMMUNICATOR_

#include "IReference.h"
#include "IMemoryBuffer.h"
#include "../Utilities/Communications/Message.h"

namespace RISE
{
	class ICommunicator : public virtual IReference
	{
	protected:
		ICommunicator(){};
		virtual ~ICommunicator(){};

	public:
		// 
		// These are the public pure virtual interface functions that
		// anybody who extends must implement
		//

		// Messages sent are a combination of the header and the buffer that
		// contains the actual message
		virtual bool CommSendMessage( const MESSAGE_TYPE type, const IMemoryBuffer* buffer ) = 0;

		// Asking to receive a messsage from a communicator is a BLOCKING operator
		// just keep that in mind.
		virtual bool CommRecvMessage( MESSAGE_TYPE& type, IMemoryBuffer*& buffer ) = 0;

		// Tells whether the connection is open
		virtual bool IsConnectionOpen( ) = 0;

		// This closes the connection
		virtual void CloseConnection( ) = 0;
	};
}

#endif

