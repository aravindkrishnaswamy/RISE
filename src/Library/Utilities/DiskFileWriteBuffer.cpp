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
#include <atomic>
#include <filesystem>
#include <memory>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif
#include <string.h>
#include "DiskFileWriteBuffer.h"
#include "../Interfaces/ILog.h"
#include "MediaPathLocator.h"

using namespace RISE::Implementation;

DiskFileWriteBuffer::DiskFileWriteBuffer( const char * file_name ) 
{
	if( !file_name ) return;
	filename = file_name;
	hFile = fopen( filename.c_str(), "wb" );

	if( hFile == 0 ) {
		GlobalLog()->PrintEx( eLog_Warning, "DiskFileWriteBuffer:: Failed to open file \'%s\'", file_name );
	}
}

DiskFileWriteBuffer::~DiskFileWriteBuffer( )
{
	if( hFile ) {
		Finish();
	}
}

bool DiskFileWriteBuffer::setChar( const char ch )
{
	return setBytes( &ch, sizeof(ch) );
}

bool DiskFileWriteBuffer::setUChar( const unsigned char ch )
{
	return setBytes( &ch, sizeof(ch) );
}

bool DiskFileWriteBuffer::setWord( const short sh )
{
#ifdef RISE_BIG_ENDIAN
	unsigned char low = sh & 0xFF;
	unsigned char high = (sh >> 8) & 0xFF;
	return setUChar( low ) && setUChar( high );
#else
	return setBytes( &sh, sizeof(sh) );
#endif
}

bool DiskFileWriteBuffer::setUWord( const unsigned short sh )
{
#ifdef RISE_BIG_ENDIAN
	unsigned char low = sh & 0xFF;
	unsigned char high = (sh >> 8) & 0xFF;
	return setUChar( low ) && setUChar( high );
#else
	return setBytes( &sh, sizeof(sh) );
#endif
}

bool DiskFileWriteBuffer::setInt( const int n )
{
#ifdef RISE_BIG_ENDIAN
	unsigned short low = n & 0xFFFF;
	unsigned short high = (n >> 16) & 0xFFF;
	return setUWord( low ) && setUWord( high );
#else
	return setBytes( &n, sizeof(n) );
#endif
}

bool DiskFileWriteBuffer::setUInt( const unsigned int n )
{
#ifdef RISE_BIG_ENDIAN
	unsigned short low = n & 0xFFFF;
	unsigned short high = (n >> 16) & 0xFFF;
	return setUWord( low ) && setUWord( high );
#else
	return setBytes( &n, sizeof(n) );
#endif
}

bool DiskFileWriteBuffer::setFloat( const float f )
{
#ifdef RISE_BIG_ENDIAN
	unsigned int n;
	memcpy( &n, &f, sizeof( float ) );
	return setUInt( n );
#else
	return setBytes( &f, sizeof(f) );
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
	return setBytes( &d, sizeof(d) );
#endif
}

bool DiskFileWriteBuffer::setBytes( const void* pSource, unsigned int amount )
{
	if( !pSource || !hFile || ferror( hFile ) ) {
		writeFailed = true;
		return false;
	}
	const size_t written = fwrite( pSource, 1, amount, hFile );
	hasWritten |= written != 0;
	writeFailed |= written != amount;
	return written == amount;
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

bool DiskFileWriteBuffer::Finish( bool requireOutput )
{
	if( !hFile ) {
		return false;
	}
	const bool written = !writeFailed && !ferror( hFile ) && (!requireOutput || hasWritten);
	const bool closed = fclose( hFile ) == 0;
	hFile = nullptr;
	if( !written || !closed ) {
		GlobalLog()->PrintEx( eLog_Error, "DiskFileWriteBuffer:: Failed to write `%s`", filename.c_str() );
		return false;
	}
	return true;
}

bool DiskFileWriteBuffer::WriteAtomically( const std::string& path,
	const std::function<bool(DiskFileWriteBuffer&)>& write )
{
	namespace fs = std::filesystem;
	static std::atomic<unsigned long long> nextId{ 0 };
#ifdef _WIN32
	const auto pid = ::_getpid();
#else
	const auto pid = ::getpid();
#endif
	const fs::path target( path );
	std::error_code error;
	fs::path directory;
	for( unsigned int attempt = 0; attempt < 16; ++attempt ) {
		directory = target.parent_path() / (".rise-write-" + std::to_string( pid ) + "-" +
			std::to_string( nextId.fetch_add( 1, std::memory_order_relaxed ) ));
		if( fs::create_directory( directory, error ) ) break;
		directory.clear();
		if( error && error != std::errc::file_exists ) break;
	}
	if( directory.empty() ) return false;

	const fs::path staging = directory / "output";
	struct Cleanup {
		const fs::path& file;
		const fs::path& directory;
		~Cleanup() {
			std::error_code ignored;
			fs::remove( file, ignored );
			fs::remove( directory, ignored );
		}
	} cleanup{ staging, directory };
	const auto release = []( DiskFileWriteBuffer* buffer ) { buffer->release(); };
	std::unique_ptr<DiskFileWriteBuffer, decltype(release)> buffer(
		new DiskFileWriteBuffer( staging.string().c_str() ), release );
	if( !buffer->ReadyToWrite() ) return false;
	const bool encoded = write( *buffer );
	if( !buffer->Finish( true ) || !encoded ) return false;
	fs::rename( staging, target, error );
	return !error;
}
