//////////////////////////////////////////////////////////////////////
//
//  VCMSpectralRecurrenceTest.cpp - Unit test for the key HWSS
//    invariant: VCMMisQuantities (dVCM, dVC, dVM) are wavelength-
//    independent, so ConvertLightSubpath / ConvertEyeSubpath produce
//    identical MIS running quantities regardless of the hero
//    wavelength used to generate the BDPTVertex array.
//
//    The recurrence reads only area-measure PDFs, cosines, and
//    distances from the subpath — none of which depend on the
//    wavelength.  This test constructs a synthetic subpath, copies
//    it, perturbs the `throughputNM` field on the copy (simulating
//    a companion-wavelength re-evaluation), runs the converter on
//    both, and asserts the emitted LightVertex::mis and the parallel
//    VCMMisQuantities arrays are bit-identical.
//
//    Step 10 relies on this invariant: VCMSpectralRasterizer shares
//    the hero-wavelength VCMMisQuantities across companion wavelengths
//    instead of recomputing per wavelength.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../src/Library/Shaders/VCMIntegrator.h"
#include "../src/Library/Shaders/BDPTVertex.h"
#include "../src/Library/Utilities/Color/RGBSpectra.h"

using namespace RISE;
using namespace RISE::Implementation;

static int g_pass = 0;
static int g_fail = 0;

static void CheckEqual( Scalar a, Scalar b, const char* label )
{
	if( a == b ) {
		g_pass++;
	} else {
		g_fail++;
		printf( "  FAIL: %s (a=%.20g b=%.20g)\n", label, (double)a, (double)b );
	}
}

// Tight-tolerance variant for values that go through RGBIlluminantSpectrum::Eval()
// compiled at two DIFFERENT call sites (once inlined inside VCMIntegrator.cpp's
// ConvertLightSubpath / this test's direct call, once inside the separately
// compiled VCMIntegrator::LightThroughputRadianceNM).  On this repo's macOS
// build (`-ffast-math` + associative math, see CLAUDE.md's fast-math entry)
// the SAME formula compiled at two call sites is not guaranteed bit-identical
// -- FP contraction/reassociation decisions depend on each call site's
// surrounding code, not just the formula.  Measured spread for this
// computation is ~1e-13 relative; a genuine "forgotten rebuild" bug (stale
// pre-clamp spectrum vs a post-clamp throughput, or the default-zero
// spectrum) differs by orders of magnitude more, so 1e-9 relative stays
// tight enough to catch the bug this test targets while tolerating the
// build's own FP nondeterminism.
static void CheckClose( Scalar a, Scalar b, const char* label )
{
	const double da = (double)a, db = (double)b;
	const double scale = ( fabs( da ) > fabs( db ) ? fabs( da ) : fabs( db ) );
	const double tol = 1e-9 * ( scale > 1.0 ? scale : 1.0 );
	if( fabs( da - db ) <= tol ) {
		g_pass++;
	} else {
		g_fail++;
		printf( "  FAIL: %s (a=%.20g b=%.20g diff=%.3g)\n", label, da, db, fabs( da - db ) );
	}
}

