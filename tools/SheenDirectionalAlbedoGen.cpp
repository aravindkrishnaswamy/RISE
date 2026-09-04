//////////////////////////////////////////////////////////////////////
//
//  SheenDirectionalAlbedoGen.cpp - Bakes the directional- and
//    bihemispherical-albedo tables for the Charlie sheen lobe
//    (docs/CLOTH_FABRIC_DESIGN.md, section 9.4 "The E table" and
//    section 9.2's `hemisphericalAlbedo` route 1 / route 1b / route 2
//    discussion, `sed -n '/^### 9.2/,/^### 9.5/p' docs/CLOTH_FABRIC_DESIGN.md`).
//
//  Writes src/Library/Materials/SheenDirectionalAlbedo_LUTData.cpp
//  directly (no intermediate binary artefact -- same choice
//  tools/HairMedullaProfileGen.cpp makes, for the same reason: the
//  generated C++ is the only consumer).  Run from the PROJECT ROOT:
//
//      c++ -O3 -std=c++17 -I src/Library -o /tmp/SheenDirectionalAlbedoGen \
//          tools/SheenDirectionalAlbedoGen.cpp
//      /tmp/SheenDirectionalAlbedoGen
//
//  (No link against the RISE library is needed or done -- see
//  "WHERE THE MATH COMES FROM" below.)  The exact invocation and the
//  git commit are recorded in the generated file's banner.  Re-run
//  only if CharlieSheen.h's D/Lambda/V coefficients ever change; the
//  output is deterministic (no RNG anywhere -- see "QUADRATURE" below),
//  so a re-run on an unchanged tool reproduces the file byte-for-byte.
//
//  ------------------------------------------------------------------
//  WHERE THE MATH COMES FROM
//  ------------------------------------------------------------------
//
//  Unlike HairMedullaProfileGen.cpp (which duplicates its own PRNG so
//  the bake is bit-reproducible independent of RISE's engine RNG) and
//  GenerateMicrofacetEnergyLUT.cpp (which predates this convention and
//  inlines its own GGX math), THIS generator #includes
//  src/Library/Materials/CharlieSheen.h directly and calls
//  RISE::Implementation::CharlieSheen::D / ::V verbatim.  CharlieSheen.h
//  has no .cpp counterpart and pulls in only Math3D.h / math_utils.h,
//  both of which compile standalone with a plain `-I src/Library`
//  (verified: no pch.h, no library link required) -- so there is no
//  duplicated-coefficient risk here the way there is in the two
//  precedents above.  If CharlieSheen.h's D or Lambda/V coefficients
//  ever change, this table MUST be regenerated; nothing else keeps it
//  in sync.
//
//  ------------------------------------------------------------------
//  WHAT IS BAKED, AND THE EXACT NORMALISATION
//  ------------------------------------------------------------------
//
//  Charlie sheen (sheenColor == 1, i.e. the achromatic lobe -- colour
//  is a linear post-multiply, so E and S are colour-independent):
//
//    f(l, v) = D(alpha, n.h) * V(alpha, n.l, n.v),   h = normalize(l+v)
//
//  Three tables, all indexed by cos(theta) rather than theta itself
//  (matching MicrofacetEnergyLUT.h's E_ss/E_avg shape, which this
//  generator's *quantities* mirror even though the on-disk layout
//  follows HairMedullaProfile's extern-array-plus-generator split):
//
//   1. E(alpha, cosThetaV) = integral over the upper hemisphere of l of
//        f(l, v) * (n.l) dl
//      -- the directional-hemispherical albedo of the bare lobe.  This
//      is exactly MicrofacetEnergyLUT's E_ss, for the Charlie lobe
//      instead of GGX.
//
//   2. EHatMean(alpha) = 2 * integral_0^1 min(E(alpha, mu), 1) * mu dmu
//      -- the hemispherical mean of the CLAMPED lobe albedo
//      Ehat = min(E, 1), MicrofacetEnergyLUT's E_avg for Ehat rather
//      than for E.  The clamp is in the NAME because it is the quantity
//      `fabric_material` actually multiplies the substrate by; baking
//      the mean of the raw E would make `hemisphericalAlbedo`'s closed
//      form disagree with `value()`'s own denominator.
//
//      Computed HERE from the BAKED 64-point E row (round 9: 32 -> 64,
//      2026-09-04) -- trapezoid over
//      the WARPED node spacing for [mu1, 1], plus an analytic
//      constant-extrapolation term for [0, mu1] mirroring the runtime's
//      floor (see ComputeEHatMeanFromRow) -- not from a separate
//      higher-resolution integral, so a test can reconstruct it from
//      the shipped E values alone.  There is exactly one number here,
//      not two that could quietly drift apart.
//
//  RETIRED: S(alpha, m).  Until 2026-09-02 this generator also baked a
//  2-D kernel table S(alpha, m) -- the bihemispherical average of the
//  round-4 `min(1 - m*E(v), 1 - m*E(l))` sheen/base mixing kernel --
//  because a `min` DOES NOT FACTOR into (a function of l) x (a function
//  of v) and its double integral could only be tabulated.  Round 5
//  replaced that kernel with the Kulla-Conty PRODUCT form
//
//      (1 - m*E(v)) * (1 - m*E(l)) / (1 - m*Ebar)
//
//  which factors by construction, so the whole table collapsed to a
//  closed form in Ebar alone:
//
//      INT INT f_base * kernel * (n.l)(n.v) dl dv
//        = rho_base * (1 - m*Ebar)^2 / (1 - m*Ebar)
//        = rho_base * (1 - m*Ebar)
//
//  There is therefore nothing left for a third table to hold.  See
//  FabricBRDF.cpp's hemisphericalAlbedo derivation and
//  docs/CLOTH_FABRIC_DESIGN.md 9.2 (round 5).
//
//
//  ------------------------------------------------------------------
//  QUADRATURE
//  ------------------------------------------------------------------
//
//  No Monte Carlo anywhere in this file -- every integral is a
//  deterministic fixed-grid quadrature, so two runs of an unchanged
//  binary produce byte-identical output.
//
//   E(alpha, cosThetaV): 2-D MIDPOINT rule over (muL, phiL) in
//     [0,1] x [0, 2*pi).  V(alpha, muL, muV) does not depend on phiL,
//     so it is hoisted out of the inner loop (and short-circuits the
//     whole phiL loop to zero when the geometric cutoff in
//     CharlieSheen::V fires -- which is every muL when muV == 0
//     exactly, giving E(alpha, 0) == 0 for free, no quadrature needed
//     for that column either).  cos(phiL)/sin(phiL) are precomputed
//     once per bake (they don't depend on alpha, cosThetaV or muL) and
//     reused across every cell.
//
//  E is run at the PRODUCTION resolution and again at DOUBLE the
//  resolution on every axis; the generator reports the worst
//  (production vs. doubled) delta over the whole table to stderr, which
//  is this bake's convergence evidence (see the printed summary at the
//  end of a run and the commit message this file's regeneration is
//  committed under).
//
//  ------------------------------------------------------------------
//  AXES
//  ------------------------------------------------------------------
//
//   alpha:     kNumAlphaBins entries, LOG-spaced over
//              [kAlphaMin, kAlphaMax].  CharlieSheen::D floors alpha at
//              1e-3 internally (`r_max(alpha, 1e-3)`), so kAlphaMin ==
//              1e-3 matches the smallest alpha the lobe ever actually
//              evaluates at -- there is no point tabulating anything
//              smaller.  Log spacing (not linear) is deliberate: D's
//              exponent is 1/alpha, so the lobe sharpens monotonically
//              as alpha -> 0, and only a LOG grid puts enough nodes in
//              that regime for the low end of the table to track the
//              curvature of E(alpha, .) there -- a linear grid would
//              spend 63 of its 64 samples above alpha ~= 0.03 and
//              completely miss the interesting part of the curve below
//              it.  Endpoints are INCLUDED (node i is
//              kAlphaMin * (kAlphaMax/kAlphaMin)^(i/(N-1))), matching
//              HairMedullaProfileGen's tau axis and
//              GenerateMicrofacetEnergyLUT's alpha axis.
//
//   cosTheta:  kNumCosThetaBins entries, GRAZING-WARPED --
//              node j at mu = (j/(N-1))^2 -- endpoints INCLUDED.  At
//              N = 64 (round 9, 2026-09-04) node 1 sits at
//              1/63^2 = 1/3969 = 2.52e-4, and about ELEVEN of the 64
//              nodes fall below mu = 0.03 -- roughly double the density
//              of the N = 32 table's ~SIX, since the warp is quadratic
//              and doubling N halves the node spacing near the origin
//              too.  A UNIFORM axis (which this generator used until
//              2026-09-02) put its first interior node at 0.0323, so the
//              runtime interpolant ramped from the exact 0 at node 0
//              across the entire band the Charlie lobe occupies -- and
//              the lobe is already near its PEAK by mu ~ 0.005.  See the
//              "WHY THE WARP" note on CosThetaAt.
//
//              cosTheta == 0 remains an exact node (E is analytically
//              zero there) and cosTheta == 1 an exact node at normal
//              incidence.  NOTE, though, that the RUNTIME does not use
//              the [0, mu1] cell: `SheenDirectionalAlbedo::E` floors its
//              argument at node 1 and constant-extrapolates below, so
//              node 0 exists as an anchor for the bake rather than as a
//              lookup destination -- and ComputeEHatMeanFromRow
//              integrates the FLOORED function to match.
//
//
//  ------------------------------------------------------------------
//  SIZE
//  ------------------------------------------------------------------
//
//  RAISED 32x32 -> 64x64 (round 9, 2026-09-04, debt 18): E: 64 x 64
//  floats == 16 KB.  EMean: 64 floats == 256 B.  Total table payload
//  16.25 KB -- ~4x the 4.125 KB of the N = 32 table (uniform refinement
//  on both axes is a 4x cell count), still an order of magnitude below
//  the 52.8 KB medulla table.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "Materials/CharlieSheen.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
	using RISE::Scalar;
	namespace CS = RISE::Implementation::CharlieSheen;

	// ---- table extents (must match SheenDirectionalAlbedo.h) -------
	// RAISED 32 -> 64 on BOTH axes (round 9, 2026-09-04, debt 18): a
	// UNIFORM refinement, not an ad-hoc node -- see SheenDirectionalAlbedo.h
	// and docs/CLOTH_FABRIC_DESIGN.md 9.2/15 debt 18 for the residual this
	// closes and what it leaves open.
	const unsigned int kNumAlphaBins    = 64;
	const unsigned int kNumCosThetaBins = 64;

	const double kAlphaMin = 1e-3;
	const double kAlphaMax = 1.0;

	// ---- quadrature resolution ---------------------------------------
	// E is baked with PER-CELL ADAPTIVE resolution (see BakeEAdaptive):
	// most cells converge at the starting resolution, but a narrow band
	// at low alpha and near-grazing cosThetaV (where V's
	// 1/(4*nDotL*nDotV*(...)) term and D's sharpening both peak at once)
	// needs far more nodes to converge to kETol -- see the file header's
	// "QUADRATURE" section and the measured E range in the emitted
	// banner.  kEMuStart/kEPhiStart is the cheap common case; kEMuCap is
	// the hard ceiling so one pathological cell cannot run away.
	const int    kEMuStart = 64;
	const int    kEPhiStart = 128;
	const int    kEMuCap    = 4096;
	const double kETol      = 3e-5;


	const double kTwoPI = 6.28318530717958647692;

	double AlphaAt( unsigned int i )
	{
		const double t = (double)i / (double)( kNumAlphaBins - 1 );
		return kAlphaMin * std::pow( kAlphaMax / kAlphaMin, t );
	}

	//! GRAZING-WARPED cosTheta node.  `mu_j = (j/(N-1))^2`, so node 1
	//! sits at `1/(N-1)^2` -- 1/961 = 0.00104 at the original N = 32,
	//! 1/3969 = 2.52e-4 since round 9's (2026-09-04) 32 -> 64 uniform
	//! refinement (docs/CLOTH_FABRIC_DESIGN.md 15 debt 18) -- instead of
	//! a uniform grid's 0.0323.
	//!
	//! WHY THE WARP (M1 review, 2026-09-02).  The Charlie lobe is
	//! already near its PEAK by mu ~ 0.005: E(alpha=0.04, mu=0.005) is
	//! 1.19, while a uniform 32-node axis has its first interior node at
	//! mu = 0.0323 and an exact 0 at mu = 0, so the runtime interpolant
	//! ramped LINEARLY FROM ZERO across the entire band the lobe lives
	//! in.  Measured consequence before the warp: the table read 0.146
	//! where the truth was 1.19, and because `value()` EMITS the true
	//! lobe while suppressing the base by the TABLED E, the white-furnace
	//! identity broke by up to +1.05 absolute (rho ~ 2.05) under grazing
	//! illumination -- at every roughness.  A quadratic warp puts ~6
	//! nodes below mu = 0.03 where the old axis had none (~11 of the 64
	//! nodes, at round 9's doubled resolution).
	//!
	//! mu = 0 stays an exact node (E is analytically 0 there:
	//! CharlieSheen::V's geometric cutoff fires for every incident
	//! direction when n.v == 0) and mu = 1 stays an exact node.  The
	//! runtime inverts this with `sqrt(mu)` -- see
	//! SheenDirectionalAlbedo.cpp's CosThetaPos.
	double CosThetaAt( unsigned int i )
	{
		const double t = (double)i / (double)( kNumCosThetaBins - 1 );
		return t * t;
	}


	//! Format a double as a valid C++ float literal (round-trips a
	//! float exactly through 9 significant digits).  Copied from
	//! HairMedullaProfileGen.cpp's FloatLit -- see its comment there
	//! for why plain %.9g is not sufficient on its own -- with one
	//! addition: E's extreme low-alpha, near-grazing-view corner
	//! underflows the true (double) integral below FLT_TRUE_MIN
	//! (~1.4e-45) -- e.g. 2e-106 -- which is a semantically harmless
	//! zero (that cell IS zero to float precision) but is NOT a legal
	//! narrowing to `float` as a C++ literal: clang/gcc both warn
	//! "-Wliteral-range" on it under -Wall -pedantic, which the
	//! "compiler warnings are bugs" rule (CLAUDE.md) does not allow
	//! shipping.  Anything smaller in magnitude than the smallest
	//! representable (denormal) float is therefore rounded to the
	//! literal `0.0f` here -- the value the narrowing conversion would
	//! produce anyway, just spelled so the compiler doesn't have to
	//! warn about spelling it as a tiny nonzero exponent first.
	std::string FloatLit( const double v )
	{
		if( v != 0.0 && std::fabs( v ) < 1e-44 ) {
			return "0.0f";
		}
		char buf[64];
		std::snprintf( buf, sizeof( buf ), "%.9g", v );
		std::string s( buf );
		if( s.find( '.' ) == std::string::npos &&
		    s.find( 'e' ) == std::string::npos &&
		    s.find( 'E' ) == std::string::npos &&
		    s.find( "inf" ) == std::string::npos &&
		    s.find( "nan" ) == std::string::npos ) {
			s += ".0";
		}
		return s + "f";
	}

	//////////////////////////////////////////////////////////////////
	//  E(alpha, cosThetaV) quadrature
	//////////////////////////////////////////////////////////////////

	//! Precomputed phi grid, shared by every (alpha, cosThetaV) cell.
	struct PhiGrid
	{
		std::vector<double> cosPhi, sinPhi;
		int n;

		void Build( int nPhi )
		{
			n = nPhi;
			cosPhi.assign( nPhi, 0.0 );
			sinPhi.assign( nPhi, 0.0 );
			const double dphi = kTwoPI / (double)nPhi;
			for( int k = 0; k < nPhi; k++ ) {
				const double phi = ( k + 0.5 ) * dphi;
				cosPhi[k] = std::cos( phi );
				sinPhi[k] = std::sin( phi );
			}
		}
	};

	//! Directional albedo of the bare (sheenColor == 1) Charlie lobe,
	//! by 2-D midpoint quadrature over the incident hemisphere.  See
	//! the file header for the exact integral and why V(alpha, muL,
	//! muV) is hoisted out of the phi loop.
	double ComputeE( double alpha, double muV, int nMu, const PhiGrid& phiGrid )
	{
		const double sinV = std::sqrt( std::max( 0.0, 1.0 - muV * muV ) );
		const double dmu  = 1.0 / (double)nMu;
		const double dphi = kTwoPI / (double)phiGrid.n;
		double sum = 0.0;

		for( int j = 0; j < nMu; j++ )
		{
			const double muL  = ( j + 0.5 ) * dmu;
			const double Vv   = CS::V( alpha, muL, muV );
			if( Vv <= 0.0 ) { continue; }		// geometric cutoff: whole phi row is zero
			const double sinL = std::sqrt( std::max( 0.0, 1.0 - muL * muL ) );

			double rowSum = 0.0;
			for( int k = 0; k < phiGrid.n; k++ )
			{
				const double hx = sinL * phiGrid.cosPhi[k] + sinV;
				const double hy = sinL * phiGrid.sinPhi[k];
				const double hz = muL + muV;
				const double hlen = std::sqrt( hx * hx + hy * hy + hz * hz );
				const double nDotH = ( hlen > 1e-12 ) ? ( hz / hlen ) : 1.0;
				rowSum += CS::D( alpha, nDotH );
			}
			sum += rowSum * Vv * muL;
		}
		return sum * dmu * dphi;
	}

	//////////////////////////////////////////////////////////////////
	//  EMean(alpha): trapezoidal rule over the BAKED E row (uniform
	//  cosTheta grid, endpoints included -- see file header).
	//////////////////////////////////////////////////////////////////

	//! `EHatMean(alpha) = 2 * integral_0^1 min(E(alpha, mu), 1) * mu dmu`
	//!
	//! TWO THINGS CHANGED HERE IN THE M1 FIX ROUND, both load-bearing.
	//!
	//! (1) THE INTEGRAND IS CLAMPED.  What `fabric_material` actually
	//!     multiplies the substrate by is `Ehat = min(E, 1)`, never the
	//!     raw E -- the Charlie+Lambda fit exceeds 1 in a narrow
	//!     near-grazing sliver and an unclamped `1 - m*E` would go
	//!     negative there.  So the mean this table holds is the mean of
	//!     the quantity the code uses; baking the mean of the RAW E
	//!     would make `hemisphericalAlbedo`'s closed form disagree with
	//!     `value()`'s own denominator.  Hence the name: EHatMean, not
	//!     EMean.
	//!
	//! (2) THE TRAPEZOID IS IN mu, NOT IN THE WARP PARAMETER.  The nodes
	//!     are no longer uniformly spaced (CosThetaAt is quadratic), so
	//!     the weights must come from the ACTUAL node spacing --
	//!     `(mu_{j+1} - mu_{j-1})/2` for an interior node.  Using the
	//!     old uniform `h` would silently integrate against the wrong
	//!     measure and put ~30 % of the mass in the wrong place.
	double ComputeEHatMeanFromRow( const std::vector<double>& eRow )
	{
		const unsigned int N = kNumCosThetaBins;
		const double mu1 = CosThetaAt( 1 );
		const double eHat1 = ( eRow[1] > 1.0 ) ? 1.0 : eRow[1];

		// [0, mu1] ANALYTICALLY, not by trapezoid through the zero node.
		//
		// `SheenDirectionalAlbedo::E` FLOORS its argument at node 1 and
		// constant-extrapolates below (see that header's "THE DOMAIN IS
		// [mu1, 1]" note), so the function the RUNTIME exposes is
		// Ehat(mu1) on the whole first cell -- not a ramp from the
		// analytically-exact 0 at node 0.  This table must be the mean
		// of the function the runtime actually returns, or
		// `hemisphericalAlbedo`'s closed form stops matching `value()`'s
		// own denominator.
		//
		//   2 * INT_0^mu1 Ehat(mu1) * mu dmu  =  Ehat(mu1) * mu1^2
		//
		// Worth ~1.1e-6 against an EhatMean of 0.07-0.30, i.e. far below
		// the bake's own convergence -- but it is the CORRECT quantity,
		// and getting it from the same first principle as the runtime is
		// cheaper than explaining a discrepancy later.
		double integral = 0.5 * eHat1 * mu1 * mu1;

		// [mu1, 1] by trapezoid over the WARPED nodes.  Weights come from
		// the ACTUAL node spacing -- the axis is quadratic, so a uniform
		// `h` would integrate against the wrong measure, silently.
		for( unsigned int j = 1; j < N; j++ )
		{
			const double muPrev = CosThetaAt( j - 1 );
			const double muNext = ( j == N - 1 ) ? CosThetaAt( N - 1 ) : CosThetaAt( j + 1 );
			const double w  = ( j == 1 )
				? 0.5 * ( muNext - CosThetaAt( 1 ) )		// half-cell: [0,mu1] is handled above
				: 0.5 * ( muNext - muPrev );
			const double mu = CosThetaAt( j );
			const double eHat = ( eRow[j] > 1.0 ) ? 1.0 : eRow[j];
			integral += w * eHat * mu;
		}
		return 2.0 * integral;
	}


	//////////////////////////////////////////////////////////////////
	//  Per-cell ADAPTIVE bake of the E table.  Starts at
	//  (kEMuStart, kEPhiStart), doubles both axes until the value
	//  changes by less than kETol between successive doublings or the
	//  kEMuCap ceiling is hit, and reports the worst per-cell delta
	//  actually observed (== this bake's convergence evidence) plus how
	//  many cells needed the ceiling.
	//////////////////////////////////////////////////////////////////

	struct EAdaptiveStats
	{
		double worstDelta = 0.0;
		unsigned int worstAlphaIdx = 0, worstCosThetaIdx = 0;
		unsigned int cellsAtCap = 0;
		int maxMuUsed = 0;
	};

	//! `eTablePrev` receives, per cell, the value one doubling BEFORE the
	//! one actually baked into `eTable` -- i.e. exactly the two most
	//! recent iterates of the same doubling sequence `stats.worstDelta`
	//! is already the max delta of.  Used only so EMean's own error can
	//! be bounded the same way (see main()), without re-deriving a
	//! second, independently-resolutioned E table.
	void BakeEAdaptive( std::vector<std::vector<double>>& eTable,
	                     std::vector<std::vector<double>>& eTablePrev,
	                     EAdaptiveStats& stats )
	{
		eTable.assign( kNumAlphaBins, std::vector<double>( kNumCosThetaBins, 0.0 ) );
		eTablePrev.assign( kNumAlphaBins, std::vector<double>( kNumCosThetaBins, 0.0 ) );
		stats = EAdaptiveStats();

		for( unsigned int ai = 0; ai < kNumAlphaBins; ai++ )
		{
			const double alpha = AlphaAt( ai );
			for( unsigned int ci = 0; ci < kNumCosThetaBins; ci++ )
			{
				const double muV = CosThetaAt( ci );

				int mu = kEMuStart, phi = kEPhiStart;
				PhiGrid grid;
				grid.Build( phi );
				double prev = ComputeE( alpha, muV, mu, grid );
				double beforePrev = prev;
				double delta = 0.0;

				while( mu < kEMuCap )
				{
					const int mu2 = mu * 2, phi2 = phi * 2;
					PhiGrid grid2;
					grid2.Build( phi2 );
					const double cur = ComputeE( alpha, muV, mu2, grid2 );
					delta = std::fabs( cur - prev );
					beforePrev = prev;
					mu = mu2; phi = phi2;
					grid = std::move( grid2 );
					prev = cur;
					if( delta < kETol ) { break; }
				}

				eTable[ai][ci] = prev;
				eTablePrev[ai][ci] = beforePrev;
				if( mu > stats.maxMuUsed ) { stats.maxMuUsed = mu; }
				if( mu >= kEMuCap && delta >= kETol ) { stats.cellsAtCap++; }
				if( delta > stats.worstDelta ) {
					stats.worstDelta = delta;
					stats.worstAlphaIdx = ai;
					stats.worstCosThetaIdx = ci;
				}
			}
		}
	}

}

