//////////////////////////////////////////////////////////////////////
//
//  StreamPrinter.cpp - Implementation of the StreamPrinter
//
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 14, 2002
//  Tabs: 4
//  Comments: Mutex-serialized ILogPrinter sink targeting any std::ostream*.
//    Deletes pStream in ~StreamPrinter() iff bFreeStream is true (pass false
//    for non-owned streams like std::cout/std::cerr). Filters via (eTypes & event.eType).
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "StreamPrinter.h"

using namespace RISE;
using namespace RISE::Implementation;

StreamPrinter::StreamPrinter( std::ostream* pOut, const bool bFlushAlways_, const bool free_stream ) :
  pStream( pOut ), bFlushAlways( bFlushAlways_ ), bFreeStream( free_stream ), eTypes( eLog_All )
{
}

StreamPrinter::StreamPrinter( std::ostream* pOut, const bool bFlushAlways_, const LOG_ENUM eTypes_,  const bool free_stream ) :
  pStream( pOut ), bFlushAlways( bFlushAlways_ ), bFreeStream( free_stream ), eTypes( eTypes_ )
{
}

StreamPrinter::~StreamPrinter( )
{
	if( pStream && bFreeStream ) {
		delete pStream;
		pStream = 0;
	}
}

void StreamPrinter::Print( const LogEvent& event )
{
	// Only print messages that we are allowed to print
	if( eTypes & event.eType ) {
		if( pStream ) {

			const std::lock_guard<std::mutex> lock( mutexPrint );
			(*pStream) << event.szMessage << std::endl;

			if( bFlushAlways ) {
				pStream->flush();
			}
		}
	}
}

void StreamPrinter::Flush( )
{
	const std::lock_guard<std::mutex> lock( mutexPrint );
	if( pStream ) {
		pStream->flush();
	}
}
