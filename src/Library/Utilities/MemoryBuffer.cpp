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
#include "LittleEndian.h"
#include <memory.h>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
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

MemoryBuffer::MemoryBuffer( const unsigned int size ) :
  nSize( size ),
  pBuffer( 0 ),
  nCursor( 0 ),
  bIOwnMemory( true )
{
	ReplaceStorage( std::unique_ptr<char[]>(new char[nSize]), nSize );
}

MemoryBuffer::MemoryBuffer( char * pMemory, const unsigned int size, bool bTakeOwnership ) :
  nSize( pMemory ? size : 0 ),
  pBuffer( pMemory ),
  nCursor( 0 ),
  bIOwnMemory( bTakeOwnership )
{
	if( bIOwnMemory ) {
		ownedStorage.reset( pMemory );
		capacity = nSize;
	}
}

MemoryBuffer::MemoryBuffer( const MemoryBuffer& mb ) :
  nSize( mb.nSize ),
  pBuffer( 0 ),
  nCursor( mb.nCursor ),
  bIOwnMemory( mb.bIOwnMemory )
{
	if( bIOwnMemory ) {
		ReplaceStorage( std::unique_ptr<char[]>(new char[nSize]), nSize );

		if( nSize ) memcpy( pBuffer, mb.pBuffer, nSize );
	} else {
		pBuffer = mb.pBuffer;
	}

}

MemoryBuffer::MemoryBuffer( IMemoryBuffer& mb ) :
  nSize( 0 ),
  pBuffer( 0 ),
  nCursor( mb.getCurPos() ),
  bIOwnMemory( mb.DoOwnMemory() )
{
	char* const source = mb.Pointer();
	nSize = source ? mb.Size() : 0;
	nCursor = std::min( nCursor, nSize );
	if( bIOwnMemory ) {
		ReplaceStorage( std::unique_ptr<char[]>(new char[nSize]), nSize );

		if( nSize ) memcpy( pBuffer, source, nSize );
	} else {
		pBuffer = source;
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
		if( stat( s.c_str(), &file_stats ) != 0 || file_stats.st_size < 0 ||
			static_cast<uintmax_t>(file_stats.st_size) > std::numeric_limits<unsigned int>::max() ) {
			// Cannot size the file — leave the buffer empty (nSize/pBuffer
			// stay 0 from the init list) rather than proceeding with a
			// bogus size.
			GlobalLog()->PrintEx( eLog_Error, "MemoryBuffer:: Failed to stat file: %s", szFileName );
			return;
		}
		std::unique_ptr<FILE, decltype(&fclose)> f( fopen( s.c_str(), "rb" ), fclose );
		if( f ) {
			const auto size = static_cast<unsigned int>(file_stats.st_size);
			std::unique_ptr<char[]> storage( new char[size] );
			// Read byte-granular and CHECK the count: a short read (file
			// truncated, or the size changed after stat) must not leave the
			// tail uninitialised.
			const size_t bytesRead = fread( storage.get(), 1, size, f.get() );
			if( ferror( f.get() ) ) {
				GlobalLog()->PrintEx( eLog_Error, "MemoryBuffer:: Failed to read file: %s", szFileName );
				return;
			}
			ReplaceStorage( std::move(storage), size );
			nSize = size;
			if( bytesRead < nSize ) {
				memset( pBuffer + bytesRead, 0, nSize - bytesRead );
				GlobalLog()->PrintEx( eLog_Error, "MemoryBuffer:: Short read on \'%s\': got %u of %u bytes (tail zeroed)", szFileName, static_cast<unsigned>(bytesRead), nSize );
			} else {
				GlobalLog()->PrintEx( eLog_Info, "MemoryBuffer:: Read file \'%s\' of size %u bytes", szFileName, nSize );
			}
		} else {
			GlobalLog()->PrintEx( eLog_Error, "MemoryBuffer:: Failed to open file: %s", szFileName );
		}
	}
}

MemoryBuffer::~MemoryBuffer()
{
	if( ownedStorage ) GlobalLog()->PrintDelete( ownedStorage.get(), __FILE__, __LINE__ );
}

