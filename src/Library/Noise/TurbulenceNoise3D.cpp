//////////////////////////////////////////////////////////////////////
//
//  TurbulenceNoise3D.cpp - Implements a 3D turbulence noise
//  function.  Turbulence sums the absolute value of each noise
//  octave instead of the raw signed value, creating sharp
//  crease features where the underlying noise crosses zero.
//
//  Reference: Perlin 1985 "An Image Synthesizer"
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 13, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "TurbulenceNoise.h"
#include "PerlinNoise.h"
#include <math.h>

using namespace RISE;
using namespace RISE::Implementation;

TurbulenceNoise3D::TurbulenceNoise3D( const RealSimpleInterpolator& interp, const Scalar persistence_, const int numOctaves_ ) :
  noise( new InterpolatedNoise3D(interp) ), persistence( persistence_ ), numOctaves( numOctaves_ ), n( numOctaves_-1 )
{
	GlobalLog()->PrintNew( noise, __FILE__, __LINE__, "noise" );

	pAmplitudesLUT = new Scalar[n > 0 ? n : 1];
	GlobalLog()->PrintNew( pAmplitudesLUT, __FILE__, __LINE__, "amplitudesLUT" );

	// Compute amplitude LUT and normalization factor.
	// The max possible output is the sum of all amplitudes (when |noise|=1 at every octave).
	// We normalize so the output is in [0, 1].
	dNormFactor = 0.0;
	for( int i=0; i<n; i++ ) {
		pAmplitudesLUT[i] = pow( persistence, Scalar(i) );
		dNormFactor += pAmplitudesLUT[i];
	}
	if( dNormFactor < 1e-10 ) dNormFactor = 1.0;
}

TurbulenceNoise3D::~TurbulenceNoise3D( )
{
	safe_release( noise );

	if( pAmplitudesLUT ) {
		GlobalLog()->PrintDelete( pAmplitudesLUT, __FILE__, __LINE__ );
		delete [] pAmplitudesLUT;
		pAmplitudesLUT = 0;
	}
}

Scalar TurbulenceNoise3D::Evaluate( const Scalar x, const Scalar y, const Scalar z ) const
{
	Scalar	total = 0;

	for( int i=0; i<n; i++ ) {
		total += fabs( noise->Evaluate( x * frequency_lut[i], y * frequency_lut[i], z * frequency_lut[i] ) ) * pAmplitudesLUT[i];
	}

	// Normalize to [0, 1] range
	return total / dNormFactor;
}
