//////////////////////////////////////////////////////////////////////
//
//  SheenDirectionalAlbedoTest.cpp - Standalone regression test for
//    the baked Charlie sheen directional-albedo tables (src/Library/Materials/SheenDirectionalAlbedo.{h,cpp},
//    data in SheenDirectionalAlbedo_LUTData.cpp; baked by
//    tools/SheenDirectionalAlbedoGen.cpp).  See
//    docs/CLOTH_FABRIC_DESIGN.md section 9.4 "The E table" and section
//    9.2's `hemisphericalAlbedo` discussion for what these tables are
//    for.
//
//  Coverage:
//    (a) baked float counts match the declared extents (compile-time
//        static_asserts already guard this in the header -- this test
//        additionally exercises the extern arrays at runtime, on
//        HairMedullaProfileGen's precedent).
//    (b) E is non-negative everywhere and stays under a measured
//        ceiling (Estevez & Kulla's Charlie+Lambda fit is NOT tightly
//        energy-conserving at extreme low alpha / near-grazing view --
//        see SheenDirectionalAlbedo.h -- so a hard E<=1 assertion would
//        be WRONG, not conservative); E() is continuous across bin
//        boundaries, to a SLOPE-RELATIVE tolerance (an
//        interpolation-correctness check, not a physical one -- and the
//        tolerance must scale with the local slope now that the
//        cosTheta axis is warped, since the first node's legitimate
//        one-sided difference exceeds the old fixed bound).
//    (c) four (alpha, cosTheta) NODE spot checks against an INDEPENDENT
//        brute-force integration of CharlieSheen.h done here, at lower
//        resolution than the generator, including the generator's own
//        worst-convergence cell -- PLUS five INTERPOLATED probes inside
//        the grazing band (mu in {0.002, 0.005, 0.010, 0.016, 0.030}),
//        which is the regime the cosTheta axis was warped for and where
//        a regression to a uniform axis would be wrong by up to 8x.
//    (d) RETIRED 2026-09-02 along with the S table itself.  Round 5 of
//        docs/CLOTH_FABRIC_DESIGN.md replaced the `min`-form sheen/base
//        kernel -- whose bihemispherical average could only be
//        tabulated, because a `min` does not factor -- with the
//        Kulla-Conty PRODUCT form, which factors into a closed form in
//        EMean alone.  `fabric_material`'s hemisphericalAlbedo now
//        reads `substrate.hemisphericalAlbedo() * (1 - m*EMean) +
//        sheenColor * EMean` with no kernel table between it and the
//        answer, so there is nothing left here to check.  The property
//        that replaced these assertions is an ENERGY one and lives in
//        tests/LayeredWhiteFurnaceTest.cpp (the fabric rows over a
//        white Lambertian must land on rho == 1) plus gate 5b in
//        tests/FabricMaterialChunkTest.cpp.
//    (e) EHatMean(alpha) at each baked alpha node reproduces
//        2*integral_0^1 min(E(alpha,mu),1)*mu dmu computed via the
//        trapezoidal rule directly over the STORED 32-point E row --
//        with the CLAMP and with weights from the WARPED node spacing
//        (the same reduction tools/SheenDirectionalAlbedoGen.cpp's
//        ComputeEHatMeanFromRow performs), bit-for-bit modulo the
//        double<->float round-trip.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#ifdef NDEBUG
#undef NDEBUG	// This standalone test relies on assert in Release builds too.
#endif
#include <cassert>
#include <cmath>
#include <algorithm>

#include "../src/Library/Materials/SheenDirectionalAlbedo.h"
#include "../src/Library/Materials/CharlieSheen.h"

using namespace RISE;
namespace CS = RISE::Implementation::CharlieSheen;
namespace SDA = RISE::SheenDirectionalAlbedo;

