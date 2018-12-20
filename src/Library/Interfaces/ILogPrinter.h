//////////////////////////////////////////////////////////////////////
//
//  ILogPrinter.h - Interface that defines a log printer.  Log printers
//  are what allow log messages to be routed to some form that is 
//  readable by the user.  
//
//  Common examples, standard output, file printer, window (WIN32 only)
//  and so on.
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

#ifndef ILOG_PRINTER_
#define ILOG_PRINTER_

#include "../Utilities/Log/LogEvent.h"

namespace RISE
{
	class ILogPrinter : public virtual IReference
	{
	protected:
		ILogPrinter(){};
		virtual ~ILogPrinter(){};

	public:

		//! Prints the log event
		virtual void Print( 
			const LogEvent& event						///< [in] Log event to print
			) = 0;

		//! Flushes the contents out to their final destination
		virtual void Flush( ) = 0;

	};
}

#endif
