//////////////////////////////////////////////////////////////////////
//
//  SpectralColorPainter.cpp - Implements the Spectal Color Painter
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: September 14, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "SpectralColorPainter.h"

using namespace RISE;
using namespace RISE::Implementation;

static Scalar EvalPacket( const void* ctx, const Scalar nm )
{
	return static_cast<const SpectralPacket*>( ctx )->ValueAtNM( nm );
}

SpectralColorPainter::SpectralColorPainter( const SpectralPacket& spectrum_, const Scalar scale ) :
  spectrum( spectrum_ * scale )
{
	// DL-464: both RGB views are projections of exactly what GetColorNM
	// returns, so an RGB render and a spectral render agree.
	reflectanceRGB = ProjectPhysicalSpectrumToRGB( &EvalPacket, &spectrum, false );
	radianceRGB = ProjectPhysicalSpectrumToRGB( &EvalPacket, &spectrum, true );
}

SpectralColorPainter::~SpectralColorPainter( )
{
}

RISEPel SpectralColorPainter::GetColor( const RayIntersectionGeometric& ) const
{
	return reflectanceRGB;
}

RISEPel SpectralColorPainter::GetRadianceColor( const RayIntersectionGeometric& ) const
{
	return radianceRGB;
}

SpectralPacket SpectralColorPainter::GetSpectrum( const RayIntersectionGeometric& ) const
{
	return spectrum;
}

Scalar SpectralColorPainter::GetColorNM( const RayIntersectionGeometric&, Scalar nm ) const
{
	// Return the color at a particular wavelength...
	return spectrum.ValueAtNM( nm );
}

