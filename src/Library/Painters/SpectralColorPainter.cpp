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

SpectralColorPainter::SpectralColorPainter( const SpectralPacket& spectrum_, const Scalar scale ) : spectrum( spectrum_ )
{
	XYZPel cxyz = spectrum.GetXYZ();
	color = cxyz * scale;
}

SpectralColorPainter::~SpectralColorPainter( )
{
}

RISEPel SpectralColorPainter::GetColor( const RayIntersectionGeometric& ) const
{
	return color;
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

