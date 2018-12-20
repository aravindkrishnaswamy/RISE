//////////////////////////////////////////////////////////////////////
//
//  StreamPrinter.h - A log printer that prints to a stream
//
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 14, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef STREAM_PRINTER_
#define STREAM_PRINTER_

#include "../../Interfaces/ILogPrinter.h"
#include "../Threads/Threads.h"
#include "../Reference.h"
#include <iostream>

namespace RISE
{
	namespace Implementation
	{
		class StreamPrinter : public virtual ILogPrinter, public virtual Reference
		{
		protected:
			virtual ~StreamPrinter();

			std::ostream*	pStream;
			bool			bFlushAlways;
			bool			bFreeStream;
			LOG_ENUM		eTypes;
			RMutex			mutexPrint;

		public:
			StreamPrinter( std::ostream* pOut, const bool bFlushAlways_, const bool free_stream=false );
			StreamPrinter( std::ostream* pOut, const bool bFlushAlways_, const LOG_ENUM eTypes_, const bool free_stream=false );

			void Print( const LogEvent& event );
			void Flush();
		};
	}
}

#endif

