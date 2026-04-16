//////////////////////////////////////////////////////////////////////
//
//  VCMEyePostPassTest.cpp - Unit tests for
//    VCMIntegrator::ConvertEyeSubpath.
//
//    Hand-builds a synthetic BDPTVertex array representing a
//    canonical 2-bounce eye subpath, runs the converter, and
//    asserts the emitted VCMMisQuantities match hand-computed
//    values from the SmallVCM recurrence.
//
//    All positions / normals are chosen so every cosine is 1.0
//    and all distances are integers.  Every field on BDPTVertex
//    is explicit, so the expected values depend only on the
//    recurrence formulas.
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
// Test 1: Canonical 2-bounce eye subpath on the +x axis.
//
//   v[0] = CAMERA at (0,0,0), normal (+x)
//   v[1] = SURFACE at (2,0,0), normal (-x)
//   v[2] = SURFACE at (5,0,0), normal (-x)
//
// Chosen BDPT fields:
//   v[0].pdfFwd                  = 1.0   (BDPT's camera convention)
//   v[0].emissionPdfW            = 2.0   (cameraPdfW)
//   v[0].cosAtGen                = 1.0
//   v[0].pdfRev                  = 0.3   (set retroactively by BDPT)
//   v[1].pdfFwd (area at v[1])   = 0.1
//   v[1].cosAtGen                = 1.0
//   v[1].pdfRev                  = 0.5   (unused — no v[3])
//   v[2].pdfFwd                  = 0.2
//   v[2].cosAtGen                = 1.0
//
// norm: W=100, H=100, r=0 => mLightSubPathCount=10000, VM factors 0.
//
static void TestTwoBounceDiffuseEye()
{
	printf( "Test 1: 2-bounce axis-aligned diffuse eye path\n" );

	std::vector<BDPTVertex> verts;

	// v[0] CAMERA
	{
		BDPTVertex v;
		v.type = BDPTVertex::CAMERA;
		v.position = Point3( 0, 0, 0 );
		v.normal = Vector3( 1, 0, 0 );
		v.pdfFwd = 1.0;
		v.pdfRev = 0.3;
		v.emissionPdfW = 2.0;
		v.cosAtGen = 1.0;
		v.isDelta = false;
		v.isConnectible = true;
		verts.push_back( v );
	}

	// v[1] SURFACE
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
		verts.push_back( v );
	}

	// v[2] SURFACE
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
		verts.push_back( v );
	}

	const VCMNormalization norm = ComputeNormalization( 100, 100, 0.0, true, false );

	std::vector<VCMMisQuantities> outMis;
	VCMIntegrator::ConvertEyeSubpath( verts, norm, outMis );

	Check( outMis.size() == 3, "size: parallel to input" );
	if( outMis.size() != 3 ) {
		return;
	}

	//
	// v[0] expected (InitCamera with cameraPdfW = 2.0):
	//   dVCM = mLightSubPathCount / cameraPdfW = 10000 / 2 = 5000
	//   dVC  = 0
	//   dVM  = 0
	//
	CheckClose( outMis[0].dVCM, 5000.0, 1e-9, "v[0] dVCM" );
	CheckClose( outMis[0].dVC,  0.0, 1e-15, "v[0] dVC" );
	CheckClose( outMis[0].dVM,  0.0, 1e-15, "v[0] dVM" );

	//
	// v[1] expected (geometric update with distSq=4, cos=1):
	//   dVCM : 5000 * 4 / 1 = 20000
	//   dVC  : 0 / 1 = 0
	//   dVM  : 0 / 1 = 0
	//
	CheckClose( outMis[1].dVCM, 20000.0, 1e-9, "v[1] dVCM" );
	CheckClose( outMis[1].dVC,  0.0, 1e-15, "v[1] dVC" );
	CheckClose( outMis[1].dVM,  0.0, 1e-15, "v[1] dVM" );

	//
	// v[2] expected:
	//   BSDF update at v[1] with:
	//     cosThetaOut   = 1
	//     bsdfDirPdfW   = 0.2 * 9 / 1 = 1.8
	//     bsdfRevPdfW   = 0.3 * 4 / 1 = 1.2
	//   factor = 1 / 1.8
	//   dVC  = (1/1.8) * (0*1.2 + 20000 + 0)      = 20000/1.8
	//   dVM  = (1/1.8) * (0*1.2 + 20000*0 + 1)    = 1/1.8
	//   dVCM = 1/1.8
	//   Geometric update at v[2] (distSq=9, cos=1):
	//     dVCM' = (1/1.8) * 9 = 5
	//     dVC'  = (20000/1.8) / 1 = 20000/1.8
	//     dVM'  = (1/1.8)  / 1 = 1/1.8
	//
	const Scalar expDVCM = 5.0;
	const Scalar expDVC  = 20000.0 / 1.8;
	const Scalar expDVM  = 1.0 / 1.8;
	CheckClose( outMis[2].dVCM, expDVCM, 1e-9, "v[2] dVCM" );
	CheckClose( outMis[2].dVC,  expDVC,  1e-9, "v[2] dVC" );
	CheckClose( outMis[2].dVM,  expDVM,  1e-9, "v[2] dVM" );
}

