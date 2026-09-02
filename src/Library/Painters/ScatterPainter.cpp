//////////////////////////////////////////////////////////////////////
//
//  ScatterPainter.cpp - Implementation of ScatterPainter (doc 88
//  P3.2, S8).  See ScatterPainter.h for the algorithm and design
//  rationale.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "ScatterPainter.h"
#include "../Utilities/ProceduralNoiseCore.h"
#include "../Utilities/Math3D/Math3D.h"
#include <cmath>

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	//! Deterministic per-cell pseudo-random scalar in [0,1) -- the
	//! IDENTICAL technique StochasticTilePainter.cpp's Hash01 uses
	//! (NoiseCore::WorleyHashCell, channel-separated, seed folded into
	//! the hashed z-coordinate).  Channels here: 0 = position x,
	//! 1 = position y, 2 = rotation, 3 = scale, 4 = the probability
	//! roll -- five independent draws per cell id, one hash call each.
	inline Scalar Hash01( int cx, int cy, int channel, unsigned int seed )
	{
		const unsigned int zu = (unsigned int)channel * 1000003u + seed * 7919u;
		const unsigned int h = NoiseCore::WorleyHashCell( cx, cy, (int)zu );
		return Scalar( h ) / Scalar( 4294967296.0 );	// [0,1)
	}

	// P2-a (S8 review round 1): straight-alpha Porter-Duff "over",
	// folding in the BACKGROUND's own alpha as well as the
	// foreground's -- not just fg_a as a plain lerp factor.  The
	// previous code computed `out = bg*(1-fg_a) + fg*fg_a`, which is
	// only correct when bg_a == 1 (an opaque background); it silently
	// dropped bg_a everywhere else, disagreeing with GetAlpha() below
	// (which already folds bg_a correctly) -- inconsistent whenever
	// `background` is itself a painter with partial coverage (e.g.
	// another scatter_painter / a cutout PNG), which the header's own
	// "stacking scatter layers" pitch requires to work.
	//
	// Standard straight-alpha over (Porter & Duff 1984):
	//   out_a     = fg_a + bg_a*(1-fg_a)
	//   out_color = ( fg*fg_a + bg*bg_a*(1-fg_a) ) / out_a
	//
	// out_a == 0 only when both fg_a and bg_a are 0 (nothing, anywhere,
	// contributes) -- there is no meaningful colour to divide out, so
	// ComposeOverColor falls back to `bg` (the same "background colour
	// convention" GetColor/GetColorNM/GetSpectrum already use for the
	// !hi.found / hi.alpha<=0 case, just generalized to cover the
	// bg_a==0 sub-case too).
	//
	// Reduces EXACTLY to the pre-fix formula when bg_a == 1 (out_a == 1
	// identically), so opaque-background scenes are bit-for-bit
	// unchanged -- verified by algebra, and the existing opaque-bg
	// goldens are the regression guard.
	inline Scalar ComposeOverAlpha( Scalar fgA, Scalar bgA )
	{
		return fgA + bgA * ( Scalar( 1 ) - fgA );
	}

	template <typename T>
	inline T ComposeOverColor( const T& fg, Scalar fgA, const T& bg, Scalar bgA, Scalar outA )
	{
		if( outA <= Scalar( 1e-9 ) ) return bg;	// nothing contributes; background colour convention
		// RISEPel has no operator/, so multiply by the reciprocal
		// (matching StochasticTilePainter.cpp's identical convention).
		const Scalar invOutA = Scalar( 1 ) / outA;
		return ( fg * fgA + bg * ( bgA * ( Scalar( 1 ) - fgA ) ) ) * invOutA;
	}
}

ScatterPainter::ScatterPainter(
	const IPainter&		source_,
	const IPainter&		background_,
	const Scalar		cellScale_,
	const Scalar		stampScale_,
	const Scalar		jitterPosition_,
	const Scalar		jitterRotationDeg_,
	const Scalar		jitterScale_,
	const Scalar		probability_,
	const unsigned int	seed_ )
	:
	source( source_ ),
	background( background_ ),
	cellScale( cellScale_ ),
	stampScale( stampScale_ ),
	jitterPosition( jitterPosition_ ),
	jitterRotationDeg( jitterRotationDeg_ ),
	jitterScale( jitterScale_ ),
	probability( probability_ ),
	seed( seed_ )
{
	source.addref();
	background.addref();
}