static void BuildSyntheticLightSubpath( std::vector<BDPTVertex>& verts )
{
	verts.clear();

	// v[0] = LIGHT at (0,0,0) with +x normal, finite area light,
	// cosine-weighted emission, directPdfA = 1, emissionPdfW = 1.
	{
		BDPTVertex v;
		v.type = BDPTVertex::LIGHT;
		v.position = Point3( 0, 0, 0 );
		v.normal = Vector3( 1, 0, 0 );
		v.pdfFwd = 1.0;
		v.pdfRev = 0.0;
		v.emissionPdfW = 1.0;
		v.cosAtGen = 1.0;
		v.isDelta = false;
		v.isConnectible = true;
		v.isBSSRDFEntry = false;
		v.throughput = RISEPel( 1, 1, 1 );
		v.throughputNM = 1.0;
		verts.push_back( v );
	}
	// v[1] = SURFACE at (2,0,0) with -x normal, distance 2, cosine 1.
	{
		BDPTVertex v;
		v.type = BDPTVertex::SURFACE;
		v.position = Point3( 2, 0, 0 );
		v.normal = Vector3( -1, 0, 0 );
		v.pdfFwd = 0.25;	// cosine-weighted area PDF on second vertex
		v.pdfRev = 0.25;
		v.emissionPdfW = 0;
		v.cosAtGen = 1.0;
		v.isDelta = false;
		v.isConnectible = true;
		v.isBSSRDFEntry = false;
		v.throughput = RISEPel( 0.5, 0.5, 0.5 );
		v.throughputNM = 0.5;
		verts.push_back( v );
	}
	// v[2] = SURFACE at (5,0,0), distance 3 from v[1], cosine 1.
	{
		BDPTVertex v;
		v.type = BDPTVertex::SURFACE;
		v.position = Point3( 5, 0, 0 );
		v.normal = Vector3( -1, 0, 0 );
		v.pdfFwd = 0.111111111111;
		v.pdfRev = 0.111111111111;
		v.emissionPdfW = 0;
		v.cosAtGen = 1.0;
		v.isDelta = false;
		v.isConnectible = true;
		v.isBSSRDFEntry = false;
		v.throughput = RISEPel( 0.25, 0.25, 0.25 );
		v.throughputNM = 0.25;
		verts.push_back( v );
	}
}


