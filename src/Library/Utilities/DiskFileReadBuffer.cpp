//////////////////////////////////////////////////////////////////////
//
//  DiskFileReadBuffer.h - Implements the DiskFileReadBuffer
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 3, 2003
//  Tabs: 4
//  Comments:
//
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "DiskFileReadBuffer.h"
#include "../Interfaces/ILog.h"
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "MediaPathLocator.h"

using namespace RISE::Implementation;

DiskFileReadBuffer::DiskFileReadBuffer( const char * file_name ) : nSize( 0 )
{
	strncpy( szFileName, GlobalMediaPathLocator().Find(file_name).c_str(), 1024 );

	struct stat file_stats = {0};

	if( stat( szFileName, &file_stats ) == -1 ) {
		GlobalLog()->PrintEx( eLog_Warning, "DiskFileReadBuffer:: Could not stat file \'%s\'", file_name );
	} else {
		nSize = static_cast<unsigned int>(file_stats.st_size);

		hFile = fopen( szFileName, "rb" );

		if( hFile == 0 ) {
			GlobalLog()->PrintEx( eLog_Warning, "DiskFileReadBuffer:: Failed to open file \'%s\'", file_name );
		} else {
			GlobalLog()->PrintEx( eLog_Info, "DiskFileReadBuffer:: Opened file \'%s\' of size %d bytes for reading", file_name, nSize );
		}
	}
}

DiskFileReadBuffer::~DiskFileReadBuffer( )
{
	if( hFile ) {
		fclose( hFile );
		hFile = 0;
	}
}

char DiskFileReadBuffer::getChar()
{
	// No file handle (open failed): degrade gracefully instead of
	// ftell(NULL)/fread(NULL) UB.
	if( !hFile ) {
		return 0;
	}
	// Bounds-check UNCONDITIONALLY (was _DEBUG-only): a read past
	// the buffer end otherwise partial-reads into the local (a
	// truncated file yields a corrupt value).  Return 0 on exhaustion.
	if( getCurPos() >= nSize ) {
	#ifdef _DEBUG
		GlobalLog()->PrintSourceError( "DiskFileReadBuffer::getChar:: Attempted read past end of buffer", __FILE__, __LINE__ );
	#endif
		fseek( hFile, 0, SEEK_END );   // consume to end so loop-until-cursor callers terminate
		return 0;
	}
	char c=0;
	fread( &c, 1, 1, hFile );
	return c;
}

unsigned char DiskFileReadBuffer::getUChar()
{
	// No file handle (open failed): degrade gracefully instead of
	// ftell(NULL)/fread(NULL) UB.
	if( !hFile ) {
		return 0;
	}
	// Bounds-check UNCONDITIONALLY (was _DEBUG-only): a read past
	// the buffer end otherwise partial-reads into the local (a
	// truncated file yields a corrupt value).  Return 0 on exhaustion.
	if( getCurPos() >= nSize ) {
	#ifdef _DEBUG
		GlobalLog()->PrintSourceError( "DiskFileReadBuffer::getUChar:: Attempted read past end of buffer", __FILE__, __LINE__ );
	#endif
		fseek( hFile, 0, SEEK_END );   // consume to end so loop-until-cursor callers terminate
		return 0;
	}

	unsigned char c=0;
	fread( &c, 1, 1, hFile );
	return c;
}

short DiskFileReadBuffer::getWord()
{
	// No file handle (open failed): degrade gracefully instead of
	// ftell(NULL)/fread(NULL) UB.
	if( !hFile ) {
		return 0;
	}
	// Bounds-check UNCONDITIONALLY (was _DEBUG-only): a read past
	// the buffer end otherwise partial-reads into the local (a
	// truncated file yields a corrupt value).  Return 0 on exhaustion.
	if( getCurPos() > nSize || sizeof(short) > nSize - getCurPos() ) {
	#ifdef _DEBUG
		GlobalLog()->PrintSourceError( "DiskFileReadBuffer::getWord:: Attempted read past end of buffer", __FILE__, __LINE__ );
	#endif
		fseek( hFile, 0, SEEK_END );   // consume to end so loop-until-cursor callers terminate
		return 0;
	}

#ifdef RISE_BIG_ENDIAN
	short Low = getUChar();
    short High = getUChar();
	return Low | (High << 8);
#else
	short sh = 0;
	fread( &sh, sizeof( short ), 1, hFile );
	return sh;
#endif
}


