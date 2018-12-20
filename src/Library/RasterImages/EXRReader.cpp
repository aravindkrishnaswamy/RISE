//////////////////////////////////////////////////////////////////////
//
//  EXRReader.cpp - Implementation of the EXRReader class
//                
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 24, 2001
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

#include "EXRReader.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

EXRReader::EXRReader( IReadBuffer& buffer, const COLOR_SPACE color_space_ ) :
  pReadBuffer( buffer ),
  color_space( color_space_ )
{
	pReadBuffer.addref();
	buffer_start_pos = pReadBuffer.getCurPos();
}

EXRReader::~EXRReader( )
{
	pReadBuffer.release();
}

bool EXRReader::BeginRead( unsigned int& width, unsigned int& height )
{
#ifndef NO_EXR_SUPPORT
	pReadBuffer.seek( IBuffer::START, buffer_start_pos );

	IStreamWrapper inbuf( pReadBuffer );
	Imf::RgbaInputFile	in( inbuf );

	// Read the file
	Imath::Box2i dw = in.dataWindow();
	float a = in.pixelAspectRatio();
	int w = dw.max.x - dw.min.x + 1;
	int h = dw.max.y - dw.min.y + 1;

	exrbuffer.resizeErase( h, w );
	in.setFrameBuffer( &exrbuffer[0][0] - dw.min.y * w - dw.min.x, 1, w );
	in.readPixels( dw.min.y, dw.max.y );

	width = w;
	height = h;
#endif
	return true;
}

void EXRReader::ReadColor( RISEColor& c, const unsigned int x, const unsigned int y )
{
#ifndef NO_EXR_SUPPORT
	Imf::Rgba pel = exrbuffer[y][x];

	switch( color_space )
	{
	case eColorSpace_Rec709RGB_Linear:
		{
			c.base = RISEPel( Rec709RGBPel( pel.r, pel.g, pel.b ) );
			c.a = pel.a;
		} break;
	case eColorSpace_sRGB:
		{
			c.base = RISEPel( sRGBPel( pel.r, pel.g, pel.b ) );
			c.a = pel.a;
		} break;
	case eColorSpace_ROMMRGB_Linear:
		{
			c.base = RISEPel( ROMMRGBPel( pel.r, pel.g, pel.b ) );
			c.a = pel.a;
		} break;
	case eColorSpace_ProPhotoRGB:
		{
			c.base = RISEPel( ProPhotoRGBPel( pel.r, pel.g, pel.b ) );
			c.a = pel.a;
		} break;
	}
#endif
}

void EXRReader::EndRead( )
{
}
