//////////////////////////////////////////////////////////////////////
//
//  GGXWhiteFurnaceTest.cpp - Validates anisotropic GGX math and
//    the GGXBRDF/GGXSPF material implementations:
//
//    1. NDF normalization: integral of D_aniso(h)*cos(h) = 1
//    2. VNDF PDF cross-validation: BRDF*cos/pdf == G2/G1(wi)
//    3. Energy conservation: E_ss = mean(G2/G1) via VNDF <= 1
//    4. Isotropic consistency: aniso(a,a) matches iso(a)
//    5. Height-correlated vs separable: G2 >= G1*G1 - epsilon
//    6. Material pointwise: GGXSPF.Scatter().pdf == GGXSPF.Pdf()
//
//  Build (from project root):
//    c++ -arch arm64 -Isrc/Library -I/opt/homebrew/include \
//        -O3 -ffast-math -funroll-loops -Wall -pedantic \
//        -Wno-c++11-long-long -DCOLORS_RGB -DMERSENNE53 \
//        -DNO_TIFF_SUPPORT -DNO_EXR_SUPPORT -DRISE_ENABLE_MAILBOXING \
//        -c tests/GGXWhiteFurnaceTest.cpp -o tests/GGXWhiteFurnaceTest.o
//    c++ -arch arm64 -o tests/ggx_furnace_test tests/GGXWhiteFurnaceTest.o \
//        bin/librise.a -L/opt/homebrew/lib -lpng -lz
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <cstdlib>
#include <cassert>
#include <iomanip>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Utilities/MicrofacetUtils.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Materials/GGXBRDF.h"
#include "../src/Library/Materials/GGXSPF.h"

#include "TestStubObject.h"

using namespace RISE;
using namespace RISE::Implementation;

// ============================================================
//  Configuration
// ============================================================

static const int NDF_QUAD_THETA  = 200;
static const int NDF_QUAD_PHI    = 400;
static const double NDF_TOL      = 0.02;     // 2% tolerance for NDF normalization

static const int VNDF_SAMPLES    = 500000;

static const int ESS_SAMPLES     = 200000;
static const double ESS_TOL      = 0.02;     // E_ss must be <= 1.0 + tolerance

static const int ISO_SAMPLES     = 100000;
static const double ISO_TOL      = 0.03;     // 3% tolerance for iso vs aniso comparison

static const int G2_SAMPLES      = 100000;

// ============================================================
//  Stub object for IOR stack operations
// ============================================================

static StubObject* g_stubObject = 0;

// ============================================================
//  Test 1: Anisotropic NDF normalization
//  integral_hemisphere D(h) * cos(theta_h) dw_h = 1
// ============================================================

static bool TestNDFNormalization(
	Scalar alpha_x,
	Scalar alpha_y
	)
{
	double sum = 0;
	const double dTheta = PI_OV_TWO / NDF_QUAD_THETA;
	const double dPhi   = TWO_PI / NDF_QUAD_PHI;

	for( int t = 0; t < NDF_QUAD_THETA; t++ )
	{
		const double theta = (t + 0.5) * dTheta;
		const double sinTheta = sin( theta );
		const double cosTheta = cos( theta );

		for( int p = 0; p < NDF_QUAD_PHI; p++ )
		{
			const double phi = (p + 0.5) * dPhi;
			const Vector3 h_local(
				sinTheta * cos(phi),
				sinTheta * sin(phi),
				cosTheta
			);

			const double D = MicrofacetUtils::GGX_D_Aniso<Scalar>( alpha_x, alpha_y, h_local );
			sum += D * cosTheta * sinTheta * dTheta * dPhi;
		}
	}

	const double err = fabs( sum - 1.0 );
	std::cout << "  NDF norm (ax=" << alpha_x << ", ay=" << alpha_y << "): "
	          << std::fixed << std::setprecision(6) << sum
	          << "  err=" << err;

	if( err > NDF_TOL ) {
		std::cout << "  FAIL" << std::endl;
		return false;
	}
	std::cout << "  OK" << std::endl;
	return true;
}

// ============================================================
//  Test 2: VNDF PDF pointwise cross-validation
//  For each VNDF-sampled direction, verify that:
//    BRDF_spec * cosWo / pdf  ==  G2 / G1(wi)
//  where BRDF_spec = D * G2 / (4 * cosWi * cosWo) with F=1.
//  Both expressions compute the same quantity analytically;
//  agreement validates that VNDF_Pdf_Aniso is consistent with
//  the sampling density of VNDF_Sample_Aniso.
// ============================================================

