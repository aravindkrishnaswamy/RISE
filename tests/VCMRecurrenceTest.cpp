//////////////////////////////////////////////////////////////////////
//
//  VCMRecurrenceTest.cpp - Unit tests for the pure-math VCM MIS
//    recurrence module (src/Library/Shaders/VCMRecurrence.h).
//
//    Every formula is validated against its hand-computed expected
//    value from Georgiev's SmallVCM (vertexcm.hxx), using the
//    balance heuristic (Mis(x) = x).  Tests cover:
//
//      1. ComputeNormalization scaling (W, H, radius, flags)
//      2. InitLight:  finite area + delta + infinite cases
//      3. InitCamera: dVCM only
//      4. ApplyGeometricUpdate: distSq gate + cosine divide
//      5. ApplyBsdfSamplingUpdate: specular vs non-specular
//      6. End-to-end: synthetic 2-bounce light path reaches a hand-
//         computed final (dVCM, dVC, dVM) after init + geometric
//         update + BSDF sampling update
//      7. Numerical edge cases: zero radius, VC/VM off, zero pdfs
//
//    There are NO scene / sampler / renderer dependencies; the
//    module is exercised in isolation against canonical inputs.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "../src/Library/Shaders/VCMRecurrence.h"

using namespace RISE;
using namespace RISE::Implementation;

static int g_pass = 0;
static int g_fail = 0;

