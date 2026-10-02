//////////////////////////////////////////////////////////////////////
//
//  VCMLightPostPassTest.cpp - Unit tests for
//    VCMIntegrator::ConvertLightSubpath.
//
//    Hand-builds a synthetic BDPTVertex array representing a
//    canonical 2-bounce light subpath, runs the converter, and
//    asserts the emitted LightVertex records carry the exact
//    dVCM/dVC/dVM values computed by hand from the SmallVCM
//    recurrence.
//
//    All positions / normals are chosen so every cosine is 1.0
//    and all distances are integers.  Every field on BDPTVertex
//    is explicit, so the expected values depend only on the
//    recurrence formulas, not on scene / sampler / renderer state.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <vector>

#include "../src/Library/Shaders/VCMIntegrator.h"
#include "../src/Library/Shaders/BDPTVertex.h"

using namespace RISE;
using namespace RISE::Implementation;

static int g_pass = 0;
static int g_fail = 0;

static inline bool ApproxEqual( Scalar a, Scalar b, Scalar tol = 1e-9 )
{
	const Scalar d = a - b;
	return ( d < 0 ? -d : d ) <= tol;
}

static void CheckClose( Scalar actual, Scalar expected, Scalar tol, const char* label )
{
	if( ApproxEqual( actual, expected, tol ) ) {
		g_pass++;
	} else {
		g_fail++;
		printf( "  FAIL: %s (expected %.12g, got %.12g)\n", label, (double)expected, (double)actual );
	}
}

static void Check( bool cond, const char* label )
{
	if( cond ) {
		g_pass++;
	} else {
		g_fail++;
		printf( "  FAIL: %s\n", label );
	}
}

