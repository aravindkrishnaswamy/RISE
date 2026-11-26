//////////////////////////////////////////////////////////////////////
//
//  JPEGReader.cpp - Implementation of the JPEGReader class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 29, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "JPEGReader.h"
#include "../Interfaces/ILog.h"

#include <vector>
#include <limits>

#include "../../../extlib/stb/stb_image.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	// stb accepts exhausted entropy input by padding missing bits with zero.
	// Require a physically complete JPEG marker stream before decoding. Segment
	// payloads may themselves contain FF D9, so a tail-byte search is insufficient.
	bool HasCompleteJPEGStream( const std::vector<unsigned char>& bytes )
	{
		if( bytes.size() < 2 || bytes[0] != 0xff || bytes[1] != 0xd8 ) return false;
		std::size_t cursor = 2;
		bool entropy = false;
		bool sawScan = false;
		while( cursor < bytes.size() ) {
			const bool insideScan = entropy;
			if( entropy ) {
				while( cursor < bytes.size() && bytes[cursor] != 0xff ) ++cursor;
			} else if( bytes[cursor] != 0xff ) {
				return false;
			}
			if( cursor == bytes.size() ) return false;
			// FF fill bytes can precede a marker, including between scans.
			while( cursor < bytes.size() && bytes[cursor] == 0xff ) ++cursor;
			if( cursor == bytes.size() ) return false;
			const auto marker = bytes[cursor++];
			if( insideScan && (marker == 0 || (marker >= 0xd0 && marker <= 0xd7) || marker == 1) )
				continue; // Byte stuffing, restart, or the standalone TEM marker.
			entropy = false;
			if( marker == 0xd9 ) return sawScan; // Legal trailing data is outside this image.
			if( marker == 0 || marker == 0xd8 || (marker >= 0xd0 && marker <= 0xd7) ) return false;
			if( marker == 1 ) continue;
			if( bytes.size() - cursor < 2 ) return false;
			const std::size_t length = (std::size_t(bytes[cursor]) << 8) | bytes[cursor+1];
			if( length < 2 || length > bytes.size() - cursor ) return false;
			cursor += length;
			// Progressive JPEGs contain multiple SOS segments. DNL, if supported
			// by the decoder, is the length-bearing marker embedded in a scan.
			entropy = marker == 0xda || (insideScan && marker == 0xdc);
			sawScan |= marker == 0xda;
		}
		return false;
	}
}

JPEGReader::JPEGReader( IReadBuffer& buffer, const COLOR_SPACE color_space_ ) :
  pReadBuffer( buffer ),
  buffer_start_pos( BufferPosition(buffer) ),
  pBuffer( 0 ),
  bufW( 0 ),
  bufH( 0 ),
  nChannels( 0 ),
  color_space( color_space_ )
{
	pReadBuffer.addref();
}

JPEGReader::~JPEGReader( )
{
	EndRead();
	pReadBuffer.release();
}

bool JPEGReader::BeginRead( unsigned int& width, unsigned int& height )
{
	EndRead();
	width = height = 0;
	if( buffer_start_pos > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
		!BufferSeek(pReadBuffer, IBuffer::START, static_cast<std::int64_t>(buffer_start_pos)) ) {
		GlobalLog()->PrintSourceError( "JPEGReader::BeginRead:: Input rewind failed", __FILE__, __LINE__ );
		return false;
	}
	const auto sourceSize = BufferBytesAvailable(pReadBuffer);
	if( sourceSize == 0 ) {
		GlobalLog()->PrintSourceError( "JPEGReader::BeginRead:: Passed empty buffer to read from", __FILE__, __LINE__ );
		return false;
	}
	if( sourceSize > static_cast<std::uint64_t>(std::numeric_limits<int>::max()) ) {
		GlobalLog()->PrintSourceError( "JPEGReader::BeginRead:: Input exceeds the decoder's signed 32-bit size limit", __FILE__, __LINE__ );
		return false;
	}
	const auto sourceLength = static_cast<unsigned int>(sourceSize);

	// Slurp the entire IReadBuffer into a contiguous block; stb_image
	// only exposes blob/callback APIs, and IReadBuffer has no raw-
	// pointer accessor we can hand straight to stbi_load_from_memory.
	std::vector<unsigned char> source( sourceLength );
	if( !pReadBuffer.getBytes( source.data(), sourceLength ) ) {
		GlobalLog()->PrintSourceError( "JPEGReader::BeginRead:: Failed to read from input buffer", __FILE__, __LINE__ );
		return false;
	}
	if( !HasCompleteJPEGStream(source) ) {
		GlobalLog()->PrintSourceError( "JPEGReader::BeginRead:: Incomplete JPEG marker stream", __FILE__, __LINE__ );
		return false;
	}

	int w = 0, h = 0, channels_in_file = 0;
	// Force RGB output (desired_channels = 3) regardless of source
	// channel count.  JPEG never carries alpha, and pinning the
	// stride here keeps ReadColor's index math trivially correct.
	pBuffer = stbi_load_from_memory(
		source.data(),
		static_cast<int>( sourceSize ),
		&w, &h, &channels_in_file, 3 );

	if( !pBuffer ) {
		GlobalLog()->PrintEx( eLog_Error, "JPEGReader::BeginRead:: stb_image decode failed: %s", stbi_failure_reason() );
		return false;
	}

	bufW = static_cast<unsigned int>( w );
	bufH = static_cast<unsigned int>( h );
	nChannels = 3;

	width = bufW;
	height = bufH;

	return true;
}

void JPEGReader::ReadColor( RISEColor& c, const unsigned int x, const unsigned int y )
{
	if( pBuffer && x < bufW && y < bufH ) {
		const auto idx = (static_cast<size_t>(y) * bufW + x) * nChannels;
		RGBA8 p = {0};
		p.r = pBuffer[idx + 0];
		p.g = pBuffer[idx + 1];
		p.b = pBuffer[idx + 2];
		p.a = 0xFF;

		switch( color_space )
		{
		case eColorSpace_Rec709RGB_Linear:
			{
				c.SetFromIntegerized<Rec709RGBPel,unsigned char>( p, 255.0 );
			} break;
		case eColorSpace_sRGB:
			{
				c.SetFromIntegerized<sRGBPel,unsigned char>( p, 255.0 );
			} break;
		case eColorSpace_ROMMRGB_Linear:
			{
				c.SetFromIntegerized<ROMMRGBPel,unsigned char>( p, 255.0 );
			} break;
		case eColorSpace_ProPhotoRGB:
			{
				c.SetFromIntegerized<ProPhotoRGBPel,unsigned char>( p, 255.0 );
			} break;
		}
	}
}

void JPEGReader::EndRead( )
{
	if( pBuffer ) {
		stbi_image_free( pBuffer );
		pBuffer = 0;
	}
	bufW = bufH = 0;
	nChannels = 0;
}
