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

#ifndef NO_PNG_SUPPORT
	#include <png.h>
#endif

using namespace RISE;
using namespace RISE::Implementation;

PNGReader::PNGReader( IReadBuffer& buffer, const COLOR_SPACE color_space_ ) :
  pReadBuffer( buffer ),
  pBuffer( 0 ),
  color_space( color_space_ )
{
	pReadBuffer.addref();
}

PNGReader::~PNGReader( )
{
	pReadBuffer.release();

	if( pBuffer ) {
		GlobalLog()->PrintDelete( pBuffer, __FILE__, __LINE__ );
		delete [] pBuffer;
		pBuffer = 0;
	}
}

#ifdef NO_PNG_SUPPORT

bool PNGReader::BeginRead( unsigned int& width, unsigned int& height )
{
	GlobalLog()->PrintSourceError( "PNGReader::BeginRead NO PNG SUPPORT was compiled!  Nothing will be read", __FILE__, __LINE__ );
	return false;
}

#else

static void
png_read_data(png_structp png_ptr, png_bytep data, png_size_t length)
{
	IReadBuffer*	pBuffer = (IReadBuffer*)png_get_io_ptr(png_ptr);

	if( pBuffer ) {
		pBuffer->getBytes( (char*)data, static_cast<unsigned int>(length) );
	} else {
		GlobalLog()->PrintSourceError( "png_read_data (callback func):: bad MemoryBuffer", __FILE__, __LINE__ );
	}
}

bool PNGReader::BeginRead( unsigned int& width, unsigned int& height )
{
	if( pReadBuffer.Size() == 0 ) {
		GlobalLog()->PrintSourceError( "PNGReader::BeginRead:: Passed empty buffer to read from", __FILE__, __LINE__ );
		return false;
	}
	
	png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);

	if( !png_ptr ) {
		GlobalLog()->PrintSourceError( "PNGReader:: Could not allocate structure", __FILE__, __LINE__ );
	}

	png_infop info_ptr = png_create_info_struct(png_ptr);

	if( !info_ptr ) {
		png_destroy_write_struct(&png_ptr, NULL);
		GlobalLog()->PrintSourceError( "PNGReader:: Could not create info structure", __FILE__, __LINE__ );
	}

//	if( setjmp( png_ptr->jumpbuf ) )
//	{
//		png_destroy_write_struct(&png_ptr, info_ptr);
//		GlobalLog()->PrintSourceError( "PNGWriter:: Could not setjmp", __FILE__, __LINE__ );
//	}
	
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

	bpp = bit_depth;

	if (color_type == PNG_COLOR_TYPE_GRAY ||
		color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
		png_set_gray_to_rgb(png_ptr);
	}

	bAlphaInfo = false;

	if (color_type == PNG_COLOR_TYPE_RGBA) {
		bAlphaInfo = true;
	}
	
	int nPngChannels = png_get_channels(png_ptr, info_ptr);

	png_read_update_info(png_ptr, info_ptr);

	// Get the gamma from the file
//	png_get_gAMA(png_ptr, info_ptr, &png_file_gamma);
	// We don't care about the file gamma, PNG's whole imlementation of this is bullshit

	width = pngwidth;
	height = pngheight;

	// Guard the destination-buffer sizing against a header-driven integer
	// overflow: width/height come from the PNG IHDR and libpng's default
	// limits allow up to 1,000,000 each, so the historical 32-bit
	// `height*image_stride` product can wrap to a tiny allocation that
	// png_read_row then writes past (heap OOB write).  Compute the stride
	// and total in 64-bit and cap at a sane ceiling; reject (releasing the
	// libpng structs) before allocating on overflow / degenerate size.  A
	// sub-8-bit depth makes (bit_depth>>3)==0 -> stride 0 -> total 0, which
	// ReadColor cannot represent anyway, so it is rejected here too.
	const unsigned long long stride64 =
		(unsigned long long)width * (unsigned long long)nPngChannels * (unsigned long long)(bit_depth>>3);
	const unsigned long long total64 = (unsigned long long)height * stride64;
	static const unsigned long long kMaxImageBytes = 2ULL * 1024 * 1024 * 1024; // 2 GiB ceiling
	if( total64 == 0 || total64 > kMaxImageBytes ) {
		GlobalLog()->Print( eLog_Error, "PNGReader: Invalid or too-large image dimensions" );
		png_destroy_read_struct( &png_ptr, &info_ptr, (png_infopp)NULL );
		return false;
	}
	const size_t image_stride = (size_t)stride64;
	const size_t nBytes = (size_t)total64;

	pBuffer = new unsigned char[nBytes];
	GlobalLog()->PrintNew( pBuffer, __FILE__, __LINE__, "buffer" );
	memset( pBuffer, 0, nBytes );          // full allocation (was width*height*nPngChannels — undersized for 16-bit depth)

	// Read scanline by scanline
	for( unsigned int y=0; y<height; y++ ) {
		png_read_row(png_ptr, &pBuffer[(size_t)y*image_stride], NULL);
	}

	// Close
	png_read_end( png_ptr, info_ptr );
	png_destroy_read_struct(&png_ptr, &info_ptr, (png_infopp)NULL);

	bufW = width;

	return true;
}

#endif

void PNGReader::ReadColor( RISEColor& c, const unsigned int x, const unsigned int y )
{
	if( pBuffer ) {
		switch( bpp ) {
		case 8:
			{
				const unsigned int idx = bAlphaInfo?(y*bufW*4+x*4):(y*bufW*3+x*3);
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

				const unsigned int idx = bAlphaInfo?(y*bufW*8+x*8):(y*bufW*6+x*6);

#ifdef RISE_BIG_ENDIAN
				p.b = *((unsigned short*)(&pBuffer[idx]));
				p.g = *((unsigned short*)(&pBuffer[idx+2]));
				p.r = *((unsigned short*)(&pBuffer[idx+4]));

				if( bAlphaInfo ) {
					p.a = *((unsigned short*)(&pBuffer[idx+6]));
				} else {
					p.a = 0xFFFF;
				}
#else
				// Byte order in PNG is flipped!  Its little endian!
				p.b = ((pBuffer[idx]&0xFF) | ((pBuffer[idx+1]<<8)&0xFF00));
				p.g = ((pBuffer[idx+2]&0xFF) | ((pBuffer[idx+3]<<8)&0xFF00));
				p.r = ((pBuffer[idx+4]&0xFF) | ((pBuffer[idx+5]<<8)&0xFF00));

				if( bAlphaInfo ) {
					p.a = ((pBuffer[idx+6]&0xFF) | ((pBuffer[idx+7]<<8)&0xFF00));
				} else {
					p.a = 0xFFFF;
				}
#endif
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
}
