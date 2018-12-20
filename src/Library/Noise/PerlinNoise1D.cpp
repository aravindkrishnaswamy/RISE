//////////////////////////////////////////////////////////////////////
//
//  PerlinNoise1D.cpp - Implements a 1D perlin noise function
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 11, 2002
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

PerlinNoise1D::PerlinNoise1D( const RealSimpleInterpolator& interp, const Scalar persistence_, const int numOctaves_ ) :
  noise( new InterpolatedNoise1D(interp) ), persistence( persistence_ ), numOctaves( numOctaves_ ), n( numOctaves_-1 )
{
	GlobalLog()->PrintNew( noise, __FILE__, __LINE__, "noise" );

	pAmplitudesLUT = new Scalar[n];
	GlobalLog()->PrintNew( pAmplitudesLUT, __FILE__, __LINE__, "amplitudesLUT" );

	for( int i=0; i<n; i++ ) {
		pAmplitudesLUT[i] = pow( persistence, Scalar(i) );
	}
}

PerlinNoise1D::~PerlinNoise1D( )
{
	safe_release( noise );

	if( pAmplitudesLUT ) {
		GlobalLog()->PrintDelete( pAmplitudesLUT, __FILE__, __LINE__ );
		delete [] pAmplitudesLUT;
		pAmplitudesLUT = 0;
	}
}

Scalar PerlinNoise1D::Evaluate( const Scalar x ) const
{
	Scalar	total = 0;

	for( int i=0; i<n; i++ ) {
		total += noise->Evaluate( x * frequency_lut[i] ) * pAmplitudesLUT[i];
	}

	return total;
}