//
// Build a canonical 2-bounce light subpath on the +x axis with
// outward-facing normals.  All cosines collapse to 1.0 so the
// expected dVCM/dVC/dVM values are expressible as exact rationals.
//
//   v[0] = LIGHT at (0,0,0), normal (+x)
//   v[1] = SURFACE at (2,0,0), normal (-x)  (faces the light)
//   v[2] = SURFACE at (5,0,0), normal (-x)  (faces v[1])
//
// Distances:
//   v[0] -> v[1] : distSq = 4
//   v[1] -> v[2] : distSq = 9
//
// Chosen BDPT fields:
//   v[0].pdfFwd (= directPdfA)           = 0.25
//   v[0].emissionPdfW                    = 0.125
//   v[0].cosAtGen (= cosLight)           = 1.0
//   v[0].pdfRev                          = 0.3  (set retroactively)
//   v[1].pdfFwd (area measure at v[1])   = 0.1
//   v[1].cosAtGen                        = 1.0
//   v[1].pdfRev                          = 0.5  (unused, v[3] doesn't exist)
//   v[1].throughput                      = (4,4,4)
//   v[2].pdfFwd                          = 0.2
//   v[2].cosAtGen                        = 1.0
//   v[2].throughput                      = (7,7,7)
//
// And we use norm with merge radius 0 so mMisVmWeightFactor and
// mMisVcWeightFactor are both 0 — the recurrence collapses to the
// pure-VC form and the arithmetic stays tractable.
//
static void TestTwoBounceDiffuseAllCosinesOne()
{
	printf( "Test 1: 2-bounce axis-aligned diffuse path\n" );

	std::vector<BDPTVertex> verts;

	// v[0]: LIGHT
	{
		BDPTVertex v;
		v.type = BDPTVertex::LIGHT;
		v.position = Point3( 0, 0, 0 );
		v.normal = Vector3( 1, 0, 0 );
		v.pdfFwd = 0.25;
		v.pdfRev = 0.3;
		v.emissionPdfW = 0.125;
		v.cosAtGen = 1.0;
		v.isDelta = false;
		v.isConnectible = true;
		v.throughput = RISEPel( 1, 1, 1 );
		verts.push_back( v );
	}

	// v[1]: SURFACE
	{
		BDPTVertex v;
		v.type = BDPTVertex::SURFACE;
		v.position = Point3( 2, 0, 0 );
		v.normal = Vector3( -1, 0, 0 );
		v.pdfFwd = 0.1;
		v.pdfRev = 0.5;
		v.cosAtGen = 1.0;
		v.isDelta = false;
		v.isConnectible = true;
		v.throughput = RISEPel( 4, 4, 4 );
		verts.push_back( v );
	}

	// v[2]: SURFACE
	{
		BDPTVertex v;
		v.type = BDPTVertex::SURFACE;
		v.position = Point3( 5, 0, 0 );
		v.normal = Vector3( -1, 0, 0 );
		v.pdfFwd = 0.2;
		v.pdfRev = 0;
		v.cosAtGen = 1.0;
		v.isDelta = false;
		v.isConnectible = true;
		v.throughput = RISEPel( 7, 7, 7 );
		verts.push_back( v );
	}

	// Merge radius 0 => mMisVmWeightFactor = 0, mMisVcWeightFactor = 0,
	// which collapses the recurrence to pure-VC arithmetic.
	const VCMNormalization norm = ComputeNormalization( 100, 100, 0.0, true, false );
	Check( norm.mMisVmWeightFactor == 0 && norm.mMisVcWeightFactor == 0,
		"norm: VM factors zero with r=0" );

	std::vector<LightVertex> out;
	VCMIntegrator::ConvertLightSubpath( verts, norm, out );

	// Two LightVertex records expected: v[1] and v[2], both non-delta
	// SURFACE vertices.  v[0] is the light endpoint and is never
	// stored.
	Check( out.size() == 2, "emit: two LightVertex records" );
	if( out.size() != 2 ) {
		return;
	}

	//
	// Hand-compute the expected dVCM/dVC/dVM at v[1] after the
	// geometric update.
	//
	//   InitLight(directPdfA=0.25, emissionPdfW=0.125, cosLight=1,
	//             finite, !delta, norm):
	//     dVCM = 0.25/0.125 = 2
	//     dVC  = 1/0.125    = 8
	//     dVM  = 8 * 0 = 0
	//
	//   Geometric update at v[1]: distSq=4, cos=1, gate=true:
	//     dVCM : 2 * 4 / 1 = 8
	//     dVC  : 8 / 1     = 8
	//     dVM  : 0 / 1     = 0
	//
	CheckClose( out[0].mis.dVCM, 8.0, 1e-12, "v[1] dVCM" );
	CheckClose( out[0].mis.dVC,  8.0, 1e-12, "v[1] dVC"  );
	CheckClose( out[0].mis.dVM,  0.0, 1e-12, "v[1] dVM"  );

	// LightVertex geometry / metadata at v[1]
	Check( out[0].ptPosition.x == 2 && out[0].ptPosition.y == 0 && out[0].ptPosition.z == 0,
		"v[1] position" );
	Check( out[0].normal.x == -1, "v[1] normal" );
	CheckClose( out[0].wi.x, 1.0, 1e-15, "v[1] wi.x (prev->this)" );
	CheckClose( out[0].wi.y, 0.0, 1e-15, "v[1] wi.y" );
	CheckClose( out[0].wi.z, 0.0, 1e-15, "v[1] wi.z" );
	CheckClose( out[0].throughput[0], 4.0, 1e-15, "v[1] throughput" );
	Check( out[0].pathLength == 1, "v[1] pathLength" );
	Check( ( out[0].flags & kLVF_IsConnectible ) != 0, "v[1] flagged connectible" );
	Check( ( out[0].flags & kLVF_IsDelta ) == 0, "v[1] not flagged delta" );

	//
	// Hand-compute the BSDF-sampling update at v[1] (preparing for v[2]).
	//
	//   cosThetaOut at v[1] for direction to v[2] = |(-1,0,0)·(1,0,0)| = 1
	//   bsdfDirPdfW_out = v[2].pdfFwd * distSq(v1→v2) / v[2].cosAtGen
	//                    = 0.2 * 9 / 1 = 1.8
	//   cosAtPrevOutgoing = |v[0].normal·(1,0,0)| = |(1,0,0)·(1,0,0)| = 1
	//   bsdfRevPdfW_out  = v[0].pdfRev * distSq(v0→v1) / cosAtPrevOutgoing
	//                    = 0.3 * 4 / 1 = 1.2
	//
	//   factor = cosThetaOut / bsdfDirPdfW_out = 1 / 1.8
	//
	//   dVC  = factor * (dVC_old*pRev + dVCM_old + 0) = (1/1.8) * (8*1.2 + 8) = (1/1.8)*17.6 = 9.777...
	//   dVM  = factor * (dVM_old*pRev + dVCM_old*0 + 1) = (1/1.8) * 1
	//   dVCM = 1/1.8
	//
	//   Geometric update at v[2]: distSq=9, cos=1:
	//     dVCM' = (1/1.8) * 9 = 5
	//     dVC'  = 9.777.. / 1 = 9.777..
	//     dVM'  = (1/1.8) / 1 = 0.555..
	//
	const Scalar expDVC_v2 = ( Scalar( 1 ) / 1.8 ) * ( 8.0 * 1.2 + 8.0 );
	const Scalar expDVM_v2 = ( Scalar( 1 ) / 1.8 );
	const Scalar expDVCM_v2 = 5.0;
	CheckClose( out[1].mis.dVCM, expDVCM_v2, 1e-12, "v[2] dVCM" );
	CheckClose( out[1].mis.dVC,  expDVC_v2,  1e-12, "v[2] dVC"  );
	CheckClose( out[1].mis.dVM,  expDVM_v2,  1e-12, "v[2] dVM"  );

	Check( out[1].pathLength == 2, "v[2] pathLength" );
}