//! Runtime-matching bilinear read of a baked E table, used ONLY to
//! derive banner numbers (the log-alpha bracket, its fractional
//! position, and the interpolated corner value) so a re-bake at a
//! different table size or reachable-alpha threshold cannot leave a
//! stale literal in the emitted file -- see round 9's follow-up fix
//! (debt 18 residual) for what this replaced.  Mirrors
//! SheenDirectionalAlbedo.cpp's AlphaPos / CosThetaPos / BuildStencil /
//! E exactly (log-alpha stencil, sqrt-inverted grazing warp, bilinear
//! blend); kept as a separate copy here rather than #including that
//! .cpp because the generator only ever needs the DOUBLE-precision
//! table it just baked, not the shipped float table.
double AlphaPosOf( double alpha )
{
	const double t = std::log( alpha / kAlphaMin ) / std::log( kAlphaMax / kAlphaMin );
	const double tc = std::min( std::max( t, 0.0 ), 1.0 );
	return tc * (double)( kNumAlphaBins - 1 );
}

double CosThetaPosOf( double mu )
{
	const double t = (double)( kNumCosThetaBins - 1 );
	const double mu1 = 1.0 / ( t * t );
	const double muFloored = std::max( mu, mu1 );
	return std::sqrt( std::min( std::max( muFloored, 0.0 ), 1.0 ) ) * t;
}

