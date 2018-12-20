//////////////////////////////////////////////////////////////////////
//
//  MemoryBuffer.cpp - Implements the MemoryBuffer methods
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: July 18, 2002
//  Tabs: 4
//  Comments:
//
//  ToAdd:  Getters and Setters for strings!
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "MemoryBuffer.h"
#include <memory.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "../Utilities/Math3D/Math3D.h"
#include "../Interfaces/ILog.h"
#include "MediaPathLocator.h"

using namespace RISE::Implementation;

MemoryBuffer::MemoryBuffer( ) : 
  nSize( 0 ),
  pBuffer( 0 ),
  nCursor( 0 ),
  bIOwnMemory( true )
{
}

  /*
MemoryBuffer::MemoryBuffer( const MemoryBuffer* mb ) :
  nSize( mb->nSize ),
  pBuffer( mb->pBuffer ),
  nCursor( mb->nCursor ),
  bIOwnMemory( false )
{
}
*/

MemoryBuffer::MemoryBuffer( const unsigned int size ) :
  nSize( size ),
  pBuffer( 0 ),
  nCursor( 0 ),
  bIOwnMemory( true )
{
	pBuffer = new char[ nSize ];
	GlobalLog()->PrintNew( pBuffer, __FILE__, __LINE__, "buffer" );
}

MemoryBuffer::MemoryBuffer( char * pMemory, const unsigned int size, bool bTakeOwnership ) :
  nSize( size ),
  pBuffer( pMemory ),
  nCursor( 0 ),
  bIOwnMemory( bTakeOwnership )
{
}

MemoryBuffer::MemoryBuffer( const MemoryBuffer& mb ) :
  nSize( mb.nSize ),
  pBuffer( 0 ),
  nCursor( mb.nCursor ),
  bIOwnMemory( mb.bIOwnMemory )
{
	if( bIOwnMemory ) {
		pBuffer = new char[ nSize ];
		GlobalLog()->PrintNew( pBuffer, __FILE__, __LINE__, "buffer" );

		memcpy( pBuffer, mb.pBuffer, nSize );
	} else {
		pBuffer = mb.pBuffer;
	}

}

MemoryBuffer::MemoryBuffer( IMemoryBuffer& mb ) :
  nSize( mb.Size() ),
  pBuffer( 0 ),
  nCursor( mb.getCurPos() ),
  bIOwnMemory( mb.DoOwnMemory() )
{
	if( bIOwnMemory ) {
		pBuffer = new char[ nSize ];
		GlobalLog()->PrintNew( pBuffer, __FILE__, __LINE__, "buffer" );

		memcpy( pBuffer, mb.Pointer(), nSize );
	} else {
		pBuffer = mb.Pointer();
	}

}

MemoryBuffer::MemoryBuffer( const char * szFileName ) :
  nSize( 0 ),
  pBuffer( 0 ),
  nCursor( 0 ),
  bIOwnMemory( true )
{
	if( szFileName )
	{
		String s = GlobalMediaPathLocator().Find( szFileName );

		struct stat file_stats = {0};
		stat( s.c_str(), &file_stats );
		nSize = file_stats.st_size;
		pBuffer = new char[ nSize ];
		GlobalLog()->PrintNew( pBuffer, __FILE__, __LINE__, "buffer" );
		
		FILE* f = fopen( s.c_str(), "rb" );
		if( f ) {
			fread( pBuffer, nSize, 1, f );
			fclose( f );
			GlobalLog()->PrintEx( eLog_Info, "MemoryBuffer:: Read file \'%s\' of size %d bytes", szFileName, nSize );
		} else {
			GlobalLog()->PrintEx( eLog_Error, "MemoryBuffer:: Failed to open file: %s", szFileName );
		}
	}
}

MemoryBuffer::~MemoryBuffer( )
{
	if( bIOwnMemory && pBuffer ) {
		GlobalLog()->PrintDelete( pBuffer, __FILE__, __LINE__ );
		delete [] pBuffer;		
	}

	pBuffer = 0;
	nSize = 0;
	nCursor = 0;
}

void MemoryBuffer::Clear( )
{
	memset( pBuffer, 0, nSize );
}

