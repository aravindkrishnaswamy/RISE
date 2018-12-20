//////////////////////////////////////////////////////////////////////
//
//  TGAWriter.cpp - Implementation of the TGAWriter class
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
#include "TGAWriter.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

TGAWriter::TGAWriter( IWriteBuffer& buffer, const COLOR_SPACE color_space_ ) :
  pWriteBuffer( buffer ),
  pBuffer( 0 ),
  color_space( color_space_ )
{
	pWriteBuffer.addref();
}

TGAWriter::~TGAWriter( )
{
	pWriteBuffer.release();

	if( pBuffer ) {
		GlobalLog()->PrintDelete( pBuffer, __FILE__, __LINE__ );
		delete [] pBuffer;
		pBuffer = 0;
	}
}

void TGAWriter::BeginWrite( const unsigned int width, const unsigned int height )
{
	// Write the header
	// Make sure there's enough room in the buffer first
	pWriteBuffer.Resize( width*height*4 + 18 );

	unsigned char b = 0;
	pWriteBuffer.seek( IBuffer::CUR, 0 );

	pWriteBuffer.setBytes( &b, 1 );		// ID length
	pWriteBuffer.setBytes( &b, 1 );		// Color Map Type
	b=2; pWriteBuffer.setBytes( &b, 1 );	// Image Type
	b=0;
	for( int i=0; i<5; i++ ) {
		pWriteBuffer.setBytes( &b, 1 );	// Color Map Specification
	}
	unsigned short s=0;
	pWriteBuffer.setUWord( s );
	pWriteBuffer.setUWord( s );
//	pWriteBuffer.setBytes( &s, 2);		// X origin
//	pWriteBuffer.setBytes( &s, 2);		// Y origin
	s = (unsigned short)(width);
//	pWriteBuffer.setBytes( &s, 2);		// X size
	pWriteBuffer.setUWord( s );
	s = (unsigned short)(height);
//	pWriteBuffer.setBytes( &s, 2);		// Y size
	pWriteBuffer.setUWord( s );
	b = 32;
	pWriteBuffer.setBytes( &b, 1 );		// bits / pixel
	b = 8;
	pWriteBuffer.setBytes( &b, 1 );		// Image descriptor

	// Setup the buffer
	bufW = width;
	bufH = height;
	pBuffer = new unsigned char[width*height*4];
	GlobalLog()->PrintNew( pBuffer, __FILE__, __LINE__, "buffer" );
}

void TGAWriter::WriteColor( const RISEColor& c, const unsigned int x, const unsigned int y )
{
	if( pBuffer ) {
		RGBA8	p;

		switch( color_space )
		{
		case eColorSpace_Rec709RGB_Linear:
			{
				// Linear with 8bpp?  The user is nuts!
				p = c.Integerize<Rec709RGBPel,unsigned char>(255.0);
			} break;
		default:
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

void TGAWriter::EndWrite( )
{
	// Write out the data to the memory buffer, in backward y order
	for( int y=bufH-1; y>=0; y-- ) {
		pWriteBuffer.setBytes( (void*)&pBuffer[y*bufW*4], 4*bufW );
	}
}
