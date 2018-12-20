//////////////////////////////////////////////////////////////////////
//
//  TGAReader.cpp - Implementation of the TGAReader class
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
#include "TGAReader.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

TGAReader::TGAReader( IReadBuffer& buffer, const COLOR_SPACE color_space_ ) :
  pReadBuffer( buffer ),
  pBuffer( 0 ),
  color_space( color_space_ )
{
	pReadBuffer.addref();
}

TGAReader::~TGAReader( )
{
	pReadBuffer.release();

	if( pBuffer ) {
		GlobalLog()->PrintDelete( pBuffer, __FILE__, __LINE__ );
		delete [] pBuffer;
		pBuffer = 0;
	}
}

bool TGAReader::BeginRead( unsigned int& width, unsigned int& height )
{	
	struct MY_TGA_PRE_HEADER
	{
		unsigned char id;
		unsigned char color_map_type;
		unsigned char image_type;
		unsigned char junk1;
		unsigned int  junk2;

		unsigned short originX;
		unsigned short originY;
	} preheader = {0};

	pReadBuffer.seek( IBuffer::START, 0 );
	pReadBuffer.getBytes( (char*)&preheader, sizeof( MY_TGA_PRE_HEADER ) );

	width = pReadBuffer.getUWord();
	height = pReadBuffer.getUWord();

	unsigned char bpp = pReadBuffer.getUChar();
	
	// Check the bits per pixel and make sure it is 32, we won't load anything but 32...
	if( bpp != 32 ) {
		GlobalLog()->Print( eLog_Error, "TGAReader: Only 32bit TGAs are supported" );
		return false;
	}

	pBuffer = new unsigned char[width*height*4];
	GlobalLog()->PrintNew( pBuffer, __FILE__, __LINE__, "buffer" );
	memset( pBuffer, 0, width*height*4 );

	pReadBuffer.seek( IBuffer::START, 17 );

	// Read the entire image into memory, in backwards y order
	for( int y=height-1; y>=0; y-- ) {
		pReadBuffer.getBytes( (char*)&pBuffer[y*width*4], 4*width );
	}

	bufW = width;

	return true;
}

void TGAReader::ReadColor( RISEColor& c, const unsigned int x, const unsigned int y )
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

void TGAReader::EndRead( )
{
	if( pBuffer ) {
		GlobalLog()->PrintDelete( pBuffer, __FILE__, __LINE__ );
		delete [] pBuffer;
		pBuffer = 0;
	}
}