bool MemoryBuffer::Resize( unsigned int new_size, bool bForce )
{
	// If we don't own the memory, don't resize!
	// If we are told to be smaller then don't do anything
	if( (!bIOwnMemory) || (new_size==nSize) || ((new_size<nSize)&&!bForce) ) {
		return false;
	}

	// Do the resizng
	char * pTempBuffer = new char[ new_size ];
	GlobalLog()->PrintNew( pTempBuffer, __FILE__, __LINE__, "temp buffer" );

	if( pBuffer ) {
		memcpy( pTempBuffer, pBuffer, r_min(new_size,nSize) );
		GlobalLog()->PrintDelete( pBuffer, __FILE__, __LINE__ );
		delete [] pBuffer;
	}
	pBuffer = pTempBuffer;
	nSize = new_size;

	if( nCursor >= nSize ) {
		nCursor = 0;
	}

	return true;
}

bool MemoryBuffer::ResizeForMore( unsigned int more_bytes )
{
	return Resize( more_bytes+nCursor );
}

unsigned int MemoryBuffer::HowFarToEnd( ) const
{
	return (nSize-nCursor-1);
}

unsigned int MemoryBuffer::Size( ) const
{
	return nSize;
}

char * MemoryBuffer::Pointer( )
{
	return pBuffer;
}

const char * MemoryBuffer::Pointer( ) const
{
	return pBuffer;
}

char * MemoryBuffer::PointerAtCursor( )
{
	return &pBuffer[nCursor];
}

const char * MemoryBuffer::PointerAtCursor( ) const
{
	return &pBuffer[nCursor];
}

bool MemoryBuffer::EndOfBuffer( ) const
{
	return (HowFarToEnd()==0);
}

bool MemoryBuffer::seek( const eSeek type, const int amount )
{
	switch( type )
	{
	case START:
		if( amount >= int(nSize) ) {
			return false;
		}
		nCursor = amount;	
		break;

	case CUR:
		{
			unsigned int newpos = nCursor + amount;
			if( newpos >= nSize ) {
				return false;
			}
			nCursor = newpos;
		}
		break;
	case END:
		if( amount >= int(nSize) ) {
			return false;
		}
		nCursor = nSize-amount-1;
		break;
	}

	return true;
}

bool MemoryBuffer::DumpToFile( const char * szFileName )
{
	if( szFileName && pBuffer ) {
		FILE* f = fopen( GlobalMediaPathLocator().Find(szFileName).c_str(), "wb" );
		if( f ) {
			fwrite( pBuffer, nSize, 1, f );
			fclose( f );
			return true;
		} else {
			GlobalLog()->PrintEasyError( "MemoryBuffer::DumpToFile:: Failed to open file for writing" );
		}
	}

	return false;
}

bool MemoryBuffer::DumpToFileToCursor( const char * szFileName )
{
	if( szFileName && pBuffer ) {
		FILE* f = fopen( GlobalMediaPathLocator().Find(szFileName).c_str(), "wb" );

		if( f ) {
			fwrite( pBuffer, nCursor, 1, f );
			fclose( f );
			return true;
		} else {
			GlobalLog()->PrintEasyError( "MemoryBuffer::DumpToFileToCursor:: Failed to open file for writing" );
		}
	} 

	return false;
}

//
// Getters
//

char MemoryBuffer::getChar()
{
#ifdef _DEBUG
	if( nCursor==nSize ) {
		GlobalLog()->PrintSourceError( "MemoryBuffer::getChar:: Attempted read past end of buffer", __FILE__, __LINE__ );
		return 0;
	}
#endif
	return (char)pBuffer[nCursor++];
}

unsigned char MemoryBuffer::getUChar()
{
#ifdef _DEBUG
	if( nCursor==nSize ) {
		GlobalLog()->PrintSourceError( "MemoryBuffer::getUChar:: Attempted read past end of buffer", __FILE__, __LINE__ );
		return 0;
	}
#endif

	return pBuffer[nCursor++];
}

short MemoryBuffer::getWord()
{
#ifdef _DEBUG
	if( nCursor+sizeof(short) > nSize ) {
		GlobalLog()->PrintSourceError( "MemoryBuffer::getWord:: Attempted read past end of buffer", __FILE__, __LINE__ );
		return 0;
	}
#endif

#ifdef RISE_BIG_ENDIAN
	short Low = getUChar();
    short High = getUChar();
	return Low | (High << 8);
#else
	short sh = 0;
	memcpy( &sh, &pBuffer[nCursor], sizeof( short ) );
	nCursor += sizeof( short );
	return sh;
#endif
}


