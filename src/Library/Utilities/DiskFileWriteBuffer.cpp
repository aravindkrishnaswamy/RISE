//////////////////////////////////////////////////////////////////////
//
//  DiskFileWriteBuffer.cpp - Implementation of the disk file write
//    buffer
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
#include <string.h>
#include "DiskFileWriteBuffer.h"
#include "../Interfaces/ILog.h"
#include "MediaPathLocator.h"

using namespace RISE::Implementation;

DiskFileWriteBuffer::DiskFileWriteBuffer( const char * file_name ) 
{
	strncpy( szFileName, file_name, 1024 );
	hFile = fopen( szFileName, "wb" );

	if( hFile == 0 ) {
		GlobalLog()->PrintEx( eLog_Warning, "DiskFileWriteBuffer:: Failed to open file \'%s\'", file_name );
	}
}

DiskFileWriteBuffer::~DiskFileWriteBuffer( )
{
	if( hFile ) {
		fclose( hFile );
		hFile = 0;
	}
}

bool DiskFileWriteBuffer::setChar( const char ch )
{
	fwrite( &ch, 1, 1, hFile );
	return true;
}

bool DiskFileWriteBuffer::setUChar( const unsigned char ch )
{
	fwrite( &ch, 1, 1, hFile );
	return true;
}

bool DiskFileWriteBuffer::setWord( const short sh )
{
#ifdef RISE_BIG_ENDIAN
	unsigned char low = sh & 0xFF;
	unsigned char high = (sh >> 8) & 0xFF;
	setUChar( low );
	setUChar( high );
#else
	fwrite( &sh, sizeof( short ), 1, hFile );
#endif
	return true;
}

bool DiskFileWriteBuffer::setUWord( const unsigned short sh )
{
#ifdef RISE_BIG_ENDIAN
	unsigned char low = sh & 0xFF;
	unsigned char high = (sh >> 8) & 0xFF;
	setUChar( low );
	setUChar( high );
#else
	fwrite( &sh, sizeof( unsigned short ), 1, hFile );
#endif
	return true;
}

bool DiskFileWriteBuffer::setInt( const int n )
{
#ifdef RISE_BIG_ENDIAN
	unsigned short low = n & 0xFFFF;
	unsigned short high = (n >> 16) & 0xFFF;
	setUWord( low );
	setUWord( high );
#else
	fwrite( &n, sizeof( int ), 1, hFile );
#endif
	return true;
}

bool DiskFileWriteBuffer::setUInt( const unsigned int n )
{
#ifdef RISE_BIG_ENDIAN
	unsigned short low = n & 0xFFFF;
	unsigned short high = (n >> 16) & 0xFFF;
	setUWord( low );
	setUWord( high );
#else
	fwrite( &n, sizeof( unsigned int ), 1, hFile );
#endif
	return true;
}

bool DiskFileWriteBuffer::setFloat( const float f )
{
#ifdef RISE_BIG_ENDIAN
	unsigned int n;
	memcpy( &n, &f, sizeof( float ) );
	setUInt( n );
#else
	fwrite( &f, sizeof( float ), 1, hFile );
#endif
	return true;
}

bool DiskFileWriteBuffer::setDouble( const double d )
{
#ifdef RISE_BIG_ENDIAN
	unsigned int first;
	unsigned int last;
	
	char* ptrd = (char*)&d;
	memcpy( &first, ptrd, 4 );
	memcpy( &last, &ptrd[4], 4 );
	setUInt( last );
	setUInt( first );
#else
	fwrite( &d, sizeof( double ), 1, hFile );
#endif
	return true;
}

bool DiskFileWriteBuffer::setBytes( const void* pSource, unsigned int amount )
{
	if( !pSource || !hFile ) {
//		GlobalLog()->PrintSourceError( "DiskFileWriteBuffer::setBytes:: source is NULL", __FILE__, __LINE__ );
		return false;
	}

	fwrite( pSource, amount, 1, hFile );
	return true;
}

void DiskFileWriteBuffer::Clear( )
{
	// Does nothing
	GlobalLog()->PrintEasyWarning( "DiskFileWriteBuffer::Clear called, does nothing, you sure about this?" );
}


bool DiskFileWriteBuffer::Resize( unsigned int , bool )
{
	// We never resize
	return false;
}

bool DiskFileWriteBuffer::ResizeForMore( unsigned int )
{
	return false;
}

bool DiskFileWriteBuffer::ReadyToWrite() const
{
	return (hFile!=0);
}