static bool TestVNDFCrossValidation(
	Scalar alpha_x,
	Scalar alpha_y,
	Scalar incomingTheta
	)
{
	OrthonormalBasis3D onb;
	onb.CreateFromW( Vector3(0,0,1) );

	const Scalar sinT = sin( incomingTheta );
	const Scalar cosT = cos( incomingTheta );
	const Vector3 wi( sinT, 0, cosT );
	const Scalar cosWi = cosT;

	RandomNumberGenerator rng;
	int validCount = 0;
	int mismatchCount = 0;
	double maxRelErr = 0;

	for( int i = 0; i < VNDF_SAMPLES; i++ )
	{
		const Scalar u1 = rng.CanonicalRandom();
		const Scalar u2 = rng.CanonicalRandom();
		const Vector3 m = MicrofacetUtils::VNDF_Sample_Aniso( wi, onb, alpha_x, alpha_y, u1, u2 );

		const Scalar wiDotM = Vector3Ops::Dot( wi, m );
		if( wiDotM <= 0 ) continue;

		const Vector3 wo = Vector3Ops::Normalize( m * (2.0 * wiDotM) - wi );
		const Scalar cosWo = Vector3Ops::Dot( wo, onb.w() );
		if( cosWo <= 0 ) continue;

		const Vector3 wi_local(
			Vector3Ops::Dot( wi, onb.u() ),
			Vector3Ops::Dot( wi, onb.v() ),
			Vector3Ops::Dot( wi, onb.w() )
		);
		const Vector3 wo_local(
			Vector3Ops::Dot( wo, onb.u() ),
			Vector3Ops::Dot( wo, onb.v() ),
			Vector3Ops::Dot( wo, onb.w() )
		);
		const Vector3 h_local(
			Vector3Ops::Dot( m, onb.u() ),
			Vector3Ops::Dot( m, onb.v() ),
			Vector3Ops::Dot( m, onb.w() )
		);

		const Scalar D = MicrofacetUtils::GGX_D_Aniso<Scalar>( alpha_x, alpha_y, h_local );
		const Scalar G2 = MicrofacetUtils::GGX_G2_Aniso( alpha_x, alpha_y, wi_local, wo_local );
		const Scalar G1wi = MicrofacetUtils::GGX_G1_Aniso( alpha_x, alpha_y, wi_local );
		const Scalar pdf = MicrofacetUtils::VNDF_Pdf_Aniso( wi, wo, onb, alpha_x, alpha_y );

		if( pdf < 1e-20 || G1wi < 1e-10 || cosWi < 1e-10 ) continue;

		// BRDF_spec with F=1: D * G2 / (4 * cosWi * cosWo)
		const Scalar brdfVal = D * G2 / (4.0 * cosWi * cosWo);

		// Two equivalent expressions for the importance weight:
		const Scalar weight_analytic = G2 / G1wi;
		const Scalar weight_brdfpdf = brdfVal * cosWo / pdf;

		const Scalar denom = (weight_analytic > 1e-10) ? weight_analytic : 1.0;
		const Scalar relErr = fabs( weight_analytic - weight_brdfpdf ) / denom;

		if( relErr > maxRelErr ) maxRelErr = relErr;

		if( relErr > 1e-4 ) {
			mismatchCount++;
		}

		validCount++;
	}

	std::cout << "  VNDF cross-val (ax=" << alpha_x << ", ay=" << alpha_y
	          << ", theta=" << std::fixed << std::setprecision(1) << (incomingTheta * 180.0 / PI)
	          << "): maxErr=" << std::setprecision(8) << maxRelErr
	          << " mismatches=" << mismatchCount << "/" << validCount;

	if( mismatchCount > validCount / 1000 ) {
		std::cout << "  FAIL" << std::endl;
		return false;
	}
	std::cout << "  OK" << std::endl;
	return true;
}

// ============================================================
//  Test 3: Energy conservation (E_ss via VNDF sampling)
//  E_ss = (1/N) * sum[ G2(wi,wo) / G1(wi) ] <= 1.0
// ============================================================

