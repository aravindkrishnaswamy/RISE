//////////////////////////////////////////////////////////////////////
//
//  PNGWriter.cpp - Implementation of the PNGWriter class
//                
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
#include "PNGWriter.h"
#include "../Interfaces/ILog.h"
#include <limits>
#include <new>
#include <stdexcept>
#ifndef NO_PNG_SUPPORT
	#include <png.h>
	#include <zlib.h>
#endif

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	thread_local bool failPNGWriteAfterHeader = false;
}

bool RISE::Implementation::ForTest_SetPNGWriteFailureAfterHeader( bool enabled )
{
	const bool previous = failPNGWriteAfterHeader;
	failPNGWriteAfterHeader = enabled;
	return previous;
}

bool RISE::Implementation::ForTest_PNGWriteFailureAfterHeader()
{
	return failPNGWriteAfterHeader;
}

PNGWriter::PNGWriter( IWriteBuffer& buffer, const unsigned char bpp_, const COLOR_SPACE color_space_ ) :
  pWriteBuffer( buffer ),
  pBuffer( 0 ),
  bufW( 0 ),
  bufH( 0 ),
  bpp( bpp_ ),
  color_space( color_space_ )
{
	pWriteBuffer.addref();

	if( bpp != 8 && bpp != 16 ) {
		GlobalLog()->PrintEx( eLog_Warning, "PNGWriter:: Invalid bpp '%d', supported values are 8, 16, defaulting to 8", bpp );
		bpp = 8;
	}
}

PNGWriter::~PNGWriter( )
{
	pWriteBuffer.release();

	if( pBuffer ) {
		GlobalLog()->PrintDelete( pBuffer, __FILE__, __LINE__ );
		delete [] pBuffer;
		pBuffer = 0;
	}
}

void PNGWriter::BeginWrite( const unsigned int width, const unsigned int height )
{
	if( pBuffer ) {
		GlobalLog()->PrintDelete( pBuffer, __FILE__, __LINE__ );
		delete [] pBuffer;
		pBuffer = nullptr;
	}
	bufW = bufH = 0;
	const size_t pixelBytes = 4 * (bpp >> 3);
	if( width == 0 || height == 0 || width > 0x7fffffffU || height > 0x7fffffffU ||
		width > std::numeric_limits<size_t>::max() / pixelBytes / height ) {
		throw std::runtime_error( "PNGWriter: Invalid image dimensions" );
	}
	pBuffer = new (std::nothrow) unsigned char[static_cast<size_t>(width) * height * pixelBytes];
	if( !pBuffer ) {
		throw std::bad_alloc();
	}
	GlobalLog()->PrintNew( pBuffer, __FILE__, __LINE__, "buffer" );
	bufW = width;
	bufH = height;
}

