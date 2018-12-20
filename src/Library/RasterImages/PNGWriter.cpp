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
#ifndef NO_PNG_SUPPORT
	#include <png.h>
#endif

using namespace RISE;
using namespace RISE::Implementation;

PNGWriter::PNGWriter( IWriteBuffer& buffer, const unsigned char bpp_, const COLOR_SPACE color_space_ ) :
  pWriteBuffer( buffer ),
  pBuffer( 0 ),
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
	// Setup the buffer
	bufW = width;
	bufH = height;
	pBuffer = new unsigned char[width*height*4*(bpp>>3)];
	GlobalLog()->PrintNew( pBuffer, __FILE__, __LINE__, "buffer" );
}

void PNGWriter::WriteColor( const RISEColor& c, const unsigned int x, const unsigned int y )
{
	if( pBuffer ) {
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

				const unsigned int idx = y*bufW*4+x*4;

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

				const unsigned int idx = y*bufW*8+x*8;

#ifdef RISE_BIG_ENDIAN
				*((unsigned short*)(&pBuffer[idx])) = p.b;
				*((unsigned short*)(&pBuffer[idx+2])) = p.g;
				*((unsigned short*)(&pBuffer[idx+4])) = p.r;
				*((unsigned short*)(&pBuffer[idx+6])) = p.a;
#else
				// Byte order in PNG is flipped!  Its little endian!
				unsigned short tempB = (unsigned short)(p.b);
				pBuffer[idx] = ((tempB>>8)&0xFF);
				pBuffer[idx+1] = (tempB&0xFF);
				unsigned short tempG = (unsigned short)(p.g);
				pBuffer[idx+2] = ((tempG>>8)&0xFF);
				pBuffer[idx+3] = (tempG&0xFF);
				unsigned short tempR = (unsigned short)(p.r);
				pBuffer[idx+4] = ((tempR>>8)&0xFF);
				pBuffer[idx+5] = (tempR&0xFF);
				unsigned short tempA = (unsigned short)(p.a);
				pBuffer[idx+6] = ((tempA>>8)&0xFF);
				pBuffer[idx+7] = (tempA&0xFF);
#endif
			}
			break;
		};
	}
}

#ifdef NO_PNG_SUPPORT

void PNGWriter::EndWrite( )
{
	GlobalLog()->PrintSourceError( "PNGWriter::EndWrite NO PNG SUPPORT was compiled!  No output will be written", __FILE__, __LINE__ );
}

#else

static void
png_write_data(png_structp png_ptr, png_bytep data, png_size_t length)
{
	IWriteBuffer*	pBuffer = (IWriteBuffer*)(png_ptr->io_ptr);

	if( pBuffer ) {
		pBuffer->setBytes( data, length );
	} else {
		GlobalLog()->PrintSourceError( "png_write_data (callback func):: bad MemoryBuffer", __FILE__, __LINE__ );
	}
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
			GlobalLog()->PrintSourceError( "PNGWriter:: Could not allocate structure", __FILE__, __LINE__ );
		}

		png_infop info_ptr = png_create_info_struct(png_ptr);

		if( !info_ptr ) {
			png_destroy_write_struct(&png_ptr, NULL);
			GlobalLog()->PrintSourceError( "PNGWriter:: Could not create info structure", __FILE__, __LINE__ );
		}

//		if( setjmp( png_ptr->jumpbuf ) )
//		{
//			png_destroy_write_struct(&png_ptr, info_ptr);
//			GlobalLog()->PrintSourceError( "PNGWriter:: Could not setjmp", __FILE__, __LINE__ );
//		}

		// Set a write function to write to our buffer rather than a file
		png_set_write_fn( png_ptr, (void*)&pWriteBuffer, png_write_data, png_flush );

		png_set_filter(png_ptr, 0, PNG_NO_FILTERS);
		png_set_compression_level(png_ptr, Z_BEST_COMPRESSION);
		png_set_bgr(png_ptr);

		if( bpp == 16 ) {
			png_set_gAMA(png_ptr, info_ptr, 1.0);
		} else {
			switch( color_space )
			{
			case eColorSpace_ROMMRGB_Linear:
			case eColorSpace_Rec709RGB_Linear:
				{
					png_set_gAMA(png_ptr, info_ptr, 1.0);
				} break;
			case eColorSpace_sRGB:
				{
					png_set_gAMA(png_ptr, info_ptr, 1.0/2.2);
				} break;
			case eColorSpace_ProPhotoRGB:
				{
					png_set_gAMA(png_ptr, info_ptr, 1.0/1.8);
				} break;
			}
		}

		png_set_IHDR(png_ptr, info_ptr,
		   bufW, bufH,
		   bpp, 
		   PNG_COLOR_TYPE_RGB_ALPHA, 
		   PNG_INTERLACE_NONE, 
		   PNG_COMPRESSION_TYPE_DEFAULT, 
		   PNG_FILTER_TYPE_DEFAULT);

		png_write_info( png_ptr, info_ptr );
		for( unsigned int y=0; y<bufH; y++ ) {
			png_write_row(png_ptr, &pBuffer[y*bufW*4*(bpp>>3)]);
		}

		// Free stuff
		png_write_end( png_ptr, info_ptr );
		png_destroy_write_struct( &png_ptr, &info_ptr );
	}
}

#endif

