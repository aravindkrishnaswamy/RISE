//////////////////////////////////////////////////////////////////////
//
//  EXRWriter.cpp - Implementation of the EXRWriter class
//                
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 10, 2006
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

// NOTE: I have shut off precompiled headers for this file because of the use of 
//   min and max which conflicts with min and max which Microsoft in their infinite wisdom
//   has decided to make a freaking MACRO!  Grrr...

#ifdef WIN32
#pragma warning( disable : 4512 )		// disables warning about not being able to generate an assignment operator (.NET 2003)
#pragma warning( disable : 4250 )		// disables silly virtual inheritance warning
#pragma warning( disable : 4344 )		// disables warning about explicit template argument passed to template function
#pragma warning( disable : 4290 )		// disables warning about C++ exception definition being ignored
#endif

#include "EXRWriter.h"
#include "../Interfaces/ILog.h"
#include "../Version.h"

#ifndef NO_EXR_SUPPORT
#include <ImfChromaticitiesAttribute.h>
#include <ImfStringAttribute.h>
#include <ImfFloatAttribute.h>
#endif

using namespace RISE;
using namespace RISE::Implementation;

#ifndef NO_EXR_SUPPORT
EXRWriter::EXRWriter(
	IWriteBuffer&         buffer_,
	const COLOR_SPACE     color_space_,
	const EXR_COMPRESSION compression_,
	const bool            with_alpha_ ) :
  out( buffer_ ),
  exrout( 0 ),
  buffer( buffer_ ),
  color_space( color_space_ ),
  compression( compression_ ),
  with_alpha( with_alpha_ ),
  horzpixels( 0 ),
  scanlines( 0 )
{
	buffer.addref();
}
#else
EXRWriter::EXRWriter(
	IWriteBuffer&         buffer_,
	const COLOR_SPACE     color_space_,
	const EXR_COMPRESSION compression_,
	const bool            with_alpha_ ) :
  buffer( buffer_ ),
  color_space( color_space_ ),
  compression( compression_ ),
  with_alpha( with_alpha_ ),
  horzpixels( 0 ),
  scanlines( 0 )
{
}
#endif

EXRWriter::~EXRWriter( )
{
	buffer.release();
}

