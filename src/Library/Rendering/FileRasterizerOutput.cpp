//////////////////////////////////////////////////////////////////////
//
//  FileRasterizerOutput.cpp - Implementation of a file rasterizer output
//  object
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 2, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "FileRasterizerOutput.h"
#include "../Interfaces/IOptions.h"
#include "../RISE_API.h"
#include "../Painters/CheckerPainter.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/DiskFileWriteBuffer.h"

#include <string.h>
#include <stdio.h>

using namespace RISE;
using namespace RISE::Implementation;

FileRasterizerOutput::FileRasterizerOutput( 
	const char* szPattern_, 
	const bool bMultiple_, 
	const FRO_TYPE type_, 
	const unsigned char bpp_,
	const COLOR_SPACE color_space_
	) : 
  bMultiple( bMultiple_ ),
  type( type_ ),
  bpp( bpp_ ),
  color_space( color_space_ )
{
	// Check the global options file to figuring out where to stick the rendered files
	RISE::IOptions& options = GlobalOptions();

	// First check to see if we should just stick stuff using a rendered subfolder from the media location
	const bool bUseMediaFolder = options.ReadBool( "rendered_output_in_rise_media_folder", false );

	if( bUseMediaFolder ) {
		// Do the concatenation
		const char* szmediapath = getenv( "RISE_MEDIA_PATH" );
		if( szmediapath ) {
			strcpy( szPattern, szmediapath );
			strcat( szPattern, szPattern_ );
		} else {
			GlobalLog()->PrintEasyWarning( "FileRenderedOutput: Asked to use media path for rendered files, but media path is not set!" );
			strcpy( szPattern, szPattern_ );
		}
	} else {
		// Look for an option that gives us the folder
		RISE::String strOutputFolder = options.ReadString( "rendered_output_folder", "" );
		strcpy( szPattern, strOutputFolder.c_str() );
		strcat( szPattern, szPattern_ );
	}

	if( color_space==eColorSpace_Rec709RGB_Linear || color_space==eColorSpace_ROMMRGB_Linear ) {
		// Spit out a warning if a linear color space is selected but only 8bits is selected for precision
		if( bpp == 8 && type != HDR && type != RGBEA && type != EXR ) {
			GlobalLog()->PrintEasyWarning( "FileRasterizerOutput:: Linear colorspace chosen but bpp is 8-bit" );
		}
	}

	if( color_space == eColorSpace_sRGB || color_space==eColorSpace_ProPhotoRGB ) {
		// Spit out a warning if a nonlinear color space is selected for an HDR format
		if( type == HDR || type == RGBEA || type == EXR ) {
			GlobalLog()->PrintEasyWarning( "FileRasterizerOutput:: Nonlinear colorspace chosen for a high dynamic range format, this is bad idea!" );
		}
	}
}

FileRasterizerOutput::~FileRasterizerOutput()
{
}

void FileRasterizerOutput::OutputIntermediateImage( const IRasterImage&, const Rect* )
{
	// File outputs don't support intermediate rasterizations dammit!
}

inline unsigned int VoidPtrToUInt( const void* v )
{
	return (unsigned int)*((unsigned int*)(&v));
}

void FileRasterizerOutput::OutputImage( const IRasterImage& pImage, const Rect* /*pRegion*/, const unsigned int frame )
{
	IRasterImageWriter*		pWriter = 0;

	char	buf[2048];

	if( bMultiple ) {
		sprintf( buf, "%s%.4d.%s", szPattern, frame, extensions[type] );
	} else {
		sprintf( buf, "%s.%s", szPattern, extensions[type] );
	}

	DiskFileWriteBuffer*		mb = new DiskFileWriteBuffer( buf );
	
	if( !mb->ReadyToWrite() ) {
		// Some tragic error happened trying to open the required file for writing
		// Note the error and write the results to a temp file so that the user doesn't
		// lose the data

		safe_release( mb );

		const FileRasterizerOutput* pMe = this;
		if( bMultiple ) {
			sprintf( buf, "fro_temp_%d_%.4d.%s", VoidPtrToUInt((void*)pMe), frame, extensions[type] );
		} else {
			sprintf( buf, "fro_temp_%d.%s", VoidPtrToUInt((void*)pMe), extensions[type] );
		}

		mb = new DiskFileWriteBuffer( buf );

		// If that doesn't work either we are just screwed
		if( !mb->ReadyToWrite() ){
			GlobalLog()->PrintEasyError( "Fatal error in trying to write image, couldn't even write the emergency file!" );
			return;
		} else {
			GlobalLog()->PrintEx( eLog_Warning, "Failed to open specified file, rendered scene written to emergency file '%s' instead!", buf );
		}
	}

	GlobalLog()->PrintNew( mb, __FILE__, __LINE__, "DiskFileWriteBuffer" );

	switch( type )
	{
	case TGA:
		RISE_API_CreateTGAWriter( &pWriter, *mb, color_space );
		break;
	case PNG:
		RISE_API_CreatePNGWriter( &pWriter, *mb, bpp, color_space );
		break;
	case HDR:
		RISE_API_CreateHDRWriter( &pWriter, *mb, color_space );
		break;
	case RGBEA:
		RISE_API_CreateRGBEAWriter( &pWriter, *mb );
		break;
	case TIFF:
		RISE_API_CreateTIFFWriter( &pWriter, *mb, color_space );
		break;
	case EXR:
		RISE_API_CreateEXRWriter( &pWriter, *mb, color_space );
		break;
	default:
	case PPM:
		RISE_API_CreatePPMWriter( &pWriter, *mb, color_space );
		break;
	};

	pImage.DumpImage( pWriter );

	safe_release( mb );

	GlobalLog()->PrintEx( eLog_Event, "FileRasterizerOutput:: Written to \'%s\'", buf );

	GlobalLog()->PrintDelete( pWriter, __FILE__, __LINE__ );
	delete pWriter;
}
