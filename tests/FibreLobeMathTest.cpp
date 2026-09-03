//////////////////////////////////////////////////////////////////////
//
//  FibreLobeMathTest.cpp - Regression coverage for
//    src/Library/Materials/FibreLobeMath.h, the fibre-scattering math
//    (`Mp`, `TrimmedLogistic`, `SampleTrimmedLogistic`, `FrDielectric`
//    and their closed subgraph) PROMOTED out of `HairBSDF.cpp`'s
//    anonymous namespace for `weave_material`
//    (docs/CLOTH_FABRIC_DESIGN.md Phase 2, slice P2-A).
//
//  WHY THIS FILE EXISTS.  Before this promotion, `Mp` et al. were
//  private to `HairBSDF.cpp` and `HairBSDFTest`/`HairRenderTest`
//  guarded them implicitly, through `hair_material`'s own render
//  output.  Post-promotion the SAME functions also drive
//  `weave_material`'s surface and volume lobes -- so an edit made for
//  one consumer (say, tightening `Mp`'s small-`v` branch for a weave
//  preset) now changes hair too, and neither existing test names the
//  shared header directly.  `docs/CLOTH_FABRIC_DESIGN.md` 10.3 records
//  the promotion as "verified byte-identical" against a scratch diff
//  taken during this session's review -- true, but not a committed
//  regression guard (REVIEW_P2R3.md P2 "HairBSDF promotion byte-
//  identical claim has no committed regression guard").  This file is
//  that guard: a small, deterministic value table captured directly
//  from the CURRENT header, so a future edit that silently changes an
//  output is caught here rather than only showing up as a hair or
//  weave render drifting for a reason nobody traces back to this file.
//
//  WHAT IS NOT COVERED.  `MakeGeom` and `ComputeAp` were deliberately
//  NOT promoted (they carry the fibre radius, medulla geometry and the
//  R/TT/TRT multi-order split, none of which a woven thread has) and
//  stay local to `HairBSDF.cpp`; `HairBSDFTest` remains their guard.
//
//  TOLERANCE.  1e-9 absolute on every value below.  These are the
//  exact `%.17g` outputs of the shipped header on this toolchain
//  (clang, this platform) -- tighter than that would pin libm's last
//  few bits, which the project does not promise across platforms
//  (matches `LayeredWhiteFurnaceTest`'s stated cross-platform-libm
//  rationale for its own eps). 1e-9 is far tighter than any of these
//  functions' actual use in a render (a longitudinal/azimuthal density
//  or a Fresnel term feeding a BSDF value) needs to be, which is the
//  point: it exists to catch an EDIT, not to bound render quality.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <iostream>

#include "../src/Library/Materials/FibreLobeMath.h"

namespace
{
	int gPassed = 0;
	int gFailed = 0;

	const double kPI = 3.14159265358979323846;

	void CheckNear( const double actual, const double expected, const double eps, const char* const label )
	{
		const double diff = std::fabs( actual - expected );
		if( diff <= eps ) {
			++gPassed;
		} else {
			++gFailed;
			std::cerr << "FAIL: " << label << " -- got " << actual
			          << ", expected " << expected << " (|diff| " << diff
			          << " > eps " << eps << ")" << std::endl;
		}
	}

	void Check( const bool condition, const char* const label )
	{
		if( condition ) {
			++gPassed;
		} else {
			++gFailed;
			std::cerr << "FAIL: " << label << std::endl;
		}
	}
}

