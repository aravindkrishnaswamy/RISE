//////////////////////////////////////////////////////////////////////
//
//  DiskBuffer.cpp - Implements the DiskBuffer helper class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 3, 2003
//  Tabs: 4
//  Comments: 64-bit file-backed IBuffer base. Legacy 32-bit IBuffer
//    accessors (Size, getCurPos, HowFarToEnd) saturate at UINT_MAX for
//    files > 4 GiB; use Size64/getCurPos64/seek64 for large files.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "DiskBuffer.h"
#include <cstdlib>
#include <limits>
#include <algorithm>
#ifdef _WIN32
#include <io.h>
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include "../Interfaces/ILog.h"

using namespace RISE::Implementation;

DiskBuffer::DiskBuffer() = default;

DiskBuffer::~DiskBuffer( )
{
}

unsigned int DiskBuffer::HowFarToEnd( ) const
{
	return static_cast<unsigned int>(std::min<std::uint64_t>(BufferBytesAvailable(*this), std::numeric_limits<unsigned int>::max()));
}

unsigned int DiskBuffer::Size( ) const
{
	return static_cast<unsigned int>(std::min<std::uint64_t>(Size64(), std::numeric_limits<unsigned int>::max()));
}

std::uint64_t DiskBuffer::Size64() const
{
	if( !hFile ) return 0;
#ifdef _WIN32
#ifdef _MSC_VER
	struct __stat64 stats{};
#else
	struct _stat64 stats{};
#endif
	if( _fstat64(_fileno(hFile.get()), &stats) != 0 || stats.st_size < 0 ) return 0;
#else
	struct stat stats{};
	if( fstat(fileno(hFile.get()), &stats) != 0 || stats.st_size < 0 ) return 0;
#endif
	return static_cast<std::uint64_t>(stats.st_size);
}

unsigned int DiskBuffer::getCurPos( ) const
{
	return static_cast<unsigned int>(std::min<std::uint64_t>(getCurPos64(), std::numeric_limits<unsigned int>::max()));
}

// Returns current byte offset, 0 if no file is open, or UINT64_MAX on tell failure.
std::uint64_t DiskBuffer::getCurPos64() const
{
	if( !hFile ) return 0;
#ifdef _WIN32
	const auto position = _ftelli64( hFile.get() );
#else
	const auto position = ftello( hFile.get() );
#endif
	return position < 0 ? std::numeric_limits<std::uint64_t>::max() : static_cast<std::uint64_t>(position);
}

bool DiskBuffer::EndOfBuffer( ) const
{
	return (HowFarToEnd()==0);
}

bool DiskBuffer::seek( const eSeek type, const int amount )
{
	return seek64( type, amount );
}

bool DiskBuffer::seek64( const eSeek type, std::int64_t amount )
{
	// No file handle (open failed): refuse rather than fseek(NULL) UB.
	// Reachable via seek-first image readers (e.g. TIFF) over an
	// existing-but-unopenable file, where Size()>0 passes their gate.
	if( !hFile ) {
		return false;
	}

	int origin;
	switch( type ) {
	case START: origin = SEEK_SET; break;
	case CUR: origin = SEEK_CUR; break;
	case END: origin = SEEK_END; break;
	default: return false;
	}
#ifdef _WIN32
	return _fseeki64( hFile.get(), amount, origin ) == 0;
#else
	if( amount < std::numeric_limits<off_t>::min() || amount > std::numeric_limits<off_t>::max() ) return false;
	return fseeko( hFile.get(), static_cast<off_t>(amount), origin ) == 0;
#endif
}