unsigned short DiskFileReadBuffer::getUWord()
{
	// No file handle (open failed): degrade gracefully instead of
	// ftell(NULL)/fread(NULL) UB.
	if( !hFile ) {
		return 0;
	}
	// Bounds-check UNCONDITIONALLY (was _DEBUG-only): a read past
	// the buffer end otherwise partial-reads into the local (a
	// truncated file yields a corrupt value).  Return 0 on exhaustion.
	if( getCurPos() > nSize || sizeof(unsigned short) > nSize - getCurPos() ) {
	#ifdef _DEBUG
		GlobalLog()->PrintSourceError( "DiskFileReadBuffer::getUWord:: Attempted read past end of buffer", __FILE__, __LINE__ );
	#endif
		fseek( hFile, 0, SEEK_END );   // consume to end so loop-until-cursor callers terminate
		return 0;
	}

#ifdef RISE_BIG_ENDIAN
	unsigned short Low = getUChar();
    unsigned short High = getUChar();
	return Low | (High << 8);
#else
	unsigned short sh = 0;
	fread( &sh, sizeof( unsigned short ), 1, hFile );
	return sh;
#endif
}

int DiskFileReadBuffer::getInt()
{
	// No file handle (open failed): degrade gracefully instead of
	// ftell(NULL)/fread(NULL) UB.
	if( !hFile ) {
		return 0;
	}
	// Bounds-check UNCONDITIONALLY (was _DEBUG-only): a read past
	// the buffer end otherwise partial-reads into the local (a
	// truncated file yields a corrupt value).  Return 0 on exhaustion.
	if( getCurPos() > nSize || sizeof(int) > nSize - getCurPos() ) {
	#ifdef _DEBUG
		GlobalLog()->PrintSourceError( "DiskFileReadBuffer::getInt:: Attempted read past end of buffer", __FILE__, __LINE__ );
	#endif
		fseek( hFile, 0, SEEK_END );   // consume to end so loop-until-cursor callers terminate
		return 0;
	}

#ifdef RISE_BIG_ENDIAN
	int Low = getUWord();
    int High = getUWord();
    return Low | (High << 16);
#else
	int n = 0;
	fread( &n, sizeof( int ), 1, hFile );
	return n;
#endif
}

unsigned int DiskFileReadBuffer::getUInt()
{
	// No file handle (open failed): degrade gracefully instead of
	// ftell(NULL)/fread(NULL) UB.
	if( !hFile ) {
		return 0;
	}
	// Bounds-check UNCONDITIONALLY (was _DEBUG-only): a read past
	// the buffer end otherwise partial-reads into the local (a
	// truncated file yields a corrupt value).  Return 0 on exhaustion.
	if( getCurPos() > nSize || sizeof(unsigned int) > nSize - getCurPos() ) {
	#ifdef _DEBUG
		GlobalLog()->PrintSourceError( "DiskFileReadBuffer::getUInt:: Attempted read past end of buffer", __FILE__, __LINE__ );
	#endif
		fseek( hFile, 0, SEEK_END );   // consume to end so loop-until-cursor callers terminate
		return 0;
	}

#ifdef RISE_BIG_ENDIAN
	unsigned int Low = getUWord();
    unsigned int High = getUWord();
    return Low | (High << 16);
#else
	unsigned int n = 0;
	fread( &n, sizeof( unsigned int ), 1, hFile );
	return n;
#endif
}

