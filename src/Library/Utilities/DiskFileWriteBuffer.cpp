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

DiskFileWriteBuffer::DiskFileWriteBuffer( const char * file_name ) :
	writeFailed_( false )
{
	strncpy( szFileName, file_name, 1024 );
	szFileName[1023] = '\0';
	hFile = fopen( szFileName, "wb" );

	if( hFile == 0 ) {
		GlobalLog()->PrintEx( eLog_Warning, "DiskFileWriteBuffer:: Failed to open file \'%s\'", file_name );
	}
}

DiskFileWriteBuffer::~DiskFileWriteBuffer( )
{
	Close();
}

bool DiskFileWriteBuffer::setChar( const char ch )
{
	return setBytes(&ch,1);
}

bool DiskFileWriteBuffer::setUChar( const unsigned char ch )
{
	return setBytes(&ch,1);
}

bool DiskFileWriteBuffer::setWord( const short sh )
{
#ifdef RISE_BIG_ENDIAN
	unsigned char low = sh & 0xFF;
	unsigned char high = (sh >> 8) & 0xFF;
	return setUChar( low ) && setUChar( high );
#else
	return setBytes(&sh,sizeof(sh));
#endif
}

bool DiskFileWriteBuffer::setUWord( const unsigned short sh )
{
#ifdef RISE_BIG_ENDIAN
	unsigned char low = sh & 0xFF;
	unsigned char high = (sh >> 8) & 0xFF;
	return setUChar( low ) && setUChar( high );
#else
	return setBytes(&sh,sizeof(sh));
#endif
}

bool DiskFileWriteBuffer::setInt( const int n )
{
#ifdef RISE_BIG_ENDIAN
	unsigned short low = n & 0xFFFF;
	unsigned short high = (n >> 16) & 0xFFF;
	return setUWord( low ) && setUWord( high );
#else
	return setBytes(&n,sizeof(n));
#endif
}

bool DiskFileWriteBuffer::setUInt( const unsigned int n )
{
#ifdef RISE_BIG_ENDIAN
	unsigned short low = n & 0xFFFF;
	unsigned short high = (n >> 16) & 0xFFF;
	return setUWord( low ) && setUWord( high );
#else
	return setBytes(&n,sizeof(n));
#endif
}

bool DiskFileWriteBuffer::setFloat( const float f )
{
#ifdef RISE_BIG_ENDIAN
	unsigned int n;
	memcpy( &n, &f, sizeof( float ) );
	return setUInt( n );
#else
	return setBytes(&f,sizeof(f));
#endif
}

bool DiskFileWriteBuffer::setDouble( const double d )
{
#ifdef RISE_BIG_ENDIAN
	unsigned int first;
	unsigned int last;
	
	char* ptrd = (char*)&d;
	memcpy( &first, ptrd, 4 );
	memcpy( &last, &ptrd[4], 4 );
	return setUInt( last ) && setUInt( first );
#else
	return setBytes(&d,sizeof(d));
#endif
}

bool DiskFileWriteBuffer::setBytes( const void* pSource, unsigned int amount )
{
	if( !hFile || writeFailed_ || (!pSource && amount) ) {
//		GlobalLog()->PrintSourceError( "DiskFileWriteBuffer::setBytes:: source is NULL", __FILE__, __LINE__ );
		return false;
	}
	if( !amount ) return true;
	if( fwrite(pSource,1,amount,hFile) != amount ) {
		writeFailed_ = true;
		return false;
	}
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
	return hFile != 0 && !writeFailed_;
}

bool DiskFileWriteBuffer::Close()
{
	bool success = !writeFailed_;
	if( hFile ) {
		if( fflush(hFile) != 0 ) success = false;
		if( fclose(hFile) != 0 ) success = false;
		hFile = 0;
	}
	writeFailed_ = !success;
	return success;
}
