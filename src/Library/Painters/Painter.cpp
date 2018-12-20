//////////////////////////////////////////////////////////////////////
//
//  Painter.cpp - Implementation of the 2D function inhertied 
//  method.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 17, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "Painter.h"

using namespace RISE;
using namespace RISE::Implementation;

Scalar Painter::GetColorNM( const RayIntersectionGeometric&, const Scalar ) const
{
	// Default implementation
	return 0;
}

SpectralPacket Painter::GetSpectrum( const RayIntersectionGeometric& ) const
{
	return dummy_spectrum;
}

Scalar Painter::Evaluate( const Scalar x, const Scalar y ) const
{
	// Set up a dummy ray intersection object and get a value from get color
	// and just average the R, G, B values... 
	RayIntersectionGeometric	r( Ray(), nullRasterizerState );
	r.bHit = true;
	r.ptCoord = Point2( x, y );
	{
		// Clamp values
		if( r.ptCoord.x > 1.0 ) r.ptCoord.x = 1.0;
		if( r.ptCoord.y > 1.0 ) r.ptCoord.y = 1.0;
		if( r.ptCoord.x < 0.0 ) r.ptCoord.x = 0.0;
		if( r.ptCoord.y < 0.0 ) r.ptCoord.y = 0.0;
	}
	const RISEPel&	c = GetColor( r );

	return c[0];
}
