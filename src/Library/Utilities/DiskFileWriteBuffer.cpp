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
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <limits>
#include <memory>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif
#include <string.h>
#include "DiskFileWriteBuffer.h"
#include "LittleEndian.h"
#include <cstdint>
#include <exception>
#include "../Interfaces/ILog.h"
#include "MediaPathLocator.h"

using namespace RISE::Implementation;

DiskFileWriteBuffer::DiskFileWriteBuffer( const char * file_name ) 
{
	if( !file_name ) return;
	filename = file_name;
	hFile.reset( fopen( filename.c_str(), "wb" ) );

	if( !hFile ) {
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
	unsigned char bytes[sizeof(std::uint16_t)];
	EncodeLittleEndian<std::uint16_t>( bytes, sh );
	return setBytes( bytes, sizeof(bytes) );
}

bool DiskFileWriteBuffer::setUWord( const unsigned short sh )
{
	unsigned char bytes[sizeof(std::uint16_t)];
	EncodeLittleEndian<std::uint16_t>( bytes, sh );
	return setBytes( bytes, sizeof(bytes) );
}

bool DiskFileWriteBuffer::setInt( const int n )
{
	unsigned char bytes[sizeof(std::uint32_t)];
	EncodeLittleEndian<std::uint32_t>( bytes, n );
	return setBytes( bytes, sizeof(bytes) );
}

bool DiskFileWriteBuffer::setUInt( const unsigned int n )
{
	unsigned char bytes[sizeof(std::uint32_t)];
	EncodeLittleEndian<std::uint32_t>( bytes, n );
	return setBytes( bytes, sizeof(bytes) );
}

bool DiskFileWriteBuffer::setFloat( const float f )
{
	unsigned char bytes[sizeof(std::uint32_t)];
	EncodeLittleEndian<std::uint32_t>( bytes, f );
	return setBytes( bytes, sizeof(bytes) );
}

bool DiskFileWriteBuffer::setDouble( const double d )
{
	unsigned char bytes[sizeof(std::uint64_t)];
	EncodeLittleEndian<std::uint64_t>( bytes, d );
	return setBytes( bytes, sizeof(bytes) );
}

bool DiskFileWriteBuffer::setBytes( const void* pSource, unsigned int amount )
{
	if( !pSource || !hFile || ferror( hFile.get() ) ) {
		writeFailed = true;
		return false;
	}
	if( amount > std::numeric_limits<std::uint64_t>::max() - cursor ) {
		writeFailed = true;
		return false;
	}
	const size_t written = fwrite( pSource, 1, amount, hFile.get() );
	cursor += written;
	logicalSize = std::max( logicalSize, cursor );
	hasWritten |= written != 0;
	writeFailed |= written != amount;
	return written == amount;
}

unsigned int DiskFileWriteBuffer::Size() const
{
	return static_cast<unsigned int>(std::min<std::uint64_t>(logicalSize, std::numeric_limits<unsigned int>::max()));
}

unsigned int DiskFileWriteBuffer::getCurPos() const
{
	return static_cast<unsigned int>(std::min<std::uint64_t>(cursor, std::numeric_limits<unsigned int>::max()));
}

bool DiskFileWriteBuffer::seek( eSeek type, int amount )
{
	return seek64( type, amount );
}

bool DiskFileWriteBuffer::seek64( eSeek type, std::int64_t amount )
{
	if( !DiskBuffer::seek64( type, amount ) ) {
		writeFailed = true;
		return false;
	}
	const auto position = DiskBuffer::getCurPos64();
	if( position == std::numeric_limits<std::uint64_t>::max() ) {
		writeFailed = true;
		return false;
	}
	cursor = static_cast<std::uint64_t>(position);
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
	return static_cast<bool>(hFile);
}

bool DiskFileWriteBuffer::Finish( bool requireOutput )
{
	if( !hFile ) {
		return false;
	}
	const bool written = !writeFailed && !ferror( hFile.get() ) && (!requireOutput || hasWritten);
	const bool closed = fclose( hFile.release() ) == 0;
	if( !written || !closed ) {
		GlobalLog()->PrintEx( eLog_Error, "DiskFileWriteBuffer:: Failed to write `%s`", filename.c_str() );
		return false;
	}
	return true;
}

bool DiskFileWriteBuffer::WriteAtomically( const std::string& path,
	const std::function<bool(DiskFileWriteBuffer&)>& write )
{
	return WriteAtomically( path, write, StagingPolicy::Required );
}

bool DiskFileWriteBuffer::WriteAtomically( const std::string& path,
	const std::function<bool(DiskFileWriteBuffer&)>& write, StagingPolicy policy )
{
	namespace fs = std::filesystem;
	const auto writeFile = [&]( const fs::path& file ) {
		const auto release = []( DiskFileWriteBuffer* buffer ) { buffer->release(); };
		std::unique_ptr<DiskFileWriteBuffer, decltype(release)> buffer(
			new DiskFileWriteBuffer( file.string().c_str() ), release );
		GlobalLog()->PrintNew( buffer.get(), __FILE__, __LINE__, "disk file write buffer" );
		if( !buffer->ReadyToWrite() ) return false;
		bool encoded = false;
		try {
			encoded = write( *buffer );
		} catch( const std::exception& failure ) {
			GlobalLog()->PrintEx( eLog_Error, "DiskFileWriteBuffer:: Encoding `%s` failed: %s", file.string().c_str(), failure.what() );
		}
		return buffer->Finish( true ) && encoded;
	};
	static std::atomic<unsigned long long> nextId{ 0 };
#ifdef _WIN32
	const auto pid = ::_getpid();
#else
	const auto pid = ::getpid();
#endif
	fs::path target( path );
	std::error_code error;
	const auto link = fs::symlink_status( target, error );
	if( error && error != std::errc::no_such_file_or_directory ) return false;
	if( fs::is_symlink( link ) ) {
		target = fs::canonical( target, error );
		if( error ) {
			return error == std::errc::no_such_file_or_directory &&
				policy == StagingPolicy::AllowInPlace && writeFile( fs::path(path) );
		}
	}
	const auto previous = fs::status( target, error );
	if( error && error != std::errc::no_such_file_or_directory ) return false;
	const bool replacing = fs::exists( previous );
	if( replacing ) {
		if( !fs::is_regular_file( previous ) ) return false;
		FILE* probe = fopen( target.string().c_str(), "r+b" );
		if( !probe ) return false;
		fclose( probe );
	}
	error.clear();
	fs::path directory;
	for( unsigned int attempt = 0; attempt < 16; ++attempt ) {
		directory = target.parent_path() / (".rise-write-" + std::to_string( pid ) + "-" +
			std::to_string( nextId.fetch_add( 1, std::memory_order_relaxed ) ));
		if( fs::create_directory( directory, error ) ) break;
		directory.clear();
		if( error && error != std::errc::file_exists ) break;
	}
	if( directory.empty() ) return policy == StagingPolicy::AllowInPlace && writeFile( target );

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
	if( !writeFile( staging ) ) return false;
	if( replacing ) {
		fs::permissions( staging, previous.permissions(), error );
		if( error ) return false;
	}
	fs::rename( staging, target, error );
	return !error;
}
