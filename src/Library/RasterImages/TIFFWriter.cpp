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
	// Setup the buffer
	bufW = width;
	bufH = height;
	pBuffer = new unsigned char[width*height*4];
	GlobalLog()->PrintNew( pBuffer, __FILE__, __LINE__, "buffer" );
}

void TIFFWriter::WriteColor( const RISEColor& c, const unsigned int x, const unsigned int y )
{
	if( pBuffer ) {
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

		const unsigned int idx = y*bufW*4+x*4;
		pBuffer[idx] = p.b;
		pBuffer[idx+1] = p.g;
		pBuffer[idx+2] = p.r;
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

		TIFF* tiff = TIFFClientOpen( "", "w", (thandle_t)&pWriteBuffer, TIFFReadDummy, TIFFWrite, TIFFSeekWriter, TIFFClose, TIFFSizeWriter, TIFFMapFile, TIFFUnmapFile );

		if( !tiff ) {
			GlobalLog()->PrintSourceError( "TIFFWriter::EndWrite:: Failed to create TIFF object", __FILE__, __LINE__ );
			return;
		}

		TIFFSetField( tiff, TIFFTAG_IMAGEWIDTH, bufW );
		TIFFSetField( tiff, TIFFTAG_IMAGELENGTH, bufH );
		TIFFSetField( tiff, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT );
		TIFFSetField( tiff, TIFFTAG_SAMPLESPERPIXEL, 4 );
		TIFFSetField( tiff, TIFFTAG_BITSPERSAMPLE, 8 );
		TIFFSetField( tiff, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG );
		TIFFSetField( tiff, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB );
		TIFFSetField( tiff, TIFFTAG_COMPRESSION, COMPRESSION_DEFLATE );

		for( unsigned int y=0; y<bufH; y++ ) {
			TIFFWriteScanline( tiff, &pBuffer[y*bufW*4], y, 0 );
		}

		TIFFClose( tiff );

	} else {
		GlobalLog()->PrintSourceError( "TIFFWriter::EndWrite:: no MemBuffer or no local buffer", __FILE__, __LINE__ );
	}
}

#endif
