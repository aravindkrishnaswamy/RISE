//////////////////////////////////////////////////////////////////////
//
//  PNGReader.cpp - Implementation of the PNGReader class
//                
//  Author: Aravind Krishnaswamy
//  Date of Birth: August 13, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "PNGReader.h"
#include "../Interfaces/ILog.h"
#include <limits>
#include <new>

#ifndef NO_PNG_SUPPORT
	#include <png.h>
#endif

using namespace RISE;
using namespace RISE::Implementation;

PNGReader::PNGReader( IReadBuffer& buffer, const COLOR_SPACE color_space_ ) :
  pReadBuffer( buffer ),
  buffer_start_pos( BufferPosition(buffer) ),
  pBuffer( 0 ),
  bufW( 0 ),
  bufH( 0 ),
  bAlphaInfo( false ),
  bpp( 0 ),
  color_space( color_space_ )
{
	pReadBuffer.addref();
}

PNGReader::~PNGReader( )
{
	EndRead();
	pReadBuffer.release();
}

#ifdef NO_PNG_SUPPORT

bool PNGReader::BeginRead( unsigned int& width, unsigned int& height )
{
	EndRead();
	width = height = 0;
	GlobalLog()->PrintSourceError( "PNGReader::BeginRead NO PNG SUPPORT was compiled!  Nothing will be read", __FILE__, __LINE__ );
	return false;
}

#else

static void
png_read_data(png_structp png_ptr, png_bytep data, png_size_t length)
{
	IReadBuffer*	pBuffer = (IReadBuffer*)png_get_io_ptr(png_ptr);

	if( !pBuffer || length > std::numeric_limits<unsigned int>::max() ||
		!pBuffer->getBytes( data, static_cast<unsigned int>(length) ) ) {
		png_error( png_ptr, "Truncated PNG data" );
	}
}

bool PNGReader::BeginRead( unsigned int& width, unsigned int& height )
{
	EndRead();
	width = height = 0;
	if( buffer_start_pos > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
		!BufferSeek(pReadBuffer, IBuffer::START, static_cast<std::int64_t>(buffer_start_pos)) ||
		BufferBytesAvailable( pReadBuffer ) == 0 ) {
		GlobalLog()->PrintSourceError( "PNGReader::BeginRead:: Passed empty buffer to read from", __FILE__, __LINE__ );
		return false;
	}
	
	png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);

	if( !png_ptr ) {
		GlobalLog()->PrintSourceError( "PNGReader:: Could not allocate structure", __FILE__, __LINE__ );
		return false;
	}

	png_infop info_ptr = png_create_info_struct(png_ptr);

	if( !info_ptr ) {
		png_destroy_read_struct( &png_ptr, nullptr, nullptr );
		GlobalLog()->PrintSourceError( "PNGReader:: Could not create info structure", __FILE__, __LINE__ );
		return false;
	}

	if( setjmp( png_jmpbuf( png_ptr ) ) ) {
		png_destroy_read_struct( &png_ptr, &info_ptr, nullptr );
		EndRead();
		return false;
	}
	
	// Set a read function to read from our buffer rather than file
	png_set_read_fn( png_ptr, (void*)&pReadBuffer, png_read_data );
	png_set_bgr(png_ptr);
	png_read_info(png_ptr, info_ptr);

	/* \\TODO Properly read gamma information
	if (info_ptr->valid & PNG_INFO_gAMA) {
		png_set_gamma(png_ptr, screen_gamma, info_ptr->gamma);
	} else {
		png_set_gamma(png_ptr, screen_gamma, 0.45);
	}
	*/


	// Get the header information
	png_uint_32 pngwidth, pngheight;
	int bit_depth, color_type, interlace_method;
	png_get_IHDR(png_ptr, info_ptr,	&pngwidth, &pngheight,
				&bit_depth, &color_type, &interlace_method, NULL, NULL);


	if (color_type == PNG_COLOR_TYPE_GRAY ||
		color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
		png_set_gray_to_rgb(png_ptr);
	}

	if( color_type == PNG_COLOR_TYPE_PALETTE ) {
		png_set_palette_to_rgb( png_ptr );
	}
	if( bit_depth < 8 && color_type == PNG_COLOR_TYPE_GRAY ) {
		png_set_expand_gray_1_2_4_to_8( png_ptr );
	}
	if( png_get_valid( png_ptr, info_ptr, PNG_INFO_tRNS ) ) {
		png_set_tRNS_to_alpha( png_ptr );
	}

	const int passes = png_set_interlace_handling( png_ptr );
	png_read_update_info(png_ptr, info_ptr);
	const int decodedDepth = png_get_bit_depth( png_ptr, info_ptr );
	const int channels = png_get_channels( png_ptr, info_ptr );

	// Get the gamma from the file