//
// Test 2: Delta vertex in the middle.  A specular bounce zeros
// dVCM and multiplies dVC/dVM by cosThetaOut only.  Here we place
// a delta vertex between two diffuse surface vertices so we can
// watch the specular branch fire and confirm it doesn't emit a
// LightVertex for the delta vertex itself.
//
static void TestDeltaVertexInMiddle()
{
	printf( "Test 2: Delta vertex in middle\n" );

	std::vector<BDPTVertex> verts;

	// v[0]: LIGHT
	{
		BDPTVertex v;
		v.type = BDPTVertex::LIGHT;
		v.position = Point3( 0, 0, 0 );
		v.normal = Vector3( 1, 0, 0 );
		v.pdfFwd = 0.5;
		v.pdfRev = 0.1;
		v.emissionPdfW = 0.25;
		v.cosAtGen = 1.0;
		v.isDelta = false;
		v.isConnectible = true;
		verts.push_back( v );
	}

	// v[1]: SURFACE (non-delta)
	{
		BDPTVertex v;
		v.type = BDPTVertex::SURFACE;
		v.position = Point3( 1, 0, 0 );
		v.normal = Vector3( -1, 0, 0 );
		v.pdfFwd = 0.25;
		v.pdfRev = 0.1;
		v.cosAtGen = 1.0;
		v.isDelta = false;
		v.isConnectible = true;
		v.throughput = RISEPel( 2, 2, 2 );
		verts.push_back( v );
	}

	// v[2]: SURFACE (delta / specular)
	{
		BDPTVertex v;
		v.type = BDPTVertex::SURFACE;
		v.position = Point3( 3, 0, 0 );
		v.normal = Vector3( -1, 0, 0 );
		v.pdfFwd = 0.1;
		v.pdfRev = 0.1;
		v.cosAtGen = 1.0;
		v.isDelta = true;		// specular!
		v.isConnectible = false;	// delta lobe
		v.throughput = RISEPel( 5, 5, 5 );
		verts.push_back( v );
	}

	// v[3]: SURFACE (non-delta again)
	{
		BDPTVertex v;
		v.type = BDPTVertex::SURFACE;
		v.position = Point3( 7, 0, 0 );
		v.normal = Vector3( -1, 0, 0 );
		v.pdfFwd = 0.1;
		v.pdfRev = 0;
		v.cosAtGen = 1.0;
		v.isDelta = false;
		v.isConnectible = true;
		v.throughput = RISEPel( 3, 3, 3 );
		verts.push_back( v );
	}

	const VCMNormalization norm = ComputeNormalization( 100, 100, 0.0, true, false );

	std::vector<LightVertex> out;
	VCMIntegrator::ConvertLightSubpath( verts, norm, out );

	// Only v[1] and v[3] should be emitted (v[0]=LIGHT, v[2]=delta).
	Check( out.size() == 2, "delta: only non-delta surfaces emitted" );
	if( out.size() < 2 ) {
		return;
	}
	Check( out[0].pathLength == 1, "delta: first emitted is v[1]" );
	Check( out[1].pathLength == 3, "delta: second emitted is v[3]" );

	Check( ( out[0].flags & kLVF_IsDelta ) == 0, "delta: v[1] not flagged delta" );
	Check( ( out[1].flags & kLVF_IsDelta ) == 0, "delta: v[3] not flagged delta" );
}