float DiskFileReadBuffer::getFloat()
{
	// No file handle (open failed): degrade gracefully instead of
	// ftell(NULL)/fread(NULL) UB.
	if( !hFile ) {
		return 0;
	}
	// Bounds-check UNCONDITIONALLY (was _DEBUG-only): a read past
	// the buffer end otherwise partial-reads into the local (a
	// truncated file yields a corrupt value).  Return 0 on exhaustion.
	if( getCurPos() > nSize || sizeof(float) > nSize - getCurPos() ) {
	#ifdef _DEBUG
		GlobalLog()->PrintSourceError( "DiskFileReadBuffer::getFloat:: Attempted read past end of buffer", __FILE__, __LINE__ );
	#endif
		fseek( hFile, 0, SEEK_END );   // consume to end so loop-until-cursor callers terminate
		return 0;
	}

#ifdef RISE_BIG_ENDIAN
	float f = 0;
	unsigned int d = getUInt();
	memcpy( &f, &d, sizeof( float ) );
	return f;
#else
	float f = 0;
	fread( &f, sizeof( float ), 1, hFile );
	return f;
#endif
}

double DiskFileReadBuffer::getDouble()
{
	// No file handle (open failed): degrade gracefully instead of
	// ftell(NULL)/fread(NULL) UB.
	if( !hFile ) {
		return 0;
	}
	// Bounds-check UNCONDITIONALLY (was _DEBUG-only): a read past
	// the buffer end otherwise partial-reads into the local (a
	// truncated file yields a corrupt value).  Return 0 on exhaustion.
	if( getCurPos() > nSize || sizeof(double) > nSize - getCurPos() ) {
	#ifdef _DEBUG
		GlobalLog()->PrintSourceError( "DiskFileReadBuffer::getDouble:: Attempted read past end of buffer", __FILE__, __LINE__ );
	#endif
		fseek( hFile, 0, SEEK_END );   // consume to end so loop-until-cursor callers terminate
		return 0;
	}

#ifdef RISE_BIG_ENDIAN
	unsigned int Low = getUInt();
    unsigned int High = getUInt();

	double d = 0;
	char* ptrd = (char*)&d;
	memcpy( ptrd, &High, sizeof( unsigned int  ) );
	memcpy( &ptrd[4], &Low, sizeof( unsigned int ) );
	return d;
#else
	double d = 0;
	if( fread( &d, sizeof( double ), 1, hFile ) == 0 ) {
		GlobalLog()->PrintEasyError( "DiskFileReadBuffer::getDouble:: Failed to read all the bytes required for a double" );
	}
	return d;
#endif
}

bool DiskFileReadBuffer::getBytes( void* pDest, unsigned int amount )
{
	if( !pDest || !hFile ) {
		return false;
	}

	// Bounds-check UNCONDITIONALLY (was _DEBUG-only) and CHECK the fread
	// count: a truncated file must not leave the caller's buffer tail
	// uninitialised.  getCurPos() <= nSize is the invariant, so the
	// subtraction cannot underflow.
	const unsigned int cur    = getCurPos();
	const unsigned int avail  = ( cur <= nSize ) ? ( nSize - cur ) : 0;
	const unsigned int toRead = ( amount <= avail ) ? amount : avail;

	size_t got = 0;
	if( toRead ) {
		got = fread( pDest, 1, toRead, hFile );
	}
	if( got < amount ) {
		// Short read (truncation / EOF): zero the tail and signal failure.
		memset( static_cast<char*>(pDest) + got, 0, amount - got );
		GlobalLog()->PrintEx( eLog_Error, "DiskFileReadBuffer::getBytes:: short read (want %u at cursor %u of %u, got %u); rest zeroed", amount, cur, nSize, static_cast<unsigned>(got) );
		return false;
	}
	return true;
}

int DiskFileReadBuffer::getLine( char* pDest, unsigned int max )
{
	if( !pDest || !hFile ) {
		return false;
	}

	// We keep reading until the end if we have to
	unsigned int numRead = 0;
	while( getCurPos() < nSize && numRead < max ) {
		pDest[numRead] = getChar();

		if( pDest[numRead] == '\n' ) {
			return numRead+1;
		}
		numRead++;
	}

	return numRead;
}
