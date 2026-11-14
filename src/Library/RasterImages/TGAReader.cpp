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
#include "../Utilities/LittleEndian.h"
#include <algorithm>
#include <cstdint>
#include <memory>
#include <new>

using namespace RISE;
using namespace RISE::Implementation;

TGAReader::TGAReader( IReadBuffer& buffer, const COLOR_SPACE color_space_ ) :
  pReadBuffer( buffer ),
  pBuffer( 0 ),
  bufW( 0 ),
  bufH( 0 ),
  color_space( color_space_ )
{
	pReadBuffer.addref();
}

TGAReader::~TGAReader( )
{
	EndRead();
	pReadBuffer.release();
}

bool TGAReader::BeginRead( unsigned int& width, unsigned int& height )
{
	EndRead();
	width = height = 0;
	unsigned char header[18];
	if( !BufferSeek(pReadBuffer, IBuffer::START, 0) || !pReadBuffer.getBytes(header, sizeof(header)) ) {
		GlobalLog()->PrintEasyError( "TGAReader: Truncated image header" );
		return false;
	}
	const auto descriptor = header[17];
	const auto alphaBits = descriptor & 15;
	if( header[1] > 1 || header[2] != 2 || header[16] != 32 ||
		(alphaBits != 0 && alphaBits != 8) || (descriptor & 0xc0) != 0 ) {
		GlobalLog()->PrintEasyError( "TGAReader: Only uncompressed truecolor 32-bit TGAs with 0 or 8 alpha bits are supported; indexed images, 24-bit pixels, RLE, and interleaving are unsupported" );
		return false;
	}
	const unsigned int imageWidth = DecodeLittleEndian<unsigned short, std::uint16_t>(header + 12);
	const unsigned int imageHeight = DecodeLittleEndian<unsigned short, std::uint16_t>(header + 14);
	const auto byteCount = static_cast<std::uint64_t>(imageWidth) * imageHeight * 4;
	constexpr std::uint64_t maxImageBytes = 2ULL * 1024 * 1024 * 1024;
	if( !imageWidth || !imageHeight || byteCount > maxImageBytes ) {
		GlobalLog()->PrintEasyError( "TGAReader: Invalid or oversized image dimensions" );
		return false;
	}
	// Truecolor images may carry an unused palette. Its declared storage
	// still precedes the pixels, after the image ID; no palette lookup occurs.
	const auto mapEntries = DecodeLittleEndian<unsigned short, std::uint16_t>(header + 5);
	const auto mapDepth = header[7];
	if( header[1] && mapEntries && mapDepth != 15 && mapDepth != 16 && mapDepth != 24 && mapDepth != 32 ) {
		GlobalLog()->PrintEasyError("TGAReader: Unsupported unused color-map entry size");
		return false;
	}
	const std::uint64_t mapBytes = header[1] ? static_cast<std::uint64_t>(mapEntries) * ((mapDepth + 7u) / 8u) : 0;
	const std::uint64_t metadataBytes = header[0] + mapBytes;
	const auto available = BufferBytesAvailable(pReadBuffer);
	if( metadataBytes > available || byteCount > available - metadataBytes ) {
		GlobalLog()->PrintEasyError( "TGAReader: Truncated image ID, color map or pixels" );
		return false;
	}
	if( !BufferSeek(pReadBuffer, IBuffer::CUR, static_cast<std::int64_t>(metadataBytes)) ) return false;
	// Publish only after every row is read, so the temporary storage needs
	// no initialization. Preserve the reader's recoverable allocation failure.
	std::unique_ptr<unsigned char[]> pixels(new (std::nothrow) unsigned char[static_cast<size_t>(byteCount)]);
	if( !pixels ) {
		GlobalLog()->PrintEasyError("TGAReader: Could not allocate pixel buffer");
		return false;
	}
	const auto rowBytes = 4 * imageWidth;
	for( unsigned int fileY = 0; fileY < imageHeight; ++fileY ) {
		const auto y = (descriptor & 0x20) ? fileY : imageHeight - fileY - 1;
		auto* row = pixels.get() + static_cast<size_t>(y) * rowBytes;
		if( !pReadBuffer.getBytes(row, rowBytes) ) {
			GlobalLog()->PrintEasyError( "TGAReader: Failed to read image pixels" );
			return false;
		}
		if( descriptor & 0x10 ) {
			for( unsigned int x = 0; x < imageWidth / 2; ++x ) {
				std::swap_ranges(row + 4 * x, row + 4 * (x + 1), row + 4 * (imageWidth - x - 1));
			}
		}
		if( alphaBits == 0 ) {
			for( unsigned int x = 0; x < imageWidth; ++x ) row[4 * x + 3] = 255;
		}
	}
	pBuffer = pixels.release();
	GlobalLog()->PrintNew( pBuffer, __FILE__, __LINE__, "TGA pixels" );
	bufW = width = imageWidth;
	bufH = height = imageHeight;
	return true;
}

void TGAReader::ReadColor( RISEColor& c, const unsigned int x, const unsigned int y )
{
	if( pBuffer && x < bufW && y < bufH ) {
		RGBA8 p = {0};

		const auto idx = (static_cast<size_t>(y) * bufW + x) * 4;

		p.b = pBuffer[idx];
		p.g = pBuffer[idx+1];
		p.r = pBuffer[idx+2];
		p.a = pBuffer[idx+3];

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
	bufW = bufH = 0;
}
