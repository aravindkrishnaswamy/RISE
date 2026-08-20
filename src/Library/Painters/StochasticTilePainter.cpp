//////////////////////////////////////////////////////////////////////
//
//  StochasticTilePainter.cpp - Implementation of StochasticTilePainter
//  (doc 88 P3.1, S8).  See StochasticTilePainter.h for the algorithm
//  and design rationale.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "StochasticTilePainter.h"
#include "../Utilities/ProceduralNoiseCore.h"
#include <cmath>
#include <algorithm>

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	//! Deterministic per-lattice-vertex pseudo-random scalar in [0,1),
	//! built from NoiseCore::WorleyHashCell -- the SAME integer-mixing
	//! hash NoiseCore already uses for Worley cell ids (see
	//! ProceduralNoiseCore.cpp).  This introduces no new hash
	//! primitive, just a new combination of an existing one: `channel`
	//! selects an independent draw per vertex (0 = offset u, 1 = offset
	//! v), and `seed` is folded into the hashed z-coordinate so
	//! re-seeding produces an unrelated draw without disturbing the
	//! lattice-vertex (x,y) hash inputs.  ScatterPainter.cpp uses the
	//! identical technique (see its Hash01) -- kept as a small
	//! duplicated helper rather than a new shared header, since it is
	//! five lines and the two callers hash different things (channel
	//! counts differ).
	inline Scalar Hash01( int vx, int vy, int channel, unsigned int seed )
	{
		const unsigned int zu = (unsigned int)channel * 1000003u + seed * 7919u;
		const unsigned int h = NoiseCore::WorleyHashCell( vx, vy, (int)zu );
		return Scalar( h ) / Scalar( 4294967296.0 );	// [0,1)
	}
}

StochasticTilePainter::StochasticTilePainter(
	const IPainter&		source_,
	const Scalar		tileScale_,
	const unsigned int	seed_,
	const RISEPel&		mean_,
	const Scalar		blendGamma_ )
	:
	source( source_ ),
	tileScale( tileScale_ ),
	seed( seed_ ),
	mean( mean_ ),
	meanSpec( RGBAlbedoSpectrum::FromRGB( mean_ ) ),
	blendGamma( blendGamma_ )
{
	source.addref();
}

StochasticTilePainter::~StochasticTilePainter()
{
	source.release();
}

void StochasticTilePainter::ComputeHexTiling( const Point2& coord, Scalar w[3], Point2 offsetUV[3] ) const
{
	// Equilateral-triangle lattice basis e1 = (1,0), e2 = (0.5,
	// sqrt(3)/2).  Solving p = i*e1 + j*e2 for the (real-valued)
	// lattice coordinates (i,j) of a point p: j = p.y / (sqrt(3)/2),
	// i = p.x - 0.5*j.  This is the standard change of basis onto an
	// equilateral-triangle grid -- self-derived from the basis
	// definition, see the file header.
	const Scalar kSqrt3Over2 = Scalar( 0.86602540378443864676 );
	const Point2 p( coord.x * tileScale, coord.y * tileScale );
	const Scalar j = p.y / kSqrt3Over2;
	const Scalar i = p.x - Scalar( 0.5 ) * j;

	const Scalar baseI = std::floor( (double)i );
	const Scalar baseJ = std::floor( (double)j );
	const Scalar fi = i - baseI;
	const Scalar fj = j - baseJ;
	// P1-A (S8 review round 1): bare (int)floor is UB for |i|/|j| >
	// INT_MAX, reachable via an unbounded tile_scale against an
	// ordinary UV.  Route through the shared, clamping
	// NoiseCore::SafeFloorToInt -- same pattern PerlinOctave3D already
	// uses (fracX is the unclamped x-floor(x), always in [0,1)
	// regardless of magnitude, while the hash-lattice integer index is
	// separately clamped) -- rather than casting the unclamped baseI/
	// baseJ above.
	const int bi = NoiseCore::SafeFloorToInt( i );
	const int bj = NoiseCore::SafeFloorToInt( j );

	// The unit cell (bi,bj)-(bi+1,bj)-(bi,bj+1)-(bi+1,bj+1) splits into
	// two triangles along fi+fj==1.  Each branch below is the plain
	// barycentric-coordinate solve for its triangle -- see the file
	// header for why the two branches agree (are continuous) across
	// the shared edge.
	int vi[3], vj[3];
	Scalar wraw[3];
	if( fi + fj <= Scalar( 1 ) ) {
		vi[0] = bi;     vj[0] = bj;     wraw[0] = Scalar( 1 ) - fi - fj;
		vi[1] = bi + 1; vj[1] = bj;     wraw[1] = fi;
		vi[2] = bi;     vj[2] = bj + 1; wraw[2] = fj;
	} else {
		vi[0] = bi + 1; vj[0] = bj + 1; wraw[0] = fi + fj - Scalar( 1 );
		vi[1] = bi + 1; vj[1] = bj;     wraw[1] = Scalar( 1 ) - fj;
		vi[2] = bi;     vj[2] = bj + 1; wraw[2] = Scalar( 1 ) - fi;
	}

	// Sharpen (w_i^gamma) and renormalize -- see file header point 3.
	// Clamp to >= 0 first: fp round-off can put a barycentric weight a
	// hair below 0 right at a triangle boundary.
	// P3 (S8 review round 1) blend_gamma==0 corner: pow(w,0)==1 for
	// every positive raw weight, so the loop below collapses to
	// UNIFORM (1/3, 1/3, 1/3) weights everywhere regardless of the
	// pre-sharpen barycentric split -- mathematically consistent (not
	// a 0^0 case: wraw[k] is clamped >= 0 above, and pow(0,0)==1 too by
	// the same convention used here) and parser-legal (blend_gamma is
	// clamped to [0, 64]).
	Scalar wsum = 0;
	for( int k = 0; k < 3; ++k ) {
		wraw[k] = std::pow( (double)std::max( wraw[k], Scalar( 0 ) ), (double)blendGamma );
		wsum += wraw[k];
	}
	// wsum > 0 always: the three PRE-sharpen barycentric weights sum to
	// exactly 1 and are all >= 0, so at least one is > 0; pow(positive,
	// finite blendGamma) > 0, so the sum of sharpened weights is > 0
	// too (parser clamps blendGamma to [0, 64], so this can't underflow
	// to a literal 0.0 in double precision for any weight that started
	// meaningfully above 0 -- see the parser's range validation).
	for( int k = 0; k < 3; ++k ) {
		w[k] = wraw[k] / wsum;
		offsetUV[k] = Point2(
			coord.x + Hash01( vi[k], vj[k], 0, seed ),
			coord.y + Hash01( vi[k], vj[k], 1, seed ) );
	}
}

