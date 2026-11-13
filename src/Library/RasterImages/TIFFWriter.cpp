//////////////////////////////////////////////////////////////////////
//
//  TIFFWriter.cpp - Implementation of the TIFFWriter class
//                
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 4, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "TIFFWriter.h"
#include <memory>
#include <stdexcept>
#include <limits>
#include <new>
#include "../Interfaces/ILog.h"

#ifndef NO_TIFF_SUPPORT
	#include <tiffio.h>
#endif

#include "TIFFCommon.h"

using namespace RISE;
using namespace RISE::Implementation;

TIFFWriter::TIFFWriter( IWriteBuffer& buffer, const COLOR_SPACE color_space_ ) :
  pWriteBuffer( buffer ),
  pBuffer( 0 ),
  color_space( color_space_ )
{
	pWriteBuffer.addref();
}

TIFFWriter::~TIFFWriter( )
{
	pWriteBuffer.release();

	if( pBuffer ) {
		GlobalLog()->PrintDelete( pBuffer, __FILE__, __LINE__ );
		delete [] pBuffer;
		pBuffer = 0;
	}
}

void TIFFWriter::BeginWrite( const unsigned int width, const unsigned int height )
{
	if( pBuffer ) {
		GlobalLog()->PrintDelete( pBuffer, __FILE__, __LINE__ );
		delete [] pBuffer;
		pBuffer = nullptr;
	}
	if( width == 0 || height == 0 || width > std::numeric_limits<unsigned int>::max() / 4 / height ||
		width > std::numeric_limits<size_t>::max() / 4 / height ) {
		GlobalLog()->PrintEasyError( "TIFFWriter: Invalid image dimensions" );
		return;
	}
	const size_t bytes = static_cast<size_t>(width) * height * 4;
	pBuffer = new (std::nothrow) unsigned char[bytes];
	if( !pBuffer ) {
		GlobalLog()->PrintEasyError( "TIFFWriter: Could not allocate pixel buffer" );
		return;
	}
	bufW = width;
	bufH = height;
	GlobalLog()->PrintNew( pBuffer, __FILE__, __LINE__, "buffer" );

}

void TIFFWriter::WriteColor( const RISEColor& c, const unsigned int x, const unsigned int y )
{
	if( pBuffer && x < bufW && y < bufH ) {
		RGBA8	p;

		switch( color_space )
		{
		case eColorSpace_Rec709RGB_Linear:
			{
				// Linear with 8bpp?  The user is nuts...
				p = c.Integerize<Rec709RGBPel,unsigned char>(255.0);
			} break;
		case eColorSpace_sRGB:
			{
				p = c.Integerize<sRGBPel,unsigned char>(255.0);
			} break;
		case eColorSpace_ROMMRGB_Linear:
			{
				p = c.Integerize<ROMMRGBPel,unsigned char>(255.0);
			} break;
		case eColorSpace_ProPhotoRGB:
			{
				p = c.Integerize<ProPhotoRGBPel,unsigned char>(255.0);
			} break;
		}

		const size_t idx = (static_cast<size_t>(y)*bufW+x)*4;
		pBuffer[idx] = p.r;
		pBuffer[idx+1] = p.g;
		pBuffer[idx+2] = p.b;
		pBuffer[idx+3] = p.a;
	}
}

#ifdef NO_TIFF_SUPPORT

void TIFFWriter::EndWrite( )
{
	GlobalLog()->PrintSourceError( "TIFFWriter::EndWrite NO TIFF SUPPORT was compiled!  No output will be written", __FILE__, __LINE__ );
}

#else

void TIFFWriter::EndWrite( )
{
	if( pBuffer ) {

		std::unique_ptr<TIFF, decltype(&::TIFFClose)> tiff(
			TIFFClientOpen( "", "w", (thandle_t)&pWriteBuffer, TIFFReadDummy, TIFFWrite,
				TIFFSeekWriter, TIFFClose, TIFFSizeWriter, TIFFMapFile, TIFFUnmapFile ), &::TIFFClose );

		if( !tiff ) {
			throw std::runtime_error("TIFF output header write failed");
		}

		uint16_t alpha = EXTRASAMPLE_UNASSALPHA;
		if( !TIFFSetField(tiff.get(), TIFFTAG_IMAGEWIDTH, bufW) ||
			!TIFFSetField(tiff.get(), TIFFTAG_IMAGELENGTH, bufH) ||
			!TIFFSetField(tiff.get(), TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT) ||
			!TIFFSetField(tiff.get(), TIFFTAG_SAMPLESPERPIXEL, 4) ||
			!TIFFSetField(tiff.get(), TIFFTAG_BITSPERSAMPLE, 8) ||
			!TIFFSetField(tiff.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG) ||
			!TIFFSetField(tiff.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB) ||
			!TIFFSetField(tiff.get(), TIFFTAG_EXTRASAMPLES, 1, &alpha) ||
			!TIFFSetField(tiff.get(), TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE) )
			throw std::runtime_error("TIFF output metadata setup failed");

		for( unsigned int y=0; y<bufH; y++ ) {
			if( TIFFWriteScanline(tiff.get(), &pBuffer[static_cast<size_t>(y)*bufW*4], y, 0) < 0 )
				throw std::runtime_error("TIFF output scanline write failed");
		}

		if( !::TIFFFlush(tiff.get()) ) throw std::runtime_error("TIFF output directory write failed");

	} else {
		GlobalLog()->PrintSourceError( "TIFFWriter::EndWrite:: no MemBuffer or no local buffer", __FILE__, __LINE__ );
	}
}

#endif