namespace
{
	//! Independent brute-force directional albedo integral, at a
	//! resolution LOWER than (and structured slightly differently from
	//! -- see below) the generator's own, so this cannot simply
	//! reproduce the same rounding the generator does.  2-D midpoint
	//! rule over (muL, phiL), exactly as CharlieSheen's own D/V are
	//! defined -- no table lookups anywhere in this function.
	double BruteForceE( double alpha, double muV, int nMu, int nPhi )
	{
		const double sinV = std::sqrt( std::max( 0.0, 1.0 - muV * muV ) );
		const double dmu = 1.0 / (double)nMu;
		const double dphi = 2.0 * 3.14159265358979323846 / (double)nPhi;
		double sum = 0.0;

		for( int j = 0; j < nMu; j++ )
		{
			const double muL = ( j + 0.5 ) * dmu;
			const double Vv = CS::V( alpha, muL, muV );
			if( Vv <= 0.0 ) { continue; }
			const double sinL = std::sqrt( std::max( 0.0, 1.0 - muL * muL ) );

			double rowSum = 0.0;
			for( int k = 0; k < nPhi; k++ )
			{
				const double phi = ( k + 0.5 ) * dphi;
				const double hx = sinL * std::cos( phi ) + sinV;
				const double hy = sinL * std::sin( phi );
				const double hz = muL + muV;
				const double hlen = std::sqrt( hx * hx + hy * hy + hz * hz );
				const double nDotH = ( hlen > 1e-12 ) ? ( hz / hlen ) : 1.0;
				rowSum += CS::D( alpha, nDotH );
			}
			sum += rowSum * Vv * muL;
		}
		return sum * dmu * dphi;
	}

	double AlphaAt( unsigned int i )
	{
		const double t = (double)i / (double)( SDA::kNumAlphaBins - 1 );
		return (double)SDA::kAlphaMin * std::pow( (double)SDA::kAlphaMax / (double)SDA::kAlphaMin, t );
	}

	//! The GRAZING-WARPED cosTheta node, mirroring
	//! SheenDirectionalAlbedoGen's CosThetaAt (and inverted at runtime
	//! by SheenDirectionalAlbedo.cpp's CosThetaPos).  Node 1 sits at
	//! 1/961 = 0.00104, not a uniform axis's 0.0323.
	double CosThetaAt( unsigned int i )
	{
		const double t = (double)i / (double)( SDA::kNumCosThetaBins - 1 );
		return t * t;
	}
}

//////////////////////////////////////////////////////////////////////
//  (a) Baked float counts vs. declared extents.
//////////////////////////////////////////////////////////////////////
void TestExtents()
{
	std::cout << "Testing baked extents..." << std::endl;

	assert( sizeof( SDA::kETable ) / sizeof( float ) ==
	        (size_t)SDA::kNumAlphaBins * SDA::kNumCosThetaBins );
	assert( sizeof( SDA::kEHatMeanTable ) / sizeof( float ) == (size_t)SDA::kNumAlphaBins );

	assert( SDA::kAlphaMin > 0.0f && SDA::kAlphaMin < SDA::kAlphaMax );
	assert( SDA::kAlphaMax <= 1.0f );

	std::cout << "TestExtents Passed!" << std::endl;
}