static inline bool ApproxEqual( Scalar a, Scalar b, Scalar tol = 1e-12 )
{
	const Scalar d = a - b;
	return ( d < 0 ? -d : d ) <= tol;
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

static void CheckClose( Scalar actual, Scalar expected, Scalar tol, const char* label )
{
	if( ApproxEqual( actual, expected, tol ) ) {
		g_pass++;
	} else {
		g_fail++;
		printf( "  FAIL: %s  (expected %.15g, got %.15g)\n", label, (double)expected, (double)actual );
	}
}

// Same PI that VCMRecurrence uses.
static const Scalar kPI = PI;

//////////////////////////////////////////////////////////////////////
// Test 1: ComputeNormalization scaling
//////////////////////////////////////////////////////////////////////
static void TestComputeNormalization()
{
	printf( "Test 1: ComputeNormalization\n" );

	// Canonical case: 1920x1080, radius 0.05, VC+VM both on.
	{
		const unsigned int W = 1920;
		const unsigned int H = 1080;
		const Scalar r = 0.05;
		const VCMNormalization n = ComputeNormalization( W, H, r, true, true );

		const Scalar expCount = Scalar( W ) * Scalar( H );
		CheckClose( n.mLightSubPathCount, expCount, 1e-9,
			"canonical: mLightSubPathCount = W*H" );

		const Scalar expEta = kPI * r * r * expCount;
		CheckClose( n.mMisVmWeightFactor, expEta, 1e-3,
			"canonical: mMisVmWeightFactor = pi*r^2*count" );

		CheckClose( n.mMisVcWeightFactor, Scalar( 1 ) / expEta, 1e-15,
			"canonical: mMisVcWeightFactor = 1/etaVCM" );

		CheckClose( n.mVmNormalization, Scalar( 1 ) / expEta, 1e-15,
			"canonical: mVmNormalization = 1/etaVCM" );

		CheckClose( n.mMergeRadius, r, 1e-15, "canonical: mMergeRadius" );
		CheckClose( n.mMergeRadiusSq, r * r, 1e-15, "canonical: mMergeRadiusSq" );
		Check( n.mEnableVC && n.mEnableVM, "canonical: flags set" );

		// Reciprocity sanity: mMisVmWeightFactor * mMisVcWeightFactor == 1
		CheckClose( n.mMisVmWeightFactor * n.mMisVcWeightFactor, 1.0, 1e-9,
			"canonical: vm*vc = 1" );
	}

	// VM off: mMisVmWeightFactor must be zero, mMisVcWeightFactor
	// still valid so a pure-VC run behaves like power-BDPT.
	{
		const VCMNormalization n = ComputeNormalization( 640, 480, 0.02, true, false );
		CheckClose( n.mMisVmWeightFactor, 0.0, 1e-15, "VM off: mMisVmWeightFactor = 0" );
		Check( n.mMisVcWeightFactor > 0, "VM off: mMisVcWeightFactor > 0" );
	}

	// VC off: mMisVcWeightFactor must be zero.
	{
		const VCMNormalization n = ComputeNormalization( 640, 480, 0.02, false, true );
		Check( n.mMisVmWeightFactor > 0, "VC off: mMisVmWeightFactor > 0" );
		CheckClose( n.mMisVcWeightFactor, 0.0, 1e-15, "VC off: mMisVcWeightFactor = 0" );
	}

	// Zero radius: every merging-related factor collapses.
	{
		const VCMNormalization n = ComputeNormalization( 640, 480, 0.0, true, true );
		CheckClose( n.mMisVmWeightFactor, 0.0, 1e-15, "r=0: mMisVmWeightFactor = 0" );
		CheckClose( n.mMisVcWeightFactor, 0.0, 1e-15, "r=0: mMisVcWeightFactor = 0" );
		CheckClose( n.mVmNormalization,   0.0, 1e-15, "r=0: mVmNormalization  = 0" );
	}

	// Zero image size: count = 0, every derived factor = 0.
	{
		const VCMNormalization n = ComputeNormalization( 0, 0, 0.05, true, true );
		CheckClose( n.mLightSubPathCount, 0.0, 1e-15, "W=H=0: count = 0" );
		CheckClose( n.mMisVmWeightFactor, 0.0, 1e-15, "W=H=0: vm = 0" );
		CheckClose( n.mMisVcWeightFactor, 0.0, 1e-15, "W=H=0: vc = 0" );
	}
}

//////////////////////////////////////////////////////////////////////
// Test 2: InitLight
//////////////////////////////////////////////////////////////////////
static void TestInitLight()
{
	printf( "Test 2: InitLight\n" );

	const VCMNormalization norm = ComputeNormalization( 100, 100, 0.1, true, true );

	// Finite area light, non-delta.  Pick round numbers so the
	// expected ratios are exact.
	//   directPdfA   = 0.25  (= 1/area for a 2x2 light)
	//   emissionPdfW = 0.05  (= directPdfA * cos * 1/pi with cos=0.628... rounded)
	//   cosLight     = 0.7
	{
		const Scalar directPdfA = 0.25;
		const Scalar emissionPdfW = 0.05;
		const Scalar cosLight = 0.7;
		const VCMMisQuantities q = InitLight( directPdfA, emissionPdfW, cosLight, true, false, norm );

		CheckClose( q.dVCM, directPdfA / emissionPdfW, 1e-15, "finite: dVCM = directPdfA/emissionPdfW" );
		CheckClose( q.dVC,  cosLight   / emissionPdfW, 1e-15, "finite: dVC = cosLight/emissionPdfW" );
		CheckClose( q.dVM,  q.dVC * norm.mMisVcWeightFactor, 1e-15, "finite: dVM = dVC*mMisVcWeightFactor" );
	}

	// Delta light (point light): dVC must be zero.
	{
		const Scalar directPdfA = 1.0;   // delta lights have "infinite" area pdf — use 1 as a normalized proxy
		const Scalar emissionPdfW = 0.5;
		const VCMMisQuantities q = InitLight( directPdfA, emissionPdfW, 1.0, true, true, norm );

		CheckClose( q.dVCM, directPdfA / emissionPdfW, 1e-15, "delta: dVCM = directPdfA/emissionPdfW" );
		CheckClose( q.dVC,  0.0, 1e-15, "delta: dVC = 0" );
		CheckClose( q.dVM,  0.0, 1e-15, "delta: dVM = 0" );
	}

	// Infinite light (env map): usedCosLight collapses to 1 and
	// dVC = 1 / emissionPdfW.
	{
		const Scalar directPdfA = 0.1;
		const Scalar emissionPdfW = 0.2;
		const VCMMisQuantities q = InitLight( directPdfA, emissionPdfW, 0.42, false, false, norm );

		CheckClose( q.dVCM, directPdfA / emissionPdfW, 1e-15, "infinite: dVCM" );
		CheckClose( q.dVC,  1.0        / emissionPdfW, 1e-15, "infinite: dVC = 1/emissionPdfW" );
		CheckClose( q.dVM,  q.dVC * norm.mMisVcWeightFactor, 1e-15, "infinite: dVM = dVC*mMisVcWeightFactor" );
	}

	// Degenerate: emissionPdfW = 0 must not produce NaN / Inf.
	{
		const VCMMisQuantities q = InitLight( 0.25, 0.0, 0.7, true, false, norm );
		CheckClose( q.dVCM, 0.0, 1e-15, "emissionPdfW=0: dVCM = 0" );
		CheckClose( q.dVC,  0.0, 1e-15, "emissionPdfW=0: dVC = 0" );
		CheckClose( q.dVM,  0.0, 1e-15, "emissionPdfW=0: dVM = 0" );
	}
}

//////////////////////////////////////////////////////////////////////
// Test 3: InitCamera
//////////////////////////////////////////////////////////////////////
static void TestInitCamera()
{
	printf( "Test 3: InitCamera\n" );

	const VCMNormalization norm = ComputeNormalization( 1920, 1080, 0.05, true, true );

	{
		const Scalar cameraPdfW = 2.5;
		const VCMMisQuantities q = InitCamera( cameraPdfW, norm );
		CheckClose( q.dVCM, Scalar( 1920 * 1080 ) / cameraPdfW, 1e-9, "dVCM = count/cameraPdfW" );
		CheckClose( q.dVC, 0.0, 1e-15, "dVC = 0" );
		CheckClose( q.dVM, 0.0, 1e-15, "dVM = 0" );
	}

	// Degenerate camera pdf: no NaN.
	{
		const VCMMisQuantities q = InitCamera( 0.0, norm );
		CheckClose( q.dVCM, 0.0, 1e-15, "cameraPdfW=0: dVCM = 0" );
		CheckClose( q.dVC, 0.0, 1e-15, "cameraPdfW=0: dVC = 0" );
		CheckClose( q.dVM, 0.0, 1e-15, "cameraPdfW=0: dVM = 0" );
	}
}

//////////////////////////////////////////////////////////////////////
// Test 4: ApplyGeometricUpdate
//////////////////////////////////////////////////////////////////////
static void TestApplyGeometricUpdate()
{
	printf( "Test 4: ApplyGeometricUpdate\n" );

	// Canonical synthetic state: dVCM=2, dVC=3, dVM=5.  After the
	// update all three should divide by cos and dVCM should
	// additionally pick up a factor of distSq when the gate is true.
	VCMMisQuantities q;
	q.dVCM = 2.0;
	q.dVC  = 3.0;
	q.dVM  = 5.0;

	// Case A: gate on (normal bounce).  distSq=4, cos=0.5.
	{
		const VCMMisQuantities r = ApplyGeometricUpdate( q, 4.0, 0.5, true );
		CheckClose( r.dVCM, 2.0 * 4.0 / 0.5, 1e-15, "gate on: dVCM *= distSq / cos" );
		CheckClose( r.dVC,  3.0        / 0.5, 1e-15, "gate on: dVC /= cos" );
		CheckClose( r.dVM,  5.0        / 0.5, 1e-15, "gate on: dVM /= cos" );
	}

	// Case B: gate off (first bounce from infinite light).  dVCM
	// must NOT pick up the distSq factor.
	{
		const VCMMisQuantities r = ApplyGeometricUpdate( q, 4.0, 0.5, false );
		CheckClose( r.dVCM, 2.0 / 0.5, 1e-15, "gate off: dVCM /= cos only" );
		CheckClose( r.dVC,  3.0 / 0.5, 1e-15, "gate off: dVC /= cos" );
		CheckClose( r.dVM,  5.0 / 0.5, 1e-15, "gate off: dVM /= cos" );
	}

	// Case C: degenerate grazing cosine clamps to zero, no div-by-zero.
	{
		const VCMMisQuantities r = ApplyGeometricUpdate( q, 4.0, 0.0, true );
		CheckClose( r.dVCM, 0.0, 1e-15, "cos=0: dVCM = 0" );
		CheckClose( r.dVC,  0.0, 1e-15, "cos=0: dVC = 0" );
		CheckClose( r.dVM,  0.0, 1e-15, "cos=0: dVM = 0" );
	}
}

//////////////////////////////////////////////////////////////////////
// Test 5: ApplyBsdfSamplingUpdate
//////////////////////////////////////////////////////////////////////
static void TestApplyBsdfSamplingUpdate()
{
	printf( "Test 5: ApplyBsdfSamplingUpdate\n" );

	const VCMNormalization norm = ComputeNormalization( 400, 300, 0.1, true, true );

	VCMMisQuantities q;
	q.dVCM = 2.0;
	q.dVC  = 3.0;
	q.dVM  = 5.0;

	// Non-specular branch.  Pick tidy inputs.
	//   cosThetaOut = 0.5
	//   bsdfDirPdfW = 0.25  -> factor = 0.5 / 0.25 = 2
	//   bsdfRevPdfW = 0.1
	{
		const Scalar cosOut = 0.5;
		const Scalar pFwd = 0.25;
		const Scalar pRev = 0.1;
		const VCMMisQuantities r = ApplyBsdfSamplingUpdate( q, cosOut, pFwd, pRev, false, norm );

		const Scalar factor = cosOut / pFwd;
		const Scalar expDVC = factor * ( 3.0 * pRev + 2.0 + norm.mMisVmWeightFactor );
		const Scalar expDVM = factor * ( 5.0 * pRev + 2.0 * norm.mMisVcWeightFactor + 1.0 );
		const Scalar expDVCM = 1.0 / pFwd;

		CheckClose( r.dVC,  expDVC,  1e-9, "non-specular: dVC" );
		CheckClose( r.dVM,  expDVM,  1e-9, "non-specular: dVM" );
		CheckClose( r.dVCM, expDVCM, 1e-15, "non-specular: dVCM = 1/bsdfDirPdfW" );
	}

	// Specular branch: dVCM zeros, dVC/dVM multiplied by cos only.
	// Choose pFwd != pRev to prove that the specular branch does
	// NOT read them (if the branch were wrong, the test would
	// produce a different value).
	{
		const Scalar cosOut = 0.8;
		const VCMMisQuantities r = ApplyBsdfSamplingUpdate( q, cosOut, 0.5, 0.25, true, norm );
		CheckClose( r.dVCM, 0.0, 1e-15, "specular: dVCM = 0" );
		CheckClose( r.dVC,  3.0 * cosOut, 1e-15, "specular: dVC *= cos" );
		CheckClose( r.dVM,  5.0 * cosOut, 1e-15, "specular: dVM *= cos" );
	}

	// Degenerate bsdfDirPdfW = 0 kills the chain.
	{
		const VCMMisQuantities r = ApplyBsdfSamplingUpdate( q, 0.5, 0.0, 0.1, false, norm );
		CheckClose( r.dVCM, 0.0, 1e-15, "pFwd=0: dVCM = 0" );
		CheckClose( r.dVC,  0.0, 1e-15, "pFwd=0: dVC = 0" );
		CheckClose( r.dVM,  0.0, 1e-15, "pFwd=0: dVM = 0" );
	}

	// Order-of-assignment check.  In the non-specular branch,
	// dVC and dVM must read the OLD dVCM.  If the implementation
	// mistakenly assigned dVCM first, dVC would use 1/bsdfDirPdfW
	// instead of the incoming q.dVCM.  The below inputs make those
	// two outcomes visibly different.
	{
		VCMMisQuantities qTest;
		qTest.dVCM = 10.0;       // old dVCM
		qTest.dVC  = 0.0;
		qTest.dVM  = 0.0;
		const Scalar cosOut = 1.0;
		const Scalar pFwd = 0.5; // would give 1/pFwd = 2.0, very different from 10.0
		const Scalar pRev = 0.0; // cancels the qTest.dVC*pRev term
		const VCMNormalization n0 = ComputeNormalization( 100, 100, 0.0, true, true );
		const VCMMisQuantities r = ApplyBsdfSamplingUpdate( qTest, cosOut, pFwd, pRev, false, n0 );
		//   factor = 1 / 0.5 = 2
		//   dVC = 2 * (0 * 0 + 10 + 0) = 20   <-- reads OLD dVCM (= 10)
		//   dVM = 2 * (0 * 0 + 10*0 + 1) = 2
		//   dVCM = 1 / 0.5 = 2
		CheckClose( r.dVC,  20.0, 1e-15, "order: dVC reads OLD dVCM" );
		CheckClose( r.dVM,  2.0,  1e-15, "order: dVM reads OLD dVCM" );
		CheckClose( r.dVCM, 2.0,  1e-15, "order: new dVCM = 1/pFwd" );
	}
}

//////////////////////////////////////////////////////////////////////
// Test 6: End-to-end synthetic light path
//
// Trace a 2-bounce light path through the recurrence and compare
// against hand-computed expected values.  All inputs are chosen so
// the arithmetic is tractable.
//
// Init at a finite area light:
//   directPdfA = 0.25, emissionPdfW = 0.05, cosLight = 0.5
// First bounce lands on a non-specular surface at distance^2=1 with
//   cosThetaFix = 0.5, sampled BSDF with
//   cosThetaOut = 0.5, bsdfDirPdfW = 0.5, bsdfRevPdfW = 0.25.
// Merging disabled (radius=0) to keep the arithmetic simple.
//////////////////////////////////////////////////////////////////////
static void TestEndToEndSyntheticPath()
{
	printf( "Test 6: End-to-end synthetic light path\n" );

	// Radius 0 => mMisVmWeightFactor = 0, mMisVcWeightFactor = 0.
	// The recurrence collapses to pure-VC (classic BDPT math).
	const VCMNormalization norm = ComputeNormalization( 100, 100, 0.0, true, false );
	CheckClose( norm.mMisVmWeightFactor, 0.0, 1e-15, "sanity: mMisVmWeightFactor=0 with r=0" );
	CheckClose( norm.mMisVcWeightFactor, 0.0, 1e-15, "sanity: mMisVcWeightFactor=0 with r=0" );

	// Init at light.
	VCMMisQuantities q = InitLight( 0.25, 0.05, 0.5, true, false, norm );
	CheckClose( q.dVCM, 5.0, 1e-15, "init: dVCM = 0.25/0.05 = 5" );
	CheckClose( q.dVC,  10.0, 1e-15, "init: dVC = 0.5/0.05 = 10" );
	CheckClose( q.dVM,  0.0, 1e-15, "init: dVM = 0 (r=0)" );

	// Geometric update at vertex 1.  This IS the first bounce from
	// the light, so applyDistSqToDVCM is true (light is finite).
	q = ApplyGeometricUpdate( q, 1.0, 0.5, true );
	CheckClose( q.dVCM, 5.0 * 1.0 / 0.5, 1e-15, "geo: dVCM = 5*1/0.5 = 10" );
	CheckClose( q.dVC,  10.0       / 0.5, 1e-15, "geo: dVC = 10/0.5 = 20" );
	CheckClose( q.dVM,  0.0,              1e-15, "geo: dVM stays 0" );

	// BSDF sample at vertex 1 (non-specular).  factor = 0.5/0.5 = 1.
	//   dVC = 1*(20*0.25 + 10 + 0) = 15
	//   dVM = 1*(0*0.25  + 10*0 + 1) = 1
	//   dVCM = 1/0.5 = 2
	q = ApplyBsdfSamplingUpdate( q, 0.5, 0.5, 0.25, false, norm );
	CheckClose( q.dVCM, 2.0,  1e-15, "bsdf: dVCM = 2" );
	CheckClose( q.dVC,  15.0, 1e-15, "bsdf: dVC = 15" );
	CheckClose( q.dVM,  1.0,  1e-15, "bsdf: dVM = 1 (even though merging off)" );

	// One more bounce: geometric update on vertex 2 with distSq=0.64, cos=0.8.
	// Gate is always true for non-first bounces.
	q = ApplyGeometricUpdate( q, 0.64, 0.8, true );
	CheckClose( q.dVCM, 2.0 * 0.64 / 0.8, 1e-15, "geo2: dVCM" );
	CheckClose( q.dVC,  15.0       / 0.8, 1e-15, "geo2: dVC" );
	CheckClose( q.dVM,  1.0        / 0.8, 1e-15, "geo2: dVM" );
}

//////////////////////////////////////////////////////////////////////
// Test 7: Specular chain does not read mMisVmWeightFactor
//
// A specular-only chain must be invariant to the normalization's
// merging factor because dVCM collapses to zero on each bounce.
//////////////////////////////////////////////////////////////////////
static void TestSpecularChainIndependence()
{
	printf( "Test 7: Specular chain independence\n" );

	const VCMNormalization normA = ComputeNormalization( 100, 100, 0.05, true, true );
	const VCMNormalization normB = ComputeNormalization( 100, 100, 0.5,  true, true );

	VCMMisQuantities q;
	q.dVCM = 1.0;
	q.dVC  = 2.0;
	q.dVM  = 3.0;

	VCMMisQuantities a = q;
	a = ApplyBsdfSamplingUpdate( a, 0.7, 1.0, 1.0, true, normA );
	a = ApplyBsdfSamplingUpdate( a, 0.6, 1.0, 1.0, true, normA );

	VCMMisQuantities b = q;
	b = ApplyBsdfSamplingUpdate( b, 0.7, 1.0, 1.0, true, normB );
	b = ApplyBsdfSamplingUpdate( b, 0.6, 1.0, 1.0, true, normB );

	CheckClose( a.dVCM, b.dVCM, 1e-15, "specular chain: dVCM same under different VM factors" );
	CheckClose( a.dVC,  b.dVC,  1e-15, "specular chain: dVC  same under different VM factors" );
	CheckClose( a.dVM,  b.dVM,  1e-15, "specular chain: dVM  same under different VM factors" );
	CheckClose( a.dVCM, 0.0, 1e-15, "specular chain: dVCM = 0" );
}

//////////////////////////////////////////////////////////////////////
// Main
//////////////////////////////////////////////////////////////////////
int main()
{
	printf( "=== VCMRecurrence Unit Test ===\n" );

	TestComputeNormalization();
	TestInitLight();
	TestInitCamera();
	TestApplyGeometricUpdate();
	TestApplyBsdfSamplingUpdate();
	TestEndToEndSyntheticPath();
	TestSpecularChainIndependence();

	printf( "\nPassed: %d\nFailed: %d\n", g_pass, g_fail );
	if( g_fail > 0 ) {
		printf( "*** TEST SUITE FAILED ***\n" );
		return 1;
	}
	printf( "All tests passed.\n" );
	return 0;
}
