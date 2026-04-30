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

#include "../../../extlib/stb/stb_image.h"

using namespace RISE;
using namespace RISE::Implementation;

JPEGReader::JPEGReader( IReadBuffer& buffer, const COLOR_SPACE color_space_ ) :
  pReadBuffer( buffer ),
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
	pReadBuffer.release();

	if( pBuffer ) {
		stbi_image_free( pBuffer );
		pBuffer = 0;
	}
}

bool JPEGReader::BeginRead( unsigned int& width, unsigned int& height )
{
	const unsigned int sourceSize = pReadBuffer.Size();
	if( sourceSize == 0 ) {
		GlobalLog()->PrintSourceError( "JPEGReader::BeginRead:: Passed empty buffer to read from", __FILE__, __LINE__ );
		return false;
	}

	// Slurp the entire IReadBuffer into a contiguous block; stb_image
	// only exposes blob/callback APIs, and IReadBuffer has no raw-
	// pointer accessor we can hand straight to stbi_load_from_memory.
	std::vector<unsigned char> source( sourceSize );
	if( !pReadBuffer.getBytes( source.data(), sourceSize ) ) {
		GlobalLog()->PrintSourceError( "JPEGReader::BeginRead:: Failed to read from input buffer", __FILE__, __LINE__ );
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
	if( pBuffer ) {
		const unsigned int idx = ( y * bufW + x ) * nChannels;
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
}
