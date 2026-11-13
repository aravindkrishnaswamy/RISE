//////////////////////////////////////////////////////////////////////
//
//  DiskBuffer.cpp - Implements the DiskBuffer helper class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 3, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "DiskBuffer.h"
#include <cstdlib>
#include <limits>
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
	const unsigned int size = Size();
	const unsigned int cursor = getCurPos();
	return cursor < size ? size - cursor : 0;
}

unsigned int DiskBuffer::Size( ) const
{	
	struct stat file_stats = {0};
	stat( filename.c_str(), &file_stats );
	return static_cast<unsigned int>(file_stats.st_size);
}

unsigned int DiskBuffer::getCurPos( ) const
{
	// No file handle (open failed): treat as position 0 rather than
	// ftell(NULL) UB/SIGSEGV.  This also makes HowFarToEnd()/EndOfBuffer()
	// (which call getCurPos) null-safe.
	if( !hFile ) {
		return 0;
	}
	#ifdef _WIN32
	const auto position = _ftelli64( hFile.get() );
#else
	const auto position = ftello( hFile.get() );
#endif
	if( position < 0 || static_cast<unsigned long long>(position) > std::numeric_limits<unsigned int>::max() )
		return std::numeric_limits<unsigned int>::max();
	return static_cast<unsigned int>(position);
}

bool DiskBuffer::EndOfBuffer( ) const
{
	return (HowFarToEnd()==0);
}

bool DiskBuffer::seek( const eSeek type, const int amount )
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
	return fseeko( hFile.get(), amount, origin ) == 0;
#endif
}
