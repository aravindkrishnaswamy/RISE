//////////////////////////////////////////////////////////////////////
//
//  Log.h - Definition of a log class.  See ILog.h for what it does
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

#ifndef LOG_
#define LOG_

#include "../../Interfaces/ILogPriv.h"
#include "LogEvent.h"
#include "MemoryTracker.h"
#include "../Threads/Threads.h"
//#include "../Reference.h"
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		class Log : public ILogPriv/*, public virtual Reference   GCC 3.x bug */
		{
		protected:
	//		virtual ~Log();
			typedef				std::vector<ILogPrinter*> PrintersList;
			PrintersList		printersList;
			RMutex				mutexPrinters;

		public:
			MemoryTracker*		memTracker;

			Log( );
			virtual ~Log();

			virtual void AddPrinter( ILogPrinter* pPrinter );
			virtual void RemoveAllPrinters( );

			virtual void Print( LOG_ENUM eType, const char * szMessage ) const;
			virtual void PrintEx( LOG_ENUM eType, const char * szFormat, ... ) const;	
			virtual void FlushPrinters();

			virtual void PrintNew( const void * ptr, const char * szFile, const int nLine, const char * szMessage ) const;
			virtual void PrintNew( const IReference* ptr, const char * szFile, const int nLine, const char * szMessage = 0 ) const;

			virtual void PrintDelete( const void * ptr, const char * szFile, const int nLine ) const;
			virtual void PrintDelete( const IReference* ptr, const char * szFile, const int nLine ) const;
		};
	}
}

#endif

