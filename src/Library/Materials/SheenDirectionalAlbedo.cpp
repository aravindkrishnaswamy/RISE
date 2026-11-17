//////////////////////////////////////////////////////////////////////
//
//  SheenDirectionalAlbedo.cpp - Interpolation of the baked Charlie
//    sheen directional- and bihemispherical-albedo tables.  See
//    SheenDirectionalAlbedo.h for what the tables hold and
//    tools/SheenDirectionalAlbedoGen.cpp for how they were produced.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "SheenDirectionalAlbedo.h"
#include "../Utilities/math_utils.h"   // r_max
#include <cmath>

using namespace RISE;

namespace
{
	inline Scalar Clamp01( const Scalar x )
	{
		return x < Scalar(0) ? Scalar(0) : ( x > Scalar(1) ? Scalar(1) : x );
	}

	//! Map `alpha` to a continuous bin position on the LOG-spaced alpha
	//! axis, clamped to the table's extent.  Shared by BOTH lookups so
	//! they cannot disagree about which alpha row(s) they read.
	Scalar AlphaPos( const Scalar alpha )
	{
		const Scalar aMin = (Scalar)SheenDirectionalAlbedo::kAlphaMin;
		const Scalar aMax = (Scalar)SheenDirectionalAlbedo::kAlphaMax;
		const Scalar aClamped = ( alpha < aMin ) ? aMin : ( ( alpha > aMax ) ? aMax : alpha );
		const Scalar logSpan = std::log( aMax / aMin );
		const Scalar t = ( logSpan > 0 ) ? ( std::log( aClamped / aMin ) / logSpan ) : Scalar(0);
		return Clamp01( t ) * (Scalar)( SheenDirectionalAlbedo::kNumAlphaBins - 1 );
	}

	//! Two-point stencil (index0, index1, blend fraction) for a
	//! continuous position on an axis of `n` endpoint-inclusive nodes.
	struct Stencil1D
	{
		unsigned int i0, i1;
		Scalar frac;
	};

	//! Map `cosTheta` to a continuous bin position on the GRAZING-WARPED
	//! cosTheta axis.  The generator lays nodes at `mu_j = (j/(N-1))^2`,
	//! so the inverse is `sqrt(mu)` -- this function and
	//! SheenDirectionalAlbedoGen's `CosThetaAt` are a matched pair and
	//! must be changed together.
	//!
	//! WHY WARPED (M1 review, 2026-09-02).  The Charlie lobe is already
	//! near its peak by mu ~ 0.005.  A UNIFORM 32-node axis puts its
	//! first interior node at mu = 0.0323 with an exact 0 at mu = 0, so
	//! this lookup ramped LINEARLY FROM ZERO across the whole band the
	//! lobe occupies -- reading 0.146 where the truth is 1.19 at
	//! alpha = 0.04, mu = 0.005.  Since `fabric_material` EMITS the true
	//! lobe while suppressing the substrate by whatever this function
	//! returns, that under-read broke the white-furnace energy identity
	//! by up to +1.05 ABSOLUTE under grazing illumination, at every
	//! roughness.  The warp puts ~11 nodes below mu = 0.03 (round 9,
	//! 2026-09-04, 32 -> 64; was ~6 of 32).
	Scalar CosThetaPos( const Scalar cosTheta )
	{
		// FLOORED AT NODE 1 -- constant extrapolation below it, and this
		// is the table's STATED DOMAIN rather than a clamp of
		// convenience.  See the header's "THE DOMAIN IS [mu1, 1]" note.
		//
		// Node 0 sits at mu = 0 holding E = 0 (analytically exact: V's
		// geometric cutoff fires for every incident direction when
		// n.v == 0), and node 1 at mu = 1/3969 (was 1/961 at the
		// retired 32-node table; round 9, 2026-09-04).  Interpolating between
		// them ramps E from ZERO across a cell in which the true lobe is
		// already at its PEAK -- E_true(alpha=0.04, mu=5e-6) is 0.53 and
		// rises to 1.15 by node 1, while the un-floored interpolant
		// read 0.08 there.  That is the SAME defect the warped axis was
		// introduced to fix, one cell narrower, and it is not bounded by
		// FabricBRDF's normaliser: `max(1, E(v), E(l))` is built from
		// the TABLED E, so where E_tab < 1 < E_true it collapses to 1,
		// the lobe is emitted undivided AND the base is barely
		// suppressed.  Measured white-Lambertian rho reached 1.714.
		//
		// Flooring works because E_true is MONOTONE INCREASING on
		// (0, mu1): E(mu1) >= E_true(mu) for every mu below it, so Ehat
		// fully suppresses the base and the normaliser divides the lobe
		// honestly.  Re-measured across the cell, rho never exceeds 1
		// and decays gracefully toward the true E -> 0 limit (0.46 at
		// mu = 5e-6, alpha = 0.04).  At and above mu1 this is a
		// bit-identical no-op.
		const Scalar t = (Scalar)( SheenDirectionalAlbedo::kNumCosThetaBins - 1 );
		const Scalar mu1 = Scalar(1) / ( t * t );		// node 1 of the warped axis
		return std::sqrt( Clamp01( r_max( cosTheta, mu1 ) ) ) * t;
	}

	Stencil1D BuildStencil( const Scalar pos, const unsigned int n )
	{
		Stencil1D s;
		Scalar p = pos;
		if( p < 0 ) { p = 0; }
		const Scalar pMax = (Scalar)( n - 1 );
		if( p > pMax ) { p = pMax; }
		s.i0 = (unsigned int)p;
		if( s.i0 >= n - 1 ) { s.i0 = n - 2; }
		s.i1 = s.i0 + 1;
		s.frac = Clamp01( p - (Scalar)s.i0 );
		return s;
	}