struct StencilOf
{
	unsigned int i0, i1;
	double frac;
};

StencilOf BuildStencilOf( double pos, unsigned int n )
{
	StencilOf s;
	double p = std::min( std::max( pos, 0.0 ), (double)( n - 1 ) );
	s.i0 = (unsigned int)p;
	if( s.i0 >= n - 1 ) { s.i0 = n - 2; }
	s.i1 = s.i0 + 1;
	s.frac = std::min( std::max( p - (double)s.i0, 0.0 ), 1.0 );
	return s;
}

double EInterpOf( const std::vector<std::vector<double>>& table, double alpha, double mu )
{
	const StencilOf sa = BuildStencilOf( AlphaPosOf( alpha ), kNumAlphaBins );
	const StencilOf sc = BuildStencilOf( CosThetaPosOf( mu ), kNumCosThetaBins );

	const double v00 = table[sa.i0][sc.i0];
	const double v01 = table[sa.i0][sc.i1];
	const double v10 = table[sa.i1][sc.i0];
	const double v11 = table[sa.i1][sc.i1];

	const double row0 = ( 1.0 - sc.frac ) * v00 + sc.frac * v01;
	const double row1 = ( 1.0 - sc.frac ) * v10 + sc.frac * v11;
	return ( 1.0 - sa.frac ) * row0 + sa.frac * row1;
}