ScatterPainter::~ScatterPainter()
{
	source.release();
	background.release();
}

bool ScatterPainter::FindStamp( const Point2& coord, Point2& localUV ) const
{
	const Point2 p( coord.x * cellScale, coord.y * cellScale );
	// P1-A (S8 review round 1): bare (int)floor is UB for |p.x|/|p.y| >
	// INT_MAX, reachable via an unbounded cell_scale against an
	// ordinary UV.  Route through the shared, clamping
	// NoiseCore::SafeFloorToInt (see StochasticTilePainter.cpp's
	// identical fix and ProceduralNoiseCore.h for the rationale).
	const int qi = NoiseCore::SafeFloorToInt( p.x );
	const int qj = NoiseCore::SafeFloorToInt( p.y );

	bool found = false;
	Scalar bestDist2 = 0;
	Point2 bestLocal;

	// Fixed row-major scan order over the 3x3 neighbourhood -- see
	// file header point 4 ("draw-order stability").
	for( int dj = -1; dj <= 1; ++dj ) {
		for( int di = -1; di <= 1; ++di ) {
			const int ci = qi + di;
			const int cj = qj + dj;

			// Channel 4: per-cell occupancy roll.  Strict `<` so
			// probability == 0 NEVER activates a cell (Hash01's range
			// is [0,1), so `roll < 0.0` is always false) and
			// probability == 1 ALWAYS activates one (Hash01 < 1.0 is
			// always true).
			const Scalar roll = Hash01( ci, cj, 4, seed );
			if( roll >= probability ) continue;

			// Channels 0/1: jittered center, guaranteed to stay within
			// the OWNING cell (ci,cj) for jitterPosition in [0,1] --
			// this is what makes the neighbourhood-reach bound in the
			// file header independent of jitterPosition.
			const Scalar rx = Scalar( 2 ) * Hash01( ci, cj, 0, seed ) - Scalar( 1 );
			const Scalar ry = Scalar( 2 ) * Hash01( ci, cj, 1, seed ) - Scalar( 1 );
			const Point2 C(
				Scalar( ci ) + Scalar( 0.5 ) + jitterPosition * Scalar( 0.5 ) * rx,
				Scalar( cj ) + Scalar( 0.5 ) + jitterPosition * Scalar( 0.5 ) * ry );

			// Channel 2: rotation, +/- jitterRotationDeg degrees.
			const Scalar rr = Scalar( 2 ) * Hash01( ci, cj, 2, seed ) - Scalar( 1 );
			const Scalar angleRad = jitterRotationDeg * rr * DEG_TO_RAD;

			// Channel 3: size, stampScale * (1 +/- jitterScale).
			// jitterScale is parser-validated to [0,1), so this factor
			// is always > 0.
			const Scalar rs = Scalar( 2 ) * Hash01( ci, cj, 3, seed ) - Scalar( 1 );
			const Scalar S = stampScale * ( Scalar( 1 ) + jitterScale * rs );
			if( S <= Scalar( 1e-9 ) ) continue;	// defensive; shouldn't occur given the parser bound above

			const Scalar dx = p.x - C.x;
			const Scalar dy = p.y - C.y;

			// Inverse-rotate (rotate by -angle) into the stamp's own
			// axes, then normalize by its side length S into the
			// [0,1]^2 local frame (0.5 offset centers it).
			const Scalar cs = Scalar( std::cos( (double)angleRad ) );
			const Scalar sn = Scalar( std::sin( (double)angleRad ) );
			const Scalar lx =  cs * dx + sn * dy;
			const Scalar ly = -sn * dx + cs * dy;

			const Scalar u = lx / S + Scalar( 0.5 );
			const Scalar v = ly / S + Scalar( 0.5 );
			if( u < Scalar( 0 ) || u > Scalar( 1 ) || v < Scalar( 0 ) || v > Scalar( 1 ) ) continue;

			const Scalar d2 = dx * dx + dy * dy;
			if( !found || d2 < bestDist2 ) {
				found = true;
				bestDist2 = d2;
				bestLocal = Point2( u, v );
			}
		}
	}

	if( found ) localUV = bestLocal;
	return found;
}

ScatterPainter::HitInfo ScatterPainter::Resolve( const RayIntersectionGeometric& ri ) const
{
	HitInfo hi( ri );
	Point2 local;
	hi.found = FindStamp( ri.ptCoord, local );
	if( hi.found ) {
		hi.ri2.ptCoord = local;
		// Local sample coordinate is unrelated to the original
		// screen-space footprint -- see file header.
		hi.ri2.txFootprint.valid = false;
		hi.alpha = source.GetAlpha( hi.ri2 );
	}
	return hi;
}