unsigned short MemoryBuffer::getUWord()
{
#ifdef _DEBUG
	if( nCursor+sizeof(unsigned short) > nSize ) {
		GlobalLog()->PrintSourceError( "MemoryBuffer::getUWord:: Attempted read past end of buffer", __FILE__, __LINE__ );
		return 0;
	}
#endif

#ifdef RISE_BIG_ENDIAN
	unsigned short Low = getUChar();
    unsigned short High = getUChar();
	return Low | (High << 8);
#else
	unsigned short sh = 0;
	memcpy( &sh, &pBuffer[nCursor], sizeof( unsigned short ) );
	nCursor += sizeof( unsigned short );
	return sh;
#endif
}

int MemoryBuffer::getInt()
{
#ifdef _DEBUG
	if( nCursor+sizeof(int) > nSize ) {
		GlobalLog()->PrintSourceError( "MemoryBuffer::getInt:: Attempted read past end of buffer", __FILE__, __LINE__ );
		return 0;
	}
#endif

#ifdef RISE_BIG_ENDIAN
	int Low = getUWord();
    int High = getUWord();
    return Low | (High << 16);
#else
	int n = 0;
	memcpy( &n, &pBuffer[nCursor], sizeof( int ) );
	nCursor += sizeof( int );
	return n;
#endif
}

unsigned int MemoryBuffer::getUInt()
{
#ifdef _DEBUG
	if( nCursor+sizeof(unsigned int) > nSize ) {
		GlobalLog()->PrintSourceError( "MemoryBuffer::getUInt:: Attempted read past end of buffer", __FILE__, __LINE__ );
		return 0;
	}
#endif

#ifdef RISE_BIG_ENDIAN
	unsigned int Low = getUWord();
    unsigned int High = getUWord();
    return Low | (High << 16);
#else
	unsigned int n = 0;
	memcpy( &n, &pBuffer[nCursor], sizeof( unsigned int ) );
	nCursor += sizeof( unsigned int );
	return n;
#endif
}

