//////////////////////////////////////////////////////////////////////
//
//  NoiseUtils.h - Utilities for Noise functions
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 10, 2002
//  Tabs: 4
//  Comments:  The noise functions here were taken from a Perlin Noise
//  tutorial, available here:
//  http://freespace.virgin.net/hugo.elias/models/m_perlin.htm
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef NOISE_UTILS_
#define NOISE_UTILS_

#include "Math3D/Constants.h"
#include <math.h>
namespace RISE
{

	template< class Type >
	inline Type LinearInterpolate( const Type a, const Type b, const Scalar x )
	{
		return (a * (1.0-x) + b * x);
	}

	template< class Type >
	inline Type CosineInterpolate( const Type a, const Type b, const Scalar x )
	{
		Scalar		ft = x * PI;
		Scalar		f = (1.0 - cos(ft)) * 0.5;
		return (a*(1.0-f) + b*f);
	}


	// v0 = the point before a
	// v1 = the point a
	// v2 = the point before b
	// v3 = the point b
	inline Scalar CubicInterpolate( const Scalar v0, const Scalar v1, const Scalar v2, const Scalar v3, const Scalar x )
	{
		Scalar		P = (v3-v2) - (v0-v1);
		Scalar		Q = (v0-v1) - P;
		Scalar		R = v2-v0;
		const Scalar&		S = v1;

		return( P*x*x*x + Q*x*x + R*x + S );
	}

	inline Scalar Noise1( int x )
	{
		x = (x<<13) ^ x;
		return ( 1.0 - Scalar( (x * (x * x * 15731 + 789221) + 1376312589) & 0x7fffffff) / 1073741824.);
	}

	inline Scalar Noise2( const int x, const int y )
	{
		int		n = x * y * 57;
		n = (n<<13) ^ n;
		return ( 1.0 - Scalar( (n * (n * n * 15731 + 789221) + 1376312589) & 0x7fffffff) / 1073741824.);
	}

	inline Scalar SmoothedNoise1( const int x )
	{
		return( Noise1(x)/2. + Noise1(x-1)/4. + Noise1(x+1)/4. );
	}

	inline Scalar SmoothedNoise2( const int x, const int y )
	{
		Scalar	corners = ( Noise2(x-1, y-1)+Noise2(x+1, y-1)+Noise2(x-1, y+1)+Noise2(x+1, y+1) ) / 16.0;
		Scalar	sides	= ( Noise2(x-1, y  )+Noise2(x+1, y  )+Noise2(x  , y-1)+Noise2(x  , y+1) ) / 8.0;
		Scalar	center = Noise2(x, y) / 4.0;
		return( corners + sides + center );
	}

	inline Scalar InterpolatedNoise1( const Scalar x )
	{
	//	int		X = int(x);
	//	double	fracX = x - double(X);

		double	fX = 0;
		double	fracX = modf( x, &fX );
		int		X = int(fX);

		Scalar	v1 = SmoothedNoise1( X );
		Scalar	v2 = SmoothedNoise1( X+1 );

		return CosineInterpolate<Scalar>( v1, v2, fracX );
	}

	inline Scalar InterpolatedNoise2( const Scalar x, const Scalar y )
	{
	//	int		X = int(x);
	//	double	fracX = x - double(X);

		double	fX = 0;
		double	fracX = modf( x, &fX );
		int		X = int(fX);

	//	int		Y = int(y);
	//	double	fracY = y - double(Y);

		double	fY = 0;
		double	fracY = modf( y, &fY );
		int		Y = int(fY);

		Scalar	v1 = SmoothedNoise2( X    , Y     );
		Scalar	v2 = SmoothedNoise2( X + 1, Y     );
		Scalar	v3 = SmoothedNoise2( X    , Y + 1 );
		Scalar	v4 = SmoothedNoise2( X + 1, Y + 1 );

		Scalar	i1 = CosineInterpolate<Scalar>( v1, v2, fracX );
		Scalar	i2 = CosineInterpolate<Scalar>( v3, v4, fracX );

		return CosineInterpolate<Scalar>( i1, i2, fracY );
	}

	inline Scalar PerlinNoise1D( const Scalar x, const Scalar persistence, const int numOctaves )
	{
		Scalar	total = 0;

		const Scalar& p = persistence;
		int		n = numOctaves - 1;

		for( int i=0; i<n; i++ )
		{
			Scalar	frequency = pow( 2.0, Scalar(i) );
			Scalar	amplitude = pow( p, Scalar(i) );
			total += InterpolatedNoise1( x * frequency ) * amplitude;
		}

		return total;
	}

	inline Scalar PerlinNoise2D( const Scalar x, const Scalar y, const Scalar persistence, const int numOctaves )
	{
		Scalar	total = 0;

		const Scalar& p = persistence;
		int		n = numOctaves - 1;

		for( int i=0; i<n; i++ )
		{
			Scalar	frequency = pow( 2.0, Scalar(i) );
			Scalar	amplitude = pow( p, Scalar(i) );
			total += InterpolatedNoise2( x * frequency, y * frequency ) * amplitude;
		}

		return total;
	}

}

#endif
