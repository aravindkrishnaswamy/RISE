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
#include "../Utilities/Color/RGBSpectra.h"
#include "../Utilities/Color/ColorUtils.h"
#include "Function1DSpectralPainter.h"

using namespace RISE;
using namespace RISE::Implementation;

// The IPainter::GetRadianceNM default lives HERE rather than inline in
// IPainter.h so that RGBSpectra.h (and through it the Jakob-Hanika LUT
// table header) does not have to be pulled into every one of the ~300
// translation units that include IPainter.h.  Painter.cpp is always
// linked -- every in-tree painter derives from Implementation::Painter --
// so the out-of-line definition is always available.
//
// SLOW PATH by design: one LUT lookup per sample.  See the declaration in
// IPainter.h for why the composed colour (not the children's spectra) is
// what a composite painter must uplift, and
// docs/SPECTRAL_ILLUMINANT_CONVENTION.md for the convention.
Scalar IPainter::GetRadianceNM( const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	// Irradiance/exitance is physically non-negative; an all-negative RGB
	// triple would otherwise uplift to poly=(0,0,0) (Eval == 0.5) times a
	// NEGATIVE scale, producing negative radiance at every wavelength.
	RISEPel c = GetColor( ri );
	ColorMath::EnsurePositve( c );
	return RGBIlluminantSpectrum::FromRGB( c ).Eval( nm );
}

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

RISEPel RISE::Implementation::ProjectPhysicalSpectrumToRGB( const IFunction1D& f, const bool bAsRadiance )
{
	// 1 nm Riemann sums; numerator and denominator share the grid, so the
	// step cancels.  The reflectance view weights by the Y-normalised D65
	// (Int D65n ybar == Int ybar), so the denominator is Int ybar in both.
	XYZPel sum( 0, 0, 0 );
	double ySum = 0.0;
	for( int nm = 380; nm <= 780; nm++ ) {
		XYZPel cmf( 0, 0, 0 );
		if( !ColorUtils::XYZFromNM( cmf, Scalar( nm ) ) ) {
			continue;
		}
		ySum += double( cmf.Y );
		const Scalar w = bAsRadiance ? Scalar( 1 ) :
			RGBIlluminantSpectrum::ReferenceIlluminant( Scalar( nm ) );
		sum = sum + cmf * ( f.Evaluate( Scalar( nm ) ) * w );
	}
	if( !( ySum > 0.0 ) ) {
		return RISEPel( 0, 0, 0 );
	}
	return ColorUtils::XYZtoRec709RGB( sum * Scalar( 1.0 / ySum ) );
}
