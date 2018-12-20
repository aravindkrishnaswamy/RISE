//////////////////////////////////////////////////////////////////////
//
//  InterpolatedNoise.cpp - Implementa the interpolated noise classes
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 11, 2002
//  Tabs: 4
//  Comments:  The noise functions here were taken from a Perlin Noise
//  tutorial, available here:
//  http://freespace.virgin.net/hugo.elias/models/m_perlin.htm
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "InterpolatedNoise.h"

//
// 1D noise
//

using namespace RISE;
using namespace RISE::Implementation;

InterpolatedNoise1D::InterpolatedNoise1D( const RealSimpleInterpolator& interp_ ) : 
  interp( interp_ )
{
	interp.addref();

	SmoothedNoise1 = new SmoothedNoise1D( );
	GlobalLog()->PrintNew( SmoothedNoise1, __FILE__, __LINE__, "noise" );
}

InterpolatedNoise1D::~InterpolatedNoise1D()
{
	interp.release();

	safe_release( SmoothedNoise1 );
}

Scalar InterpolatedNoise1D::Evaluate( const Scalar x ) const 
{
	double			fX = 0;
	const double	fracX = modf( x, &fX );
	const int		X = int(fX);

	const Scalar	v1 = SmoothedNoise1->Evaluate( X );
	const Scalar	v2 = SmoothedNoise1->Evaluate( X+1 );

	return interp.InterpolateValues( v1, v2, fracX );
}

//
// 2D noise
//

InterpolatedNoise2D::InterpolatedNoise2D( const RealSimpleInterpolator& interp_ ) : 
  interp( interp_ )
{
	interp.addref();

	SmoothedNoise2 = new SmoothedNoise2D( );
	GlobalLog()->PrintNew( SmoothedNoise2, __FILE__, __LINE__, "noise" );
}

InterpolatedNoise2D::~InterpolatedNoise2D()
{
	interp.release();

	safe_release( SmoothedNoise2 );
}

Scalar InterpolatedNoise2D::Evaluate( const Scalar x, const Scalar y ) const 
{
//		int		X = int(x);
//		double	fracX = x - double(X);

	double	fX = 0;
	const double	fracX = modf( x, &fX );
	const int		X = int(fX);

//		int		Y = int(y);
//		double	fracY = y - double(Y);

	double	fY = 0;
	const double	fracY = modf( y, &fY );
	const int		Y = int(fY);

	const Scalar	v1 = SmoothedNoise2->Evaluate( X    , Y     );
	const Scalar	v2 = SmoothedNoise2->Evaluate( X + 1, Y     );
	const Scalar	v3 = SmoothedNoise2->Evaluate( X    , Y + 1 );
	const Scalar	v4 = SmoothedNoise2->Evaluate( X + 1, Y + 1 );

	const Scalar	i1 = interp.InterpolateValues( v1, v2, fracX );
	const Scalar	i2 = interp.InterpolateValues( v3, v4, fracX );

	return interp.InterpolateValues( i1, i2, fracY );
}


//
// 3D noise
//

InterpolatedNoise3D::InterpolatedNoise3D( const RealSimpleInterpolator& interp_ ) : 
  interp( interp_ )
{
	interp.addref();

	SmoothedNoise3 = new SmoothedNoise3D( );
	GlobalLog()->PrintNew( SmoothedNoise3, __FILE__, __LINE__, "noise" );
}


InterpolatedNoise3D::~InterpolatedNoise3D()
{
	interp.release();

	safe_release( SmoothedNoise3 );
}

Scalar InterpolatedNoise3D::Evaluate( const Scalar x, const Scalar y, const Scalar z ) const 
{
//		int		X = int(x);
//		double	fracX = x - double(X);

	double			fX = 0;
	const double	fracX = modf( x, &fX );
	const int		X = int(fX);

//		int		Y = int(y);
//		double	fracY = y - double(Y);

	double			fY = 0;
	const double	fracY = modf( y, &fY );
	const int		Y = int(fY);

	double			fZ = 0;
	const double	fracZ = modf( z, &fZ );
	const int		Z = int(fZ);

	const Scalar	v1 = SmoothedNoise3->Evaluate( X    , Y    , Z );
	const Scalar	v2 = SmoothedNoise3->Evaluate( X + 1, Y    , Z );
	const Scalar	v3 = SmoothedNoise3->Evaluate( X    , Y + 1, Z );
	const Scalar	v4 = SmoothedNoise3->Evaluate( X + 1, Y + 1, Z );

	const Scalar	v5 = SmoothedNoise3->Evaluate( X    , Y    , Z + 1 );
	const Scalar	v6 = SmoothedNoise3->Evaluate( X + 1, Y    , Z + 1 );
	const Scalar	v7 = SmoothedNoise3->Evaluate( X    , Y + 1, Z + 1 );
	const Scalar	v8 = SmoothedNoise3->Evaluate( X + 1, Y + 1, Z + 1 );

	const Scalar	i1 = interp.InterpolateValues( v1, v2, fracX );
	const Scalar	i2 = interp.InterpolateValues( v3, v4, fracX );

	const Scalar	i3 = interp.InterpolateValues( v5, v6, fracX );
	const Scalar	i4 = interp.InterpolateValues( v7, v8, fracX );

	const Scalar	j1 = interp.InterpolateValues( i1, i2, fracY );
	const Scalar	j2 = interp.InterpolateValues( i3, i4, fracY );

	return interp.InterpolateValues( j1, j2, fracZ );
}