RISEPel StochasticTilePainter::GetColor( const RayIntersectionGeometric& ri ) const
{
	Scalar w[3];
	Point2 uv[3];
	ComputeHexTiling( ri.ptCoord, w, uv );

	RayIntersectionGeometric ri2 = ri;
	// Each sample is at a hash-derived offset UV unrelated to the
	// original screen-space footprint -- see file header.
	ri2.txFootprint.valid = false;

	RISEPel weighted( 0, 0, 0 );
	Scalar wsq = 0;
	for( int k = 0; k < 3; ++k ) {
		ri2.ptCoord = uv[k];
		weighted = weighted + ( source.GetColor( ri2 ) - mean ) * w[k];
		wsq += w[k] * w[k];
	}
	// Denominator bound: sqrt(wsq) in [1/sqrt(3), 1] -- see file header
	// point 4.  No runtime guard needed.  RISEPel has no operator/, so
	// multiply by the reciprocal (matching how the rest of the tree
	// scales a Pel by a Scalar).
	const Scalar invDenom = Scalar( 1 ) / Scalar( std::sqrt( (double)wsq ) );
	return mean + weighted * invDenom;
}

Scalar StochasticTilePainter::GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	Scalar w[3];
	Point2 uv[3];
	ComputeHexTiling( ri.ptCoord, w, uv );

	RayIntersectionGeometric ri2 = ri;
	ri2.txFootprint.valid = false;

	const Scalar mu = meanSpec.Eval( nm );
	Scalar weighted = 0;
	Scalar wsq = 0;
	for( int k = 0; k < 3; ++k ) {
		ri2.ptCoord = uv[k];
		weighted += ( source.GetColorNM( ri2, nm ) - mu ) * w[k];
		wsq += w[k] * w[k];
	}
	return mu + weighted / Scalar( std::sqrt( (double)wsq ) );
}

SpectralPacket StochasticTilePainter::GetSpectrum( const RayIntersectionGeometric& ri ) const
{
	Scalar w[3];
	Point2 uv[3];
	ComputeHexTiling( ri.ptCoord, w, uv );

	Scalar wsq = 0;
	for( int k = 0; k < 3; ++k ) wsq += w[k] * w[k];
	const Scalar denom = Scalar( std::sqrt( (double)wsq ) );

	const Scalar lambda_begin = Scalar( 380 );
	const Scalar lambda_end   = Scalar( 780 );
	const unsigned int nbins  = 81;
	SpectralPacket sp( lambda_begin, lambda_end, nbins );
	const Scalar delta = ( lambda_end - lambda_begin ) / Scalar( nbins );

	RayIntersectionGeometric ri2 = ri;
	ri2.txFootprint.valid = false;

	for( unsigned int b = 0; b < nbins; ++b ) {
		const Scalar lambda = lambda_begin + Scalar( b ) * delta;
		const Scalar mu = meanSpec.Eval( lambda );
		Scalar weighted = 0;
		for( int k = 0; k < 3; ++k ) {
			ri2.ptCoord = uv[k];
			weighted += ( source.GetColorNM( ri2, lambda ) - mu ) * w[k];
		}
		sp.SetAtIndex( b, mu + weighted / denom );
	}
	return sp;
}

Scalar StochasticTilePainter::GetAlpha( const RayIntersectionGeometric& ri ) const
{
	Scalar w[3];
	Point2 uv[3];
	ComputeHexTiling( ri.ptCoord, w, uv );

	RayIntersectionGeometric ri2 = ri;
	ri2.txFootprint.valid = false;

	// Plain convex combination -- NOT histogram-preserving-restored;
	// alpha is coverage, not a value distribution.  See file header.
	Scalar sum = 0;
	for( int k = 0; k < 3; ++k ) {
		ri2.ptCoord = uv[k];
		sum += source.GetAlpha( ri2 ) * w[k];
	}
	return sum;
}