//
// Test 2: Specular vertex in the middle.  Non-emitting (no store),
// but the recurrence still zeros dVCM and scales dVC/dVM by
// cosThetaOut at the specular step.
//
static void TestSpecularInMiddle()
{
	printf( "Test 2: Specular vertex update\n" );

	std::vector<BDPTVertex> verts;

	// v[0] CAMERA
	{
		BDPTVertex v;
		v.type = BDPTVertex::CAMERA;
		v.position = Point3( 0, 0, 0 );
		v.normal = Vector3( 1, 0, 0 );
		v.pdfFwd = 1.0;
		v.pdfRev = 0.1;
		v.emissionPdfW = 1.0;
		v.cosAtGen = 1.0;
		v.isDelta = false;
		v.isConnectible = true;
		verts.push_back( v );
	}

	// v[1] SURFACE (specular)
	{
		BDPTVertex v;
		v.type = BDPTVertex::SURFACE;
		v.position = Point3( 1, 0, 0 );
		v.normal = Vector3( -1, 0, 0 );
		v.pdfFwd = 1.0;    // delta sample treated as 1
		v.pdfRev = 1.0;
		v.cosAtGen = 1.0;
		v.isDelta = true;
		v.isConnectible = false;
		verts.push_back( v );
	}

	// v[2] SURFACE (diffuse)
	{
		BDPTVertex v;
		v.type = BDPTVertex::SURFACE;
		v.position = Point3( 4, 0, 0 );
		v.normal = Vector3( -1, 0, 0 );
		v.pdfFwd = 0.5;
		v.pdfRev = 0;
		v.cosAtGen = 1.0;
		v.isDelta = false;
		v.isConnectible = true;
		verts.push_back( v );
	}

	const VCMNormalization norm = ComputeNormalization( 100, 100, 0.0, true, false );

	std::vector<VCMMisQuantities> outMis;
	VCMIntegrator::ConvertEyeSubpath( verts, norm, outMis );

	Check( outMis.size() == 3, "specular: size 3" );

	//
	// After init at camera:      dVCM = 10000/1 = 10000, dVC=0, dVM=0
	// Geometric at v[1] (distSq=1, cos=1):
	//   dVCM = 10000 * 1 / 1 = 10000
	//   dVC  = 0
	//   dVM  = 0
	//
	CheckClose( outMis[1].dVCM, 10000.0, 1e-9, "v[1] dVCM after geo" );
	CheckClose( outMis[1].dVC,  0.0, 1e-15, "v[1] dVC" );
	CheckClose( outMis[1].dVM,  0.0, 1e-15, "v[1] dVM" );

	//
	// BSDF update at v[1], specular branch:
	//   dVCM = 0
	//   dVC *= cosThetaOut = 1   -> stays 0
	//   dVM *= cosThetaOut       -> stays 0
	// Geometric update at v[2] (distSq=9, cos=1):
	//   dVCM = 0 * 9 / 1 = 0
	//   dVC  = 0 / 1 = 0
	//   dVM  = 0 / 1 = 0
	//
	CheckClose( outMis[2].dVCM, 0.0, 1e-15, "v[2] dVCM (specular collapsed)" );
	CheckClose( outMis[2].dVC,  0.0, 1e-15, "v[2] dVC" );
	CheckClose( outMis[2].dVM,  0.0, 1e-15, "v[2] dVM" );
}