//! Scan results, hoisted so the emitted banner can quote them.
static double gSmallestSafeAlpha  = 0.0;
static double gReachableMin       = 0.0;
static double gReachableMax       = 0.0;
static double gReachableWorstDelta = 0.0;
static unsigned int gAlphaBracketLo = 0;
static unsigned int gAlphaBracketHi = 0;
static double gAlphaBracketPos    = 0.0;
static double gEAtReachableCorner = 0.0;

int main( int argc, char** argv )
{
	std::string outPath = "src/Library/Materials/SheenDirectionalAlbedo_LUTData.cpp";
	for( int i = 1; i < argc; i++ ) {
		if( strcmp( argv[i], "--output" ) == 0 && i + 1 < argc ) {
			outPath = argv[++i];
		} else {
			fprintf( stderr, "usage: %s [--output <path>]\n", argv[0] );
			return 1;
		}
	}

	fprintf( stderr, "SheenDirectionalAlbedoGen: baking E (%u alpha x %u cosTheta), "
	                  "EMean (%u)\n",
	         kNumAlphaBins, kNumCosThetaBins, kNumAlphaBins );

	// ---- E: per-cell adaptive bake ------------------------------------
	std::vector<std::vector<double>> eTable, eTablePrev;
	EAdaptiveStats eStats;
	BakeEAdaptive( eTable, eTablePrev, eStats );

	fprintf( stderr, "SheenDirectionalAlbedoGen: E adaptive bake done -- "
	                  "worst per-cell doubling delta %.3e at alpha[%u]=%.6g, cosTheta[%u]=%.4g "
	                  "(mu resolution used there: up to %d) ; %u/%u cells hit the %d cap "
	                  "without reaching tol %.1e\n",
	         eStats.worstDelta, eStats.worstAlphaIdx, AlphaAt( eStats.worstAlphaIdx ),
	         eStats.worstCosThetaIdx, CosThetaAt( eStats.worstCosThetaIdx ), eStats.maxMuUsed,
	         eStats.cellsAtCap, kNumAlphaBins * kNumCosThetaBins, kEMuCap, kETol );

	std::vector<double> eHatMean( kNumAlphaBins );
	double eMeanDelta = 0.0;
	for( unsigned int ai = 0; ai < kNumAlphaBins; ai++ ) {
		eHatMean[ai] = ComputeEHatMeanFromRow( eTable[ai] );
		const double eMeanPrev = ComputeEHatMeanFromRow( eTablePrev[ai] );
		eMeanDelta = std::max( eMeanDelta, std::fabs( eHatMean[ai] - eMeanPrev ) );
	}
	fprintf( stderr, "SheenDirectionalAlbedoGen: max|EHatMean - EHatMean_at_prior_doubling| = %.3e\n",
	         eMeanDelta );

	double eMin = 1e300, eMax = -1e300;
	for( unsigned int ai = 0; ai < kNumAlphaBins; ai++ )
		for( unsigned int ci = 0; ci < kNumCosThetaBins; ci++ ) {
			eMin = std::min( eMin, eTable[ai][ci] );
			eMax = std::max( eMax, eTable[ai][ci] );
		}
	fprintf( stderr, "SheenDirectionalAlbedoGen: E range over the table: [%.6f, %.6f]\n", eMin, eMax );

	// ---- the kMinSheenAlpha scan -------------------------------------
	// FabricBRDF::kMinSheenAlpha's criterion, stated and evaluated here
	// rather than eyeballed off the emitted table: the smallest baked
	// alpha for which  max_{mu >= kFloorScanMu} E(alpha, mu) <= 1.
	// Below that mu the lobe is allowed to exceed 1 and the symmetric
	// normaliser in FabricBRDF handles it (bounded, not conserving);
	// at and above it the product form must be exactly conserving, which
	// needs E <= 1.
	{
		const double kFloorScanMu = 0.03;
		double smallestSafe = -1.0;
		fprintf( stderr, "SheenDirectionalAlbedoGen: kMinSheenAlpha scan "
		                  "(criterion: max_{mu >= %.2f} E(alpha, mu) <= 1)\n", kFloorScanMu );
		for( unsigned int ai = 0; ai < kNumAlphaBins; ai++ ) {
			double worst = 0.0;
			for( unsigned int ci = 0; ci < kNumCosThetaBins; ci++ ) {
				if( CosThetaAt( ci ) < kFloorScanMu ) continue;
				worst = std::max( worst, eTable[ai][ci] );
			}
			const bool safe = ( worst <= 1.0 );
			if( safe && smallestSafe < 0.0 ) smallestSafe = AlphaAt( ai );
			fprintf( stderr, "    alpha[%2u] = %.6f   max_{mu>=%.2f} E = %.6f   %s\n",
			         ai, AlphaAt( ai ), kFloorScanMu, worst, safe ? "SAFE" : "over 1" );
		}
		fprintf( stderr, "SheenDirectionalAlbedoGen: smallest baked alpha meeting the "
		                  "criterion = %.6f (FabricBRDF::kMinSheenAlpha must be >= this, and "
		                  "above the next lower node so bilinear interpolation cannot "
		                  "reintroduce a cell over 1)\n", smallestSafe );

		// The whole-table convergence and range numbers above are
		// dominated by the extreme low-alpha corner, which
		// FabricBRDF::kMinSheenAlpha makes UNREACHABLE.  Report the
		// restricted figures too, so the banner can state what the
		// shipping material actually evaluates against rather than the
		// worst cell in a region no scene can reach.
		gSmallestSafeAlpha = smallestSafe;
		const double kReachableAlpha = 0.04;
		double rMin = 1e300, rMax = -1e300, rWorstDelta = 0.0;
		for( unsigned int ai = 0; ai < kNumAlphaBins; ai++ ) {
			if( AlphaAt( ai ) < kReachableAlpha ) continue;
			for( unsigned int ci = 0; ci < kNumCosThetaBins; ci++ ) {
				rMin = std::min( rMin, eTable[ai][ci] );
				rMax = std::max( rMax, eTable[ai][ci] );
				rWorstDelta = std::max( rWorstDelta,
					std::fabs( eTable[ai][ci] - eTablePrev[ai][ci] ) );
			}
		}
		gReachableMin = rMin; gReachableMax = rMax; gReachableWorstDelta = rWorstDelta;
		fprintf( stderr, "SheenDirectionalAlbedoGen: REACHABLE rows only (alpha >= %.2f): "
		                  "E range [%.6f, %.6f], worst per-cell doubling delta %.3e\n",
		         kReachableAlpha, rMin, rMax, rWorstDelta );

		// The banner below discusses the specific runtime-interpolated
		// corner (alpha = kReachableAlpha, mu = kFloorScanMu) -- DERIVED
		// here with the same log-alpha stencil / sqrt-warp / bilinear
		// blend SheenDirectionalAlbedo.cpp's E() uses (see EInterpOf
		// above), so a re-bake at a different table size or threshold
		// cannot leave the bracketing rows, the fractional position, or
		// the interpolated value stale in the emitted file the way the
		// hard-coded "rows 16 AND 17 / position 16.556 / 0.961508" was
		// left behind by the 32 -> 64 node round-9 rebake.
		gAlphaBracketPos = AlphaPosOf( kReachableAlpha );
		const StencilOf alphaBracket = BuildStencilOf( gAlphaBracketPos, kNumAlphaBins );
		gAlphaBracketLo = alphaBracket.i0;
		gAlphaBracketHi = alphaBracket.i1;
		gEAtReachableCorner = EInterpOf( eTable, kReachableAlpha, kFloorScanMu );
		fprintf( stderr, "SheenDirectionalAlbedoGen: interpolated E(%.2f, %.2f) = %.6f "
		                  "(log-alpha position %.3f, bracketing rows %u/%u)\n",
		         kReachableAlpha, kFloorScanMu, gEAtReachableCorner,
		         gAlphaBracketPos, gAlphaBracketLo, gAlphaBracketHi );
	}


	const double eDelta = eStats.worstDelta;

	// mu1 and the below-0.03 node count are DERIVED here (not hard-coded
	// into the banner text below) so a future re-bake at a different
	// kNumCosThetaBins cannot leave a stale number in the emitted file --
	// exactly the kind of drift round 9 (2026-09-04, debt 18) found and
	// fixed when the axis moved from 32 to 64 nodes.
	const double mu1 = 1.0 / (double)( (kNumCosThetaBins - 1) * (kNumCosThetaBins - 1) );
	unsigned int belowGrazeCount = 0;
	for( unsigned int ci = 0; ci < kNumCosThetaBins; ci++ ) {
		if( CosThetaAt( ci ) < 0.03 ) belowGrazeCount++;
	}

	// ---- emit --------------------------------------------------------
	FILE* f = fopen( outPath.c_str(), "wb" );
	if( !f ) {
		fprintf( stderr, "SheenDirectionalAlbedoGen: cannot open `%s` for writing\n", outPath.c_str() );
		return 1;
	}

	fprintf( f,
		"//////////////////////////////////////////////////////////////////////\n"
		"//\n"
		"//  SheenDirectionalAlbedo_LUTData.cpp - Baked directional- and\n"
		"//    bihemispherical-albedo tables for the Charlie sheen lobe.\n"
		"//\n"
		"//  AUTO-GENERATED by tools/SheenDirectionalAlbedoGen.cpp.  DO NOT EDIT\n"
		"//  BY HAND.  Regenerate from the PROJECT ROOT with:\n"
		"//\n"
		"//      c++ -O3 -std=c++17 -I src/Library -o /tmp/SheenDirectionalAlbedoGen \\\n"
		"//          tools/SheenDirectionalAlbedoGen.cpp\n"
		"//      /tmp/SheenDirectionalAlbedoGen\n"
		"//\n"
		"//  Layout, extents, the exact integrals baked and the quadrature are\n"
		"//  documented in SheenDirectionalAlbedo.h and in the generator's file\n"
		"//  header.\n"
		"//\n"
		"//  cosTheta axis: GRAZING-WARPED, mu_j = (j/(N-1))^2, so node 1 sits\n"
		"//  at %.6g and %u of the %u nodes fall below mu = 0.03.  A\n"
		"//  UNIFORM axis (which this table used until 2026-09-02) has its\n"
		"//  first interior node at mu = 0.0323 with an exact 0 at mu = 0, so\n"
		"//  the runtime interpolant ramped LINEARLY FROM ZERO across the\n"
		"//  entire band the Charlie lobe actually lives in -- it is near its\n"
		"//  peak by mu ~ 0.005.  Measured: the old table read 0.146 where the\n"
		"//  truth is 1.19 (alpha 0.04, mu 0.005), and since fabric_material\n"
		"//  EMITS the true lobe while suppressing the base by the TABLED E,\n"
		"//  the white-furnace identity broke by up to +1.05 absolute under\n"
		"//  grazing illumination.  The runtime inverts the warp with sqrt().\n"
		"//\n"
		"//  Quadrature: E via 2-D midpoint rule over (muL, phiL), PER-CELL\n"
		"//  ADAPTIVE -- starts at %d (mu) x %d (phi) nodes, doubles both axes\n"
		"//  until successive doublings agree to %.1e or the %d-mu ceiling is\n"
		"//  hit (a narrow band at low alpha and near-grazing cosThetaV needs\n"
		"//  far more nodes than the rest of the table; see\n"
		"//  SheenDirectionalAlbedoGen.cpp's file header).  No Monte Carlo\n"
		"//  anywhere; deterministic; byte-identical across re-runs.\n"
		"//\n"
		"//  Convergence evidence (see the generator's stderr log for the full\n"
		"//  per-cell detail and the kMinSheenAlpha scan):\n"
		"//    worst per-cell |E - E_at_prior_doubling|, WHOLE table  = %.3e\n"
		"//    the same, restricted to REACHABLE rows (alpha >= 0.04) = %.3e\n"
		"//    worst |EHatMean - EHatMean_at_prior_doubling|          = %.3e\n"
		"//  E range, whole table:      [%.6f, %.6f]\n"
		"//  E range, REACHABLE ROWS:   [%.6f, %.6f]  -- rows whose alpha is\n"
		"//    >= 0.04.  NOT the runtime-reachable max, which is ~1.196: at\n"
		"//    alpha = 0.04 the bilinear reads rows %u AND %u (log-alpha\n"
		"//    position %.3f), so E() interpolates ABOVE the larger of the\n"
		"//    two shipped rows.  The floor's criterion is unaffected --\n"
		"//    max E() over alpha >= 0.04 AND mu >= 0.03 is %.6f.\n"
		"//\n"
		"//  E EXCEEDS 1 AND THAT IS THE LOBE, NOT THE BAKE.  Estevez &\n"
		"//  Kulla's Charlie+Lambda fit is production-friendly, not tightly\n"
		"//  energy-conserving.  With the grazing band now RESOLVED, the true\n"
		"//  peak is far higher than the old uniform axis could see, and it\n"
		"//  survives above the roughness floor: even at alpha >= 0.04 the\n"
		"//  table reaches %.6f in its shipped cells (and E() ~1.196 once\n"
		"//  interpolated), inside the near-grazing sliver mu < 0.03.\n"
		"//\n"
		"//  WHAT PROTECTS THE CALLER (docs/CLOTH_FABRIC_DESIGN.md 9.2).  Two\n"
		"//  things, and the roughness floor is only one of them:\n"
		"//    * FabricBRDF::kMinSheenAlpha == 0.04 keeps E <= 1 for\n"
		"//      mu >= 0.03.  The scan's criterion is exactly that, and the\n"
		"//      smallest baked alpha meeting it is %.6f -- so 0.04 clears it\n"
		"//      with both bilinear-bracketing rows already under 1.\n"
		"//    * Inside the sliver mu < 0.03, where E still exceeds 1, a\n"
		"//      SYMMETRIC normaliser max(1, E(v), E(l)) divides the sheen\n"
		"//      lobe and Ehat = min(E, 1) scales the base.  That keeps\n"
		"//      reciprocity and keeps rho <= 1, at the cost of exact\n"
		"//      conservation there: fabric is CONSERVING for mu >= 0.03 and\n"
		"//      BOUNDED below it.\n"
		"//\n"
		"//  kEHatMeanTable holds the mean of Ehat = min(E, 1), NOT of the raw\n"
		"//  E -- it must be the mean of the quantity the code multiplies by,\n"
		"//  or hemisphericalAlbedo's closed form would disagree with\n"
		"//  value()'s own denominator.  A THIRD table (S(alpha, m), the\n"
		"//  bihemispherical average of the old `min`-form kernel) used to be\n"
		"//  baked here; round 5's product form factors, so it collapsed to a\n"
		"//  closed form in EHatMean and was retired on 2026-09-02.\n"
		"//\n"
		"//////////////////////////////////////////////////////////////////////\n"
		"\n"
		"#include \"pch.h\"\n"
		"#include \"SheenDirectionalAlbedo.h\"\n"
		"\n"
		"namespace RISE\n"
		"{\n"
		"\tnamespace SheenDirectionalAlbedo\n"
		"\t{\n"
		"\t\textern const float kAlphaMin = %s;\n"
		"\t\textern const float kAlphaMax = %s;\n"
		"\n",
		mu1, belowGrazeCount, kNumCosThetaBins,
		kEMuStart, kEPhiStart, kETol, kEMuCap,
		eDelta, gReachableWorstDelta, eMeanDelta, eMin, eMax,
		gReachableMin, gReachableMax,
		gAlphaBracketLo, gAlphaBracketHi, gAlphaBracketPos, gEAtReachableCorner,
		gReachableMax, gSmallestSafeAlpha,
		FloatLit( kAlphaMin ).c_str(), FloatLit( kAlphaMax ).c_str() );

	fprintf( f, "\t\textern const float kETable[ kNumAlphaBins ][ kNumCosThetaBins ] = {\n" );
	for( unsigned int ai = 0; ai < kNumAlphaBins; ai++ ) {
		fprintf( f, "\t\t\t{ " );
		for( unsigned int ci = 0; ci < kNumCosThetaBins; ci++ ) {
			fprintf( f, "%s%s", FloatLit( eTable[ai][ci] ).c_str(),
			         ( ci + 1 < kNumCosThetaBins ) ? ", " : "" );
		}
		fprintf( f, " }%s\n", ( ai + 1 < kNumAlphaBins ) ? "," : "" );
	}
	fprintf( f, "\t\t};\n\n" );

	fprintf( f, "\t\textern const float kEHatMeanTable[ kNumAlphaBins ] = {\n\t\t\t" );
	for( unsigned int ai = 0; ai < kNumAlphaBins; ai++ ) {
		fprintf( f, "%s%s", FloatLit( eHatMean[ai] ).c_str(), ( ai + 1 < kNumAlphaBins ) ? ", " : "" );
		if( ( ai + 1 ) % 8 == 0 && ai + 1 < kNumAlphaBins ) { fprintf( f, "\n\t\t\t" ); }
	}
	fprintf( f, "\n\t\t};\n\n" );


	fprintf( f, "\t}\n}\n" );
	fclose( f );

	fprintf( stderr, "SheenDirectionalAlbedoGen: wrote %s\n", outPath.c_str() );
	return 0;
}
