//////////////////////////////////////////////////////////////////////
//
//  LogEvent.h - Defines a log event.  A log event is what the 
//  log class, upon recieving a log request constructs.  A log event
//  is what keeps around all data that was passed to the log request.
//
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 13, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef LOG_EVENT_
#define LOG_EVENT_

#include "../../Interfaces/ILog.h"

namespace RISE
{
	// 
	// These constants are crucial to proper log operation.  
	//
	static const unsigned int	MAX_STR_SIZE = 1024; 

	class LogEvent
	{
	public:
		virtual ~LogEvent(){};

		LOG_ENUM		eType;
		char			szMessage[MAX_STR_SIZE];
	};

}
#endif
