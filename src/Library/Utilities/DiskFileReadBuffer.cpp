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
		nSize = file_stats.st_size;

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
#ifdef _DEBUG
	if( getCurPos()==nSize ) {
		GlobalLog()->PrintSourceError( "DiskFileReadBuffer::getChar:: Attempted read past end of buffer", __FILE__, __LINE__ );
		return 0;
	}
#endif
	char c=0;
	fread( &c, 1, 1, hFile );
	return c;
}

unsigned char DiskFileReadBuffer::getUChar()
{
#ifdef _DEBUG
	if( getCurPos()==nSize ) {
		GlobalLog()->PrintSourceError( "DiskFileReadBuffer::getUChar:: Attempted read past end of buffer", __FILE__, __LINE__ );
		return 0;
	}
#endif

	unsigned char c=0;
	fread( &c, 1, 1, hFile );
	return c;
}

short DiskFileReadBuffer::getWord()
{
#ifdef _DEBUG
	if( getCurPos()+sizeof(short) > nSize ) {
		GlobalLog()->PrintSourceError( "DiskFileReadBuffer::getWord:: Attempted read past end of buffer", __FILE__, __LINE__ );
		return 0;
	}
#endif

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
#ifdef _DEBUG
	if( getCurPos()+sizeof(unsigned short) > nSize ) {
		GlobalLog()->PrintSourceError( "DiskFileReadBuffer::getUWord:: Attempted read past end of buffer", __FILE__, __LINE__ );
		return 0;
	}
#endif

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
#ifdef _DEBUG
	if( getCurPos()+sizeof(int) > nSize ) {
		GlobalLog()->PrintSourceError( "DiskFileReadBuffer::getInt:: Attempted read past end of buffer", __FILE__, __LINE__ );
		return 0;
	}
#endif

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
#ifdef _DEBUG
	if( getCurPos()+sizeof(unsigned int) > nSize ) {
		GlobalLog()->PrintSourceError( "DiskFileReadBuffer::getUInt:: Attempted read past end of buffer", __FILE__, __LINE__ );
		return 0;
	}
#endif

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
#ifdef _DEBUG
	if( getCurPos()+sizeof(float) > nSize ) {
		GlobalLog()->PrintSourceError( "DiskFileReadBuffer::getFloat:: Attempted read past end of buffer", __FILE__, __LINE__ );
		return 0;
	}
#endif

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
#ifdef _DEBUG
	if( getCurPos()+sizeof(double) > nSize ) {
		GlobalLog()->PrintSourceError( "DiskFileReadBuffer::getDouble:: Attempted read past end of buffer", __FILE__, __LINE__ );
		return 0;
	}
#endif

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
#ifdef _DEBUG
	if( getCurPos()+amount > nSize ) {
		GlobalLog()->PrintEx( eLog_Error, "DiskFileReadBuffer::getBytes:: Attempted read past end of buffer, read %d bytes, cursor at %d bytes", amount, getCurPos() );
		return false;
	}
#endif
	if( !pDest ) {
		return false;
	}

	fread( pDest, amount, 1, hFile );
	return true;
}

int DiskFileReadBuffer::getLine( char* pDest, unsigned int max )
{
	if( !pDest ) {
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
