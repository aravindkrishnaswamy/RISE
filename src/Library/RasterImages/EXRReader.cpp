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

		Imath::Box2i dw;
		unsigned int imageWidth = 0;
		unsigned int imageHeight = 0;
		bool hasChroma = false;
		{
			// Finish this InputFile before rewinding for the SDK's YCA
			// decoder: two live decoders must not share an input cursor.
			IStreamWrapper inbuf( pReadBuffer );
			Imf::InputFile in( inbuf );
			const Imf::Header& hdr = in.header();
			dw = hdr.dataWindow();
			const bool hasLuminance = hdr.channels().findChannel( "Y" ) != nullptr;
			hasChroma = hdr.channels().findChannel( "RY" ) != nullptr ||
				hdr.channels().findChannel( "BY" ) != nullptr;
			const std::int64_t w = static_cast<std::int64_t>( dw.max.x ) - dw.min.x + 1;
			const std::int64_t h = static_cast<std::int64_t>( dw.max.y ) - dw.min.y + 1;
			constexpr std::uint64_t floatBytesPerPixel = 4u * sizeof(float);
			// YCA conversion additionally owns the SDK's half-RGBA result.
			// Apply the combined pixel budget before allocating either image.
			const std::uint64_t bytesPerPixel = floatBytesPerPixel +
				(hasChroma ? sizeof(Imf::Rgba) : 0);
			const std::uint64_t maxBytes = std::min<std::uint64_t>(
				2ULL * 1024 * 1024 * 1024, std::numeric_limits<std::size_t>::max() );
			if( w <= 0 || h <= 0 || static_cast<std::uint64_t>( w ) >
				maxBytes / bytesPerPixel / static_cast<std::uint64_t>( h ) )
				throw std::runtime_error( "EXR invalid or too-large decoded image dimensions" );

			imageWidth = static_cast<unsigned int>( w );
			imageHeight = static_cast<unsigned int>( h );
			const std::size_t pixelCount = static_cast<std::size_t>( imageWidth ) * imageHeight;
			const std::size_t floatCount = pixelCount * 4u;
			const std::uint64_t halfBytes = hasChroma ? pixelCount * sizeof(Imf::Rgba) : 0;
			// A retained allocation from a larger RGB image must not defeat
			// the combined budget on a later YCA read.
			if( decoded.capacity() < floatCount ||
				decoded.capacity() > (maxBytes - halfBytes) / sizeof(float) )
				std::vector<float>().swap( decoded );
			decoded.assign( floatCount, 0.0f );

			if( !hasChroma ) {
				// The SDK gives Y precedence over RGB whenever it is present.
				// Read plain Y/YA as FLOAT so values above half's range survive;
				// otherwise preserve the full FLOAT RGB/alpha channel range.
				const std::size_t xstride = floatBytesPerPixel;
				const std::size_t ystride = static_cast<std::size_t>( imageWidth ) * xstride;
				char* const pixels = reinterpret_cast<char*>( decoded.data() );
				Imf::FrameBuffer fb;
				if( hasLuminance ) {
					fb.insert( "Y", Imf::Slice::Make( Imf::FLOAT, pixels, dw, xstride, ystride, 1, 1, 0.0 ) );
				} else {
					fb.insert( "R", Imf::Slice::Make( Imf::FLOAT, pixels, dw, xstride, ystride, 1, 1, 0.0 ) );
					fb.insert( "G", Imf::Slice::Make( Imf::FLOAT, pixels + sizeof(float), dw, xstride, ystride, 1, 1, 0.0 ) );
					fb.insert( "B", Imf::Slice::Make( Imf::FLOAT, pixels + 2u * sizeof(float), dw, xstride, ystride, 1, 1, 0.0 ) );
				}
				fb.insert( "A", Imf::Slice::Make( Imf::FLOAT, pixels + 3u * sizeof(float), dw, xstride, ystride, 1, 1, 1.0 ) );
				in.setFrameBuffer( fb );
				in.readPixels( dw.min.y, dw.max.y );
				if( hasLuminance ) {
					for( std::size_t i = 0; i < floatCount; i += 4u )
						decoded[i + 1u] = decoded[i + 2u] = decoded[i];
				}
			}
		}

		if( hasChroma ) {
			if( !BufferSeek(pReadBuffer, IBuffer::START, static_cast<std::int64_t>(buffer_start_pos)) )
				throw std::runtime_error( "EXR YCA input rewind failed" );
			const std::size_t pixelCount = static_cast<std::size_t>( imageWidth ) * imageHeight;
			std::vector<Imf::Rgba> rgba( pixelCount );
			IStreamWrapper inbuf( pReadBuffer );
			Imf::RgbaInputFile in( inbuf );
			if( in.dataWindow() != dw )
				throw std::runtime_error( "EXR data window changed during YCA decode" );
			// Preserve the SDK's historical half-precision YCA reconstruction,
			// including chroma filtering, channel precedence and chromaticities.
			in.setFrameBuffer( Imf::ComputeBasePointer( rgba.data(), dw ), 1, imageWidth );
			in.readPixels( dw.min.y, dw.max.y );
			for( std::size_t i = 0; i < pixelCount; ++i ) {
				decoded[i * 4u] = static_cast<float>( rgba[i].r );
				decoded[i * 4u + 1u] = static_cast<float>( rgba[i].g );
				decoded[i * 4u + 2u] = static_cast<float>( rgba[i].b );
				decoded[i * 4u + 3u] = static_cast<float>( rgba[i].a );
			}
		}

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
