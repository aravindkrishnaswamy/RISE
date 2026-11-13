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
#include "LittleEndian.h"
#include "../Interfaces/ILog.h"
#include <string.h>
#include <limits>
#include <algorithm>
#include <cstdint>
#ifdef _WIN32
#include <io.h>
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include "MediaPathLocator.h"

using namespace RISE::Implementation;

DiskFileReadBuffer::DiskFileReadBuffer( const char * file_name ) : nSize( 0 )
{
	if( !file_name ) return;
	filename = GlobalMediaPathLocator().Find( file_name ).c_str();

	hFile.reset( fopen( filename.c_str(), "rb" ) );
	if( !hFile ) {
		GlobalLog()->PrintEx( eLog_Warning, "DiskFileReadBuffer:: Failed to open file '%s'", file_name );
		return;
	}
#ifdef _WIN32
	struct _stat64 fileStats{};
	const bool regular = _fstat64( _fileno(hFile.get()), &fileStats ) == 0 &&
		(fileStats.st_mode & _S_IFMT) == _S_IFREG;
#else
	struct stat fileStats{};
	const bool regular = fstat( fileno(hFile.get()), &fileStats ) == 0 && S_ISREG( fileStats.st_mode );
#endif
	if( !regular || fileStats.st_size < 0 ) {
		hFile.reset();
		GlobalLog()->PrintEx( eLog_Warning, "DiskFileReadBuffer:: Unsupported file or size '%s'", file_name );
		return;
	}
	nSize = static_cast<std::uint64_t>(fileStats.st_size);
	GlobalLog()->PrintEx( eLog_Info, "DiskFileReadBuffer:: Opened file '%s' of size %llu bytes for reading", file_name, static_cast<unsigned long long>(nSize) );
}

DiskFileReadBuffer::~DiskFileReadBuffer() = default;

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
	if( getCurPos64() >= nSize ) {
	#ifdef _DEBUG
		GlobalLog()->PrintSourceError( "DiskFileReadBuffer::getChar:: Attempted read past end of buffer", __FILE__, __LINE__ );
	#endif
		seek( IBuffer::END, 0 );   // consume to end so loop-until-cursor callers terminate
		return 0;
	}
	char c=0;
	fread( &c, 1, 1, hFile.get() );
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
	if( getCurPos64() >= nSize ) {
	#ifdef _DEBUG
		GlobalLog()->PrintSourceError( "DiskFileReadBuffer::getUChar:: Attempted read past end of buffer", __FILE__, __LINE__ );
	#endif
		seek( IBuffer::END, 0 );   // consume to end so loop-until-cursor callers terminate
		return 0;
	}

	unsigned char c=0;
	fread( &c, 1, 1, hFile.get() );
	return c;
}

short DiskFileReadBuffer::getWord()
{
	unsigned char bytes[sizeof(std::uint16_t)];
	if( !hFile ) return 0;
	const auto cursor = getCurPos64();
	if( cursor > nSize || sizeof(bytes) > nSize - cursor ) {
		seek( IBuffer::END, 0 );
		return 0;
	}
	if( fread( bytes, 1, sizeof(bytes), hFile.get() ) != sizeof(bytes) ) return 0;
	return DecodeLittleEndian<short, std::uint16_t>( bytes );
}


unsigned short DiskFileReadBuffer::getUWord()
{
	unsigned char bytes[sizeof(std::uint16_t)];
	if( !hFile ) return 0;
	const auto cursor = getCurPos64();
	if( cursor > nSize || sizeof(bytes) > nSize - cursor ) {
		seek( IBuffer::END, 0 );
		return 0;
	}
	if( fread( bytes, 1, sizeof(bytes), hFile.get() ) != sizeof(bytes) ) return 0;
	return DecodeLittleEndian<unsigned short, std::uint16_t>( bytes );
}

int DiskFileReadBuffer::getInt()
{
	unsigned char bytes[sizeof(std::uint32_t)];
	if( !hFile ) return 0;
	const auto cursor = getCurPos64();
	if( cursor > nSize || sizeof(bytes) > nSize - cursor ) {
		seek( IBuffer::END, 0 );
		return 0;
	}
	if( fread( bytes, 1, sizeof(bytes), hFile.get() ) != sizeof(bytes) ) return 0;
	return DecodeLittleEndian<int, std::uint32_t>( bytes );
}

unsigned int DiskFileReadBuffer::getUInt()
{
	unsigned char bytes[sizeof(std::uint32_t)];
	if( !hFile ) return 0;
	const auto cursor = getCurPos64();
	if( cursor > nSize || sizeof(bytes) > nSize - cursor ) {
		seek( IBuffer::END, 0 );
		return 0;
	}
	if( fread( bytes, 1, sizeof(bytes), hFile.get() ) != sizeof(bytes) ) return 0;
	return DecodeLittleEndian<unsigned int, std::uint32_t>( bytes );
}

float DiskFileReadBuffer::getFloat()
{
	unsigned char bytes[sizeof(std::uint32_t)];
	if( !hFile ) return 0;
	const auto cursor = getCurPos64();
	if( cursor > nSize || sizeof(bytes) > nSize - cursor ) {
		seek( IBuffer::END, 0 );
		return 0;
	}
	if( fread( bytes, 1, sizeof(bytes), hFile.get() ) != sizeof(bytes) ) return 0;
	return DecodeLittleEndian<float, std::uint32_t>( bytes );
}

double DiskFileReadBuffer::getDouble()
{
	unsigned char bytes[sizeof(std::uint64_t)];
	if( !hFile ) return 0;
	const auto cursor = getCurPos64();
	if( cursor > nSize || sizeof(bytes) > nSize - cursor ) {
		seek( IBuffer::END, 0 );
		return 0;
	}
	if( fread( bytes, 1, sizeof(bytes), hFile.get() ) != sizeof(bytes) ) return 0;
	return DecodeLittleEndian<double, std::uint64_t>( bytes );
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
	const auto cur = getCurPos64();
	const auto available = cur <= nSize ? nSize - cur : 0;
	const auto toRead = static_cast<unsigned int>(std::min<std::uint64_t>(amount, available));

	size_t got = 0;
	if( toRead ) {
		got = fread( pDest, 1, toRead, hFile.get() );
	}
	if( got < amount ) {
		// Short read (truncation / EOF): zero the tail and signal failure.
		memset( static_cast<char*>(pDest) + got, 0, amount - got );
		GlobalLog()->PrintEx( eLog_Error, "DiskFileReadBuffer::getBytes:: short read (want %u at cursor %llu of %llu, got %u); rest zeroed", amount,
			static_cast<unsigned long long>(cur), static_cast<unsigned long long>(nSize), static_cast<unsigned>(got) );
		return false;
	}
	return true;
}

int DiskFileReadBuffer::getLine( char* pDest, unsigned int max )
{
	if( !pDest || !hFile ) {
		return false;
	}

	unsigned int numRead = 0;
	auto cursor = getCurPos64();
	while( cursor < nSize && numRead < max ) {
		const int byte = fgetc( hFile.get() );
		if( byte == EOF ) break;
		pDest[numRead++] = static_cast<char>(byte);
		++cursor;
		if( byte == '\n' ) break;
	}

	return numRead;
}
