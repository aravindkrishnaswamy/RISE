//////////////////////////////////////////////////////////////////////
//
//  TIFFReader.cpp - Implementation of the TIFFReader class
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

#include "pch.h"
#include "TIFFReader.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include "../Interfaces/ILog.h"

#ifndef NO_TIFF_SUPPORT
	#include <tiffio.h>
#endif

#include "TIFFCommon.h"

using namespace RISE;
using namespace RISE::Implementation;

TIFFReader::TIFFReader( IReadBuffer& buffer, const COLOR_SPACE color_space_ ) :
  pReadBuffer( buffer ),
  pBuffer( 0 ),
  bufW( 0 ),
  bufH( 0 ),
  color_space( color_space_ )
{
	pReadBuffer.addref();
}

TIFFReader::~TIFFReader( )
{
	EndRead();
	pReadBuffer.release();
}

#ifdef NO_TIFF_SUPPORT

bool TIFFReader::BeginRead( unsigned int& width, unsigned int& height )
{
	EndRead();
	width = height = 0;
	GlobalLog()->PrintSourceError( "TIFFReader::BeginRead NO TIFF SUPPORT was compiled!  Nothing will be read", __FILE__, __LINE__ );
	return false;
}

#else

bool TIFFReader::BeginRead( unsigned int& outputWidth, unsigned int& outputHeight )
{
	EndRead();
	outputWidth = outputHeight = 0;
	if( BufferSize(pReadBuffer) == 0 ) {
		GlobalLog()->PrintSourceError( "TIFFReader::BeginRead:: Passed empty buffer to read from", __FILE__, __LINE__ );
		return false;
	}

	if( !BufferSeek(pReadBuffer, IBuffer::START, 0) ) return false;
	std::unique_ptr<TIFF, decltype(&::TIFFClose)> tiff(
		TIFFClientOpen( "", "r", (thandle_t)&pReadBuffer, TIFFRead, TIFFWriteDummy, TIFFSeekReader,
			RISE::Implementation::TIFFClose, TIFFSizeReader, TIFFMapFile, TIFFUnmapFile ), &::TIFFClose );
	if( !tiff ) {
		GlobalLog()->PrintSourceError( "TIFFReader::BeginRead:: Failed to create TIFF object", __FILE__, __LINE__ );
		return false;
	}

	std::uint32_t width = 0, height = 0;
	if( !TIFFGetField(tiff.get(), TIFFTAG_IMAGEWIDTH, &width) ||
		!TIFFGetField(tiff.get(), TIFFTAG_IMAGELENGTH, &height) ) return false;

	// Guard the destination-buffer sizing against a header-driven integer
	// overflow: width/height are the TIFF ImageWidth/ImageLength tags (up to
	// ~4e9 each), so the historical 32-bit `height*width*4` product can wrap
	// to a tiny allocation that TIFFReadRGBAImage then overruns (libtiff does
	// NOT validate the caller's raster buffer size).  Compute in 64-bit and
	// cap at a sane ceiling; reject (closing the TIFF) before allocating.
	if( width == 0 || height == 0 ) {
		GlobalLog()->Print( eLog_Error, "TIFFReader: Invalid (zero) image dimensions" );
		return false;
	}
	static const unsigned long long kMaxImageBytes = 2ULL * 1024 * 1024 * 1024; // 2 GiB ceiling
	if( width > kMaxImageBytes / 4 / height ) {
		GlobalLog()->Print( eLog_Error, "TIFFReader: Image dimensions too large" );
		return false;
	}
	const unsigned long long nBytes64 = static_cast<unsigned long long>(width) * height * 4;
	const size_t nBytes = (size_t)nBytes64;

	std::unique_ptr<unsigned char[]> pixels(new (std::nothrow) unsigned char[nBytes]);
	if( !pixels ) {
		GlobalLog()->PrintEasyError( "TIFFReader: Failed to allocate image pixels" );
		return false;
	}
	memset( pixels.get(), 0, nBytes );
	if( !TIFFReadRGBAImageOriented(tiff.get(), width, height,
		reinterpret_cast<std::uint32_t*>(pixels.get()), ORIENTATION_TOPLEFT, 1) ) return false;
	pBuffer = pixels.release();
	GlobalLog()->PrintNew( pBuffer, __FILE__, __LINE__, "TIFF pixels" );
	bufW = outputWidth = width;
	bufH = outputHeight = height;
	return true;
}

#endif

#ifdef NO_TIFF_SUPPORT
void TIFFReader::ReadColor( RISEColor&, const unsigned int, const unsigned int )
{
}
#else
void TIFFReader::ReadColor( RISEColor& c, const unsigned int x, const unsigned int y )
{
	if( pBuffer && x < bufW && y < bufH ) {
		RGBA8 p = {0};
		const auto idx = (static_cast<size_t>(y) * bufW + x) * 4;
		std::uint32_t pixel = 0;
		std::memcpy( &pixel, pBuffer + idx, sizeof(pixel) );
		p.a = TIFFGetA( pixel );
		// The libtiff RGBA interface returns associated alpha.
		const auto straight = [alpha = p.a]( unsigned int value ) {
			return alpha ? static_cast<unsigned char>(std::min(255U, (value * 255 + alpha / 2) / alpha)) : 0;
		};
		p.r = straight( TIFFGetR(pixel) );
		p.g = straight( TIFFGetG(pixel) );
		p.b = straight( TIFFGetB(pixel) );

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
#endif

void TIFFReader::EndRead( )
{
	if( pBuffer ) {
		GlobalLog()->PrintDelete( pBuffer, __FILE__, __LINE__ );
		delete [] pBuffer;
		pBuffer = 0;
	}
	bufW = bufH = 0;
}