static bool TestEnergyConservation(
	Scalar alpha_x,
	Scalar alpha_y,
	Scalar incomingTheta
	)
{
	OrthonormalBasis3D onb;
	onb.CreateFromW( Vector3(0,0,1) );

	const Scalar sinT = sin( incomingTheta );
	const Scalar cosT = cos( incomingTheta );
	const Vector3 wi( sinT, 0, cosT );

	RandomNumberGenerator rng;
	double sum = 0;
	int validCount = 0;

	for( int i = 0; i < ESS_SAMPLES; i++ )
	{
		const Scalar u1 = rng.CanonicalRandom();
		const Scalar u2 = rng.CanonicalRandom();
		const Vector3 m = MicrofacetUtils::VNDF_Sample_Aniso( wi, onb, alpha_x, alpha_y, u1, u2 );

		const Scalar wiDotM = Vector3Ops::Dot( wi, m );
		if( wiDotM <= 0 ) continue;

		const Vector3 wo = Vector3Ops::Normalize( m * (2.0 * wiDotM) - wi );
		const Scalar cosWo = Vector3Ops::Dot( wo, onb.w() );
		if( cosWo <= 0 ) continue;

		// Transform to local for G2 evaluation
		const Vector3 wi_local(
			Vector3Ops::Dot( wi, onb.u() ),
			Vector3Ops::Dot( wi, onb.v() ),
			Vector3Ops::Dot( wi, onb.w() )
		);
		const Vector3 wo_local(
			Vector3Ops::Dot( wo, onb.u() ),
			Vector3Ops::Dot( wo, onb.v() ),
			Vector3Ops::Dot( wo, onb.w() )
		);

		const Scalar G2 = MicrofacetUtils::GGX_G2_Aniso( alpha_x, alpha_y, wi_local, wo_local );
		const Scalar G1wi = MicrofacetUtils::GGX_G1_Aniso( alpha_x, alpha_y, wi_local );

		if( G1wi > 1e-10 )
		{
			sum += G2 / G1wi;
			validCount++;
		}
	}

	const double Ess = (validCount > 0) ? sum / validCount : 0;

	std::cout << "  E_ss (ax=" << alpha_x << ", ay=" << alpha_y
	          << ", theta=" << std::fixed << std::setprecision(1) << (incomingTheta * 180.0 / PI)
	          << "): " << std::setprecision(6) << Ess;

	if( Ess > 1.0 + ESS_TOL ) {
		std::cout << "  FAIL (exceeds 1.0)" << std::endl;
		return false;
	}
	std::cout << "  OK" << std::endl;
	return true;
}

// ============================================================
//  Test 4: Isotropic consistency
//  When alpha_x == alpha_y == alpha, aniso functions should
//  match isotropic functions.
// ============================================================