float MemoryBuffer::getFloat()
{
#ifdef _DEBUG
	if( nCursor+sizeof(float) > nSize ) {
		GlobalLog()->PrintSourceError( "MemoryBuffer::getFloat:: Attempted read past end of buffer", __FILE__, __LINE__ );
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
	memcpy( &f, &pBuffer[nCursor], sizeof( float ) );
	nCursor += sizeof( float );
	return f;
#endif
}

double MemoryBuffer::getDouble()
{
#ifdef _DEBUG
	if( nCursor+sizeof(double) > nSize ) {
		GlobalLog()->PrintSourceError( "MemoryBuffer::getDouble:: Attempted read past end of buffer", __FILE__, __LINE__ );
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
	double f = 0;
	memcpy( &f, &pBuffer[nCursor], sizeof( double ) );
	nCursor += sizeof( double );
	return f;
#endif
}

bool MemoryBuffer::getBytes( void* pDest, unsigned int amount )
{
#ifdef _DEBUG
	if( nCursor+amount > nSize ) {
		GlobalLog()->PrintEx( eLog_Error, "MemoryBuffer::getBytes:: Attempted read past end of buffer, read %d bytes, cursor at %d bytes", amount, nCursor );
		return false;
	}
#endif
	if( !pDest ) {
		return false;
	}

	memcpy( pDest, &pBuffer[nCursor], amount );
	nCursor += amount;
	return true;
}

int MemoryBuffer::getLine( char* pDest, unsigned int max )
{
	if( !pDest ) {
		return false;
	}

	// We keep reading until the end if we have to
	unsigned int numRead = 0;
	while( nCursor < nSize && numRead < max ) {
		pDest[numRead] = getChar();

		if( pDest[numRead] == '\n' ) {
			return numRead+1;
		}
		numRead++;
	}

	return numRead;
}

//
// Setters
//

bool MemoryBuffer::setChar( const char ch )
{
#ifdef _DEBUG
	if( nCursor==nSize ) {
		GlobalLog()->PrintSourceError( "MemoryBuffer::setChar:: Attempted write past end of buffer", __FILE__, __LINE__ );
		return false;
	}
#endif
	pBuffer[nCursor++] = ch;
	return true;
}

bool MemoryBuffer::setUChar( const unsigned char ch )
{
#ifdef _DEBUG
	if( nCursor==nSize ) {
		GlobalLog()->PrintSourceError( "MemoryBuffer::setUChar:: Attempted write past end of buffer", __FILE__, __LINE__ );
		return false;
	}
#endif
	pBuffer[nCursor++] = ch;
	return true;
}

bool MemoryBuffer::setWord( const short sh )
{
#ifdef _DEBUG
	if( nCursor+sizeof(short)>nSize ) {
		GlobalLog()->PrintSourceError( "MemoryBuffer::setWord:: Attempted write past end of buffer", __FILE__, __LINE__ );
		return false;
	}
#endif

#ifdef RISE_BIG_ENDIAN
	unsigned char low = sh & 0xFF;
	unsigned char high = (sh >> 8) & 0xFF;
	setUChar( low );
	setUChar( high );
#else
	memcpy( &pBuffer[nCursor], &sh, sizeof( short ) );
	nCursor += sizeof( short );
#endif
	return true;
}

bool MemoryBuffer::setUWord( const unsigned short sh )
{
#ifdef _DEBUG
	if( nCursor+sizeof(unsigned short)>nSize ) {
		GlobalLog()->PrintSourceError( "MemoryBuffer::setUWord:: Attempted write past end of buffer", __FILE__, __LINE__ );
		return false;
	}
#endif
#ifdef RISE_BIG_ENDIAN
	unsigned char low = sh & 0xFF;
	unsigned char high = (sh >> 8) & 0xFF;
	setUChar( low );
	setUChar( high );
#else
	memcpy( &pBuffer[nCursor], &sh, sizeof( unsigned short ) );
	nCursor += sizeof( unsigned short );
#endif
	return true;
}

bool MemoryBuffer::setInt( const int n )
{
#ifdef _DEBUG
	if( nCursor+sizeof(int)>nSize ) {
		GlobalLog()->PrintSourceError( "MemoryBuffer::setInt:: Attempted write past end of buffer", __FILE__, __LINE__ );
		return false;
	}
#endif

#ifdef RISE_BIG_ENDIAN
	unsigned short low = n & 0xFFFF;
	unsigned short high = (n >> 16) & 0xFFF;
	setUWord( low );
	setUWord( high );
#else
	memcpy( &pBuffer[nCursor], &n, sizeof( int ) );
	nCursor += sizeof( int );
#endif
	return true;
}

bool MemoryBuffer::setUInt( const unsigned int n )
{
#ifdef _DEBUG
	if( nCursor+sizeof(unsigned int)>nSize ) {
		GlobalLog()->PrintSourceError( "MemoryBuffer::setUInt:: Attempted write past end of buffer", __FILE__, __LINE__ );
		return false;
	}
#endif

#ifdef RISE_BIG_ENDIAN
	unsigned short low = n & 0xFFFF;
	unsigned short high = (n >> 16) & 0xFFF;
	setUWord( low );
	setUWord( high );
#else
	memcpy( &pBuffer[nCursor], &n, sizeof( unsigned int ) );
	nCursor += sizeof( unsigned int );
#endif
	return true;
}

bool MemoryBuffer::setFloat( const float f )
{
#ifdef _DEBUG
	if( nCursor+sizeof(float)>nSize ) {
		GlobalLog()->PrintSourceError( "MemoryBuffer::setFloat:: Attempted write past end of buffer", __FILE__, __LINE__ );
		return false;
	}
#endif

#ifdef RISE_BIG_ENDIAN
	unsigned int n;
	memcpy( &n, &f, sizeof( float ) );
	setUInt( n );
#else
	memcpy( &pBuffer[nCursor], &f, sizeof( float ) );
	nCursor += sizeof( float );
#endif
	return true;
}

bool MemoryBuffer::setDouble( const double d )
{
#ifdef _DEBUG
	if( nCursor+sizeof(double)>nSize ) {
		GlobalLog()->PrintSourceError( "MemoryBuffer::setDouble:: Attempted write past end of buffer", __FILE__, __LINE__ );
		return false;
	}
#endif

#ifdef RISE_BIG_ENDIAN
	unsigned int first;
	unsigned int last;
	
	char* ptrd = (char*)&d;
	memcpy( &first, ptrd, 4 );
	memcpy( &last, &ptrd[4], 4 );
	setUInt( last );
	setUInt( first );
#else
	memcpy( &pBuffer[nCursor], &d, sizeof( double ) );
	nCursor += sizeof( double );
#endif
	return true;
}

bool MemoryBuffer::setBytes( const void* pSource, unsigned int amount )
{
	if( !pSource ) {
		GlobalLog()->PrintSourceError( "MemoryBuffer::setBytes:: source is NULL", __FILE__, __LINE__ );
		return false;
	}

	if( nCursor+amount > nSize ) {
		GlobalLog()->PrintEx( eLog_Info, "MemoryBuffer::setBytes:: resizing buffer, old size (%d), new size (%d)", nSize, nCursor+amount );
		Resize( nCursor+amount );
	}

	memcpy( &pBuffer[nCursor], pSource, amount );
	nCursor += amount;
	return true;
}

