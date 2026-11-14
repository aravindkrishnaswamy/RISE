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
#include <cstdint>
#include <exception>
#include <algorithm>

using namespace RISE;
using namespace RISE::Implementation;

EXRReader::EXRReader( IReadBuffer& buffer, const COLOR_SPACE color_space_ ) :
  pReadBuffer( buffer ),
  color_space( color_space_ )
{
	pReadBuffer.addref();
	buffer_start_pos = BufferPosition(pReadBuffer);
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
	EndRead();
	width = height = 0;
#ifndef NO_EXR_SUPPORT
	// Decode with no active image. Moving the cleared storage into a local
	// transaction reuses capacity while keeping failed reads inaccessible.
	std::vector<float> decoded;
	decoded.swap( floatbuffer );
	try {
		if( buffer_start_pos > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
			!BufferSeek(pReadBuffer, IBuffer::START, static_cast<std::int64_t>(buffer_start_pos)) )
			throw std::runtime_error( "EXR input rewind failed" );

		IStreamWrapper inbuf( pReadBuffer );
		// FLOAT slices preserve the full channel range, including files
		// stored as FLOAT instead of the narrower historical half format.
		Imf::InputFile in( inbuf );
		const Imf::Header& hdr = in.header();
		const Imath::Box2i dw = hdr.dataWindow();
		const std::int64_t w = static_cast<std::int64_t>( dw.max.x ) - dw.min.x + 1;
		const std::int64_t h = static_cast<std::int64_t>( dw.max.y ) - dw.min.y + 1;
		constexpr std::uint64_t bytesPerPixel = 4u * sizeof(float);
		const std::uint64_t maxBytes = std::min<std::uint64_t>(
			2ULL * 1024 * 1024 * 1024, std::numeric_limits<std::size_t>::max() );
		if( w <= 0 || h <= 0 || static_cast<std::uint64_t>( w ) >
			maxBytes / bytesPerPixel / static_cast<std::uint64_t>( h ) )
			throw std::runtime_error( "EXR invalid or too-large decoded image dimensions" );

		const auto imageWidth = static_cast<unsigned int>( w );
		const auto imageHeight = static_cast<unsigned int>( h );
		decoded.assign( static_cast<std::size_t>( imageWidth ) * imageHeight * 4u, 0.0f );
		const std::size_t xstride = bytesPerPixel;
		const std::size_t ystride = static_cast<std::size_t>( imageWidth ) * xstride;
		char* const pixels = reinterpret_cast<char*>( decoded.data() );

		// Let OpenEXR compute offset-window addressing without forming an
		// out-of-array pointer in application code. Missing RGB channels are
		// zero and missing alpha is one, matching the previous reader.
		Imf::FrameBuffer fb;
		fb.insert( "R", Imf::Slice::Make( Imf::FLOAT, pixels, dw, xstride, ystride, 1, 1, 0.0 ) );
		fb.insert( "G", Imf::Slice::Make( Imf::FLOAT, pixels + sizeof(float), dw, xstride, ystride, 1, 1, 0.0 ) );
		fb.insert( "B", Imf::Slice::Make( Imf::FLOAT, pixels + 2u * sizeof(float), dw, xstride, ystride, 1, 1, 0.0 ) );
		fb.insert( "A", Imf::Slice::Make( Imf::FLOAT, pixels + 3u * sizeof(float), dw, xstride, ystride, 1, 1, 1.0 ) );
		in.setFrameBuffer( fb );
		in.readPixels( dw.min.y, dw.max.y );

		floatbuffer.swap( decoded );
		img_width = width = imageWidth;
		img_height = height = imageHeight;
		return true;
	} catch( const std::exception& failure ) {
		decoded.clear();
		floatbuffer.swap( decoded );
		GlobalLog()->PrintEx( eLog_Error, "EXRReader::BeginRead: %s", failure.what() );
		return false;
	}
#else
	return false;
#endif
}

void EXRReader::ReadColor( RISEColor& c, const unsigned int x, const unsigned int y )
{
#ifndef NO_EXR_SUPPORT
	if( x >= img_width || y >= img_height || floatbuffer.empty() ) return;
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
#ifndef NO_EXR_SUPPORT
	floatbuffer.clear();
	img_width = img_height = 0;
#endif
}