//
// Test 3: Empty / single-vertex paths produce no LightVertex records.
//
static void TestDegenerateInputs()
{
	printf( "Test 3: Degenerate inputs\n" );

	const VCMNormalization norm = ComputeNormalization( 100, 100, 0.0, true, false );

	// Empty path
	{
		std::vector<BDPTVertex> verts;
		std::vector<LightVertex> out;
		VCMIntegrator::ConvertLightSubpath( verts, norm, out );
		Check( out.empty(), "empty path: no output" );
	}

	// Single vertex: just the light, no bounces
	{
		std::vector<BDPTVertex> verts;
		BDPTVertex v;
		v.type = BDPTVertex::LIGHT;
		v.position = Point3( 0, 0, 0 );
		v.normal = Vector3( 0, 0, 1 );
		v.pdfFwd = 0.5;
		v.emissionPdfW = 0.25;
		v.cosAtGen = 1.0;
		v.isDelta = false;
		v.isConnectible = true;
		verts.push_back( v );

		std::vector<LightVertex> out;
		VCMIntegrator::ConvertLightSubpath( verts, norm, out );
		Check( out.empty(), "lone light vertex: no output" );
	}

	// Malformed: root vertex not a LIGHT
	{
		std::vector<BDPTVertex> verts;
		BDPTVertex v;
		v.type = BDPTVertex::SURFACE;
		v.cosAtGen = 1.0;
		v.pdfFwd = 1.0;
		verts.push_back( v );
		std::vector<LightVertex> out;
		VCMIntegrator::ConvertLightSubpath( verts, norm, out );
		Check( out.empty(), "malformed root: no output" );
	}
}

//
// Test 4: Medium vertex is traversed but NOT emitted to the store
// (surface-only merging in v1).
//
static void TestMediumVertexNotEmitted()
{
	printf( "Test 4: Medium vertex skipped\n" );

	std::vector<BDPTVertex> verts;

	// v[0]: LIGHT
	{
		BDPTVertex v;
		v.type = BDPTVertex::LIGHT;
		v.position = Point3( 0, 0, 0 );
		v.normal = Vector3( 1, 0, 0 );
		v.pdfFwd = 0.25;
		v.pdfRev = 0.1;
		v.emissionPdfW = 0.125;
		v.cosAtGen = 1.0;
		v.isDelta = false;
		v.isConnectible = true;
		verts.push_back( v );
	}

	// v[1]: MEDIUM (cosAtGen = 0 sentinel)
	{
		BDPTVertex v;
		v.type = BDPTVertex::MEDIUM;
		v.position = Point3( 1, 0, 0 );
		v.normal = Vector3( 0, 0, 1 );
		v.pdfFwd = 0.3;
		v.cosAtGen = 0;		// sentinel
		v.isDelta = false;
		v.isConnectible = true;
		v.throughput = RISEPel( 2, 2, 2 );
		verts.push_back( v );
	}

	// v[2]: SURFACE (after the medium vertex)
	{
		BDPTVertex v;
		v.type = BDPTVertex::SURFACE;
		v.position = Point3( 3, 0, 0 );
		v.normal = Vector3( -1, 0, 0 );
		v.pdfFwd = 0.1;
		v.cosAtGen = 1.0;
		v.isDelta = false;
		v.isConnectible = true;
		v.throughput = RISEPel( 3, 3, 3 );
		verts.push_back( v );
	}

	const VCMNormalization norm = ComputeNormalization( 100, 100, 0.0, true, false );

	std::vector<LightVertex> out;
	VCMIntegrator::ConvertLightSubpath( verts, norm, out );

	// v[1] is a medium vertex with cosAtGen=0: the post-pass
	// should skip its recurrence and NOT emit a LightVertex.
	// v[2] should be emitted.
	Check( out.size() == 1, "medium: one LightVertex (skipping medium)" );
	if( out.size() >= 1 ) {
		Check( out[0].pathLength == 2, "medium: emitted record is v[2]" );
	}
}