//	png_get_gAMA(png_ptr, info_ptr, &png_file_gamma);
	// We don't care about the file gamma, PNG's whole imlementation of this is bullshit

	// Guard the destination-buffer sizing against a header-driven integer
	// overflow: width/height come from the PNG IHDR and libpng's default
	// limits allow up to 1,000,000 each, so the historical 32-bit
	// `height*image_stride` product can wrap to a tiny allocation that
	// png_read_row then writes past (heap OOB write).  Compute the stride
	// and total in 64-bit and cap at a sane ceiling; reject (releasing the
	// libpng structs) before allocating on overflow or unsupported layout.
	const auto image_stride = png_get_rowbytes( png_ptr, info_ptr );
	static const unsigned long long kMaxImageBytes = 2ULL * 1024 * 1024 * 1024; // 2 GiB ceiling
	if( (decodedDepth != 8 && decodedDepth != 16) || (channels != 3 && channels != 4) ||
		pngwidth == 0 || pngheight == 0 ||
		image_stride != static_cast<unsigned long long>(pngwidth) * channels * (decodedDepth/8) ||
		image_stride == 0 || image_stride > kMaxImageBytes / pngheight ||
		image_stride > std::numeric_limits<size_t>::max() / pngheight ) {
		GlobalLog()->Print( eLog_Error, "PNGReader: Invalid or too-large image dimensions" );
		png_destroy_read_struct( &png_ptr, &info_ptr, (png_infopp)NULL );
		return false;
	}
	const size_t nBytes = image_stride * pngheight;

	pBuffer = new (std::nothrow) unsigned char[nBytes];
	if( !pBuffer ) {
		png_destroy_read_struct( &png_ptr, &info_ptr, nullptr );
		GlobalLog()->PrintEasyError( "PNGReader: Could not allocate pixel buffer" );
		return false;
	}
	GlobalLog()->PrintNew( pBuffer, __FILE__, __LINE__, "buffer" );
	memset( pBuffer, 0, nBytes );          // full allocation (was width*height*nPngChannels — undersized for 16-bit depth)

	// Read scanline by scanline. Keep this setjmp region free of automatic
	// objects with non-trivial destructors: libpng errors longjmp directly
	// to the cleanup above. Pending pixels live in the reader until commit.
	for( int pass=0; pass<passes; ++pass ) {
		for( unsigned int y=0; y<pngheight; y++ ) {
			png_read_row(png_ptr, &pBuffer[(size_t)y*image_stride], NULL);
		}
	}

	// Close
	png_read_end( png_ptr, info_ptr );
	png_destroy_read_struct(&png_ptr, &info_ptr, (png_infopp)NULL);

	bufW = width = pngwidth;
	bufH = height = pngheight;
	bpp = static_cast<unsigned char>( decodedDepth );
	bAlphaInfo = channels == 4;
	return true;
}

#endif

void PNGReader::ReadColor( RISEColor& c, const unsigned int x, const unsigned int y )
{
	if( pBuffer && x < bufW && y < bufH ) {
		switch( bpp ) {
		case 8:
			{
				const size_t idx = (static_cast<size_t>(y) * bufW + x) * (bAlphaInfo ? 4 : 3);
				RGBA8 p = {0};
				p.b = pBuffer[idx];
				p.g = pBuffer[idx+1];
				p.r = pBuffer[idx+2];
				if( bAlphaInfo ) {
					p.a = pBuffer[idx+3];
				} else {
					p.a = 0xFF;
				}
		
				switch( color_space )
				{
				case eColorSpace_Rec709RGB_Linear:
					{
						// Do nothing
						c.SetFromIntegerized<Rec709RGBPel,unsigned char>( p, 255.0 );
					} break;
				case eColorSpace_sRGB:
					{
						// User is saying that the person who wrote this PNG is a crack monkey
						// and didn't write the gAMA chunk properly, so force it to be sRGB
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
			break;
		case 16:
			{
				RGBA16	p = {0};

				const size_t idx = (static_cast<size_t>(y) * bufW + x) * (bAlphaInfo ? 8 : 6);

				// libpng leaves 16-bit samples in PNG's network byte order.
				const auto readSample = [this]( size_t offset ) {
					return static_cast<unsigned short>( (pBuffer[offset] << 8) | pBuffer[offset+1] );
				};
				p.b = readSample( idx );
				p.g = readSample( idx+2 );
				p.r = readSample( idx+4 );
				p.a = bAlphaInfo ? readSample( idx+6 ) : 0xFFFF;
				switch( color_space )
				{
				case eColorSpace_Rec709RGB_Linear:
					{
						// Do nothing
						c.SetFromIntegerized<Rec709RGBPel,unsigned short>( p, 65535.0 );
					} break;
				case eColorSpace_sRGB:
					{
						// User is saying that the person who wrote this PNG is a crack monkey
						// and didn't write the gAMA chunk properly, so force it to be sRGB
						c.SetFromIntegerized<sRGBPel,unsigned short>( p, 65535.0 );
					} break;
				case eColorSpace_ROMMRGB_Linear:
					{
						c.SetFromIntegerized<ROMMRGBPel,unsigned short>( p, 65535.0 );
					} break;

				case eColorSpace_ProPhotoRGB:
					{
						c.SetFromIntegerized<ProPhotoRGBPel,unsigned short>( p, 65535.0 );
					} break;
				}
			}
			break;
		};
	}
}

void PNGReader::EndRead( )
{
	if( pBuffer ) {
		GlobalLog()->PrintDelete( pBuffer, __FILE__, __LINE__ );
		delete [] pBuffer;
		pBuffer = 0;
	}
	bufW = bufH = 0;
	bAlphaInfo = false;
	bpp = 0;
}
