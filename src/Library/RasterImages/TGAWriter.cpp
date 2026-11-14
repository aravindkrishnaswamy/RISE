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
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>

using namespace RISE;
using namespace RISE::Implementation;

TGAWriter::TGAWriter( IWriteBuffer& buffer, const COLOR_SPACE color_space_ ) :
  pWriteBuffer( buffer ),
  pBuffer( 0 ),
  bufW( 0 ),
  bufH( 0 ),
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

void TGAWriter::ValidateDimensions( const unsigned int width, const unsigned int height )
{
	if( !width || !height || width > std::numeric_limits<std::uint16_t>::max() ||
		height > std::numeric_limits<std::uint16_t>::max() ) {
		throw std::invalid_argument( "TGAWriter: Dimensions must be between 1 and 65535" );
	}
	constexpr unsigned int headerSize = 18;
	if( width > (std::numeric_limits<unsigned int>::max() - headerSize) / 4 / height ||
		width > (std::numeric_limits<size_t>::max() - headerSize) / 4 / height ) {
		throw std::length_error( "TGAWriter: Image exceeds the output buffer size limit" );
	}
}

void TGAWriter::BeginWrite( const unsigned int width, const unsigned int height )
{
	if( pBuffer ) {
		GlobalLog()->PrintDelete( pBuffer, __FILE__, __LINE__ );
		delete [] pBuffer;
		pBuffer = nullptr;
	}
	bufW = bufH = 0;
	ValidateDimensions(width, height);
	constexpr unsigned int headerSize = 18;
	const auto bytes = static_cast<size_t>(width) * height * 4;
	auto pixels = std::make_unique<unsigned char[]>( bytes );
	const unsigned char header[headerSize] = {
		0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		static_cast<unsigned char>(width), static_cast<unsigned char>(width >> 8),
		static_cast<unsigned char>(height), static_cast<unsigned char>(height >> 8), 32, 8
	};
	// Resize is advisory: disk and borrowed buffers may already have capacity.
	pWriteBuffer.Resize( static_cast<unsigned int>(bytes + headerSize) );
	if( !pWriteBuffer.setBytes(header, headerSize) ) {
		throw std::runtime_error( "TGAWriter: Failed to write image header" );
	}
	bufW = width;
	bufH = height;
	pBuffer = pixels.release();
	GlobalLog()->PrintNew( pBuffer, __FILE__, __LINE__, "buffer" );
}

void TGAWriter::WriteColor( const RISEColor& c, const unsigned int x, const unsigned int y )
{
	if( pBuffer && x < bufW && y < bufH ) {
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

		const auto idx = (static_cast<size_t>(y) * bufW + x) * 4;

		pBuffer[idx] = p.b;
		pBuffer[idx+1] = p.g;
		pBuffer[idx+2] = p.r;
		pBuffer[idx+3] = p.a;
	}
}

void TGAWriter::EndWrite( )
{
	// Write out the data to the memory buffer, in backward y order
	if( !pBuffer ) return;
	for( unsigned int y = bufH; y > 0; --y ) {
		const auto offset = static_cast<size_t>(y - 1) * bufW * 4;
		if( !pWriteBuffer.setBytes(pBuffer + offset, 4 * bufW) ) {
			throw std::runtime_error( "TGAWriter: Failed to write image pixels" );
		}
	}
}
