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
#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <limits>
#include <memory>
#include <exception>
#include "../Library/Utilities/RString.h"
#include "../Library/Utilities/DiskFileWriteBuffer.h"
#include <string.h>

using namespace RISE;

namespace
{
	struct ReleaseReference
	{
		void operator()( const IReference* reference ) const
		{
			if( reference ) reference->release();
		}
	};

	template<typename T> using OwnedReference = std::unique_ptr<T, ReleaseReference>;
}


static std::string Extension( const char* path )
{
	std::string extension = std::filesystem::path( path ).extension().string();
	std::transform( extension.begin(), extension.end(), extension.begin(),
		[]( unsigned char c ) { return static_cast<char>( std::tolower(c) ); } );
	return extension;
}

static int Convert( int argc, char** argv )
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

	const std::string inputExtension = Extension( argv[1] );
	const std::string outputExtension = Extension( argv[2] );
	if( outputExtension != ".tga" && outputExtension != ".png" &&
		outputExtension != ".hdr" && outputExtension != ".exr" &&
		outputExtension != ".tif" && outputExtension != ".ppm" ) {
		std::cerr << "Unsupported output extension: " << outputExtension << '\n';
		return 1;
	}

	// Try and load the input image
	IMemoryBuffer*				pBuffer = 0;
	RISE_API_CreateMemoryBufferFromFile( &pBuffer, argv[1] );
	OwnedReference<IMemoryBuffer> inputBuffer( pBuffer );
	if( !pBuffer ) return 1;

	IRasterImageReader*			reader = 0;

	// Options
	COLOR_SPACE in = eColorSpace_sRGB;
	COLOR_SPACE out = eColorSpace_sRGB;

	unsigned int new_width = 0;
	unsigned int new_height = 0;
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
		if( s[0] == 'w' || s[0] == 'h' ) {
			unsigned int& dimension = s[0] == 'w' ? new_width : new_height;
			const char* begin = s.c_str() + 1;
			const char* end = s.c_str() + s.size() - 1;
			const auto result = std::from_chars( begin, end, dimension );
			if( result.ec != std::errc{} || result.ptr != end || dimension == 0 ) {
				std::cerr << "Invalid resize dimension: " << argv[i] << '\n';
				return 1;
			}
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

		if( inputExtension == ".tga" ) {
			std::cout << "Using TGA image reader" << std::endl;
			RISE_API_CreateTGAReader( &reader, *pBuffer, in );
		} else if( inputExtension == ".png" ) {
			std::cout << "Using PNG image reader" << std::endl;
			RISE_API_CreatePNGReader( &reader, *pBuffer, in );
		} else if( inputExtension == ".hdr" ) {
			std::cout << "Using HDR image reader" << std::endl;
			RISE_API_CreateHDRReader( &reader, *pBuffer );
		} else if( inputExtension == ".exr" ) {
			std::cout << "Using EXR image reader" << std::endl;
			RISE_API_CreateEXRReader( &reader, *pBuffer, in );
		} else if( inputExtension == ".tif" ) {
			std::cout << "Using TIFF image reader" << std::endl;
			RISE_API_CreateTIFFReader( &reader, *pBuffer, in );
		} else {
			std::cout << "Unknown extension for reader: " << inputExtension << " on file" << std::endl;
			return 1;
		}
	}
	
	OwnedReference<IRasterImageReader> ownedReader( reader );
	unsigned int width = 0, height = 0;
	if( !reader || !reader->BeginRead( width, height ) ) {
		return 1;
	}
	if( width == 0 || height == 0 || width > std::numeric_limits<unsigned int>::max() / height ) {
		reader->EndRead();
		return 1;
	}

	IRasterImage* pImage = nullptr;
	RISE_API_CreateRISEColorRasterImage( &pImage, width, height, RISEColor() );
	OwnedReference<IRasterImage> image( pImage );
	if( !image ) return 1;
	for( unsigned int y = 0; y < height; ++y ) {
		for( unsigned int x = 0; x < width; ++x ) {
			RISEColor color;
			reader->ReadColor( color, x, y );
			pImage->SetPEL( x, y, color );
		}
	}
	reader->EndRead();
	ownedReader.reset();
	inputBuffer.reset();
	const unsigned int outputWidth = new_width ? new_width : width;
	const unsigned int outputHeight = new_height ? new_height : height;
	if( outputWidth > std::numeric_limits<unsigned int>::max() / 8 / outputHeight ) {
		std::cerr << "Output image is too large\n";
		return 1;
	}

	IRasterImageWriter*			writer = 0;
	
	// Then dump the image out
	IWriteBuffer* rawOutput = nullptr;
	RISE_API_CreateDiskFileWriteBuffer( &rawOutput, argv[2] );
	OwnedReference<IWriteBuffer> output( rawOutput );
	auto* pOutBuffer = dynamic_cast<Implementation::DiskFileWriteBuffer*>( rawOutput );
	if( !pOutBuffer || !pOutBuffer->ReadyToWrite() ) {
		return 1;
	}
	
	{
		// Figure out what writer to use, depending on the extension of the file

		if( outputExtension == ".tga" ) {
			std::cout << "Using TGA image writer" << std::endl;
			RISE_API_CreateTGAWriter( &writer, *pOutBuffer, out );
		} else if( outputExtension == ".png" ) {
			std::cout << "Using PNG image writer" << std::endl;
			RISE_API_CreatePNGWriter( &writer, *pOutBuffer, bpp, out );
		} else if( outputExtension == ".hdr" ) {
			std::cout << "Using HDR image writer" << std::endl;
			RISE_API_CreateHDRWriter( &writer, *pOutBuffer, out );
		} else if( outputExtension == ".exr" ) {
			std::cout << "Using EXR image writer" << std::endl;
			// Landing 1 (PB pipeline) defaults: PIZ compression, RGBA channels
			// — match the file_rasterizeroutput chunk's defaults.
			RISE_API_CreateEXRWriter( &writer, *pOutBuffer, out,
				eExrCompression_Piz, /*with_alpha*/ true );
		} else if( outputExtension == ".tif" ) {
			std::cout << "Using TIFF image writer" << std::endl;
			RISE_API_CreateTIFFWriter( &writer, *pOutBuffer, out );
		} else if( outputExtension == ".ppm" ) {
			std::cout << "Using PPM image writer" << std::endl;
			RISE_API_CreatePPMWriter( &writer, *pOutBuffer, out );
		}
	}

	OwnedReference<IRasterImageWriter> ownedWriter( writer );
	if( !writer ) {
		return 1;
	}

	if( new_width || new_height )
	{
		// Stream resized pixels explicitly so finalization errors occur in
		// this call, where main can report them, rather than a destructor.
		std::cout << "Resizing the image: [";
		IRasterImageAccessor* rawAccessor = nullptr;
		RISE_API_CreateUniformBSplineBicubicRasterImageAccessor( &rawAccessor, *pImage );
		OwnedReference<IRasterImageAccessor> accessor( rawAccessor );
		if( !accessor ) return 1;
		writer->BeginWrite( outputWidth, outputHeight );
		for( unsigned int y = 0; y < outputHeight; ++y ) {
			for( unsigned int x = 0; x < outputWidth; ++x ) {
				RISEColor color;
				const Scalar v = outputHeight > 1 ? Scalar(y) / (outputHeight - 1) : 0.5;
				const Scalar u = outputWidth > 1 ? Scalar(x) / (outputWidth - 1) : 0.5;
				accessor->GetPEL( v, u, color );
				writer->WriteColor( color, x, y );
			}
			std::cout << '.';
		}
		writer->EndWrite();
		std::cout << ']' << std::endl;
	} else {
		pImage->DumpImage( writer );
	}

	ownedWriter.reset();
	return pOutBuffer->Finish( true ) ? 0 : 1;
}

int main( int argc, char** argv )
{
	try {
		return Convert( argc, argv );
	} catch( const std::exception& failure ) {
		std::cerr << "Image conversion failed: " << failure.what() << '\n';
		return 1;
	}
}
