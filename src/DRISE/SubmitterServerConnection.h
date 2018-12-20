//////////////////////////////////////////////////////////////////////
//
//  SubmitterServerConnection.h - Defines a submitter client that is 
//    talking to a server
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 30, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SUBMITTER_SERVER_CONNECTION_
#define SUBMITTER_SERVER_CONNECTION_

#include "ServerConnection.h"

namespace RISE
{
	class SubmitterServerConnection : public ServerConnection
	{
	protected:
		virtual ~SubmitterServerConnection();

	public:
		SubmitterServerConnection( ICommunicator* pCommunicator );
		bool ProcessServerRequest();
		bool SubmitJob( const char * szFileName, unsigned int x, unsigned int y, const char* szOutputName, unsigned int xgran, unsigned int ygran );
		bool SubmitAnimationJob( const char * szFileName, unsigned int x, unsigned int y, const char* szOutputName, const unsigned int frames );
	};
}

#endif

