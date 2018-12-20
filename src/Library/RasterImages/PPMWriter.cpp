//////////////////////////////////////////////////////////////////////
//
//  PPMWriter.cpp - Implementation of the TGAWriter class
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
#include <stdio.h>
#include <string.h>
#include "../Utilities/Math3D/Math3D.h"
#include "../Utilities/Color/Color.h"
#include "../Interfaces/ILog.h"
#include "PPMWriter.h"

using namespace RISE;
using namespace RISE::Implementation;

PPMWriter::PPMWriter( IWriteBuffer& buffer, const COLOR_SPACE color_space_ ) :
  pWriteBuffer( buffer ),
  pBuffer( 0 ),
  color_space( color_space_ )
{
	pWriteBuffer.addref();
}

PPMWriter::~PPMWriter( )
{
	pWriteBuffer.release();

	if( pBuffer ) {
		GlobalLog()->PrintDelete( pBuffer, __FILE__, __LINE__ );
		delete [] pBuffer;
		pBuffer = 0;
	}
}

void PPMWriter::BeginWrite( const unsigned int width, const unsigned int height )
{
	// Write the header
	// Get a string with the header
	char	header[256] = {0};
	sprintf( header, "P6\n%d %d\n255\n", width, height );

	// Make sure the buffer is the correct size
	pWriteBuffer.Resize( strlen( header ) + width*height*3 );

	// Copy the header
	pWriteBuffer.setBytes( header, strlen( header ) );
	
	bufW = width;
	bufH = height;
	pBuffer = new unsigned char[width*height*3];
	GlobalLog()->PrintNew( pBuffer, __FILE__, __LINE__, "buffer" );
}

void PPMWriter::WriteColor( const RISEColor& c, const unsigned int x, const unsigned int y )
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

		ColorUtils::PremultiplyAlphaRGB<RGBA8,0xFF>( p );
		const unsigned int idx = y*bufW*3+x*3;
		pBuffer[idx] = (unsigned char)p.r;
		pBuffer[idx+1] = (unsigned char)p.g;
		pBuffer[idx+2] = (unsigned char)p.b;
	}
}

void PPMWriter::EndWrite( )
{
	pWriteBuffer.setBytes( pBuffer, 3*bufW*bufH );
}