static bool TestIsotropicConsistency( Scalar alpha )
{
	OrthonormalBasis3D onb;
	onb.CreateFromW( Vector3(0,0,1) );

	RandomNumberGenerator rng;
	bool passed = true;

	// 4a. Pointwise NDF comparison
	for( int i = 0; i < 100; i++ )
	{
		const Scalar theta = rng.CanonicalRandom() * PI_OV_TWO * 0.99;
		const Scalar phi = rng.CanonicalRandom() * TWO_PI;
		const Vector3 h_local(
			sin(theta) * cos(phi),
			sin(theta) * sin(phi),
			cos(theta)
		);

		const Scalar D_iso  = MicrofacetUtils::GGX_D<Scalar>( alpha, h_local.z );
		const Scalar D_aniso = MicrofacetUtils::GGX_D_Aniso<Scalar>( alpha, alpha, h_local );

		if( D_iso > 1e-10 ) {
			const double relErr = fabs( D_iso - D_aniso ) / D_iso;
			if( relErr > 1e-6 ) {
				std::cout << "  NDF iso/aniso mismatch: D_iso=" << D_iso
				          << " D_aniso=" << D_aniso << std::endl;
				passed = false;
			}
		}
	}

	// 4b. G1 comparison
	for( int i = 0; i < 100; i++ )
	{
		const Scalar cosTheta = 0.01 + rng.CanonicalRandom() * 0.98;
		const Scalar phi = rng.CanonicalRandom() * TWO_PI;
		const Scalar sinTheta = sqrt( 1.0 - cosTheta * cosTheta );
		const Vector3 v_local( sinTheta * cos(phi), sinTheta * sin(phi), cosTheta );

		const Scalar G1_iso  = MicrofacetUtils::GGX_G1( alpha, cosTheta );
		const Scalar G1_aniso = MicrofacetUtils::GGX_G1_Aniso( alpha, alpha, v_local );

		if( G1_iso > 1e-10 ) {
			const double relErr = fabs( G1_iso - G1_aniso ) / G1_iso;
			if( relErr > 1e-4 ) {
				std::cout << "  G1 iso/aniso mismatch: G1_iso=" << G1_iso
				          << " G1_aniso=" << G1_aniso
				          << " cosTheta=" << cosTheta << std::endl;
				passed = false;
			}
		}
	}

	// 4c. G2 vs separable comparison -- G2 should generally be >= G1*G1
	//     (height-correlated is less lossy than separable)
	for( int i = 0; i < 100; i++ )
	{
		const Scalar cosWi = 0.05 + rng.CanonicalRandom() * 0.9;
		const Scalar cosWo = 0.05 + rng.CanonicalRandom() * 0.9;

		const Scalar G2_hc = MicrofacetUtils::GGX_G2( alpha, cosWi, cosWo );
		const Scalar G_sep = MicrofacetUtils::GGX_G( alpha, cosWi, cosWo );

		if( G2_hc < G_sep - 1e-6 ) {
			std::cout << "  G2_hc < G_sep: G2=" << G2_hc << " G_sep=" << G_sep
			          << " cosWi=" << cosWi << " cosWo=" << cosWo << std::endl;
			passed = false;
		}
	}

	// 4d. VNDF PDF comparison (MC: same distribution test)
	{
		const Vector3 wi( sin(0.5), 0, cos(0.5) );
		double pdfIsoSum = 0, pdfAnisoSum = 0;
		int count = 0;

		for( int i = 0; i < ISO_SAMPLES; i++ )
		{
			const Scalar u1 = rng.CanonicalRandom();
			const Scalar u2 = rng.CanonicalRandom();

			// Sample using isotropic method, evaluate both PDFs
			const Vector3 m = MicrofacetUtils::VNDF_Sample( wi, onb, alpha, u1, u2 );
			const Scalar wiDotM = Vector3Ops::Dot( wi, m );
			if( wiDotM <= 0 ) continue;

			const Vector3 wo = Vector3Ops::Normalize( m * (2.0 * wiDotM) - wi );
			const Scalar cosWo = Vector3Ops::Dot( wo, onb.w() );
			if( cosWo <= 0 ) continue;

			const Scalar pdf_iso  = MicrofacetUtils::VNDF_Pdf( wi, wo, onb.w(), alpha );
			const Scalar pdf_aniso = MicrofacetUtils::VNDF_Pdf_Aniso( wi, wo, onb, alpha, alpha );

			if( pdf_iso > 1e-10 ) {
				pdfIsoSum += pdf_iso;
				pdfAnisoSum += pdf_aniso;
				count++;
			}
		}

		if( count > 0 ) {
			const double relErr = fabs( pdfIsoSum - pdfAnisoSum ) / pdfIsoSum;
			if( relErr > ISO_TOL ) {
				std::cout << "  VNDF PDF iso/aniso mean mismatch: rel_err=" << relErr << std::endl;
				passed = false;
			}
		}
	}

	std::cout << "  Isotropic consistency (alpha=" << alpha << "): "
	          << (passed ? "OK" : "FAIL") << std::endl;
	return passed;
}

// ============================================================
//  Test 5: Height-correlated G2 >= separable G1*G1
// ============================================================

static bool TestG2VsSeparable( Scalar alpha )
{
	RandomNumberGenerator rng;
	int violations = 0;

	for( int i = 0; i < G2_SAMPLES; i++ )
	{
		const Scalar cosWi = 0.01 + rng.CanonicalRandom() * 0.98;
		const Scalar cosWo = 0.01 + rng.CanonicalRandom() * 0.98;

		const Scalar G2_hc = MicrofacetUtils::GGX_G2( alpha, cosWi, cosWo );
		const Scalar G_sep = MicrofacetUtils::GGX_G( alpha, cosWi, cosWo );

		if( G2_hc < G_sep - 1e-6 ) {
			violations++;
		}
	}

	std::cout << "  G2 >= G_sep (alpha=" << alpha << "): violations=" << violations;
	if( violations > 0 ) {
		std::cout << "  FAIL" << std::endl;
		return false;
	}
	std::cout << "  OK" << std::endl;
	return true;
}