void MemoryBuffer::ReplaceStorage( std::unique_ptr<char[]> replacement, unsigned int newCapacity )
{
	if( ownedStorage ) GlobalLog()->PrintDelete( ownedStorage.get(), __FILE__, __LINE__ );
	ownedStorage = std::move(replacement);
	pBuffer = ownedStorage.get();
	capacity = newCapacity;
	GlobalLog()->PrintNew( pBuffer, __FILE__, __LINE__, "buffer" );
}

std::unique_ptr<char[]> MemoryBuffer::AllocateGrowth( unsigned int required, unsigned int& allocated ) const
{
	const auto grown = std::uint64_t(capacity) + std::max( 1U, capacity / 2 );
	allocated = static_cast<unsigned int>(std::max( std::uint64_t(required),
		std::min( grown, std::uint64_t(std::numeric_limits<unsigned int>::max()) ) ));
	if( allocated > required ) {
		std::unique_ptr<char[]> spare( new (std::nothrow) char[allocated] );
		if( spare ) return spare;
	}
	allocated = required;
	return std::unique_ptr<char[]>( new char[required] );
}

void MemoryBuffer::Clear( )
{
	if( nSize ) memset( pBuffer, 0, nSize );
}

bool MemoryBuffer::Resize( unsigned int new_size, bool bForce )
{
	// If we don't own the memory, don't resize!
	// If we are told to be smaller then don't do anything
	if( (!bIOwnMemory) || (new_size==nSize) || ((new_size<nSize)&&!bForce) ) {
		return false;
	}

	// Keep explicit shrink behavior and grow spare storage only as needed.
	if( new_size < nSize || new_size > capacity ) {
		unsigned int newCapacity = new_size;
		auto replacement = new_size < nSize
			? std::unique_ptr<char[]>( new char[new_size] )
			: AllocateGrowth( new_size, newCapacity );
		const auto preserved = std::min( new_size, nSize );
		if( preserved ) memcpy( replacement.get(), pBuffer, preserved );
		ReplaceStorage( std::move(replacement), newCapacity );
	}
	nSize = new_size;

	if( nCursor >= nSize ) {
		nCursor = 0;
	}

	return true;
}

bool MemoryBuffer::ResizeForMore( unsigned int more_bytes )
{
	if( more_bytes > std::numeric_limits<unsigned int>::max() - nCursor ) {
		return false;
	}
	return Resize( more_bytes+nCursor );
}

unsigned int MemoryBuffer::HowFarToEnd( ) const
{
	return nCursor < nSize ? nSize-nCursor-1 : 0;
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
	return pBuffer && nCursor <= nSize ? pBuffer+nCursor : nullptr;
}

const char * MemoryBuffer::PointerAtCursor( ) const
{
	return pBuffer && nCursor <= nSize ? pBuffer+nCursor : nullptr;
}

bool MemoryBuffer::EndOfBuffer( ) const
{
	return (HowFarToEnd()==0);
}

bool MemoryBuffer::seek( const eSeek type, const int amount )
{
	std::int64_t position = amount;
	switch( type ) {
	case START:
		break;
	case CUR:
		position += nCursor;
		break;
	case END:
		if( amount < 0 ) return false;
		position = static_cast<std::int64_t>( nSize ) - 1 - amount;
		break;
	default:
		return false;
	}
	if( position < 0 || position > nSize ) {
		return false;
	}
	nCursor = static_cast<unsigned int>( position );
	return true;
}

bool MemoryBuffer::DumpToFile( const char * szFileName )
{
	if( szFileName && pBuffer ) {
		FILE* f = fopen( GlobalMediaPathLocator().Find(szFileName).c_str(), "wb" );
		if( f ) {
			const bool written = fwrite( pBuffer, 1, nSize, f ) == nSize;
			const bool closed = fclose( f ) == 0;
			return written && closed;
		} else {
			GlobalLog()->PrintEasyError( "MemoryBuffer::DumpToFile:: Failed to open file for writing" );
		}
	}

	return false;
}