//
// Test 3: Degenerate inputs.
//
static void TestDegenerateInputs()
{
	printf( "Test 3: Degenerate inputs\n" );

	const VCMNormalization norm = ComputeNormalization( 100, 100, 0.0, true, false );

	// Empty path: outMis stays empty.
	{
		std::vector<BDPTVertex> verts;
		std::vector<VCMMisQuantities> outMis;
		VCMIntegrator::ConvertEyeSubpath( verts, norm, outMis );
		Check( outMis.empty(), "empty path: no output" );
	}

	// Malformed: root not CAMERA.
	{
		std::vector<BDPTVertex> verts;
		BDPTVertex v;
		v.type = BDPTVertex::SURFACE;
		v.position = Point3( 0, 0, 0 );
		v.normal = Vector3( 1, 0, 0 );
		v.pdfFwd = 1.0;
		v.cosAtGen = 1.0;
		verts.push_back( v );
		std::vector<VCMMisQuantities> outMis;
		VCMIntegrator::ConvertEyeSubpath( verts, norm, outMis );
		Check( outMis.empty(), "malformed root: cleared" );
	}

	// Single camera vertex: InitCamera fills outMis[0], nothing else.
	{
		std::vector<BDPTVertex> verts;
		BDPTVertex v;
		v.type = BDPTVertex::CAMERA;
		v.position = Point3( 0, 0, 0 );
		v.normal = Vector3( 0, 0, 1 );
		v.pdfFwd = 1.0;
		v.emissionPdfW = 4.0;
		v.cosAtGen = 1.0;
		verts.push_back( v );

		std::vector<VCMMisQuantities> outMis;
		VCMIntegrator::ConvertEyeSubpath( verts, norm, outMis );
		Check( outMis.size() == 1, "lone camera: size 1" );
		if( outMis.size() == 1 ) {
			CheckClose( outMis[0].dVCM, 10000.0 / 4.0, 1e-9, "lone camera: dVCM" );
			CheckClose( outMis[0].dVC, 0.0, 1e-15, "lone camera: dVC" );
			CheckClose( outMis[0].dVM, 0.0, 1e-15, "lone camera: dVM" );
		}
	}
}

//
// Test 4: Parallel-array indexing is preserved even when a medium
// vertex sits in the middle of the subpath.
//
static void TestMediumPreservesIndex()
{
	printf( "Test 4: Medium vertex preserves indexing\n" );

	std::vector<BDPTVertex> verts;

	// v[0] CAMERA
	{
		BDPTVertex v;
		v.type = BDPTVertex::CAMERA;
		v.position = Point3( 0, 0, 0 );
		v.normal = Vector3( 1, 0, 0 );
		v.pdfFwd = 1.0;
		v.emissionPdfW = 1.0;
		v.cosAtGen = 1.0;
		verts.push_back( v );
	}

	// v[1] MEDIUM (cosAtGen = 0 sentinel)
	{
		BDPTVertex v;
		v.type = BDPTVertex::MEDIUM;
		v.position = Point3( 1, 0, 0 );
		v.normal = Vector3( 0, 0, 1 );
		v.pdfFwd = 0.3;
		v.cosAtGen = 0;
		v.isDelta = false;
		v.isConnectible = true;
		verts.push_back( v );
	}

	// v[2] SURFACE
	{
		BDPTVertex v;
		v.type = BDPTVertex::SURFACE;
		v.position = Point3( 3, 0, 0 );
		v.normal = Vector3( -1, 0, 0 );
		v.pdfFwd = 0.1;
		v.cosAtGen = 1.0;
		v.isDelta = false;
		v.isConnectible = true;
		verts.push_back( v );
	}

	const VCMNormalization norm = ComputeNormalization( 100, 100, 0.0, true, false );

	std::vector<VCMMisQuantities> outMis;
	VCMIntegrator::ConvertEyeSubpath( verts, norm, outMis );

	Check( outMis.size() == 3, "medium: size 3 (parallel to input)" );
	if( outMis.size() != 3 ) {
		return;
	}
	// outMis[1] is filled (carry-through of v[0] state — medium is a skip).
	// outMis[2] is the SURFACE entry — note that the geometric
	// update uses distance from v[1] (the medium vertex) not v[0],
	// because mkVector3 in ConvertEyeSubpath always steps from the
	// previous array entry regardless of its type.  This is the
	// documented approximation: medium traversal is not corrected.
	// The TEST just asserts that indexing is preserved and no
	// NaNs leak — we don't assert exact numeric values at v[2]
	// across a medium vertex.
	Check( outMis[0].dVCM == outMis[1].dVCM, "medium: dVCM carries through" );
	Check( outMis[0].dVC == outMis[1].dVC, "medium: dVC carries through" );
}

//////////////////////////////////////////////////////////////////////
// Main
//////////////////////////////////////////////////////////////////////
int main()
{
	printf( "=== VCM Eye Post-Pass Unit Test ===\n" );

	TestTwoBounceDiffuseEye();
	TestSpecularInMiddle();
	TestDegenerateInputs();
	TestMediumPreservesIndex();

	printf( "\nPassed: %d\nFailed: %d\n", g_pass, g_fail );
	if( g_fail > 0 ) {
		printf( "*** TEST SUITE FAILED ***\n" );
		return 1;
	}
	printf( "All tests passed.\n" );
	return 0;
}