void PNGWriter::WriteColor( const RISEColor& c, const unsigned int x, const unsigned int y )
{
	if( pBuffer && x < bufW && y < bufH ) {
		switch( bpp ) {
		case 8:
			{
				RGBA8	p;

				switch( color_space )
				{
				case eColorSpace_Rec709RGB_Linear:
					{
						// Linear with 8 bpp?  The user is nuts
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

				const size_t idx = (static_cast<size_t>(y)*bufW+x)*4;

				pBuffer[idx] = p.b;
				pBuffer[idx+1] = p.g;
				pBuffer[idx+2] = p.r;
				pBuffer[idx+3] = p.a;

			}
			break;
		case 16:
			{
				RGBA16	p;

				switch( color_space )
				{
				case eColorSpace_Rec709RGB_Linear:
					{
						p = c.Integerize<Rec709RGBPel,unsigned short>(65535.0);
					} break;
				case eColorSpace_sRGB:
					{
						p = c.Integerize<sRGBPel,unsigned short>(65535.0);
					} break;
				case eColorSpace_ROMMRGB_Linear:
					{
						p = c.Integerize<ROMMRGBPel,unsigned short>(65535.0);
					} break;
				case eColorSpace_ProPhotoRGB:
					{
						p = c.Integerize<ProPhotoRGBPel,unsigned short>(65535.0);
					} break;
				}

				const size_t idx = (static_cast<size_t>(y)*bufW+x)*8;

				// PNG stores 16-bit samples most significant byte first.
				const unsigned short samples[] = { p.b, p.g, p.r, p.a };
				for( unsigned int i = 0; i < 4; ++i ) {
					pBuffer[idx + i*2] = static_cast<unsigned char>(samples[i] >> 8);
					pBuffer[idx + i*2 + 1] = static_cast<unsigned char>(samples[i]);
				}
			}
			break;
		};
	}
}

#ifdef NO_PNG_SUPPORT

void PNGWriter::EndWrite( )
{
	GlobalLog()->PrintSourceError( "PNGWriter::EndWrite NO PNG SUPPORT was compiled!  No output will be written", __FILE__, __LINE__ );
	throw std::runtime_error( "PNG encoding is unavailable: NO_PNG_SUPPORT" );
}

#else

static void
png_write_data(png_structp png_ptr, png_bytep data, png_size_t length)
{
	IWriteBuffer*	pBuffer = (IWriteBuffer*)png_get_io_ptr(png_ptr);

	bool written = false;
	try {
		written = pBuffer && length <= std::numeric_limits<unsigned int>::max() &&
			pBuffer->setBytes( data, static_cast<unsigned int>(length) );
	} catch( ... ) {
		// Leave the catch before invoking libpng's longjmp cleanup path.
	}
	if( !written ) png_error( png_ptr, "PNG output write failed" );
}

static void
png_flush(png_structp /*png_ptr*/)
{
	// No flush action required!
}

void PNGWriter::EndWrite( )
{
	if( pBuffer )
	{
		// Setup the png structure info
		png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);

		if( !png_ptr ) {
			throw std::runtime_error( "PNGWriter: Could not allocate PNG structure" );
		}

		png_infop info_ptr = png_create_info_struct(png_ptr);

		if( !info_ptr ) {
			png_destroy_write_struct(&png_ptr, NULL);
			throw std::runtime_error( "PNGWriter: Could not allocate PNG info structure" );
		}

		if( setjmp( png_jmpbuf(png_ptr) ) ) {
			png_destroy_write_struct( &png_ptr, &info_ptr );
			throw std::runtime_error( "PNGWriter: libpng encoding failed" );
		}

		// Set a write function to write to our buffer rather than a file
		png_set_write_fn( png_ptr, (void*)&pWriteBuffer, png_write_data, png_flush );

		png_set_filter(png_ptr, 0, PNG_NO_FILTERS);
		png_set_compression_level(png_ptr, Z_BEST_COMPRESSION);
		png_set_bgr(png_ptr);

		// WriteColor applies the selected transfer function at either depth.
		// Preserve the existing approximate gAMA convention for that space.
		switch( color_space )
		{
		case eColorSpace_ROMMRGB_Linear:
		case eColorSpace_Rec709RGB_Linear:
			png_set_gAMA(png_ptr, info_ptr, 1.0);
			break;
		case eColorSpace_sRGB:
			png_set_gAMA(png_ptr, info_ptr, 1.0/2.2);
			break;
		case eColorSpace_ProPhotoRGB:
			png_set_gAMA(png_ptr, info_ptr, 1.0/1.8);
			break;
		}

		png_set_IHDR(png_ptr, info_ptr,
		   bufW, bufH,
		   bpp, 
		   PNG_COLOR_TYPE_RGB_ALPHA, 
		   PNG_INTERLACE_NONE, 
		   PNG_COMPRESSION_TYPE_DEFAULT, 
		   PNG_FILTER_TYPE_DEFAULT);

		png_write_info( png_ptr, info_ptr );
		if( ForTest_PNGWriteFailureAfterHeader() )
			png_error( png_ptr, "injected PNG codec failure after header" );
		for( unsigned int y=0; y<bufH; y++ ) {
			png_write_row(png_ptr, &pBuffer[static_cast<size_t>(y)*bufW*4*(bpp>>3)]);
		}

		// Free stuff
		png_write_end( png_ptr, info_ptr );
		png_destroy_write_struct( &png_ptr, &info_ptr );
	}
}

#endif

