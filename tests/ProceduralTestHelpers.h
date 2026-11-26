#ifndef PROCEDURAL_TEST_HELPERS_H
#define PROCEDURAL_TEST_HELPERS_H

#include <cmath>

#include "../src/Library/Noise/PerlinNoise.h"

namespace ProceduralTestHelpers
{
	inline bool IsClose( double a, double b, double tol = 1e-6 )
	{
		return std::fabs( a - b ) < tol;
	}

	inline bool IsInUnitInterval( RISE::Scalar v, RISE::Scalar eps = 1e-6 )
	{
		return v >= -eps && v <= 1.0 + eps;
	}

	inline RISE::Scalar Clamp01( RISE::Scalar v )
	{
		if( v < 0.0 ) return 0.0;
		if( v > 1.0 ) return 1.0;
		return v;
	}

	inline RISE::Scalar NormalizedPerlinValue(
		const RISE::Implementation::PerlinNoise3D& perlin,
		const RISE::Scalar x,
		const RISE::Scalar y,
		const RISE::Scalar z )
	{
		return Clamp01( ( perlin.Evaluate( x, y, z ) + 1.0 ) / 2.0 );
	}

	template< typename EvalA, typename EvalB >
	inline int CountDifferentSamples(
		const int sampleCount,
		const EvalA& evalA,
		const EvalB& evalB,
		const double tol = 1e-6 )
	{
		int differCount = 0;
		for( int i = 0; i < sampleCount; i++ ) {
			if( !IsClose( evalA( i ), evalB( i ), tol ) ) {
				differCount++;
			}
		}
		return differCount;
	}

	template< typename EvalA, typename EvalB >
	inline bool AllCloseOnSamples(
		const int sampleCount,
		const EvalA& evalA,
		const EvalB& evalB,
		const double tol = 1e-6 )
	{
		return CountDifferentSamples( sampleCount, evalA, evalB, tol ) == 0;
	}
}

#endif
