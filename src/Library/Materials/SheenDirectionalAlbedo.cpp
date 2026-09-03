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
	//! roughness.  The warp puts ~6 nodes below mu = 0.03.
	Scalar CosThetaPos( const Scalar cosTheta )
	{
		// FLOORED AT NODE 1 -- constant extrapolation below it, and this
		// is the table's STATED DOMAIN rather than a clamp of
		// convenience.  See the header's "THE DOMAIN IS [mu1, 1]" note.
		//
		// Node 0 sits at mu = 0 holding E = 0 (analytically exact: V's
		// geometric cutoff fires for every incident direction when
		// n.v == 0), and node 1 at mu = 1/961.  Interpolating between
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
}

Scalar SheenDirectionalAlbedo::E( const Scalar alpha, const Scalar cosTheta )
{
	const Stencil1D sa = BuildStencil( AlphaPos( alpha ), kNumAlphaBins );
	const Stencil1D sc = BuildStencil( CosThetaPos( cosTheta ), kNumCosThetaBins );

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