//////////////////////////////////////////////////////////////////////
// Stage C slice 2: the stored light-subpath throughput's RGB -> nm
// projection carries the reference illuminant, not a flat luma scalar.
//
// The LightVertexStore holds Pel throughputs (the light pass is not
// wavelength-matched to the eye pass), and the NM merge estimator
// projects them through VCMIntegrator::LightThroughputRadianceNM.
//
// CORRECTED (post-09d723b6): production no longer calls
// LightThroughputRadianceNM at merge time.  It has zero production
// callers now -- the NM merge estimator instead reads the per-vertex
// cached `lv.throughputSpectrum.Eval(nm)` (an RGBIlluminantSpectrum
// built once, at each of the three throughput writers, by
// VCMIntegrator::LightThroughputSpectrum -- deposit in
// ConvertLightSubpath, LightVertexStore::ClampOutlierThroughputs, and
// the VCMRasterizerBase median clamp).  LightThroughputRadianceNM
// itself is exactly `LightThroughputSpectrum(p).Eval(nm)`, so it
// remains a faithful equivalent for exercising the projection formula
// in this test, but it is no longer "the same function production
// calls" -- see the ConvertLightSubpath-based assertions below, which
// pin the cached field itself against this free function.
//
// It used to be `0.2126 r + 0.7152 g + 0.0722 b`: one scalar reused at
// every wavelength.  A flat spectrum resolves to (1.205, 0.948, 0.909)
// on this film, so the VM (merge) strategies landed on a different
// chroma from the VC (connect) strategies they share a MIS partition
// with, and a coloured light merged GREY.
//
// Mutation guard: restore the luma projection and every ratio below
// becomes exactly 1.0.
//////////////////////////////////////////////////////////////////////
static void TestLightThroughputCarriesIlluminant()
{
	printf( "\nLightThroughputRadianceNM -- illuminant shape\n" );

	// (1) WHITE throughput must equal the Y-normalised reference
	// illuminant itself, wavelength for wavelength.
	const double nms[] = { 420, 450, 500, 550, 600, 650, 700 };
	bool whiteTracksD65 = true;
	for( int i = 0; i < 7; i++ ) {
		const double got = double( VCMIntegrator::LightThroughputRadianceNM(
			RISEPel( 1, 1, 1 ), Scalar( nms[i] ) ) );
		const double want = double(
			RGBIlluminantSpectrum::ReferenceIlluminant( Scalar( nms[i] ) ) );
		if( fabs( got - want ) > 1e-3 * ( want > 1.0 ? want : 1.0 ) ) {
			whiteTracksD65 = false;
			printf( "  at %g nm: got %.6f, D65norm %.6f\n", nms[i], got, want );
		}
	}
	if( whiteTracksD65 ) { g_pass++; } else {
		g_fail++;
		printf( "  FAIL: white throughput != reference illuminant\n" );
	}

	// (2) ... and is therefore NOT wavelength-flat.  This is the check
	// the old luma projection fails: it returned 1.0 at both.
	const double w450 = double( VCMIntegrator::LightThroughputRadianceNM(
		RISEPel( 1, 1, 1 ), Scalar( 450 ) ) );
	const double w650 = double( VCMIntegrator::LightThroughputRadianceNM(
		RISEPel( 1, 1, 1 ), Scalar( 650 ) ) );
	const double d65_450 = double(
		RGBIlluminantSpectrum::ReferenceIlluminant( Scalar( 450 ) ) );
	const double d65_650 = double(
		RGBIlluminantSpectrum::ReferenceIlluminant( Scalar( 650 ) ) );
	printf( "  white: S(450) = %.6f, S(650) = %.6f, ratio = %.6f "
		"(D65norm ratio %.6f; was 1.000000)\n",
		w450, w650, w650 / w450, d65_650 / d65_450 );
	// 1e-3 relative, not exact: the authored-white sigmoid is ~0.99999
	// rather than identically 1 and its residual varies slightly with
	// wavelength, so the ratio tracks D65norm's to ~5e-5, not to the
	// last bit.  This tolerance is still ~6000x tighter than the
	// mutation it guards (the luma projection gives exactly 1.0).
	if( fabs( w650 / w450 - d65_650 / d65_450 ) < 1e-3 ) { g_pass++; } else {
		g_fail++;
		printf( "  FAIL: white S(650)/S(450) != the D65-normalised ratio\n" );
	}
	if( fabs( w650 / w450 - 1.0 ) > 1e-3 ) { g_pass++; } else {
		g_fail++;
		printf( "  FAIL: white throughput is wavelength-FLAT (luma projection)\n" );
	}

	// (3) A coloured throughput must be chromatic, not grey.  Red and
	// blue throughputs of equal luma used to give the IDENTICAL spectrum.
	const double r650 = double( VCMIntegrator::LightThroughputRadianceNM(
		RISEPel( 1.0, 0.2, 0.2 ), Scalar( 650 ) ) );
	const double r450 = double( VCMIntegrator::LightThroughputRadianceNM(
		RISEPel( 1.0, 0.2, 0.2 ), Scalar( 450 ) ) );
	const double b650 = double( VCMIntegrator::LightThroughputRadianceNM(
		RISEPel( 0.2, 0.2, 1.0 ), Scalar( 650 ) ) );
	const double b450 = double( VCMIntegrator::LightThroughputRadianceNM(
		RISEPel( 0.2, 0.2, 1.0 ), Scalar( 450 ) ) );
	printf( "  red throughput S(650)/S(450) = %.6f; "
		"blue throughput S(650)/S(450) = %.6f\n",
		r650 / r450, b650 / b450 );
	if( r650 / r450 > 2.0 ) { g_pass++; } else {
		g_fail++;
		printf( "  FAIL: red throughput is not red-weighted\n" );
	}
	if( b650 / b450 < 0.5 ) { g_pass++; } else {
		g_fail++;
		printf( "  FAIL: blue throughput is not blue-weighted\n" );
	}

	// (4) A negative component must not flip the max-channel scale.
	// EnsurePositve guards this at the projection boundary.
	const double neg = double( VCMIntegrator::LightThroughputRadianceNM(
		RISEPel( -0.5, 0.3, 0.1 ), Scalar( 550 ) ) );
	if( neg >= 0.0 ) { g_pass++; } else {
		g_fail++;
		printf( "  FAIL: a negative component produced negative radiance (%.6f)\n", neg );
	}
}

