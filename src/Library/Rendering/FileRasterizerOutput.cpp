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
#include "DisplayTransformWriter.h"
#include "../Interfaces/IOptions.h"
#include "../RISE_API.h"
#include "../Painters/CheckerPainter.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/DiskFileWriteBuffer.h"
#include "../Utilities/RasterSanityScan.h"

#include <string.h>
#include <stdio.h>

using namespace RISE;
using namespace RISE::Implementation;

FileRasterizerOutput::FileRasterizerOutput(
	const char* szPattern_,
	const bool bMultiple_,
	const FRO_TYPE type_,
	const unsigned char bpp_,
	const COLOR_SPACE color_space_,
	const Scalar exposureEV_,
	const DISPLAY_TRANSFORM display_transform_,
	const EXR_COMPRESSION exr_compression_,
	const bool exr_with_alpha_
	) :
  bMultiple( bMultiple_ ),
  type( type_ ),
  bpp( bpp_ ),
  color_space( color_space_ ),
  exposureEV( exposureEV_ ),
  display_transform( display_transform_ ),
  exr_compression( exr_compression_ ),
  exr_with_alpha( exr_with_alpha_ )
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

	// New (Landing 1) — sanity-check the display-pipeline knobs against the format.
	// Tone mapping and exposure are LDR-only concepts; applying them
	// to an HDR archival output would corrupt the radiometric ground
	// truth.  Force them off and warn the user if they were set
	// non-default on an HDR format.
	if( IsHDRFormat( type ) ) {
		if( display_transform != eDisplayTransform_None ) {
			GlobalLog()->PrintEasyWarning(
				"FileRasterizerOutput:: display_transform is ignored for HDR formats "
				"(EXR / HDR / RGBEA) — those write linear radiance verbatim.  "
				"If you want a tone-mapped preview, declare a separate PNG output." );
			display_transform = eDisplayTransform_None;
		}
		if( exposureEV != Scalar( 0 ) ) {
			GlobalLog()->PrintEasyWarning(
				"FileRasterizerOutput:: exposure is ignored for HDR formats "
				"(EXR / HDR / RGBEA) — those write linear radiance verbatim.  "
				"Apply exposure on a separate LDR output, or post-process the EXR." );
			exposureEV = Scalar( 0 );
		}
	}

	// EXR-specific knobs warn (but we keep the values for future use)
	// when set on a non-EXR type.  The chunk parser defaults them to
	// the v1 sane values so the warning only fires on intentional
	// misuse.
	if( type != EXR ) {
		if( exr_compression != eExrCompression_Piz || !exr_with_alpha ) {
			GlobalLog()->PrintEx( eLog_Warning,
				"FileRasterizerOutput:: exr_compression / exr_with_alpha are EXR-only "
				"and have no effect on type=%s", extensions[type] );
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

void FileRasterizerOutput::WriteImageToFile( const IRasterImage& pImage, const unsigned int frame, const char* szSuffix )
{
	IRasterImageWriter*		pWriter = 0;

	static const int MAX_BUFFER_SIZE = 2048;
	char	buf[MAX_BUFFER_SIZE];

	if( bMultiple ) {
		snprintf( buf, MAX_BUFFER_SIZE, "%s%s%.4d.%s", szPattern, szSuffix, frame, extensions[type] );
	} else {
		snprintf( buf, MAX_BUFFER_SIZE, "%s%s.%s", szPattern, szSuffix, extensions[type] );
	}

	DiskFileWriteBuffer*		mb = new DiskFileWriteBuffer( buf );

	if( !mb->ReadyToWrite() ) {
		// Some tragic error happened trying to open the required file for writing
		// Note the error and write the results to a temp file so that the user doesn't
		// lose the data

		safe_release( mb );

		const FileRasterizerOutput* pMe = this;
		if( bMultiple ) {
			snprintf( buf, MAX_BUFFER_SIZE, "fro_temp_%d%s_%.4d.%s", VoidPtrToUInt((void*)pMe), szSuffix, frame, extensions[type] );
		} else {
			snprintf( buf, MAX_BUFFER_SIZE, "fro_temp_%d%s.%s", VoidPtrToUInt((void*)pMe), szSuffix, extensions[type] );
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
		RISE_API_CreateEXRWriter( &pWriter, *mb, color_space, exr_compression, exr_with_alpha );
		break;
	default:
	case PPM:
		RISE_API_CreatePPMWriter( &pWriter, *mb, color_space );
		break;
	};

	// Wrap the format writer in a DisplayTransformWriter when this
	// output has a non-default display pipeline AND the format is
	// LDR (the IsHDRFormat gate is enforced in the constructor for
	// HDR types — this is belt-and-suspenders).  The wrapper holds
	// an addref'd reference to pWriter; we keep our own ref too and
	// release both at end-of-write.
	IRasterImageWriter* pEffectiveWriter = pWriter;
	DisplayTransformWriter* pDtWriter = 0;
	const bool useDisplayTransform =
		!IsHDRFormat( type ) &&
		( display_transform != eDisplayTransform_None || exposureEV != Scalar( 0 ) );
	if( useDisplayTransform ) {
		pDtWriter = new DisplayTransformWriter( *pWriter, exposureEV, display_transform );
		GlobalLog()->PrintNew( pDtWriter, __FILE__, __LINE__, "DisplayTransformWriter" );
		pEffectiveWriter = pDtWriter;
	}

	pImage.DumpImage( pEffectiveWriter );

	safe_release( mb );

	GlobalLog()->PrintEx( eLog_Event, "FileRasterizerOutput:: Written to \'%s\'", buf );

	// Pre-write would have been ideal but the writer consumes the
	// image via DumpImage so we scan afterwards — same cost, purely
	// diagnostic.  Negative radiance is physically impossible; small
	// counts typically indicate a pixel reconstruction filter with
	// negative side lobes (Mitchell / Lanczos) interacting with
	// bright neighbors.  Larger counts or large magnitudes point at
	// an actual integrator bug worth investigating.
	{
		const RasterSanityReport r = ScanRasterImageForPathologicalPixels( pImage );
		if( r.negativeCount > 0 ) {
			GlobalLog()->PrintEx( eLog_Warning,
				"FileRasterizerOutput:: '%s' contains %u pixels with negative "
				"radiance (max magnitude %.4e).  Common cause: pixel reconstruction "
				"filter with negative side lobes (Mitchell / Lanczos) interacting "
				"with bright neighbors.  Switch to a non-negative filter "
				"(gaussian, triangle, box) if this is visually distracting, or "
				"accept minor clamping to 0 in LDR output formats.",
				buf, r.negativeCount, (double)r.maxNegativeMagnitude );
		}
	}

	// Release the wrapper first (drops its addref on pWriter), then
	// release our own ref on pWriter.  Order matters: if we released
	// pWriter first, the wrapper's destructor would call release()
	// on an already-freed inner writer.
	if( pDtWriter ) {
		GlobalLog()->PrintDelete( pDtWriter, __FILE__, __LINE__ );
		safe_release( pDtWriter );
	}
	GlobalLog()->PrintDelete( pWriter, __FILE__, __LINE__ );
	safe_release( pWriter );
}

void FileRasterizerOutput::OutputImage( const IRasterImage& pImage, const Rect* /*pRegion*/, const unsigned int frame )
{
	WriteImageToFile( pImage, frame, "" );
}

void FileRasterizerOutput::OutputPreDenoisedImage( const IRasterImage& pImage, const Rect* /*pRegion*/, const unsigned int frame )
{
	WriteImageToFile( pImage, frame, "" );
}

void FileRasterizerOutput::OutputDenoisedImage( const IRasterImage& pImage, const Rect* /*pRegion*/, const unsigned int frame )
{
	WriteImageToFile( pImage, frame, "_denoised" );
}