//////////////////////////////////////////////////////////////////////
//  (b) E >= 0 everywhere; bounded by a measured ceiling (NOT 1 -- see
//      file header); E() continuous across bin boundaries.
//////////////////////////////////////////////////////////////////////
void TestERangeAndContinuity()
{
	std::cout << "Testing E range and continuity..." << std::endl;

	// Measured table max is ~6.09 (see SheenDirectionalAlbedo_LUTData.cpp's
	// banner) -- Charlie+Lambda is not tightly energy-conserving near
	// grazing, and since the cosTheta axis was WARPED toward grazing
	// (2026-09-02) the bake finally RESOLVES that peak instead of
	// interpolating across it.  The old uniform axis topped out at 1.665
	// purely because it could not see the band.  10.0 is a loose sanity
	// ceiling: it catches a gross regression (a unit error blowing the
	// table up by an order of magnitude) without asserting a physically
	// wrong bound.
	const double kSanityCeiling = 10.0;

	double worstMax = 0.0;
	for( unsigned int ai = 0; ai < SDA::kNumAlphaBins; ai++ ) {
		for( unsigned int ci = 0; ci < SDA::kNumCosThetaBins; ci++ ) {
			const double e = SDA::kETable[ai][ci];
			assert( e >= 0.0 );
			assert( e < kSanityCeiling );
			worstMax = std::max( worstMax, e );
		}
	}
	assert( worstMax > 1.0 );	// the exceedance is real and expected -- see file header;
	                          	// a regenerated table that stopped exceeding 1 anywhere
	                          	// would mean the bake had silently changed behaviour.

	// Continuity: evaluate E() just below and just above each INTERIOR
	// grid cosTheta node (holding alpha fixed at a representative
	// low-alpha value, where the table has the most curvature) and
	// confirm the two agree to a tight tolerance.  Bilinear
	// interpolation is continuous by construction, so this is a check
	// on SheenDirectionalAlbedo.cpp's stencil arithmetic (no off-by-one
	// at a cell boundary), not a physical claim.
	// THE TOLERANCE IS SLOPE-RELATIVE, NOT ABSOLUTE, AND IT HAS TO BE.
	//
	// A fixed `1e-3` worked while the cosTheta axis was uniform.  With
	// the axis warped toward grazing the first interior node sits at
	// mu = 0.00104 where the lobe's local slope is ~4766 per unit mu, so
	// a legitimate +/- 1e-6 probe moves E by 4.8e-3 -- three times the
	// old bound -- while the interpolant is perfectly continuous.  That
	// steepness is the whole POINT of the warp: it is the curvature the
	// uniform axis could not represent.
	//
	// Scaling by the local slope keeps every bit of the discriminating
	// power.  This check exists to catch a STENCIL off-by-one, which
	// would read from the wrong cell either side of a node and so
	// produce a jump of order (slope x node gap) -- about 5.0 at node 1
	// -- against a legitimate (slope x eps) of 4.8e-3.  Three orders of
	// magnitude of separation, so an 8x safety factor costs nothing.
	// The absolute floor covers the high-mu rows where the slope decays
	// to zero and both differences are denormal.
	const double alpha = AlphaAt( 2 );
	const double eps = 1e-6;
	for( unsigned int ci = 1; ci < SDA::kNumCosThetaBins - 1; ci++ ) {
		const double c = CosThetaAt( ci );
		const double below = SDA::E( alpha, c - eps );
		const double at    = SDA::E( alpha, c );
		const double above = SDA::E( alpha, c + eps );

		// Local slope from the neighbouring NODES, so the bound is set
		// by the function's own steepness rather than by a constant.
		const double cLo = CosThetaAt( ci - 1 );
		const double cHi = CosThetaAt( ci + 1 );
		const double slope = std::max(
			std::fabs( at - SDA::E( alpha, cLo ) ) / std::max( 1e-12, c - cLo ),
			std::fabs( SDA::E( alpha, cHi ) - at ) / std::max( 1e-12, cHi - c ) );
		const double tol = std::max( 1e-9, 8.0 * eps * slope );

		assert( std::fabs( below - at ) < tol );
		assert( std::fabs( above - at ) < tol );
	}

	std::cout << "TestERangeAndContinuity Passed! (max E over table = " << worstMax << ")" << std::endl;
}