void EXRWriter::BeginWrite( const unsigned int width, const unsigned int height )
{
#ifndef NO_EXR_SUPPORT
	// OpenEXR's RgbaOutputFile constructor accepts (1, 1) and larger
	// but rejects 0 dimensions inconsistently across versions.  Reject
	// here so we don't drag the OpenEXR error into the writer's call
	// site; a zero-size image is meaningless anyway.
	if( width == 0 || height == 0 ) {
		GlobalLog()->PrintEx( eLog_Error,
			"EXRWriter::BeginWrite:: refusing zero-size image (%ux%u)",
			width, height );
		// Leave exrout / horzpixels / scanlines at their defaults (0 / NULL);
		// WriteColor will be a no-op because exrbuffer is empty, and EndWrite
		// will skip the write because exrout is null.
		return;
	}

	if( exrout ) {
		// this would be bad
		GlobalLog()->PrintEasyError( "exrout object already exists!" );
		delete exrout;
		exrout = 0;	// reset before re-construction so a throwing
					// RgbaOutputFile ctor doesn't leave us with a
					// dangling pointer the next call would re-delete
	}

	// Map our compression enum to OpenEXR's.  Anything outside the
	// supported set falls back to PIZ (the v0 default) with a warning.
	Imf::Compression imfCompression = Imf::PIZ_COMPRESSION;
	switch( compression )
	{
		case eExrCompression_None: imfCompression = Imf::NO_COMPRESSION;  break;
		case eExrCompression_Zip:  imfCompression = Imf::ZIP_COMPRESSION; break;
		case eExrCompression_Piz:  imfCompression = Imf::PIZ_COMPRESSION; break;
		case eExrCompression_Dwaa: imfCompression = Imf::DWAA_COMPRESSION; break;
		default:
			GlobalLog()->PrintEx( eLog_Warning,
				"EXRWriter:: Unknown compression mode %d, falling back to PIZ",
				(int)compression );
			imfCompression = Imf::PIZ_COMPRESSION;
			break;
	}

	// Write the header
	Imf::Header header( width, height,
						1.0,
						Imath::V2f (0, 0),
						1,
						Imf::INCREASING_Y,
						imfCompression );

	// Software / version stamp.  Lets a future inspector trace an
	// EXR back to the build that produced it.
	{
		char szSoftware[256];
		snprintf( szSoftware, sizeof(szSoftware),
			"R.I.S.E. v%d.%d.%d build %d",
			RISE_VER_MAJOR_VERSION, RISE_VER_MINOR_VERSION,
			RISE_VER_REVISION_VERSION, RISE_VER_BUILD_VERSION );
		header.insert( "software", Imf::StringAttribute( szSoftware ) );
	}

	// Chromaticity tag — declares the colour primaries the pixel
	// values are encoded in.  Colour-managed viewers (tev, mrViewer,
	// Nuke with OCIO) honour this; viewers that ignore it will show
	// slightly wrong hues but the data is recoverable.  We tag the
	// primaries that match the writer's color_space, since that's
	// what the WriteColor path actually serialises.
	{
		Imath::V2f red, green, blue, white;
		switch( color_space )
		{
			default:
			case eColorSpace_ROMMRGB_Linear:
			case eColorSpace_ProPhotoRGB:
				// ROMM / ProPhoto primaries (ISO 22028-2), D50 white
				red   = Imath::V2f( 0.7347f, 0.2653f );
				green = Imath::V2f( 0.1596f, 0.8404f );
				blue  = Imath::V2f( 0.0366f, 0.0001f );
				white = Imath::V2f( 0.3457f, 0.3585f );
				break;
			case eColorSpace_Rec709RGB_Linear:
			case eColorSpace_sRGB:
				// Rec.709 / sRGB primaries, D65 white
				red   = Imath::V2f( 0.6400f, 0.3300f );
				green = Imath::V2f( 0.3000f, 0.6000f );
				blue  = Imath::V2f( 0.1500f, 0.0600f );
				white = Imath::V2f( 0.3127f, 0.3290f );
				break;
		}
		header.insert( "chromaticities",
			Imf::ChromaticitiesAttribute(
				Imf::Chromaticities( red, green, blue, white ) ) );
	}

	// whiteLuminance — interprets pixel value 1.0 as 1 cd/m².  This
	// is a starting convention until Landing 5 (physical camera)
	// gives us EV-driven absolute brightness.  Compositing tools
	// use this to scale HDR display output appropriately.
	header.insert( "whiteLuminance", Imf::FloatAttribute( 1.0f ) );

	// with_alpha controls the channel set: WRITE_RGBA writes RGBA,
	// WRITE_RGB writes RGB only (smaller files; appropriate for
	// outputs where alpha carries no meaningful information).
	const Imf::RgbaChannels channels = with_alpha ? Imf::WRITE_RGBA : Imf::WRITE_RGB;

	exrout = new Imf::RgbaOutputFile( out, header, channels );
	horzpixels = width;
	scanlines = height;

	// Note this is flipped around becase of an oddity in the EXR Array class...
	exrbuffer.resizeErase( height, width );
#endif
}

void EXRWriter::WriteColor( const RISEColor& c, const unsigned int x, const unsigned int y )
{
#ifndef NO_EXR_SUPPORT
	switch( color_space )
	{
	case eColorSpace_Rec709RGB_Linear:
		{
			WriteColorToEXRBuffer( Color_Template<Rec709RGBPel>( Rec709RGBPel(c.base), c.a ), x, y );
		} break;
	default:
	case eColorSpace_sRGB:
		{
			WriteColorToEXRBuffer( Color_Template<sRGBPel>( sRGBPel(c.base), c.a ), x, y );
		} break;
	case eColorSpace_ROMMRGB_Linear:
		{
			WriteColorToEXRBuffer( Color_Template<ROMMRGBPel>( ROMMRGBPel(c.base), c.a ), x, y );
		} break;
	case eColorSpace_ProPhotoRGB:
		{
			WriteColorToEXRBuffer( Color_Template<ProPhotoRGBPel>( ProPhotoRGBPel(c.base), c.a ), x, y );
		} break;
	}
#endif
}

void EXRWriter::EndWrite( )
{
#ifndef NO_EXR_SUPPORT
	// Write out the data to the memory buffer.  Skip when BeginWrite
	// rejected a zero-size image (exrout left null) or when the
	// caller never called BeginWrite at all.
	if( !exrout ) {
		return;
	}

	exrout->setFrameBuffer( &exrbuffer[0][0], 1, horzpixels );
	exrout->writePixels( scanlines );

	delete exrout;
	exrout = 0;
#endif
}