// ============================================================
//  Test 6: Material-level BRDF/SPF pointwise consistency
//  Instantiate actual GGXBRDF and GGXSPF classes, scatter via
//  SPF, then verify kray * pdf == BRDF::value(wo,ri) * cosWo.
//  This catches Fresnel-path and weight-derivation bugs that
//  the MicrofacetUtils-only tests above cannot detect.
// ============================================================

static RayIntersectionGeometric MakeRI( double incomingTheta )
{
	double sinT = sin(incomingTheta);
	double cosT = cos(incomingTheta);
	Vector3 inDir( sinT, 0, -cosT );

	Ray inRay( Point3(sinT, 0, 1.0), inDir );
	RasterizerState rs = {0, 0};
	RayIntersectionGeometric ri( inRay, rs );

	ri.bHit = true;
	ri.range = 1.0 / cosT;
	ri.ptIntersection = Point3(0, 0, 0);
	ri.vNormal = Vector3(0, 0, 1);
	ri.onb.CreateFromW( Vector3(0, 0, 1) );
	ri.ptCoord = Point2(0.5, 0.5);

	return ri;
}

static bool TestMaterialPointwiseConsistency(
	Scalar alphaXval,
	Scalar alphaYval,
	Scalar incomingTheta,
	int numSamples,
	FresnelMode fresnelMode = eFresnelConductor
	)
{
	// Create painters.  In schlick_f0 mode `specular` is interpreted as F0
	// directly; we use 0.5 to give a clear specular contribution that's
	// neither pure-dielectric (0.04) nor pure-metal (~baseColor).
	const Scalar specVal = (fresnelMode == eFresnelSchlickF0) ? 0.5 : 0.8;
	UniformColorPainter* diffuse = new UniformColorPainter( RISEPel(0.3, 0.3, 0.3) );  diffuse->addref();
	UniformColorPainter* specular = new UniformColorPainter( RISEPel(specVal, specVal, specVal) );  specular->addref();
	UniformColorPainter* pAlphaX = new UniformColorPainter( RISEPel(alphaXval, alphaXval, alphaXval) );  pAlphaX->addref();
	UniformColorPainter* pAlphaY = new UniformColorPainter( RISEPel(alphaYval, alphaYval, alphaYval) );  pAlphaY->addref();
	UniformColorPainter* ior = new UniformColorPainter( RISEPel(2.5, 2.5, 2.5) );  ior->addref();
	UniformColorPainter* ext = new UniformColorPainter( RISEPel(3.0, 3.0, 3.0) );  ext->addref();

	GGXBRDF* brdf = new GGXBRDF( *diffuse, *specular, *pAlphaX, *pAlphaY, *ior, *ext, fresnelMode );  brdf->addref();
	GGXSPF*  spf = new GGXSPF( *diffuse, *specular, *pAlphaX, *pAlphaY, *ior, *ext, fresnelMode );   spf->addref();

	RayIntersectionGeometric ri = MakeRI( incomingTheta );

	RandomNumberGenerator rng;
	IndependentSampler sampler( rng );
	IORStack iorStack = MakeTestIORStack( g_stubObject );
	const Vector3 n = ri.onb.w();

	int validSamples = 0;
	int pdfFailures = 0;
	double maxPdfErr = 0;

	// Accumulate two independent MC estimates of directional albedo:
	//   spfEstimate = mean(kray)              [SPF importance sampling]
	//   brdfEstimate = mean(BRDF*cos/mixPdf)  [BRDF evaluated at SPF-sampled directions]
	// Both should converge to integral(BRDF * cos dw) if kray and BRDF are consistent.
	double spfAccum = 0;
	double brdfAccum = 0;
	int furnaceSamples = 0;

	for( int i = 0; i < numSamples; i++ )
	{
		ScatteredRayContainer scattered;
		spf->Scatter( ri, sampler, scattered, iorStack );

		for( unsigned int r = 0; r < scattered.Count(); r++ )
		{
			const ScatteredRay& scat = scattered[r];
			if( scat.isDelta ) continue;

			const Vector3 wo = scat.ray.Dir();
			const Scalar cosWo = Vector3Ops::Dot( wo, n );
			if( cosWo <= 0 ) continue;

			// Evaluate BRDF at this direction
			const RISEPel brdfVal = brdf->value( wo, ri );
			const Scalar brdfMax = ColorMath::MaxValue( brdfVal );
			const Scalar krayMax = ColorMath::MaxValue( scat.kray );

			if( brdfMax < 1e-10 && krayMax < 1e-10 ) continue;

			const Scalar spfPdf = spf->Pdf( ri, wo, iorStack );
			if( spfPdf < 1e-10 ) continue;

			validSamples++;

			// Check 1: scat.pdf == SPF::Pdf(wo) (PDF self-consistency)
			const Scalar pdfErr = (scat.pdf > 1e-10) ?
				fabs(scat.pdf - spfPdf) / scat.pdf : 0;
			if( pdfErr > 1e-4 ) pdfFailures++;
			if( pdfErr > maxPdfErr ) maxPdfErr = pdfErr;

			// Accumulate both MC estimates of the directional albedo.
			// kray is the per-lobe throughput weight (includes 1/pSelect);
			// BRDF*cos/mixPdf is the full-BRDF importance weight.
			// They differ per-sample (kray is for one lobe, BRDF is all lobes)
			// but their means must converge to the same integral.
			spfAccum += krayMax;
			brdfAccum += brdfMax * cosWo / spfPdf;
			furnaceSamples++;
		}
	}

	// Mean of both MC estimators
	const double spfAlbedo = (furnaceSamples > 0) ? spfAccum / furnaceSamples : 0;
	const double brdfAlbedo = (furnaceSamples > 0) ? brdfAccum / furnaceSamples : 0;

	const char* modeLabel = (fresnelMode == eFresnelSchlickF0) ? "schlick" : "cond";
	std::cout << "  Material test [" << modeLabel << "] (ax=" << alphaXval << ", ay=" << alphaYval
	          << ", theta=" << std::fixed << std::setprecision(1) << (incomingTheta * 180.0 / PI)
	          << "): valid=" << validSamples
	          << " pdfFail=" << pdfFailures
	          << " maxPdfErr=" << std::setprecision(6) << maxPdfErr
	          << " spfAlbedo=" << std::setprecision(4) << spfAlbedo
	          << " brdfAlbedo=" << std::setprecision(4) << brdfAlbedo;

	brdf->release();
	spf->release();
	diffuse->release();
	specular->release();
	pAlphaX->release();
	pAlphaY->release();
	ior->release();
	ext->release();

	bool passed = true;

	// PDF consistency: no more than 1% failures at 1e-4 tolerance
	if( pdfFailures > validSamples / 100 ) {
		std::cout << "  FAIL (pdf)" << std::endl;
		passed = false;
	}
	// SPF and BRDF estimators must agree within 5% (statistical convergence)
	else if( brdfAlbedo > 1e-3 && fabs(spfAlbedo - brdfAlbedo) / brdfAlbedo > 0.05 ) {
		std::cout << "  FAIL (SPF/BRDF mismatch: " << spfAlbedo << " vs " << brdfAlbedo << ")" << std::endl;
		passed = false;
	}
	// Energy conservation: albedo must not exceed 1.0 + tolerance
	else if( spfAlbedo > 1.05 ) {
		std::cout << "  FAIL (energy gain: " << spfAlbedo << ")" << std::endl;
		passed = false;
	}
	else {
		std::cout << "  OK" << std::endl;
	}
	return passed;
}