RISEPel ScatterPainter::GetColor( const RayIntersectionGeometric& ri ) const
{
	const HitInfo hi = Resolve( ri );
	if( !hi.found || hi.alpha <= Scalar( 0 ) ) return background.GetColor( ri );
	const RISEPel sc = source.GetColor( hi.ri2 );
	if( hi.alpha >= Scalar( 1 ) ) return sc;
	const RISEPel bg = background.GetColor( ri );
	const Scalar bgA = background.GetAlpha( ri );
	return ComposeOverColor( sc, hi.alpha, bg, bgA, ComposeOverAlpha( hi.alpha, bgA ) );
}

Scalar ScatterPainter::GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	const HitInfo hi = Resolve( ri );
	if( !hi.found || hi.alpha <= Scalar( 0 ) ) return background.GetColorNM( ri, nm );
	const Scalar sc = source.GetColorNM( hi.ri2, nm );
	if( hi.alpha >= Scalar( 1 ) ) return sc;
	const Scalar bg = background.GetColorNM( ri, nm );
	const Scalar bgA = background.GetAlpha( ri );
	return ComposeOverColor( sc, hi.alpha, bg, bgA, ComposeOverAlpha( hi.alpha, bgA ) );
}

Scalar ScatterPainter::GetRadianceNM( const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	// Coverage compose over the two sources' RADIANCE -- see the header
	// declaration's comment.  Structurally identical to GetColorNM; only
	// the per-source call changes.
	const HitInfo hi = Resolve( ri );
	if( !hi.found || hi.alpha <= Scalar( 0 ) ) return background.GetRadianceNM( ri, nm );
	const Scalar sc = source.GetRadianceNM( hi.ri2, nm );
	if( hi.alpha >= Scalar( 1 ) ) return sc;
	const Scalar bg = background.GetRadianceNM( ri, nm );
	const Scalar bgA = background.GetAlpha( ri );
	return ComposeOverColor( sc, hi.alpha, bg, bgA, ComposeOverAlpha( hi.alpha, bgA ) );
}

SpectralPacket ScatterPainter::GetSpectrum( const RayIntersectionGeometric& ri ) const
{
	const HitInfo hi = Resolve( ri );

	const Scalar lambda_begin = Scalar( 380 );
	const Scalar lambda_end   = Scalar( 780 );
	const unsigned int nbins  = 81;
	SpectralPacket sp( lambda_begin, lambda_end, nbins );
	const Scalar delta = ( lambda_end - lambda_begin ) / Scalar( nbins );

	// Alpha (background's and the winning stamp's) is achromatic --
	// computed once outside the per-wavelength loop, same as the
	// colour paths above compute it once per call rather than once
	// per channel.
	const bool composite = hi.found && hi.alpha > Scalar( 0 ) && hi.alpha < Scalar( 1 );
	const Scalar bgA = composite ? background.GetAlpha( ri ) : Scalar( 0 );
	const Scalar outA = composite ? ComposeOverAlpha( hi.alpha, bgA ) : Scalar( 0 );

	for( unsigned int b = 0; b < nbins; ++b ) {
		const Scalar lambda = lambda_begin + Scalar( b ) * delta;
		const Scalar bg = background.GetColorNM( ri, lambda );
		if( !hi.found || hi.alpha <= Scalar( 0 ) ) {
			sp.SetAtIndex( b, bg );
			continue;
		}
		const Scalar sc = source.GetColorNM( hi.ri2, lambda );
		sp.SetAtIndex( b, hi.alpha >= Scalar( 1 ) ? sc : ComposeOverColor( sc, hi.alpha, bg, bgA, outA ) );
	}
	return sp;
}

Scalar ScatterPainter::GetAlpha( const RayIntersectionGeometric& ri ) const
{
	const HitInfo hi = Resolve( ri );
	const Scalar bgA = background.GetAlpha( ri );
	if( !hi.found ) return bgA;
	// Standard Porter-Duff "over" alpha, matching the colour path's
	// "over" compositing -- see file header point 6 and the shared
	// ComposeOverAlpha helper above (single source of truth for both).
	return ComposeOverAlpha( hi.alpha, bgA );
}