//////////////////////////////////////////////////////////////////////
//  (c) Spot-check against an independent brute-force integral.
//////////////////////////////////////////////////////////////////////
void TestESpotChecks()
{
	std::cout << "Testing E spot checks against independent brute force..." << std::endl;

	struct Spot { unsigned int ai, ci; };
	const Spot spots[] = {
		{ 1,  5  },		// the generator's hardest cell
		{ 10, 16 },		// moderate alpha, mid cosTheta
		{ 31, 31 },		// alpha == 1 (roughest), normal incidence
		{ 4,  10 },		// low alpha, moderate-grazing cosTheta
	};

	const double kTol = 2e-3;
	for( const Spot& s : spots )
	{
		const double alpha = AlphaAt( s.ai );
		const double muV   = CosThetaAt( s.ci );
		const double reference = BruteForceE( alpha, muV, 500, 1000 );
		const double tabled    = (double)SDA::kETable[s.ai][s.ci];
		const double delta = std::fabs( reference - tabled );
		std::cout << "  alpha=" << alpha << " cosTheta=" << muV
		          << " tabled=" << tabled << " bruteForce=" << reference
		          << " delta=" << delta << std::endl;
		assert( delta < kTol );
	}

	// ---- THE GRAZING BAND, which is why the axis is warped at all.
	//
	// A uniform cosTheta axis put its first interior node at mu = 0.0323
	// and an exact 0 at mu = 0, so the runtime lookup ramped LINEARLY
	// FROM ZERO across the whole band the Charlie lobe occupies -- it is
	// already near its peak by mu ~ 0.005.  These checks probe the
	// INTERPOLANT (SDA::E, not the raw cells) at mu values that fall
	// BETWEEN nodes down there, which is exactly where the old axis was
	// wrong by up to 8x and where a regression to a uniform axis would
	// reappear.
	{
		std::cout << "  -- grazing band (interpolated, between nodes):" << std::endl;
		struct GrazeSpot { double alpha, mu, tol; };
		const GrazeSpot graze[] = {
			// alpha, mu, tolerance.  The tolerance widens toward mu = 0
			// because the lobe's curvature does; 0.002 sits between
			// nodes 1 (0.00104) and 2 (0.00416) where the interpolant
			// works hardest.
			{ 0.04, 0.002, 2.0e-2 },
			{ 0.04, 0.010, 5.0e-3 },
			{ 0.04, 0.030, 5.0e-3 },
			{ 0.20, 0.005, 5.0e-3 },
			{ 1.00, 0.016, 5.0e-3 },
		};
		for( const GrazeSpot& g : graze )
		{
			const double reference = BruteForceE( g.alpha, g.mu, 3000, 1500 );
			const double interpolated = SDA::E( g.alpha, g.mu );
			const double delta = std::fabs( reference - interpolated );
			std::cout << "     alpha=" << g.alpha << " mu=" << g.mu
			          << " E()=" << interpolated << " bruteForce=" << reference
			          << " delta=" << delta << std::endl;
			assert( delta < g.tol );
		}
		// And the property the whole warp exists to restore: at mu well
		// inside the old first bin, E must be LARGE (near the lobe's
		// peak), not a fraction of it.  A uniform-axis regression would
		// read ~0.15 here instead of ~1.19.
		assert( SDA::E( 0.04, 0.005 ) > 1.0 );
	}

	// ---- OFF-NODE PROBES ACROSS THE WHOLE AXIS.
	//
	// The continuity check below is blind to a UNIFORM stencil shift
	// (every lookup off by one cell): shifting the whole index just
	// reindexes into a still-continuous piecewise-linear table, so the
	// one-sided differences it measures stay tiny at every node.  It is
	// also near-zero-margin at the flat high-mu end, where both the
	// difference and the slope-derived tolerance collapse toward FP
	// noise (M5 review, 2026-09-03).
	//
	// These probes close that: each sits STRICTLY BETWEEN nodes and is
	// compared against an INDEPENDENT brute-force integration of
	// CharlieSheen.h.  A shifted stencil reads a neighbouring cell whose
	// value differs by far more than the tolerance, at every one of
	// them.  The high-mu probe is the one the continuity loop cannot
	// defend at all.
	{
		std::cout << "  -- off-node probes vs brute force (stencil-shift guard):" << std::endl;
		struct OffNode { double alpha, mu, tol; };
		const OffNode probes[] = {
			{ 0.20, 0.0620, 2.0e-3 },	// between nodes 7 and 8
			{ 0.50, 0.3000, 2.0e-3 },	// mid-axis
			{ 1.00, 0.9000, 2.0e-3 },	// between nodes 29 and 30 -- the flat end
			{ 0.08, 0.6200, 2.0e-3 },	// a second alpha row, upper-mid
		};
		for( const OffNode& o : probes )
		{
			const double reference = BruteForceE( o.alpha, o.mu, 3000, 1500 );
			const double interpolated = SDA::E( o.alpha, o.mu );
			const double delta = std::fabs( reference - interpolated );
			std::cout << "     alpha=" << o.alpha << " mu=" << o.mu
			          << " E()=" << interpolated << " bruteForce=" << reference
			          << " delta=" << delta << std::endl;
			assert( delta < o.tol );
		}
	}

	// ---- THE FLOORED DOMAIN, asserted as the documented behaviour.
	//
	// `E()` floors its argument at node 1 (mu1 = 1/961) and
	// constant-extrapolates below -- the table's stated domain, and the
	// fix for the M4 review's P1 (an un-floored first cell ramped E from
	// zero across the lobe's peak and let a white Lambertian fabric
	// return rho = 1.714).  Below mu1 the correct oracle is NOT the true
	// lobe: the floor deliberately OVER-READS so the base is fully
	// suppressed and the normaliser divides the lobe honestly.
	{
		const double mu1 = 1.0 / ( (double)( SDA::kNumCosThetaBins - 1 )
		                         * (double)( SDA::kNumCosThetaBins - 1 ) );

		// THE DOMINATION PROPERTY IS NOT EXACT, and an earlier revision
		// of this block asserted that it was -- passing only because its
		// probe grid (alpha in {0.04, 0.2, 1.0} x mu <= 5e-4) missed
		// both the alpha and the mu where it fails (M6 review).
		//
		// It fails because `E_tab(mu1)` is a LINEAR INTERPOLATION IN
		// LOG-ALPHA, and E at node 1 is CONCAVE in alpha with a peak
		// near 0.9: in the last, widest log-alpha cell (0.800 -> 1.000)
		// the chord UNDER-reads the true lobe.  Measured worst shortfall
		// over this grid: E_true - E_tab(mu1) = +0.006674 at
		// alpha = 0.90, mu = 0.999*mu1.  The shipped consequence is a
		// bound, not a blow-up -- rho <= 1.0067 there (FabricBRDF.h's
		// exactness class) -- but the PROPERTY is "dominates to within
		// 0.0068", not "dominates".
		//
		// kDominationSlack = 0.008 is that measured 0.006674 plus ~20 %
		// headroom for the resolution of BruteForceE below.  It is NOT a
		// free parameter: tightening it fails, and loosening it past
		// ~0.01 would stop discriminating, since a genuine regression
		// here (a dropped floor, a wrong node) moves E by 0.1 or more.
		static const double kDominationSlack = 0.008;

		std::cout << "  -- floored domain below mu1 = " << mu1
		          << " (domination slack " << kDominationSlack << "):" << std::endl;

		// The alpha set now BRACKETS AND ENTERS the failing region, and
		// the mu set reaches mu1 where the shortfall is largest -- the
		// property degrades toward mu1, so probing only mu <= 5e-4
		// sampled the easy end of the cell.
		double worstShortfall = -1e9;
		double worstAlpha = 0, worstMu = 0;
		for( double alpha : { 0.04, 0.2, 0.6, 0.8, 0.9, 0.95, 1.0 } )
		{
			const double atNode = SDA::E( alpha, mu1 );

			// (a) CONSTANT below the floor -- exactly, not approximately.
			//     This is the check a regression that DROPS the floor
			//     fails first: unfloored, E(alpha, 0) is 0, not atNode.
			for( double mu : { 0.0, 1e-6, 1e-5, 1e-4, 5e-4, mu1 * 0.999 } ) {
				assert( SDA::E( alpha, mu ) == atNode );
			}

			// (b) the floored value dominates the true lobe across the
			//     cell TO WITHIN kDominationSlack, which is what makes
			//     constant extrapolation energy-bounded.
			for( double mu : { 1e-5, 1e-4, 5e-4, mu1 * 0.9, mu1 * 0.999 } ) {
				const double trueE = BruteForceE( alpha, mu, 3000, 1500 );
				const double shortfall = trueE - atNode;
				if( shortfall > worstShortfall ) {
					worstShortfall = shortfall; worstAlpha = alpha; worstMu = mu;
				}
				assert( atNode >= trueE - kDominationSlack );
			}
		}
		std::printf( "     worst shortfall (E_true - E_tab(mu1)) = %+.6f "
		             "at alpha=%.2f mu=%.6g  (slack %.3f)\n",
		             worstShortfall, worstAlpha, worstMu, kDominationSlack );
		// The shortfall must be REAL, not an artefact: if it ever went
		// negative everywhere, the concavity that causes it would have
		// gone away and this slack should be tightened back to zero.
		assert( worstShortfall > 0.0 );
	}

	std::cout << "TestESpotChecks Passed!" << std::endl;
}