// ============================================================
//  Main
// ============================================================

int main()
{
	std::cout << "=== GGX White Furnace Test ===" << std::endl;
	bool allPassed = true;

	// Stub object for IOR stack operations (mimics scene object identity)
	g_stubObject = new StubObject();
	g_stubObject->addref();

	// Test 1: NDF normalization
	std::cout << std::endl << "--- Test 1: NDF Normalization ---" << std::endl;
	{
		Scalar alphaX[] = { 0.1, 0.3, 0.5, 0.8, 1.0, 0.05, 0.1, 0.3 };
		Scalar alphaY[] = { 0.1, 0.3, 0.5, 0.8, 1.0, 0.5,  0.8, 0.05 };
		for( int i = 0; i < 8; i++ ) {
			allPassed &= TestNDFNormalization( alphaX[i], alphaY[i] );
		}
	}

	// Test 2: VNDF PDF cross-validation
	std::cout << std::endl << "--- Test 2: VNDF PDF Cross-Validation ---" << std::endl;
	{
		Scalar alphaX[] = { 0.1, 0.3, 0.5, 0.1, 0.3 };
		Scalar alphaY[] = { 0.1, 0.3, 0.5, 0.5, 0.05 };
		Scalar thetas[] = { 0.0, 0.3, 0.8, 1.2, 1.4 };

		for( int a = 0; a < 5; a++ ) {
			for( int t = 0; t < 5; t++ ) {
				allPassed &= TestVNDFCrossValidation( alphaX[a], alphaY[a], thetas[t] );
			}
		}
	}

	// Test 3: Energy conservation
	std::cout << std::endl << "--- Test 3: Energy Conservation (E_ss) ---" << std::endl;
	{
		Scalar alphaX[] = { 0.1, 0.3, 0.5, 0.8, 0.1, 0.3 };
		Scalar alphaY[] = { 0.1, 0.3, 0.5, 0.8, 0.5, 0.05 };
		Scalar thetas[] = { 0.0, 0.5, 1.0, 1.4 };

		for( int a = 0; a < 6; a++ ) {
			for( int t = 0; t < 4; t++ ) {
				allPassed &= TestEnergyConservation( alphaX[a], alphaY[a], thetas[t] );
			}
		}
	}

	// Test 4: Isotropic consistency
	std::cout << std::endl << "--- Test 4: Isotropic Consistency ---" << std::endl;
	{
		Scalar alphas[] = { 0.05, 0.1, 0.3, 0.5, 0.8, 1.0 };
		for( int i = 0; i < 6; i++ ) {
			allPassed &= TestIsotropicConsistency( alphas[i] );
		}
	}

	// Test 5: G2 >= G_separable
	std::cout << std::endl << "--- Test 5: G2 Height-Correlated >= G Separable ---" << std::endl;
	{
		Scalar alphas[] = { 0.05, 0.1, 0.3, 0.5, 0.8, 1.0 };
		for( int i = 0; i < 6; i++ ) {
			allPassed &= TestG2VsSeparable( alphas[i] );
		}
	}

	// Test 6: Material-level pointwise consistency (exercises GGXBRDF + GGXSPF
	// in eFresnelConductor mode — preserves the existing regression coverage)
	std::cout << std::endl << "--- Test 6: Material BRDF/SPF Pointwise Consistency [conductor] ---" << std::endl;
	{
		// Initialize the global log to prevent null pointer crashes
		GlobalLog();

		Scalar alphaX[] = { 0.2, 0.5, 0.2, 0.1 };
		Scalar alphaY[] = { 0.2, 0.5, 0.5, 0.8 };
		Scalar thetas[] = { 0.3, 0.8, 1.2 };

		for( int a = 0; a < 4; a++ ) {
			for( int t = 0; t < 3; t++ ) {
				allPassed &= TestMaterialPointwiseConsistency( alphaX[a], alphaY[a], thetas[t], 10000, eFresnelConductor );
			}
		}
	}

	// Test 7: Same battery, schlick_f0 mode.  Validates that PDF / kray
	// derivation, multiscatter compensation, and the (1 - max(F0)) diffuse
	// split all preserve PDF-self-consistency, SPF/BRDF agreement, and
	// energy conservation when the BRDF uses Schlick-from-F0 instead of
	// conductor Fresnel.
	std::cout << std::endl << "--- Test 7: Material BRDF/SPF Pointwise Consistency [schlick_f0] ---" << std::endl;
	{
		Scalar alphaX[] = { 0.2, 0.5, 0.2, 0.1 };
		Scalar alphaY[] = { 0.2, 0.5, 0.5, 0.8 };
		Scalar thetas[] = { 0.3, 0.8, 1.2 };

		for( int a = 0; a < 4; a++ ) {
			for( int t = 0; t < 3; t++ ) {
				allPassed &= TestMaterialPointwiseConsistency( alphaX[a], alphaY[a], thetas[t], 10000, eFresnelSchlickF0 );
			}
		}
	}

	g_stubObject->release();

	// Summary
	std::cout << std::endl << "=== " << (allPassed ? "ALL TESTS PASSED" : "SOME TESTS FAILED") << " ===" << std::endl;
	return allPassed ? 0 : 1;
}