	//! DL-11 fix (2026-09-14).  `BuildStencil`'s frac is linear in the
	//! WARPED POSITION s = sqrt(mu) -- fine for CHOOSING the bracket
	//! (s is monotone in mu, so `BuildStencil(CosThetaPos(mu), ...)`
	//! still picks the correct two nodes), but wrong for BLENDING
	//! between them: CLOTH_FABRIC_DESIGN.md's round-9 exhaustive search
	//! found the middle band's (mu1 <= n.v < 0.0349) worst-case rho
	//! (1.0075, +0.75%) sitting at alpha ~= 0.065 -- close to
	//! `kMinSheenAlpha`, essentially AT an alpha grid node, not between
	//! two -- which pins the driver to the cosTheta axis alone.  An
	//! independent brute-force probe (scratch, not shipped) confirmed
	//! it: between mu-nodes 1 and 2 at low alpha, `E` is measurably
	//! concave in s = sqrt(mu) (linear-in-s under-reads the true curve
	//! by up to ~0.017 there), so the very fix that closed the
	//! round-6/7 grazing catastrophe -- warp harder in s -- makes this
	//! narrower, lower-alpha residual WORSE, not better: pushing node 1
	//! closer to `V`'s hard cutoff (`CharlieSheen::V` zeros when
	//! `n.l*n.v < 1e-6`) sharpens the curvature linear-in-s has to
	//! track, rather than resolving it (measured: raising N from 64 to
	//! 128/256/512 at the SAME warp power, or raising the warp power at
	//! N=64, both eventually REGRESS the worst-case error instead of
	//! shrinking it -- "another blind resolution doubling" the doc's
	//! own text warned against, confirmed empirically rather than
	//! merely asserted).
	//!
	//! The fix is not more/sharper warping; it is the RIGHT interpolation
	//! VARIABLE.  The alpha axis already blends log-linearly (`AlphaPos`
	//! above) because `D`'s exponent is 1/alpha -- log-alpha is what
	//! makes that axis's curve close to piecewise-linear.  The cosTheta
	//! axis never got the same treatment; it blends linearly in s
	//! instead of in log(mu).  Switching the BLEND FRACTION (not the
	//! node placement -- the shipped `kETable` is unchanged, so no
	//! rebake) to log(mu) cuts the measured worst-case interpolation
	//! error at EVERY alpha tested (0.04 to 1.0), most where it matters:
	//! -0.0172 -> -0.0099 absolute at alpha=0.04 (the global worst),
	//! -0.0104 -> -0.0046 at alpha=0.065.  No regression found anywhere
	//! in a full-domain scan (mu1..1, alpha 0.04..1.0).
	//!
	//! Node 0 (mu=0, log(mu)=-inf) is never a bracket endpoint here --
	//! `CosThetaPos` already floors its input at mu1 before this runs,
	//! so `i0 >= 1` always (see that function's own floor note) and
	//! `log(muI0)` is always finite.
	Scalar CosThetaLogFrac( const Scalar cosThetaFloored, const Stencil1D& sc, const unsigned int n )
	{
		const Scalar t = (Scalar)( n - 1 );
		const Scalar s0 = (Scalar)sc.i0 / t;
		const Scalar s1 = (Scalar)sc.i1 / t;
		const Scalar muI0 = s0 * s0;
		const Scalar muI1 = s1 * s1;
		if( !( muI1 > muI0 ) || !( cosThetaFloored > 0 ) ) {
			return sc.frac;		// degenerate cell or non-positive mu: fall back to the s-linear frac (matches pre-fix behaviour, unreachable at n>=2 with a floored mu)
		}
		const Scalar f = ( std::log( cosThetaFloored ) - std::log( muI0 ) )
		                / ( std::log( muI1 ) - std::log( muI0 ) );
		return Clamp01( f );
	}
}

Scalar SheenDirectionalAlbedo::E( const Scalar alpha, const Scalar cosTheta )
{
	const Stencil1D sa = BuildStencil( AlphaPos( alpha ), kNumAlphaBins );
	Stencil1D sc = BuildStencil( CosThetaPos( cosTheta ), kNumCosThetaBins );

	// DL-11: blend the cosTheta axis in log(mu), not in the warped
	// position s -- see CosThetaLogFrac above.  `cosTheta` is re-floored
	// here identically to CosThetaPos's own floor (mu1 = 1/(N-1)^2) so
	// the two never disagree about what "the floored mu" was.
	{
		const Scalar t = (Scalar)( kNumCosThetaBins - 1 );
		const Scalar mu1 = Scalar(1) / ( t * t );
		const Scalar muFloored = Clamp01( r_max( cosTheta, mu1 ) );
		sc.frac = CosThetaLogFrac( muFloored, sc, kNumCosThetaBins );
	}

	const Scalar v00 = kETable[sa.i0][sc.i0];
	const Scalar v01 = kETable[sa.i0][sc.i1];
	const Scalar v10 = kETable[sa.i1][sc.i0];
	const Scalar v11 = kETable[sa.i1][sc.i1];

	const Scalar row0 = ( Scalar(1) - sc.frac ) * v00 + sc.frac * v01;
	const Scalar row1 = ( Scalar(1) - sc.frac ) * v10 + sc.frac * v11;
	return ( Scalar(1) - sa.frac ) * row0 + sa.frac * row1;
}

Scalar SheenDirectionalAlbedo::EHatMean( const Scalar alpha )
{
	const Stencil1D sa = BuildStencil( AlphaPos( alpha ), kNumAlphaBins );
	return ( Scalar(1) - sa.frac ) * kEHatMeanTable[sa.i0] + sa.frac * kEHatMeanTable[sa.i1];
}
