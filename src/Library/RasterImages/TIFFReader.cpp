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
#include <cstring>
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
  color_space( color_space_ )
{
	pReadBuffer.addref();
}

TIFFReader::~TIFFReader( )
{
	pReadBuffer.release();

	if( pBuffer ) {
		GlobalLog()->PrintDelete( pBuffer, __FILE__, __LINE__ );
		delete [] pBuffer;
		pBuffer = 0;
	}
}

#ifdef NO_TIFF_SUPPORT

bool TIFFReader::BeginRead( unsigned int& width, unsigned int& height )
{
	GlobalLog()->PrintSourceError( "TIFFReader::BeginRead NO TIFF SUPPORT was compiled!  Nothing will be read", __FILE__, __LINE__ );
	return false;
}

#else

bool TIFFReader::BeginRead( unsigned int& width, unsigned int& height )
{
	if( pReadBuffer.Size() == 0 ) {
		GlobalLog()->PrintSourceError( "TIFFReader::BeginRead:: Passed empty buffer to read from", __FILE__, __LINE__ );
		return false;
	}

	TIFF* tiff = TIFFClientOpen( "", "r", (thandle_t)&pReadBuffer, TIFFRead, TIFFWriteDummy, TIFFSeekReader, TIFFClose, TIFFSizeReader, TIFFMapFile, TIFFUnmapFile );
	if( !tiff ) {
		GlobalLog()->PrintSourceError( "TIFFReader::BeginRead:: Failed to create TIFF object", __FILE__, __LINE__ );
		return false;
	}

	{
		uint32_t w = 0, h = 0;
		if( !TIFFGetField( tiff, TIFFTAG_IMAGEWIDTH, &w ) ||
			!TIFFGetField( tiff, TIFFTAG_IMAGELENGTH, &h ) ) {
			TIFFClose( tiff );
			return false;
		}

		width = w;
		height = h;

		bufW = width;
	}

	// Guard the destination-buffer sizing against a header-driven integer
	// overflow: width/height are the TIFF ImageWidth/ImageLength tags (up to
	// ~4e9 each), so the historical 32-bit `height*width*4` product can wrap
	// to a tiny allocation that TIFFReadRGBAImage then overruns (libtiff does
	// NOT validate the caller's raster buffer size).  Compute in 64-bit and
	// cap at a sane ceiling; reject (closing the TIFF) before allocating.
	if( width == 0 || height == 0 ) {
		GlobalLog()->Print( eLog_Error, "TIFFReader: Invalid (zero) image dimensions" );
		TIFFClose( tiff );
		return false;
	}
	static const unsigned long long kMaxImageBytes = 2ULL * 1024 * 1024 * 1024; // 2 GiB ceiling
	if( width > kMaxImageBytes / 4 / height ) {
		GlobalLog()->Print( eLog_Error, "TIFFReader: Image dimensions too large" );
		TIFFClose( tiff );
		return false;
	}
	const unsigned long long nBytes64 = static_cast<unsigned long long>(width) * height * 4;
	const size_t nBytes = (size_t)nBytes64;

	pBuffer = new unsigned char[nBytes];
	GlobalLog()->PrintNew( pBuffer, __FILE__, __LINE__, "buffer" );
	memset( pBuffer, 0, nBytes );

	const bool read = TIFFReadRGBAImageOriented( tiff, width, height,
		reinterpret_cast<uint32_t*>(pBuffer), ORIENTATION_TOPLEFT, 1 ) != 0;
	TIFFClose( tiff );
	if( !read ) EndRead();
	return read;
}

#endif

void TIFFReader::ReadColor( RISEColor& c, const unsigned int x, const unsigned int y )
{
	if( pBuffer ) {
		RGBA8 p = {0};
		const unsigned int idx = y*bufW*4+x*4;
		uint32_t pixel = 0;
		std::memcpy( &pixel, pBuffer + idx, sizeof(pixel) );
#ifndef NO_TIFF_SUPPORT
		p.r = TIFFGetR( pixel );
		p.g = TIFFGetG( pixel );
		p.b = TIFFGetB( pixel );
		p.a = TIFFGetA( pixel );
#endif

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

void TIFFReader::EndRead( )
{
	if( pBuffer ) {
		GlobalLog()->PrintDelete( pBuffer, __FILE__, __LINE__ );
		delete [] pBuffer;
		pBuffer = 0;
	}
}