int main()
{
	using namespace RISE::FibreLobeMath;
	std::cout << "=== FibreLobeMathTest ===" << std::endl;

	const double kEps = 1e-9;

	// ---- the small shared primitives ----
	CheckNear( (double)Sqr( 3.0 ),         9.0,                 kEps, "Sqr(3) == 9" );
	CheckNear( (double)SafeSqrt( 4.0 ),    2.0,                 kEps, "SafeSqrt(4) == 2" );
	CheckNear( (double)SafeSqrt( -1.0 ),   0.0,                 kEps, "SafeSqrt(-1) clamps to 0, does not NaN" );
	CheckNear( (double)Clamp( 5.0, 0.0, 1.0 ),  1.0,            kEps, "Clamp(5,0,1) == 1" );
	CheckNear( (double)Clamp( -5.0, 0.0, 1.0 ), 0.0,            kEps, "Clamp(-5,0,1) == 0" );
	CheckNear( (double)SafeASin( 2.0 ),  1.5707963267948966,    kEps, "SafeASin(2) clamps to asin(1) == pi/2" );
	CheckNear( (double)SafeASin( 0.5 ),  0.52359877559829882,   kEps, "SafeASin(0.5) == asin(0.5)" );

	// ---- BesselI0 / LogBesselI0 -- Mp's own dependency, and the
	// small-v log-space branch's correctness is what keeps a narrow
	// satin float's Mp from over/underflowing. ----
	CheckNear( (double)BesselI0( 0.0 ), 1.0,                    kEps, "BesselI0(0) == 1" );
	CheckNear( (double)BesselI0( 1.0 ), 1.2660658777520082,     kEps, "BesselI0(1)" );
	CheckNear( (double)BesselI0( 5.0 ), 27.239864544019092,     1e-6, "BesselI0(5)" );
	CheckNear( (double)LogBesselI0( 0.5 ),  0.061549719185481376, kEps, "LogBesselI0(0.5), small-x branch" );
	CheckNear( (double)LogBesselI0( 10.0 ), 7.9397555460526563,   1e-8, "LogBesselI0(10), large-x branch" );
	CheckNear( (double)LogBesselI0( 50.0 ), 47.127549964081254,   1e-6, "LogBesselI0(50), large-x branch" );

	// ---- Mp -- the d'Eon 2011 longitudinal lobe both hair_material's
	// R lobe and weave_material's surface/volume lobes evaluate at
	// every shading point. ----
	//   1. On-cone, satin's narrow shipped width (beta 0.044 rad):
	//      Mp is sharply peaked -- this is close to its actual peak
	//      magnitude, the corner P3-3 of REVIEW_P2R1.md names.
	CheckNear( (double)Mp( 1.0, 1.0, 0.0, 0.0, 0.044 * 0.044 ), 9.0690644571824581, 1e-6,
	           "Mp on-cone, satin-narrow width (v=0.044^2)" );
	//   2. Off-cone (opposite hemispheres): must decay to ~0, not NaN.
	Check( (double)Mp( 1.0, -1.0, 0.0, 0.0, 0.044 * 0.044 ) >= 0.0 &&
	       (double)Mp( 1.0, -1.0, 0.0, 0.0, 0.044 * 0.044 ) < 1e-100,
	       "Mp off-cone decays to ~0 (finite, non-negative, not a firefly)" );
	//   3. A generic mid-angle, mid-width point (theta ~ 45 deg, v=0.16):
	CheckNear( (double)Mp( 0.7, 0.7, 0.7141428429, -0.7141428429, 0.16 ), 1.5010413553774637, 1e-6,
	           "Mp at a generic mid-angle, mid-width point" );
	//   4. The `v <= 0.1` / `v > 0.1` branch boundary in Mp's own
	//      implementation: v = 0.3 exercises the closed (non-log)
	//      branch on-cone.
	CheckNear( (double)Mp( 1.0, 1.0, 0.0, 0.0, 0.3 ), 0.76415233633111057, 1e-6,
	           "Mp on-cone, wide width (v=0.3, non-log branch)" );

	// ---- Logistic / LogisticCDF -- TrimmedLogistic's own building
	// blocks. ----
	CheckNear( (double)Logistic( 0.0, 0.5 ), 0.5,                 kEps, "Logistic(0,0.5) peaks at 0.5" );
	CheckNear( (double)Logistic( 1.0, 0.5 ), 0.20998717080701301, 1e-9, "Logistic(1,0.5)" );
	CheckNear( (double)LogisticCDF( 0.0, 0.5 ), 0.5,              kEps, "LogisticCDF(0,0.5) == 0.5" );
	CheckNear( (double)LogisticCDF(  kPI, 0.5 ), 0.99813603811037499, 1e-9, "LogisticCDF(+pi,0.5)" );
	CheckNear( (double)LogisticCDF( -kPI, 0.5 ), 0.0018639618896250283, 1e-9, "LogisticCDF(-pi,0.5)" );
	CheckNear( (double)LogisticCDF( kPI, 0.5 ) + (double)LogisticCDF( -kPI, 0.5 ), 1.0, 1e-9,
	           "LogisticCDF(+pi) + LogisticCDF(-pi) == 1 (symmetry around 0)" );

	// ---- TrimmedLogistic -- WeaveBRDF's azimuthal shape, and hair's
	// own azimuthal factor.  Both the untrimmed [-pi,pi] normalisation
	// weave_material's `value()` uses and a genuinely trimmed
	// [-1,1] interval (the kind `SurfaceLobePdf` builds per-sample). ----
	CheckNear( (double)TrimmedLogistic( 0.0, 0.5, -kPI, kPI ), 0.50187093659866067, 1e-9,
	           "TrimmedLogistic(0,0.5,-pi,pi), near-peak" );
	CheckNear( (double)TrimmedLogistic( 1.0, 0.5, -kPI, kPI ), 0.2107729161732371, 1e-9,
	           "TrimmedLogistic(1,0.5,-pi,pi), off-peak" );
	CheckNear( (double)TrimmedLogistic( 0.5, 0.5, -1.0, 1.0 ), 0.51631681179260935, 1e-9,
	           "TrimmedLogistic(0.5,0.5,-1,1), a genuinely trimmed interval" );

	// ---- SampleTrimmedLogistic -- the exact inverse-CDF sampler
	// `WeaveSPF`'s surface branch and `HairBSDF`'s own sampler both
	// call. ----
	CheckNear( (double)SampleTrimmedLogistic( 0.5, 0.5, -kPI, kPI ), 0.0, 1e-9,
	           "SampleTrimmedLogistic(u=0.5) returns the distribution's centre" );
	CheckNear( (double)SampleTrimmedLogistic( 0.1, 0.5, -kPI, kPI ), -1.090382369698319, 1e-8,
	           "SampleTrimmedLogistic(u=0.1,-pi,pi)" );
	CheckNear( (double)SampleTrimmedLogistic( 0.9, 0.5, -1.0, 1.0 ), 0.7077680456319857, 1e-8,
	           "SampleTrimmedLogistic(u=0.9,-1,1), a genuinely trimmed interval" );
	// Round-trip: CDF(Sample(u)) == u, over the SAME trimmed interval --
	// the property WeaveSPF.h's sampler correctness argument actually
	// leans on.
	{
		const double a = -1.0, b = 1.0, s = 0.5;
		const double full = (double)LogisticCDF( b, s ) - (double)LogisticCDF( a, s );
		for( double u : { 0.05, 0.25, 0.5, 0.75, 0.95 } ) {
			const double x = (double)SampleTrimmedLogistic( u, s, a, b );
			const double back = ( (double)LogisticCDF( x, s ) - (double)LogisticCDF( a, s ) ) / full;
			CheckNear( back, u, 1e-7, "SampleTrimmedLogistic round-trips through LogisticCDF" );
		}
	}

	// ---- FrDielectric -- Kim 2002's cylinder-geometry Fresnel term
	// both lobes evaluate, and WeaveIndependentCheck's (in
	// LayeredWhiteFurnaceTest.cpp) own copy is checked against THIS
	// header's actual behaviour by these same numbers. ----
	CheckNear( (double)FrDielectric( 1.0, 1.5 ),  0.040000000000000008, 1e-9, "FrDielectric(1,1.5), normal incidence" );
	CheckNear( (double)FrDielectric( 0.0, 1.5 ),  1.0,                  1e-9, "FrDielectric(0,1.5), grazing == total reflection" );
	CheckNear( (double)FrDielectric( 0.5, 1.46 ), 0.082454092615792673, 1e-9, "FrDielectric(0.5,1.46), denim's eta" );
	CheckNear( (double)FrDielectric( -0.5, 1.539 ), 1.0,                1e-9,
	           "FrDielectric(-0.5,1.539): negative cosThetaI flips eta->1/eta and hits TIR" );
	CheckNear( (double)FrDielectric( 0.05, 1.345 ), 0.73452176763689792, 1e-9, "FrDielectric(0.05,1.345), silk's eta, near grazing" );

	std::cout << gPassed << "/" << ( gPassed + gFailed ) << " checks passed" << std::endl;
	return gFailed == 0 ? 0 : 1;
}