int main()
{
	printf( "VCMSpectralRecurrenceTest: HWSS wavelength-invariance\n" );

	// Both paths use the same VCM normalization (the recurrence
	// constants are scene-wide, not wavelength-dependent).
	const VCMNormalization norm = ComputeNormalization(
		256, 256, 0.05, true, true );

	std::vector<BDPTVertex> heroVerts;
	BuildSyntheticLightSubpath( heroVerts );

	// Companion wavelength: every throughputNM differs, but positions,
	// normals, cosines, and area PDFs are identical — exactly what
	// BDPTIntegrator::RecomputeSubpathThroughputNM produces.
	std::vector<BDPTVertex> compVerts = heroVerts;
	for( std::size_t i = 0; i < compVerts.size(); i++ ) {
		compVerts[i].throughputNM *= 3.7;	// arbitrary scale
	}

	std::vector<LightVertex> heroOut, compOut;
	std::vector<VCMMisQuantities> heroMis, compMis;

	VCMIntegrator::ConvertLightSubpath( heroVerts, norm, heroOut, &heroMis );
	VCMIntegrator::ConvertLightSubpath( compVerts, norm, compOut, &compMis );

	// Assertion 1: the parallel VCMMisQuantities arrays are
	// bit-identical — the recurrence does not read throughputNM.
	if( heroMis.size() == compMis.size() ) {
		g_pass++;
	} else {
		g_fail++;
		printf( "  FAIL: heroMis.size != compMis.size\n" );
	}

	for( std::size_t i = 0; i < heroMis.size() && i < compMis.size(); i++ ) {
		char lbl[64];
		snprintf( lbl, sizeof( lbl ), "mis[%zu].dVCM invariant", i );
		CheckEqual( heroMis[i].dVCM, compMis[i].dVCM, lbl );
		snprintf( lbl, sizeof( lbl ), "mis[%zu].dVC invariant", i );
		CheckEqual( heroMis[i].dVC, compMis[i].dVC, lbl );
		snprintf( lbl, sizeof( lbl ), "mis[%zu].dVM invariant", i );
		CheckEqual( heroMis[i].dVM, compMis[i].dVM, lbl );
	}

	// Assertion 2: the emitted LightVertex records carry the same MIS
	// quantities regardless of wavelength.  (They carry different
	// throughput because the Pel RISEPel field is still populated from
	// the source BDPTVertex, but mis state is wavelength-free.)
	if( heroOut.size() == compOut.size() ) {
		g_pass++;
	} else {
		g_fail++;
		printf( "  FAIL: heroOut.size != compOut.size\n" );
	}

	for( std::size_t i = 0; i < heroOut.size() && i < compOut.size(); i++ ) {
		char lbl[64];
		snprintf( lbl, sizeof( lbl ), "lv[%zu].mis.dVCM invariant", i );
		CheckEqual( heroOut[i].mis.dVCM, compOut[i].mis.dVCM, lbl );
		snprintf( lbl, sizeof( lbl ), "lv[%zu].mis.dVC invariant", i );
		CheckEqual( heroOut[i].mis.dVC, compOut[i].mis.dVC, lbl );
		snprintf( lbl, sizeof( lbl ), "lv[%zu].mis.dVM invariant", i );
		CheckEqual( heroOut[i].mis.dVM, compOut[i].mis.dVM, lbl );
	}

	// Assertion 2b: pin the production cache.  ConvertLightSubpath is
	// one of the three sites that must build `throughputSpectrum`
	// (deposit) -- verify every vertex it emits carries a cache that
	// matches (see CheckClose above for why this is a tight tolerance
	// rather than `==`) what LightThroughputRadianceNM computes fresh
	// from the same `throughput`.  Both sides go through the identical
	// LightThroughputSpectrum() construction, so a forgotten rebuild
	// after a future write site would leave `throughputSpectrum` stale
	// by orders of magnitude more than this test's tolerance (typically
	// the zero spectrum from LightVertex's default constructor) and
	// this would catch it immediately instead of only showing up as a
	// silently-black merge contribution.
	{
		const double checkNMs[2] = { 450.0, 650.0 };
		for( std::size_t i = 0; i < heroOut.size(); i++ ) {
			for( int k = 0; k < 2; k++ ) {
				const Scalar nm = Scalar( checkNMs[k] );
				const Scalar cached = heroOut[i].throughputSpectrum.Eval( nm );
				const Scalar fresh = VCMIntegrator::LightThroughputRadianceNM(
					heroOut[i].throughput, nm );
				char lbl[80];
				snprintf( lbl, sizeof( lbl ),
					"lv[%zu].throughputSpectrum.Eval(%g) ~= LightThroughputRadianceNM",
					i, checkNMs[k] );
				CheckClose( cached, fresh, lbl );
			}
		}
	}

	// Assertion 2c: the default-constructed LightVertex's
	// throughputSpectrum must evaluate to exactly 0 at every
	// wavelength -- this is the silent-zero failure signature a
	// forgotten rebuild at some future fourth write site would
	// produce (LightVertex's default ctor leaves `throughput` at
	// (0,0,0) and builds throughputSpectrum from that same zero
	// triple, per its header comment).
	{
		LightVertex defaultLv;
		const double checkNMs[3] = { 450.0, 550.0, 650.0 };
		bool allZero = true;
		for( int k = 0; k < 3; k++ ) {
			const Scalar v = defaultLv.throughputSpectrum.Eval( Scalar( checkNMs[k] ) );
			if( v != Scalar( 0 ) ) {
				allZero = false;
				printf( "  FAIL: default LightVertex throughputSpectrum.Eval(%g) = %.6f, want 0\n",
					checkNMs[k], (double)v );
			}
		}
		if( allZero ) { g_pass++; } else { g_fail++; }
	}

	// Assertion 3: ConvertEyeSubpath exhibits the same invariance.
	std::vector<BDPTVertex> heroEye, compEye;
	BuildSyntheticLightSubpath( heroEye );
	heroEye[0].type = BDPTVertex::CAMERA;
	heroEye[0].pdfFwd = 1.0;
	heroEye[0].emissionPdfW = 1.0;
	heroEye[0].cosAtGen = 1.0;
	compEye = heroEye;
	for( std::size_t i = 0; i < compEye.size(); i++ ) {
		compEye[i].throughputNM *= 2.3;
	}

	std::vector<VCMMisQuantities> heroEyeMis, compEyeMis;
	VCMIntegrator::ConvertEyeSubpath( heroEye, norm, heroEyeMis );
	VCMIntegrator::ConvertEyeSubpath( compEye, norm, compEyeMis );

	if( heroEyeMis.size() == compEyeMis.size() ) {
		g_pass++;
	} else {
		g_fail++;
		printf( "  FAIL: heroEyeMis.size != compEyeMis.size\n" );
	}

	for( std::size_t i = 0; i < heroEyeMis.size() && i < compEyeMis.size(); i++ ) {
		char lbl[64];
		snprintf( lbl, sizeof( lbl ), "eyeMis[%zu].dVCM invariant", i );
		CheckEqual( heroEyeMis[i].dVCM, compEyeMis[i].dVCM, lbl );
		snprintf( lbl, sizeof( lbl ), "eyeMis[%zu].dVC invariant", i );
		CheckEqual( heroEyeMis[i].dVC, compEyeMis[i].dVC, lbl );
		snprintf( lbl, sizeof( lbl ), "eyeMis[%zu].dVM invariant", i );
		CheckEqual( heroEyeMis[i].dVM, compEyeMis[i].dVM, lbl );
	}

	TestLightThroughputCarriesIlluminant();

	printf( "Passed: %d\n", g_pass );
	printf( "Failed: %d\n", g_fail );
	if( g_fail == 0 ) {
		printf( "All tests passed.\n" );
		return 0;
	}
	printf( "FAILED.\n" );
	return 1;
}