bool MemoryBuffer::DumpToFileToCursor( const char * szFileName )
{
	if( szFileName && pBuffer && nCursor <= nSize ) {
		FILE* f = fopen( GlobalMediaPathLocator().Find(szFileName).c_str(), "wb" );

		if( f ) {
			const bool written = fwrite( pBuffer, 1, nCursor, f ) == nCursor;
			const bool closed = fclose( f ) == 0;
			return written && closed;
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
	// Bounds-check UNCONDITIONALLY (was _DEBUG-only): reading a
	// scalar past the buffer end otherwise OOB-reads the heap on a
	// malformed / truncated file.  Overflow-safe; returns 0 on exhaustion.
	if( nCursor >= nSize ) {
	#ifdef _DEBUG
		GlobalLog()->PrintSourceError( "MemoryBuffer::getChar:: Attempted read past end of buffer", __FILE__, __LINE__ );
	#endif
		nCursor = nSize;   // consume to end so loop-until-cursor callers terminate
		return 0;
	}
	return (char)pBuffer[nCursor++];
}

unsigned char MemoryBuffer::getUChar()
{
	// Bounds-check UNCONDITIONALLY (was _DEBUG-only): reading a
	// scalar past the buffer end otherwise OOB-reads the heap on a
	// malformed / truncated file.  Overflow-safe; returns 0 on exhaustion.
	if( nCursor >= nSize ) {
	#ifdef _DEBUG
		GlobalLog()->PrintSourceError( "MemoryBuffer::getUChar:: Attempted read past end of buffer", __FILE__, __LINE__ );
	#endif
		nCursor = nSize;   // consume to end so loop-until-cursor callers terminate
		return 0;
	}

	return pBuffer[nCursor++];
}

short MemoryBuffer::getWord()
{
	// Bounds-check UNCONDITIONALLY (was _DEBUG-only): reading a
	// scalar past the buffer end otherwise OOB-reads the heap on a
	// malformed / truncated file.  Overflow-safe; returns 0 on exhaustion.
	if( nCursor > nSize || sizeof(short) > nSize - nCursor ) {
	#ifdef _DEBUG
		GlobalLog()->PrintSourceError( "MemoryBuffer::getWord:: Attempted read past end of buffer", __FILE__, __LINE__ );
	#endif
		nCursor = nSize;   // consume to end so loop-until-cursor callers terminate
		return 0;
	}

	const auto* bytes = reinterpret_cast<const unsigned char*>(pBuffer + nCursor);
	nCursor += sizeof(std::uint16_t);
	return DecodeLittleEndian<short, std::uint16_t>( bytes );
}


unsigned short MemoryBuffer::getUWord()
{
	// Bounds-check UNCONDITIONALLY (was _DEBUG-only): reading a
	// scalar past the buffer end otherwise OOB-reads the heap on a
	// malformed / truncated file.  Overflow-safe; returns 0 on exhaustion.
	if( nCursor > nSize || sizeof(unsigned short) > nSize - nCursor ) {
	#ifdef _DEBUG
		GlobalLog()->PrintSourceError( "MemoryBuffer::getUWord:: Attempted read past end of buffer", __FILE__, __LINE__ );
	#endif
		nCursor = nSize;   // consume to end so loop-until-cursor callers terminate
		return 0;
	}

	const auto* bytes = reinterpret_cast<const unsigned char*>(pBuffer + nCursor);
	nCursor += sizeof(std::uint16_t);
	return DecodeLittleEndian<unsigned short, std::uint16_t>( bytes );
}

int MemoryBuffer::getInt()
{
	// Bounds-check UNCONDITIONALLY (was _DEBUG-only): reading a
	// scalar past the buffer end otherwise OOB-reads the heap on a
	// malformed / truncated file.  Overflow-safe; returns 0 on exhaustion.
	if( nCursor > nSize || sizeof(int) > nSize - nCursor ) {
	#ifdef _DEBUG
		GlobalLog()->PrintSourceError( "MemoryBuffer::getInt:: Attempted read past end of buffer", __FILE__, __LINE__ );
	#endif
		nCursor = nSize;   // consume to end so loop-until-cursor callers terminate
		return 0;
	}

	const auto* bytes = reinterpret_cast<const unsigned char*>(pBuffer + nCursor);
	nCursor += sizeof(std::uint32_t);
	return DecodeLittleEndian<int, std::uint32_t>( bytes );
}

unsigned int MemoryBuffer::getUInt()
{
	// Bounds-check UNCONDITIONALLY (was _DEBUG-only): reading a
	// scalar past the buffer end otherwise OOB-reads the heap on a
	// malformed / truncated file.  Overflow-safe; returns 0 on exhaustion.
	if( nCursor > nSize || sizeof(unsigned int) > nSize - nCursor ) {
	#ifdef _DEBUG
		GlobalLog()->PrintSourceError( "MemoryBuffer::getUInt:: Attempted read past end of buffer", __FILE__, __LINE__ );
	#endif
		nCursor = nSize;   // consume to end so loop-until-cursor callers terminate
		return 0;
	}

	const auto* bytes = reinterpret_cast<const unsigned char*>(pBuffer + nCursor);
	nCursor += sizeof(std::uint32_t);
	return DecodeLittleEndian<unsigned int, std::uint32_t>( bytes );
}

float MemoryBuffer::getFloat()
{
	// Bounds-check UNCONDITIONALLY (was _DEBUG-only): reading a
	// scalar past the buffer end otherwise OOB-reads the heap on a
	// malformed / truncated file.  Overflow-safe; returns 0 on exhaustion.
	if( nCursor > nSize || sizeof(float) > nSize - nCursor ) {
	#ifdef _DEBUG
		GlobalLog()->PrintSourceError( "MemoryBuffer::getFloat:: Attempted read past end of buffer", __FILE__, __LINE__ );
	#endif
		nCursor = nSize;   // consume to end so loop-until-cursor callers terminate
		return 0;
	}

	const auto* bytes = reinterpret_cast<const unsigned char*>(pBuffer + nCursor);
	nCursor += sizeof(std::uint32_t);
	return DecodeLittleEndian<float, std::uint32_t>( bytes );
}

double MemoryBuffer::getDouble()
{
	// Bounds-check UNCONDITIONALLY (was _DEBUG-only): reading a
	// scalar past the buffer end otherwise OOB-reads the heap on a
	// malformed / truncated file.  Overflow-safe; returns 0 on exhaustion.
	if( nCursor > nSize || sizeof(double) > nSize - nCursor ) {
	#ifdef _DEBUG
		GlobalLog()->PrintSourceError( "MemoryBuffer::getDouble:: Attempted read past end of buffer", __FILE__, __LINE__ );
	#endif
		nCursor = nSize;   // consume to end so loop-until-cursor callers terminate
		return 0;
	}

	const auto* bytes = reinterpret_cast<const unsigned char*>(pBuffer + nCursor);
	nCursor += sizeof(std::uint64_t);
	return DecodeLittleEndian<double, std::uint64_t>( bytes );
}

bool MemoryBuffer::getBytes( void* pDest, unsigned int amount )
{
	if( !pDest ) {
		return false;
	}
	if( amount == 0 ) {
		return true;
	}

	// Bounds-check UNCONDITIONALLY (was _DEBUG-only): a malformed / truncated
	// file whose header claims more bytes than remain would otherwise drive
	// an OUT-OF-BOUNDS heap read in the memcpy below.  Overflow-safe form:
	// nCursor <= nSize is the invariant, so `amount > nSize - nCursor` cannot
	// underflow.  On overrun, copy what is available and zero the rest (never
	// read OOB, never leave pDest uninitialised) and signal failure.
	if( nCursor > nSize || amount > nSize - nCursor ) {
		const unsigned int avail = ( nCursor <= nSize ) ? ( nSize - nCursor ) : 0;
		GlobalLog()->PrintEx( eLog_Error, "MemoryBuffer::getBytes:: read past end of buffer (want %u at cursor %u of %u); returning %u available, rest zeroed", amount, nCursor, nSize, avail );
		if( avail ) {
			memcpy( pDest, &pBuffer[nCursor], avail );
		}
		memset( static_cast<char*>(pDest) + avail, 0, amount - avail );
		nCursor = nSize;
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

// Single-value setters grow the buffer to accommodate the write when needed, matching
// the setBytes contract. Resize only fails (and thus the setter returns false) when the
// buffer does not own its memory, in which case the caller must pre-size it.
bool MemoryBuffer::setChar( const char ch )
{
	if( nCursor > nSize || (sizeof(char) > nSize - nCursor && !ResizeForMore( sizeof(char) )) ) {
		return false;
	}
	pBuffer[nCursor++] = ch;
	return true;
}

bool MemoryBuffer::setUChar( const unsigned char ch )
{
	if( nCursor > nSize || (sizeof(unsigned char) > nSize - nCursor && !ResizeForMore( sizeof(unsigned char) )) ) {
		return false;
	}
	pBuffer[nCursor++] = ch;
	return true;
}

bool MemoryBuffer::setWord( const short sh )
{
	if( nCursor > nSize || (sizeof(short) > nSize - nCursor && !ResizeForMore( sizeof(short) )) ) {
		return false;
	}

	EncodeLittleEndian<std::uint16_t>( reinterpret_cast<unsigned char*>(pBuffer + nCursor), sh );
	nCursor += sizeof(std::uint16_t);
	return true;
}

bool MemoryBuffer::setUWord( const unsigned short sh )
{
	if( nCursor > nSize || (sizeof(unsigned short) > nSize - nCursor && !ResizeForMore( sizeof(unsigned short) )) ) {
		return false;
	}
	EncodeLittleEndian<std::uint16_t>( reinterpret_cast<unsigned char*>(pBuffer + nCursor), sh );
	nCursor += sizeof(std::uint16_t);
	return true;
}

bool MemoryBuffer::setInt( const int n )
{
	if( nCursor > nSize || (sizeof(int) > nSize - nCursor && !ResizeForMore( sizeof(int) )) ) {
		return false;
	}

	EncodeLittleEndian<std::uint32_t>( reinterpret_cast<unsigned char*>(pBuffer + nCursor), n );
	nCursor += sizeof(std::uint32_t);
	return true;
}

bool MemoryBuffer::setUInt( const unsigned int n )
{
	if( nCursor > nSize || (sizeof(unsigned int) > nSize - nCursor && !ResizeForMore( sizeof(unsigned int) )) ) {
		return false;
	}

	EncodeLittleEndian<std::uint32_t>( reinterpret_cast<unsigned char*>(pBuffer + nCursor), n );
	nCursor += sizeof(std::uint32_t);
	return true;
}

bool MemoryBuffer::setFloat( const float f )
{
	if( nCursor > nSize || (sizeof(float) > nSize - nCursor && !ResizeForMore( sizeof(float) )) ) {
		return false;
	}

	EncodeLittleEndian<std::uint32_t>( reinterpret_cast<unsigned char*>(pBuffer + nCursor), f );
	nCursor += sizeof(std::uint32_t);
	return true;
}

bool MemoryBuffer::setDouble( const double d )
{
	if( nCursor > nSize || (sizeof(double) > nSize - nCursor && !ResizeForMore( sizeof(double) )) ) {
		return false;
	}

	EncodeLittleEndian<std::uint64_t>( reinterpret_cast<unsigned char*>(pBuffer + nCursor), d );
	nCursor += sizeof(std::uint64_t);
	return true;
}

bool MemoryBuffer::setBytes( const void* pSource, unsigned int amount )
{
	if( !pSource ) {
		GlobalLog()->PrintSourceError( "MemoryBuffer::setBytes:: source is NULL", __FILE__, __LINE__ );
		return false;
	}

	if( nCursor > nSize || amount > std::numeric_limits<unsigned int>::max() - nCursor ) {
		return false;
	}
	if( amount == 0 ) {
		return true;
	}

	const unsigned int end = nCursor + amount;
	if( end > nSize && !bIOwnMemory ) return false;

	if( end > nSize && end > capacity ) {
		// An input span can alias this buffer. Copy it before replacing storage.
		unsigned int newCapacity;
		auto replacement = AllocateGrowth( end, newCapacity );
		if( nSize ) memcpy( replacement.get(), pBuffer, nSize );
		memcpy( replacement.get() + nCursor, pSource, amount );
		ReplaceStorage( std::move(replacement), newCapacity );
	} else {
		memmove( pBuffer + nCursor, pSource, amount );
	}
	nCursor = end;
	nSize = std::max( nSize, end );
	return true;
}
