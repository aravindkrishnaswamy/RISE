//////////////////////////////////////////////////////////////////////
//
//  Log.cpp - Implementation of the Log class
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

#include "pch.h"
#include "Log.h"
#include <stdarg.h>
#include <stdio.h>

#include "StreamPrinter.h"
#include <fstream>

using namespace RISE;
using namespace RISE::Implementation;

//#define LOG_MEMORY_ALLOCS_FREES
//#define LOG_TRACK_MEMORY

Log::Log( ) : 
  memTracker( 0 )
{
#ifdef LOG_TRACK_MEMORY
	memTracker = new MemoryTracker();
#endif
}

Log::~Log( )
{
	RemoveAllPrinters();

	if( memTracker ) {
		delete memTracker;
		memTracker = 0;
	}
}

void Log::AddPrinter( ILogPrinter* pPrinter )
{
	mutexPrinters.lock();

	if( pPrinter ) {
		pPrinter->addref();
		printersList.push_back( pPrinter );
	}

	mutexPrinters.unlock();
}

void Log::RemoveAllPrinters( )
{
	mutexPrinters.lock();

	PrintersList::iterator		i;
	for( i=printersList.begin(); i!=printersList.end(); ) {
		ILogPrinter* pPrinter = *i;
		printersList.erase( i );
		safe_release( pPrinter );
	}
	printersList.clear();

	mutexPrinters.unlock();
}

void Log::Print( LOG_ENUM eType, const char * szMessage ) const
{
	// Construct a LogEvent and pass it to all the printers
	LogEvent		le;
	strncpy( le.szMessage, szMessage, MAX_STR_SIZE );
	le.eType = eType;

	PrintersList::const_iterator		i, e;
	for( i=printersList.begin(), e=printersList.end(); i!=e; i++ ) {
		(*i)->Print( le );
	}
}

void Log::PrintEx( LOG_ENUM eType, const char * szFormat, ... ) const
{
	// Construct a LogEvent and pass it to all the printers
	va_list			list;
	LogEvent		le;

	va_start( list, szFormat );
#ifdef _WIN32
	_vsnprintf( le.szMessage, MAX_STR_SIZE, szFormat, list );
#else
	vsnprintf( le.szMessage, MAX_STR_SIZE, szFormat, list );
#endif
	va_end(list);
	le.eType = eType;

	PrintersList::const_iterator		i, e;
	for( i=printersList.begin(), e=printersList.end(); i!=e; i++ ) {
		(*i)->Print( le );
	}
}

void Log::FlushPrinters()
{
	PrintersList::const_iterator		i, e;
	for( i=printersList.begin(), e=printersList.end(); i!=e; i++ ) {
		(*i)->Flush( );
	}
}

//
// Utilities for tracing memory allocation / deletion, helps track memory leaks
//
void Log::PrintNew( const void * ptr, const char * szFile, const int nLine, const char * szMessage  ) const
{
#ifdef LOG_MEMORY_ALLOCS_FREES
	PrintEx( eLog_Info, "Allocated memory for \"%s\", [%s] at line:%d, ptr = 0x%x", szMessage, szFile, nLine, (unsigned int)ptr );
#endif

#ifdef	LOG_TRACK_MEMORY
	if( memTracker && ptr ) {
		memTracker->NewAllocation( ptr, szFile, nLine, szMessage );
	}
#endif
}

void Log::PrintDelete( const void * ptr, const char * szFile, const int nLine ) const
{
#ifdef LOG_MEMORY_ALLOCS_FREES
	PrintEx( eLog_Info, "Freed memory,\t [%s]\t\t at line:%d, ptr = 0x%x", szFile, nLine, (unsigned int)ptr );
#endif

#ifdef	LOG_TRACK_MEMORY
	if( memTracker && ptr ) {
		memTracker->FreeAllocation( ptr, szFile, nLine );
	}
#endif
}

void Log::PrintNew( const IReference* ptr, const char * szFile, const int nLine, const char * szMessage ) const
{
	PrintNew( (const void*)ptr, szFile, nLine, szMessage );
}

void Log::PrintDelete( const IReference* ptr, const char * szFile, const int nLine ) const
{
	PrintDelete( (const void*)ptr, szFile, nLine );
}


namespace RISE
{
	static Log*	global_log = 0;
	static char szGlobalLogFileName[1024] = "RISELog.txt";

	void CreateGlobalLog( )
	{
		global_log = new Log( );

		StreamPrinter*	pA = new StreamPrinter( &std::cout, true, eLog_Console, false );
		global_log->PrintNew( pA, __FILE__, __LINE__, "stdout stream printer" );
		global_log->AddPrinter( pA );

		std::ofstream*		fs = new std::ofstream( szGlobalLogFileName );
		StreamPrinter*	pB = new StreamPrinter( fs, true, true );
		global_log->PrintNew( pB, __FILE__, __LINE__, "file stream printer" );
		global_log->AddPrinter( pB );

		safe_release( pA );
		safe_release( pB );
	}

	void SetGlobalLogFileName( const char * name )
	{
		strncpy( szGlobalLogFileName, name, 1024 );
	}

	ILog* GlobalLog( )
	{
		if( !global_log ) {
			CreateGlobalLog();
		}

		return global_log;
	}

	ILogPriv* GlobalLogPriv()
	{
		if( !global_log ) {
			CreateGlobalLog();
		}

		return global_log;
	}

	// Warning! once this is called, thats it... its all over, don't call this
	// unless you totally mean it!
	void GlobalLogCleanupAndShutdown()
	{
		// Can't delete the global log because of reference counting memory tracking issues
		// Turf all the printers
		if( global_log ) {
			global_log->RemoveAllPrinters();

			delete global_log->memTracker;
			global_log->memTracker = 0;

			delete global_log;
			global_log = 0;
		}
	}
}
