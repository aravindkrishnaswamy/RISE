//////////////////////////////////////////////////////////////////////
//
//  ILog.h - Definition of the log interface.  The log class is meant as the
//  central place for which the library to log warnings, errors
//  and events in general.  These events are then passed to LogPrinters
//  who can then output those events they feel are important
//
//  This is the file to include to be able to just log messages to the 
//  global application log.  In order to create custom logs, printers
//  and Such, one must include Log.h
//
//  The logger must be able to function even if the application becomes
//  critically unstable.  Thus no memory allocation or deallocation 
//  can occur inside the logger itself.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 13, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ILOG_
#define ILOG_

namespace RISE
{
	enum LOG_ENUM {
		eLog_Event		= 1,				///< An event, usually stuff we want to show the user
		eLog_Info		= 2,				///< Just information, really benign
		eLog_Warning	= 4,				///< Warnings are errors, but their severity keeps them from being so
		eLog_Error		= 8,				///< Errors are rough, you should find and fix these, these shouldn't happen
		eLog_Fatal		= 16,				///< Its the end of the world!

		eLog_Benign		= eLog_Event | eLog_Info,
		eLog_Serious	= eLog_Warning | eLog_Error | eLog_Fatal,
		eLog_All		= eLog_Info | eLog_Warning | eLog_Error | eLog_Fatal | eLog_Event,

		eLog_Console = eLog_Serious | eLog_Event
	};
	
	class IReference;

	class ILog/* : public virtual IReference*/	// GCC 3.x bug with virtual inheritance and var args, !@@ Add this back later
	{
	protected:
		ILog(){};
		virtual ~ILog(){};

	public:
		virtual void Print( LOG_ENUM eType, const char * szMessage ) const = 0;
		virtual void PrintEx( LOG_ENUM eType, const char * szFormat, ... ) const = 0;	

		inline void PrintEasyWarning( const char * szMessage ) const { Print( eLog_Warning, szMessage ); }
		inline void PrintEasyError( const char * szMessage ) const { Print( eLog_Error, szMessage ); }
		inline void PrintEasyInfo( const char * szMessage ) const { Print( eLog_Info, szMessage ); }
		inline void PrintEasyEvent( const char * szMessage ) const { Print( eLog_Event, szMessage ); }

		inline void PrintSourceWarning( const char * szMessage, const char * szFile, const int nLine ) const { PrintEx( eLog_Warning, "%s [%s] line:%d", szMessage, szFile, nLine ); }
		inline void PrintSourceError( const char * szMessage, const char * szFile, const int nLine ) const { PrintEx( eLog_Error, "%s [%s] line:%d", szMessage, szFile, nLine ); }
		inline void PrintSourceInfo( const char * szMessage, const char * szFile, const int nLine ) const { PrintEx( eLog_Info, "%s [%s] line:%d", szMessage, szFile, nLine ); }
		inline void PrintSourceEvent( const char * szMessage, const char * szFile, const int nLine ) const { PrintEx( eLog_Info, "%s [%s] line:%d", szMessage, szFile, nLine ); }

		virtual void PrintNew( const void * ptr, const char * szFile, const int nLine, const char * szMessage = 0 ) const = 0;
		virtual void PrintNew( const IReference* ptr, const char * szFile, const int nLine, const char * szMessage = 0 ) const = 0;
		virtual void PrintDelete( const void * ptr, const char * szFile, const int nLine ) const = 0;
		virtual void PrintDelete( const IReference* ptr, const char * szFile, const int nLine ) const = 0;

		virtual void FlushPrinters() = 0;
	};

	// Handy dandy functions
	extern ILog* GlobalLog();
	extern void GlobalLogCleanupAndShutdown();
}

#include "IReference.h"

#endif
