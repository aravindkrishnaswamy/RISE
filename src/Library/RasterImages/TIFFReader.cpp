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
		uint32 w, h;
		TIFFGetField( tiff, TIFFTAG_IMAGEWIDTH, &w );
		TIFFGetField( tiff, TIFFTAG_IMAGELENGTH, &h );

		width = w;
		height = h;

		bufW = width;
	}

	pBuffer = new unsigned char[height*width*4];
	GlobalLog()->PrintNew( pBuffer, __FILE__, __LINE__, "buffer" );
	memset( pBuffer, 0, width*height*4 );

	TIFFReadRGBAImage( tiff, width, height, (uint32*)pBuffer, 0 );
	TIFFClose( tiff );

	return true;
}

#endif

void TIFFReader::ReadColor( RISEColor& c, const unsigned int x, const unsigned int y )
{
	if( pBuffer ) {
		RGBA8 p = {0};
		const unsigned int idx = y*bufW*4+x*4;
		p.a = pBuffer[idx];
		p.b = pBuffer[idx+1];
		p.g = pBuffer[idx+2];
		p.r = pBuffer[idx+3];

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
