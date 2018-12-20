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

using namespace RISE;
using namespace RISE::Implementation;

#ifndef NO_EXR_SUPPORT
EXRWriter::EXRWriter( IWriteBuffer& buffer_, const COLOR_SPACE color_space_ ) :
  out( buffer_ ),
  exrout( 0 ),
  buffer( buffer_ ),
  color_space( color_space_ ),
  horzpixels( 0 ),
  scanlines( 0 )
{
	buffer.addref();
}
#else
EXRWriter::EXRWriter( IWriteBuffer& buffer_, const COLOR_SPACE color_space_ ) :
  buffer( buffer_ ),
  color_space( color_space_ ),
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
	if( exrout ) {
		// this would be bad
		GlobalLog()->PrintEasyError( "exrout object already exists!" );
		delete exrout;
	}

	// Write the header
	Imf::Header header( width, height,
						1.0,
						Imath::V2f (0, 0),
						1,
						Imf::INCREASING_Y,
						Imf::PIZ_COMPRESSION );
	
	exrout = new Imf::RgbaOutputFile( out, header, Imf::WRITE_RGBA );
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
	// Write out the data to the memory buffer

	exrout->setFrameBuffer( &exrbuffer[0][0], 1, horzpixels );
	exrout->writePixels( scanlines );

	delete exrout;
	exrout = 0;
#endif
}
