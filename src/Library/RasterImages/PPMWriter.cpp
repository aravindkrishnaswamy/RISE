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
#include <limits>
#include <memory>
#include <stdexcept>
#include "../Utilities/Math3D/Math3D.h"
#include "../Utilities/Color/Color.h"
#include "../Interfaces/ILog.h"
#include "PPMWriter.h"

using namespace RISE;
using namespace RISE::Implementation;

PPMWriter::PPMWriter( IWriteBuffer& buffer, const COLOR_SPACE color_space_ ) :
  pWriteBuffer( buffer ),
  pBuffer( 0 ),
  bufW( 0 ),
  bufH( 0 ),
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
	if( pBuffer ) {
		GlobalLog()->PrintDelete( pBuffer, __FILE__, __LINE__ );
		delete [] pBuffer;
		pBuffer = nullptr;
	}
	bufW = bufH = 0;
	if( !width || !height ) throw std::invalid_argument("PPMWriter: Dimensions must be nonzero");
	char header[64];
	const int headerLength=snprintf(header,sizeof(header),"P6\n%u %u\n255\n",width,height);
	if( headerLength<=0 || static_cast<size_t>(headerLength)>=sizeof(header) )
		throw std::runtime_error("PPMWriter: Could not format image header");
	const auto headerBytes=static_cast<unsigned int>(headerLength);
	if( width>(std::numeric_limits<unsigned int>::max()-headerBytes)/3/height ||
		width>(std::numeric_limits<size_t>::max()-headerBytes)/3/height )
		throw std::length_error("PPMWriter: Image exceeds the output buffer size limit");
	const auto bytes=static_cast<size_t>(width)*height*3;
	// Allocate before mutating output. A failed replacement leaves no active
	// image, and the temporary pixels are reclaimed if writing the header fails.
	auto pixels=std::make_unique<unsigned char[]>(bytes);
	// Resize is advisory: borrowed buffers may already have adequate capacity.
	pWriteBuffer.Resize(static_cast<unsigned int>(bytes+headerBytes));
	if( !pWriteBuffer.setBytes(header,headerBytes) )
		throw std::runtime_error("PPMWriter: Failed to write image header");
	bufW = width;
	bufH = height;
	pBuffer = pixels.release();
	GlobalLog()->PrintNew( pBuffer, __FILE__, __LINE__, "buffer" );
}

void PPMWriter::WriteColor( const RISEColor& c, const unsigned int x, const unsigned int y )
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

		// P6 has no alpha channel. Preserve the historical straight RGB bytes:
		// the old ignored-return premultiplication call never modified p.
		const auto idx=(static_cast<size_t>(y)*bufW+x)*3;
		pBuffer[idx] = (unsigned char)p.r;
		pBuffer[idx+1] = (unsigned char)p.g;
		pBuffer[idx+2] = (unsigned char)p.b;
	}
}

void PPMWriter::EndWrite( )
{
	if( !pBuffer ) return;
	const auto bytes=static_cast<size_t>(bufW)*bufH*3;
	if( !pWriteBuffer.setBytes(pBuffer,static_cast<unsigned int>(bytes)) )
		throw std::runtime_error("PPMWriter: Failed to write image pixels");
}