//
// Test 5: a light-side BSSRDF entry ENDS the usable light subpath
// (DL-317).  A path through a subsurface jump is owned by the EYE-
// sampled jump -- the light-sampled one has no reverse jump density to
// be MIS-combined with it -- so nothing at or past the entry is stored
// for merging and its MIS entries stay zero.  The hit where the light
// went INTO the material is an ordinary arrival and is stored.
//
static void TestBSSRDFEntryAreaPdf()
{
	printf( "Test 5: a light-side BSSRDF entry ends the usable light subpath (DL-317)\n" );

	std::vector<BDPTVertex> verts;

	{
		BDPTVertex v;
		v.type = BDPTVertex::LIGHT;
		v.position = Point3( 0, 0, 0 );
		v.normal = Vector3( 1, 0, 0 );
		v.pdfFwd = 0.25;
		v.emissionPdfW = 0.125;
		v.cosAtGen = 1.0;
		v.isDelta = false;
		v.isConnectible = true;
		verts.push_back( v );
	}

	{
		BDPTVertex v;
		v.type = BDPTVertex::SURFACE;
		v.position = Point3( 1, 0, 0 );
		v.normal = Vector3( -1, 0, 0 );
		v.pdfFwd = 1.0;
		v.cosAtGen = 1.0;
		v.isDelta = true;
		v.isConnectible = true;
		v.throughput = RISEPel( 2, 2, 2 );
		verts.push_back( v );
	}

	{
		BDPTVertex v;
		v.type = BDPTVertex::SURFACE;
		v.position = Point3( 2, 0, 0 );
		v.normal = Vector3( -1, 0, 0 );
		v.pdfFwd = 0.5;
		v.cosAtGen = 0.0;
		v.isDelta = false;
		v.isConnectible = true;
		v.isBSSRDFEntry = true;
		v.throughput = RISEPel( 3, 3, 3 );
		verts.push_back( v );
	}

	{
		BDPTVertex v;
		v.type = BDPTVertex::SURFACE;
		v.position = Point3( 3, 0, 0 );
		v.normal = Vector3( -1, 0, 0 );
		v.pdfFwd = 0.25;
		v.cosAtGen = 1.0;
		v.isDelta = false;
		v.isConnectible = true;
		v.throughput = RISEPel( 4, 4, 4 );
		verts.push_back( v );
	}

	const VCMNormalization norm = ComputeNormalization( 100, 100, 0.0, true, false );

	std::vector<LightVertex> out;
	std::vector<VCMMisQuantities> outMis;
	VCMIntegrator::ConvertLightSubpath( verts, norm, out, &outMis );

	Check( out.size() == 1, "bssrdf: only the pre-entry hit is stored (DL-317)" );
	if( out.size() >= 1 ) {
		Check( out[0].pathLength == 1, "bssrdf: the stored vertex is the hit where the light went in" );
	}
	Check( outMis.size() == 4, "bssrdf: MIS array parallel to input" );
	if( outMis.size() != 4 ) {
		return;
	}
	// Pre-DL-317 these read dVCM = 1/pdfSurface = 2 at the entry and
	// 1/p_w = 4 one vertex later, and the post-entry vertex was stored.
	CheckClose( outMis[2].dVCM, 0.0, 1e-15, "bssrdf: no MIS state at the entry (DL-317)" );
	CheckClose( outMis[2].dVC, 0.0, 1e-15, "bssrdf: dVC zero at the entry" );
	CheckClose( outMis[2].dVM, 0.0, 1e-15, "bssrdf: dVM zero at the entry" );
	CheckClose( outMis[3].dVCM, 0.0, 1e-15, "bssrdf: nothing computed past the entry (DL-317)" );
	CheckClose( outMis[3].dVC, 0.0, 1e-15, "bssrdf: dVC zero past the entry" );
	CheckClose( outMis[3].dVM, 0.0, 1e-15, "bssrdf: dVM zero past the entry" );
}

