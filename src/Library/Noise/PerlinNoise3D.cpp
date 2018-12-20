//////////////////////////////////////////////////////////////////////
//
//  PerlinNoise3D.cpp - Implements a 3D perlin noise function
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 20, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "PerlinNoise.h"

using namespace RISE;
using namespace RISE::Implementation;

PerlinNoise3D::PerlinNoise3D( const RealSimpleInterpolator& interp, const Scalar persistence_, const int numOctaves_ ) :
  noise( new InterpolatedNoise3D(interp) ), persistence( persistence_ ), numOctaves( numOctaves_ ), n( numOctaves_-1 )
{
	GlobalLog()->PrintNew( noise, __FILE__, __LINE__, "noise" );

	pAmplitudesLUT = new Scalar[n];
	GlobalLog()->PrintNew( pAmplitudesLUT, __FILE__, __LINE__, "amplitudesLUT" );

	for( int i=0; i<n; i++ ) {
		pAmplitudesLUT[i] = pow( persistence, Scalar(i) );
	}
}

PerlinNoise3D::~PerlinNoise3D( )
{
	safe_release( noise );

	if( pAmplitudesLUT ) {
		GlobalLog()->PrintDelete( pAmplitudesLUT, __FILE__, __LINE__ );
		delete [] pAmplitudesLUT;
		pAmplitudesLUT = 0;
	}
}

Scalar PerlinNoise3D::Evaluate( const Scalar x, const Scalar y, const Scalar z ) const
{
	Scalar	total = 0;

	for( int i=0; i<n; i++ ) {
		total += noise->Evaluate( x * frequency_lut[i], y * frequency_lut[i], z * frequency_lut[i] ) * pAmplitudesLUT[i];
	}

	return total;
}
