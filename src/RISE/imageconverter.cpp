//////////////////////////////////////////////////////////////////////
//
//  imageconverter.cpp - Converts between one image type to another
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 12, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include "../Library/RISE_API.h"
#include "../Library/Utilities/RString.h"
#include <string.h>

using namespace RISE;

void tolower( String& s )
{
	const size_t length = strlen(s.c_str());
	for( size_t i=0; i<length; i++ ) {
		s[i] = tolower(s[i]);
	}
}

int main( int argc, char** argv )
{
	if( argc < 3 ) {
		std::cout << "Usage: imageconverter <input image file> <output image file> <options>" << std::endl;
		std::cout << "Options: -sRGBin          treat input as sRGB" << std::endl;
		std::cout << "         -sRGBout         write output as sRGB" << std::endl;
		std::cout << "         -w<value>        new output width" << std::endl;
		std::cout << "         -h<value>        new output height" << std::endl;
		std::cout << "         -b<value>        bits / pixel (PNG only)" << std::endl;
		std::cout << std::endl;
		std::cout << "Examples: meshconverter in.tga out.png" << std::endl;
		std::cout << "          meshconverter in.tga out.png -sRGBin -sRGBout" << std::endl;
		std::cout << "          meshconverter in.tga out.png -i2.2 -o2.0" << std::endl;
		std::cout << "          meshconverter in.png out.hdr" << std::endl;
		std::cout << "          meshconverter in.png out.png -w256 -h256" << std::endl;
		return 1;
	}

	// Try and load the input image
	IMemoryBuffer*				pBuffer = 0;
	RISE_API_CreateMemoryBufferFromFile( &pBuffer, argv[1] );

	IRasterImageReader*			reader = 0;

	// Options
	COLOR_SPACE in = eColorSpace_sRGB;
	COLOR_SPACE out = eColorSpace_sRGB;

	int new_width = 0;
	int new_height = 0;
	int bpp = 8;

	// Read all the options
	for( int i=3; i<argc; i++ ) {
		if( argv[i][0] != '-' ) {
			continue;
		}
		
		if( strlen( argv[i] ) < 2 ) {
			continue;
		}

		// Its an option
		String s( &argv[i][1] );

		if( s == "sRGBin" ) {
			in = eColorSpace_sRGB;
			continue;
		} else if( s == "sRGBout" ) {
			out = eColorSpace_sRGB;
			continue;
		}

		// Check for width/height changes
		if( s[0] == 'w' ) {
			new_width = atoi( &s[1] );
			continue;
		} else if( s[0] == 'h' ) {
			new_height = atoi( &s[1] );
			continue;
		}

		// Check bpp
		if( s[0] == 'b' ) {
			bpp = atoi( &s[1] );
			continue;
		}

		std::cout << "Unknown option: " << argv[i] << std::endl;
	}

	{
		// Figure out what loader to use, depending on the extension of the file
		const char* three_letter_extension = &((argv[1])[strlen(argv[1])-4]);
		String szExt( three_letter_extension );
		tolower( szExt );

		if( szExt == ".tga" ) {
			std::cout << "Using TGA image reader" << std::endl;
			RISE_API_CreateTGAReader( &reader, *pBuffer, in );
		} else if( szExt == ".png" ) {
			std::cout << "Using PNG image reader" << std::endl;
			RISE_API_CreatePNGReader( &reader, *pBuffer, in );
		} else if( szExt == ".hdr" ) {
			std::cout << "Using HDR image reader" << std::endl;
			RISE_API_CreateHDRReader( &reader, *pBuffer );
		} else if( szExt == ".exr" ) {
			std::cout << "Using EXR image reader" << std::endl;
			RISE_API_CreateEXRReader( &reader, *pBuffer, in );
		} else if( szExt == ".tif" ) {
			std::cout << "Using TIFF image reader" << std::endl;
			RISE_API_CreateTIFFReader( &reader, *pBuffer, in );
		} else {
			std::cout << "Unknown extension for reader: " << three_letter_extension << " on file" << std::endl;
			pBuffer->release();
			return 1;
		}
	}
	
	IRasterImage* pImage = 0;
	RISE_API_CreateRISEColorRasterImage( &pImage, 0, 0, RISEColor() );
//	RISE_API_CreateReadOnlyRISEColorRasterImage( &pImage );     // use this for memory efficiency

	pImage->LoadImage( reader );

	IRasterImageWriter*			writer = 0;
	
	// Then dump the image out
	IWriteBuffer* pOutBuffer = 0;
	RISE_API_CreateDiskFileWriteBuffer( &pOutBuffer, argv[2] );
	
	{
		// Figure out what writer to use, depending on the extension of the file
		const char* three_letter_extension = &((argv[2])[strlen(argv[2])-4]);
		String szExt( three_letter_extension );
		tolower( szExt );

		if( szExt == ".tga" ) {
			std::cout << "Using TGA image writer" << std::endl;
			RISE_API_CreateTGAWriter( &writer, *pOutBuffer, out );
		} else if( szExt == ".png" ) {
			std::cout << "Using PNG image writer" << std::endl;
			RISE_API_CreatePNGWriter( &writer, *pOutBuffer, bpp, out );
		} else if( szExt == ".hdr" ) {
			std::cout << "Using HDR image writer" << std::endl;
			RISE_API_CreateHDRWriter( &writer, *pOutBuffer, out );
		} else if( szExt == ".exr" ) {
			std::cout << "Using EXR image writer" << std::endl;
			RISE_API_CreateEXRWriter( &writer, *pOutBuffer, out );
		} else if( szExt == ".tif" ) {
			std::cout << "Using TIFF image writer" << std::endl;
			RISE_API_CreateTIFFWriter( &writer, *pOutBuffer, out );
		} else if( szExt == ".ppm" ) {
			std::cout << "Using PPM image writer" << std::endl;
			RISE_API_CreatePPMWriter( &writer, *pOutBuffer, out );
		} else {
			std::cout << "Unknown extension for writer: " << three_letter_extension << " on file" << std::endl;
			pImage->release();
			pOutBuffer->release();
			return 1;
		}
	}

	if( new_width || new_height )
	{
		// Resize the image
		std::cout << "Resizing the image: [";

		IRasterImageAccessor* pAccessor = 0;
//		RISE_API_CreateCatmullRomBicubicRasterImageAccessor( &pAccessor, *pImage ); // turns out this may not be a good idea for images
		RISE_API_CreateUniformBSplineBicubicRasterImageAccessor( &pAccessor, *pImage );

		// When resizing always use the write only raster image, at least that way the memory requirements
		// don't kill us for large images
		IRasterImage* pNewImage = 0;
		RISE_API_CreateWriteOnlyRISEColorRasterImage( &pNewImage, new_width?new_width:pImage->GetWidth(), new_height?new_height:pImage->GetHeight() );

		// Set the writer
		pNewImage->DumpImage( writer );

		for( unsigned int y=0; y<pNewImage->GetHeight(); y++ ) {
			for( unsigned int x=0; x<pNewImage->GetWidth(); x++ ) {
				RISEColor c;
				pAccessor->GetPEL( Scalar(y)/Scalar(pNewImage->GetHeight()-1), Scalar(x)/Scalar(pNewImage->GetWidth()-1), c );
				pNewImage->SetPEL( x, y, c );
			}
			std::cout << '.';
		}

		std::cout << ']' << std::endl;

		pImage->release();
		pImage = pNewImage;
	} else {
		// No resize, so just dump it out
		pImage->DumpImage( writer );
	}

	reader->release();
	pBuffer->release();

	pImage->release();
	pOutBuffer->release();

	return 0;
}
