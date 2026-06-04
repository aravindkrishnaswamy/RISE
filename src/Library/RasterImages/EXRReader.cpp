//////////////////////////////////////////////////////////////////////
//
//  EXRReader.cpp - Implementation of the EXRReader class
//                
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 24, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

// NOTE: I have shut off precompiled headers for this file because of the use of 
//   min and max which conflicts with min and max which Microsoft in their infinite wisdom
//   has decided to make a freaking MACRO!  Grrr...

#ifdef WIN32
#pragma warning( disable : 4512 )		// disables warning about not being able to generate an assignment operator (.NET 2003)
#pragma warning( disable : 4250 )		// disables silly virtual inheritance warning
#pragma warning( disable : 4344 )		// disables warning about explicit template argument passed to template function
#pragma warning( disable : 4290 )		// disables warning about C++ exception definition being ignored
#endif

#include "EXRReader.h"
#include "../Interfaces/ILog.h"
#include <cstddef>

using namespace RISE;
using namespace RISE::Implementation;

EXRReader::EXRReader( IReadBuffer& buffer, const COLOR_SPACE color_space_ ) :
  pReadBuffer( buffer ),
  color_space( color_space_ )
{
	pReadBuffer.addref();
	buffer_start_pos = pReadBuffer.getCurPos();
#ifndef NO_EXR_SUPPORT
	img_width = 0;
	img_height = 0;
#endif
}

EXRReader::~EXRReader( )
{
	pReadBuffer.release();
}

bool EXRReader::BeginRead( unsigned int& width, unsigned int& height )
{
#ifndef NO_EXR_SUPPORT
	pReadBuffer.seek( IBuffer::START, buffer_start_pos );

	IStreamWrapper inbuf( pReadBuffer );
	// General InputFile + FLOAT framebuffer slices (NOT RgbaInputFile,
	// whose Imf::Rgba half storage overflows any channel > 65504 to
	// +Inf on read).  OpenEXR converts a half-stored file to FLOAT
	// losslessly and reads a 32-bit FLOAT file at full range.
	Imf::InputFile	in( inbuf );

	const Imf::Header& hdr = in.header();
	Imath::Box2i dw = hdr.dataWindow();
	const int w = dw.max.x - dw.min.x + 1;
	const int h = dw.max.y - dw.min.y + 1;

	img_width  = static_cast<unsigned int>( w );
	img_height = static_cast<unsigned int>( h );

	// Interleaved R,G,B,A.  Missing channels fall back to 0 (R/G/B) or
	// 1 (A) via the Slice fillValue, matching the historical Imf::Rgba
	// defaults.
	floatbuffer.assign(
		static_cast<std::size_t>( w ) * static_cast<std::size_t>( h ) * 4u, 0.0f );

	const bool hasA = ( hdr.channels().findChannel( "A" ) != 0 );

	const std::size_t xstride = 4u * sizeof( float );
	const std::size_t ystride = static_cast<std::size_t>( w ) * 4u * sizeof( float );

	// Offset the slice base so data-window origin (dw.min) maps to
	// floatbuffer[0] — mirrors the historical Rgba framebuffer offset.
	char* const base = reinterpret_cast<char*>( floatbuffer.data() )
		- ( static_cast<std::ptrdiff_t>( dw.min.y ) * static_cast<std::ptrdiff_t>( w )
		  + static_cast<std::ptrdiff_t>( dw.min.x ) )
		  * static_cast<std::ptrdiff_t>( xstride );

	Imf::FrameBuffer fb;
	fb.insert( "R", Imf::Slice( Imf::FLOAT, base + 0u * sizeof( float ), xstride, ystride, 1, 1, 0.0 ) );
	fb.insert( "G", Imf::Slice( Imf::FLOAT, base + 1u * sizeof( float ), xstride, ystride, 1, 1, 0.0 ) );
	fb.insert( "B", Imf::Slice( Imf::FLOAT, base + 2u * sizeof( float ), xstride, ystride, 1, 1, 0.0 ) );
	fb.insert( "A", Imf::Slice( Imf::FLOAT, base + 3u * sizeof( float ), xstride, ystride, 1, 1, hasA ? 0.0 : 1.0 ) );

	in.setFrameBuffer( fb );
	in.readPixels( dw.min.y, dw.max.y );

	width = img_width;
	height = img_height;
#endif
	return true;
}

void EXRReader::ReadColor( RISEColor& c, const unsigned int x, const unsigned int y )
{
#ifndef NO_EXR_SUPPORT
	const std::size_t idx =
		( static_cast<std::size_t>( y ) * img_width + x ) * 4u;
	const float pr = floatbuffer[idx + 0u];
	const float pg = floatbuffer[idx + 1u];
	const float pb = floatbuffer[idx + 2u];
	const float pa = floatbuffer[idx + 3u];

	switch( color_space )
	{
	case eColorSpace_Rec709RGB_Linear:
		{
			c.base = RISEPel( Rec709RGBPel( pr, pg, pb ) );
			c.a = pa;
		} break;
	case eColorSpace_sRGB:
		{
			c.base = RISEPel( sRGBPel( pr, pg, pb ) );
			c.a = pa;
		} break;
	case eColorSpace_ROMMRGB_Linear:
		{
			c.base = RISEPel( ROMMRGBPel( pr, pg, pb ) );
			c.a = pa;
		} break;
	case eColorSpace_ProPhotoRGB:
		{
			c.base = RISEPel( ProPhotoRGBPel( pr, pg, pb ) );
			c.a = pa;
		} break;
	}
#endif
}

void EXRReader::EndRead( )
{
}