//////////////////////////////////////////////////////////////////////
//  (e) EHatMean matches 2*integral min(E,1)*mu dmu reconstructed from
//      the table.
//
//  TWO THINGS THIS CHECK PINS, both of which changed on 2026-09-02:
//
//   * THE CLAMP.  The table holds the mean of Ehat = min(E, 1), not of
//     the raw E, because Ehat is what fabric_material multiplies the
//     substrate by.  Reconstructing WITHOUT the clamp would disagree by
//     the whole grazing sliver's contribution, so this check is what
//     would catch a generator that went back to baking the raw mean.
//   * THE WEIGHTS.  The nodes are no longer uniformly spaced (the axis
//     is quadratic in the warp parameter), so the trapezoid weights
//     must come from the ACTUAL node spacing.  Using a uniform `h`
//     would integrate against the wrong measure -- and would do so
//     SILENTLY, since both sides of the comparison would still be
//     finite and plausible.
//////////////////////////////////////////////////////////////////////
void TestEHatMeanConsistency()
{
	std::cout << "Testing EHatMean consistency with the baked E table..." << std::endl;

	const unsigned int N = SDA::kNumCosThetaBins;

	for( unsigned int ai = 0; ai < SDA::kNumAlphaBins; ai++ ) {
		double integral = 0.0;
		for( unsigned int ci = 0; ci < N; ci++ ) {
			const double muPrev = ( ci == 0 )     ? CosThetaAt( 0 )     : CosThetaAt( ci - 1 );
			const double muNext = ( ci == N - 1 ) ? CosThetaAt( N - 1 ) : CosThetaAt( ci + 1 );
			const double w  = 0.5 * ( muNext - muPrev );
			const double mu = CosThetaAt( ci );
			const double e  = (double)SDA::kETable[ai][ci];
			const double eHat = ( e > 1.0 ) ? 1.0 : e;
			integral += w * eHat * mu;
		}
		const double reconstructed = 2.0 * integral;
		const double stored = (double)SDA::kEHatMeanTable[ai];
		const double delta = std::fabs( reconstructed - stored );
		// Generous float-rounding tolerance: the generator itself
		// computes this reduction in double and stores the result as
		// float, and this test re-derives it from the ALREADY-ROUNDED
		// float E values, so a little more slop than raw float epsilon
		// is expected. 1e-4 is still 100x tighter than the design doc's
		// convergence target.
		assert( delta < 1e-4 );
	}

	std::cout << "TestEHatMeanConsistency Passed!" << std::endl;
}

int main()
{
	TestExtents();
	TestERangeAndContinuity();
	TestESpotChecks();
	TestEHatMeanConsistency();
	std::cout << "All SheenDirectionalAlbedo tests passed!" << std::endl;
	return 0;
}