//
// Test 6 (DL-317 review P1): a light-side jump the EYE family cannot
// cover is KEPT.  A delta (point) light feeding a random-walk entry: the
// eye family would arrive at the entry's point, jump to the light's hit,
// and find no NEE / connection / merge there (non-connectible) and no
// s=0 (delta light), so this light-sampled path is the only estimator.
// The same light feeding a DIFFUSION entry is covered (NEE at the eye's
// entry) and cut, as is a random-walk entry under a non-delta root (s=0).
// DL-375: the generator now emits EVERY entry connectible (a random-walk
// entry prices Sw through RandomWalkEntryBSDF), so a point light feeding a
// random-walk entry has the `connectibleEntry = true` shape and is CUT --
// the eye family covers it through NEE at its entry.  The kept case below
// is the predicate's logic for a non-connectible entry (the pre-DL-375
// random-walk shape); a delta light behind a delta interface is the class
// that still keeps a light-side jump.
//
static std::vector<BDPTVertex> MakeJumpLightPath( const bool deltaRoot, const bool connectibleEntry )
{
	std::vector<BDPTVertex> verts;
	BDPTVertex v;
	v.type = BDPTVertex::LIGHT; v.position = Point3( 0, 0, 0 ); v.normal = Vector3( 1, 0, 0 );
	v.pdfFwd = 0.25; v.emissionPdfW = 0.125; v.cosAtGen = 1.0;
	v.isDelta = deltaRoot; v.isConnectible = !deltaRoot;
	verts.push_back( v );
	v = BDPTVertex();
	v.type = BDPTVertex::SURFACE; v.position = Point3( 1, 0, 0 ); v.normal = Vector3( -1, 0, 0 );
	v.pdfFwd = 1.0; v.cosAtGen = 1.0; v.isDelta = true; v.isConnectible = true;
	v.throughput = RISEPel( 2, 2, 2 );
	verts.push_back( v );
	v = BDPTVertex();
	v.type = BDPTVertex::SURFACE; v.position = Point3( 2, 0, 0 ); v.normal = Vector3( -1, 0, 0 );
	v.pdfFwd = connectibleEntry ? 0.5 : 0.0; v.cosAtGen = 0.0;
	v.isDelta = !connectibleEntry; v.isConnectible = connectibleEntry; v.isBSSRDFEntry = true;
	v.throughput = RISEPel( 3, 3, 3 );
	verts.push_back( v );
	v = BDPTVertex();
	v.type = BDPTVertex::SURFACE; v.position = Point3( 3, 0, 0 ); v.normal = Vector3( -1, 0, 0 );
	v.pdfFwd = 0.25; v.cosAtGen = 1.0; v.isDelta = false; v.isConnectible = true;
	v.throughput = RISEPel( 4, 4, 4 );
	verts.push_back( v );
	return verts;
}

static void TestUncoverableLightJumpKept()
{
	printf( "Test 6: a light-side jump the eye family cannot cover is kept (DL-317)\n" );
	const VCMNormalization norm = ComputeNormalization( 100, 100, 0.0, true, false );

	{
		std::vector<LightVertex> out;
		std::vector<VCMMisQuantities> outMis;
		VCMIntegrator::ConvertLightSubpath( MakeJumpLightPath( true, false ), norm, out, &outMis );
		Check( out.size() == 2, "point light -> random-walk entry: the vertex past the entry is stored (kept)" );
		if( out.size() == 2 ) {
			Check( out[1].pathLength == 3, "point light -> random-walk entry: stored past-entry vertex is index 3" );
			CheckClose( out[1].mis.dVCM, 0.0, 1e-15, "kept random-walk entry: no connection to it reserved" );
			CheckClose( out[1].mis.dVC, 0.0, 1e-15, "kept random-walk entry: dVC zero past it" );
			CheckClose( out[1].mis.dVM, 0.0, 1e-15, "kept random-walk entry: dVM zero past it" );
		}
	}
	{
		std::vector<LightVertex> out;
		std::vector<VCMMisQuantities> outMis;
		VCMIntegrator::ConvertLightSubpath( MakeJumpLightPath( true, true ), norm, out, &outMis );
		Check( out.size() == 1, "point light -> diffusion entry: covered by the eye's NEE, cut" );
	}
	{
		std::vector<LightVertex> out;
		std::vector<VCMMisQuantities> outMis;
		VCMIntegrator::ConvertLightSubpath( MakeJumpLightPath( false, false ), norm, out, &outMis );
		Check( out.size() == 1, "area light -> random-walk entry: covered by the eye's s=0, cut" );
	}
}

//////////////////////////////////////////////////////////////////////
// Main
//////////////////////////////////////////////////////////////////////
int main()
{
	printf( "=== VCM Light Post-Pass Unit Test ===\n" );

	TestTwoBounceDiffuseAllCosinesOne();
	TestDeltaVertexInMiddle();
	TestDegenerateInputs();
	TestMediumVertexNotEmitted();
	TestBSSRDFEntryAreaPdf();
	TestUncoverableLightJumpKept();

	printf( "\nPassed: %d\nFailed: %d\n", g_pass, g_fail );
	if( g_fail > 0 ) {
		printf( "*** TEST SUITE FAILED ***\n" );
		return 1;
	}
	printf( "All tests passed.\n" );
	return 0;
}
