//////////////////////////////////////////////////////////////////////
//
//  LightExitanceTest.cpp - Verifies that radiantExitance() matches
//    the Monte Carlo integral of emittedRadiance() for all light
//    types.  This catches energy-budget regressions in light sources.
//
//    For each light with non-zero radiantExitance, we uniformly
//    sample directions on the sphere, evaluate emittedRadiance(),
//    and compare the integral (average * 4pi) against the reported
//    radiantExitance.
//
//    Additionally verifies that pdfDirection() integrates to 1
//    over the emission solid angle for lights that support it,
//    and that generateRandomPhoton() produces directions within
//    the declared emission cone.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iomanip>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Lights/PointLight.h"
#include "../src/Library/Lights/SpotLight.h"
#include "../src/Library/Lights/DirectionalLight.h"
#include "../src/Library/Lights/AmbientLight.h"

using namespace RISE;
using namespace RISE::Implementation;

// ============================================================
//  Configuration
// ============================================================

static int          gMcSamples       = 100000;
static const double EXITANCE_TOL     = 0.01;    // 1% relative tolerance for MC
static const double QUAD_TOL         = 1e-4;    // 0.01% relative tolerance for piecewise quadrature
static int          gPhotonSamples   = 20000;

// ============================================================
//  Helpers
// ============================================================

/// Piecewise polar quadrature of emittedRadiance over the full sphere,
/// aligned to the light's emission axis (with 8 azimuth samples per polar
/// ring to verify 3D frame alignment and azimuthal symmetry, and 1e-6 outside
/// the outer cone to verify zero leakage).
static RISEPel QuadratureConeExitance( const ILightPriv& light, const Scalar halfInner, const Scalar halfOuter )
{
	static const double kGL4X[4] = {
		-0.86113631159405257522, -0.33998104358485626480,
		 0.33998104358485626480,  0.86113631159405257522
	};
	static const double kGL4W[4] = {
		 0.34785484513745385737,  0.65214515486254614263,
		 0.65214515486254614263,  0.34785484513745385737
	};
	OrthonormalBasis3D onb;
	onb.CreateFromW( light.emissionDirection() );

	auto integrateBand = [&]( Scalar theta0, Scalar theta1, int nPanels ) -> RISEPel {
		if( theta1 <= theta0 ) return RISEPel( 0, 0, 0 );
		const int nPhi = 8;
		const Scalar dPanel = ( theta1 - theta0 ) / Scalar( nPanels );
		const Scalar dPhi = TWO_PI / Scalar( nPhi );
		RISEPel acc( 0, 0, 0 );
		for( int ipn = 0; ipn < nPanels; ++ipn ) {
			const Scalar a = theta0 + Scalar( ipn ) * dPanel;
			const Scalar mid = a + 0.5 * dPanel;
			const Scalar half = 0.5 * dPanel;
			for( int g = 0; g < 4; ++g ) {
				const Scalar theta = mid + half * kGL4X[g];
				const Scalar wTheta = half * kGL4W[g];
				const Scalar sinT = sin( theta );
				const Scalar cosT = cos( theta );
				for( int ip = 0; ip < nPhi; ++ip ) {
					const Scalar phi = ( Scalar( ip ) + 0.5 ) * dPhi;
					const Vector3 localDir( cos( phi ) * sinT, sin( phi ) * sinT, cosT );
					const Vector3 worldDir(
						onb.u().x * localDir.x + onb.v().x * localDir.y + onb.w().x * localDir.z,
						onb.u().y * localDir.x + onb.v().y * localDir.y + onb.w().y * localDir.z,
						onb.u().z * localDir.x + onb.v().z * localDir.y + onb.w().z * localDir.z );
					acc = acc + light.emittedRadiance( worldDir ) * ( sinT * wTheta * dPhi );
				}
			}
		}
		return acc;
	};

	return integrateBand( 0.0, halfInner, 16 )
	     + integrateBand( halfInner, halfOuter, 32 )
	     + integrateBand( halfOuter, PI, 8 );
}

static double QuadratureConePdfIntegral( const ILightPriv& light, const Scalar halfInner, const Scalar halfOuter )
{
	static const double kGL4X[4] = {
		-0.86113631159405257522, -0.33998104358485626480,
		 0.33998104358485626480,  0.86113631159405257522
	};
	static const double kGL4W[4] = {
		 0.34785484513745385737,  0.65214515486254614263,
		 0.65214515486254614263,  0.34785484513745385737
	};
	OrthonormalBasis3D onb;
	onb.CreateFromW( light.emissionDirection() );

	auto integrateBand = [&]( Scalar theta0, Scalar theta1, int nPanels ) -> double {
		if( theta1 <= theta0 ) return 0.0;
		const int nPhi = 8;
		const double dPanel = ( theta1 - theta0 ) / double( nPanels );
		const double dPhi = TWO_PI / double( nPhi );
		double acc = 0.0;
		for( int ipn = 0; ipn < nPanels; ++ipn ) {
			const double a = theta0 + double( ipn ) * dPanel;
			const double mid = a + 0.5 * dPanel;
			const double half = 0.5 * dPanel;
			for( int g = 0; g < 4; ++g ) {
				const double theta = mid + half * kGL4X[g];
				const double wTheta = half * kGL4W[g];
				const double sinT = sin( theta );
				const double cosT = cos( theta );
				for( int ip = 0; ip < nPhi; ++ip ) {
					const double phi = ( double( ip ) + 0.5 ) * dPhi;
					const Vector3 localDir( cos( phi ) * sinT, sin( phi ) * sinT, cosT );
					const Vector3 worldDir(
						onb.u().x * localDir.x + onb.v().x * localDir.y + onb.w().x * localDir.z,
						onb.u().y * localDir.x + onb.v().y * localDir.y + onb.w().y * localDir.z,
						onb.u().z * localDir.x + onb.v().z * localDir.y + onb.w().z * localDir.z );
					acc += light.pdfDirection( worldDir ) * sinT * wTheta * dPhi;
				}
			}
		}
		return acc;
	};

	return integrateBand( 0.0, halfInner, 16 )
	     + integrateBand( halfInner, halfOuter, 32 )
	     + integrateBand( halfOuter, PI, 8 );
}

/// Check that the relative error between two values is within tolerance.
/// Returns true if they match.
static bool CheckRelative( const double actual, const double expected, const double tol, const char* label )
{
	if( expected == 0 && actual == 0 ) return true;
	const double denom = (expected != 0) ? fabs(expected) : fabs(actual);
	const double relErr = fabs(actual - expected) / denom;
	if( relErr > tol )
	{
		std::cout << "  FAIL: " << label
				  << " expected=" << expected
				  << " actual=" << actual
				  << " relErr=" << std::fixed << std::setprecision(4) << (100.0*relErr) << "%"
				  << " (tol=" << (100.0*tol) << "%)" << std::endl;
		return false;
	}
	return true;
}

// ============================================================
//  Tests
// ============================================================

static void TestPointLight()
{
	std::cout << "TestPointLight..." << std::endl;

	const Scalar energy = 500.0;
	const RISEPel color( 0.8, 0.9, 1.0 );
	PointLight* pLight = new PointLight( energy, color, true );
	const ILightPriv& light = *pLight;

	// radiantExitance should equal integral of emittedRadiance over sphere
	const RISEPel exitance = light.radiantExitance();
	const RISEPel quadExitance = QuadratureConeExitance( light, PI / 2.0, PI );

	assert( CheckRelative( quadExitance[0], exitance[0], 1e-10, "PointLight R" ) );
	assert( CheckRelative( quadExitance[1], exitance[1], 1e-10, "PointLight G" ) );
	assert( CheckRelative( quadExitance[2], exitance[2], 1e-10, "PointLight B" ) );

	// Verify analytical value: exitance = color * energy * 4pi
	const RISEPel expected = color * energy * FOUR_PI;
	assert( CheckRelative( exitance[0], expected[0], 1e-10, "PointLight analytical R" ) );
	assert( CheckRelative( exitance[1], expected[1], 1e-10, "PointLight analytical G" ) );
	assert( CheckRelative( exitance[2], expected[2], 1e-10, "PointLight analytical B" ) );

	// pdfDirection should integrate to 1
	const double pdfInt = QuadratureConePdfIntegral( light, PI / 2.0, PI );
	assert( CheckRelative( pdfInt, 1.0, 1e-10, "PointLight PDF integral" ) );

	std::cout << "  exitance:   (" << exitance[0] << ", " << exitance[1] << ", " << exitance[2] << ")" << std::endl;
	std::cout << "  quad check: (" << quadExitance[0] << ", " << quadExitance[1] << ", " << quadExitance[2] << ")" << std::endl;
	std::cout << "TestPointLight passed!" << std::endl;

	pLight->release();
}

static void TestSpotLight( const Scalar innerDeg, const Scalar outerDeg )
{
	std::cout << "TestSpotLight (inner=" << innerDeg << " outer=" << outerDeg << ")..." << std::endl;

	const Scalar energy = 1800.0;
	const RISEPel color( 1.0, 0.95, 0.85 );

	// Angles are stored as full cone angles in radians
	const Scalar inner = innerDeg * DEG_TO_RAD;
	const Scalar outer = outerDeg * DEG_TO_RAD;

	// SpotLight starts at origin, target determines direction
	SpotLight* pLight = new SpotLight( energy, Point3( 0, 0, -10 ), inner, outer, color, true );
	const ILightPriv& light = *pLight;

	const RISEPel exitance = light.radiantExitance();
	const RISEPel quadExitance = QuadratureConeExitance( light, inner / 2.0, outer / 2.0 );

	std::cout << "  exitance:   (" << exitance[0] << ", " << exitance[1] << ", " << exitance[2] << ")" << std::endl;
	std::cout << "  quad check: (" << quadExitance[0] << ", " << quadExitance[1] << ", " << quadExitance[2] << ")" << std::endl;

	assert( CheckRelative( quadExitance[0], exitance[0], QUAD_TOL, "SpotLight R" ) );
	assert( CheckRelative( quadExitance[1], exitance[1], QUAD_TOL, "SpotLight G" ) );
	assert( CheckRelative( quadExitance[2], exitance[2], QUAD_TOL, "SpotLight B" ) );

	// pdfDirection should integrate to 1
	const double pdfInt = QuadratureConePdfIntegral( light, inner / 2.0, outer / 2.0 );
	std::cout << "  PDF integral: " << pdfInt << std::endl;
	assert( CheckRelative( pdfInt, 1.0, 1e-10, "SpotLight PDF integral" ) );

	// All photons should land within the outer half-cone
	const Scalar halfOuter = outer / 2.0;
	const Vector3 dir = pLight->emissionDirection();
	RandomNumberGenerator rng;
	int outsideCone = 0;
	for( int i = 0; i < gPhotonSamples; i++ )
	{
		const Ray photon = pLight->generateRandomPhoton( Point3(
			rng.CanonicalRandom(), rng.CanonicalRandom(), rng.CanonicalRandom() ) );
		const Scalar cosAngle = Vector3Ops::Dot( photon.Dir(), dir );
		const Scalar angle = acos( r_min( 1.0, cosAngle ) );
		if( angle > halfOuter + 1e-6 ) {
			outsideCone++;
		}
	}
	if( outsideCone > 0 ) {
		std::cout << "  FAIL: " << outsideCone << "/" << gPhotonSamples
				  << " photons outside outer cone" << std::endl;
	}
	assert( outsideCone == 0 );

	std::cout << "TestSpotLight (inner=" << innerDeg << " outer=" << outerDeg << ") passed!" << std::endl;

	pLight->release();
}

static void TestSpotLightDegenerateCases()
{
	std::cout << "TestSpotLightDegenerateCases..." << std::endl;

	const Scalar energy = 1000.0;
	const RISEPel color( 1.0, 1.0, 1.0 );

	// Case 1: inner == outer (no falloff zone, uniform cone)
	{
		const Scalar angle = 30.0 * DEG_TO_RAD;
		SpotLight* pLight = new SpotLight( energy, Point3( 0, 0, -1 ), angle, angle, color, true );

		const RISEPel exitance = pLight->radiantExitance();
		const Scalar halfAngle = angle / 2.0;
		const Scalar expectedSolidAngle = TWO_PI * (1.0 - cos( halfAngle ));
		const RISEPel expected = color * energy * expectedSolidAngle;

		std::cout << "  inner==outer=30deg: exitance=" << exitance[0]
				  << " expected=" << expected[0] << std::endl;
		assert( CheckRelative( exitance[0], expected[0], 1e-10, "SpotLight inner==outer" ) );
		pLight->release();
	}

	// Case 2: very narrow spot (inner=2, outer=5)
	{
		const Scalar inner = 2.0 * DEG_TO_RAD;
		const Scalar outer = 5.0 * DEG_TO_RAD;
		SpotLight* pLight = new SpotLight( energy, Point3( 0, 0, -1 ), inner, outer, color, true );

		const RISEPel exitance = pLight->radiantExitance();
		const RISEPel quadExitance = QuadratureConeExitance( *pLight, inner / 2.0, outer / 2.0 );

		std::cout << "  narrow (2/5): exitance=" << exitance[0]
				  << " quad=" << quadExitance[0] << std::endl;
		assert( CheckRelative( quadExitance[0], exitance[0], QUAD_TOL, "SpotLight narrow" ) );
		pLight->release();
	}

	// Case 3: wide spot (inner=60, outer=120)
	{
		const Scalar inner = 60.0 * DEG_TO_RAD;
		const Scalar outer = 120.0 * DEG_TO_RAD;
		SpotLight* pLight = new SpotLight( energy, Point3( 0, 0, -1 ), inner, outer, color, true );

		const RISEPel exitance = pLight->radiantExitance();
		const RISEPel quadExitance = QuadratureConeExitance( *pLight, inner / 2.0, outer / 2.0 );

		std::cout << "  wide (60/120): exitance=" << exitance[0]
				  << " quad=" << quadExitance[0] << std::endl;
		assert( CheckRelative( quadExitance[0], exitance[0], QUAD_TOL, "SpotLight wide" ) );
		pLight->release();
	}

	std::cout << "TestSpotLightDegenerateCases passed!" << std::endl;
}

static void TestDirectionalLight()
{
	std::cout << "TestDirectionalLight..." << std::endl;

	// DirectionalLight has zero radiantExitance by definition
	// (infinite distance, no meaningful total flux)
	DirectionalLight* pLight = new DirectionalLight( 100.0, RISEPel( 1, 1, 1 ), Vector3( 0, -1, 0 ) );

	const RISEPel exitance = pLight->radiantExitance();
	assert( exitance[0] == 0 && exitance[1] == 0 && exitance[2] == 0 );

	std::cout << "TestDirectionalLight passed!" << std::endl;
	pLight->release();
}

static void TestAmbientLight()
{
	std::cout << "TestAmbientLight..." << std::endl;

	// AmbientLight has zero radiantExitance (non-physical hack light)
	AmbientLight* pLight = new AmbientLight( 10.0, RISEPel( 0.5, 0.5, 0.5 ) );

	const RISEPel exitance = pLight->radiantExitance();
	assert( exitance[0] == 0 && exitance[1] == 0 && exitance[2] == 0 );

	std::cout << "TestAmbientLight passed!" << std::endl;
	pLight->release();
}

/// Verify that photon power weighting (emittedRadiance/pdfDirection)
/// averaged over sampled photon directions equals radiantExitance.
/// This is the invariant that the photon tracer relies on.
static void TestPhotonPowerWeighting( const char* label, ILightPriv& light )
{
	std::cout << "TestPhotonPowerWeighting (" << label << ")..." << std::endl;

	const RISEPel exitance = light.radiantExitance();
	const Scalar maxExitance = ColorMath::MaxValue( exitance );

	if( maxExitance == 0 ) {
		std::cout << "  Skipped (zero exitance)" << std::endl;
		return;
	}

	// Sample photon directions and accumulate weighted power
	RISEPel sumWeightedPower( 0, 0, 0 );
	RandomNumberGenerator rng;
	const int N = gMcSamples;

	for( int i = 0; i < N; i++ )
	{
		const Ray photon = light.generateRandomPhoton( Point3(
			rng.CanonicalRandom(), rng.CanonicalRandom(), rng.CanonicalRandom() ) );
		const Scalar pdf = light.pdfDirection( photon.Dir() );
		if( pdf > 0 ) {
			sumWeightedPower = sumWeightedPower + light.emittedRadiance( photon.Dir() ) * (1.0 / pdf);
		}
	}

	const RISEPel avgPower = sumWeightedPower * (1.0 / Scalar(N));

	std::cout << "  radiantExitance:    (" << exitance[0] << ", " << exitance[1] << ", " << exitance[2] << ")" << std::endl;
	std::cout << "  avg photon weight:  (" << avgPower[0] << ", " << avgPower[1] << ", " << avgPower[2] << ")" << std::endl;

	assert( CheckRelative( avgPower[0], exitance[0], EXITANCE_TOL, "PhotonWeight R" ) );
	assert( CheckRelative( avgPower[1], exitance[1], EXITANCE_TOL, "PhotonWeight G" ) );
	assert( CheckRelative( avgPower[2], exitance[2], EXITANCE_TOL, "PhotonWeight B" ) );

	std::cout << "TestPhotonPowerWeighting (" << label << ") passed!" << std::endl;
}

// ============================================================
//  Main
// ============================================================

int main( int argc, char** argv )
{
	if( const char* ext = std::getenv( "RISE_TEST_EXTENDED" ) ) {
		if( ext[0] == '1' ) { gMcSamples = 4000000; gPhotonSamples = 100000; }
	}
	for( int i = 1; i < argc; ++i ) {
		if( std::strcmp( argv[i], "--extended" ) == 0 ) {
			gMcSamples = 4000000;
			gPhotonSamples = 100000;
		}
	}

	std::cout << "=== LightExitanceTest ===" << std::endl;
	std::cout << "MC samples: " << gMcSamples << std::endl;
	std::cout << std::endl;

	TestPointLight();
	std::cout << std::endl;

	// Test SpotLight with various angle combinations
	TestSpotLight( 20, 45 );   // typical scene values
	std::cout << std::endl;

	TestSpotLight( 10, 25 );   // tighter spot
	std::cout << std::endl;

	TestSpotLight( 25, 50 );   // wider spot
	std::cout << std::endl;

	TestSpotLightDegenerateCases();
	std::cout << std::endl;

	TestDirectionalLight();
	TestAmbientLight();
	std::cout << std::endl;

	// Verify photon power weighting: avg(emittedRadiance/pdf) == radiantExitance
	{
		PointLight* p = new PointLight( 500.0, RISEPel( 0.8, 0.9, 1.0 ), true );
		TestPhotonPowerWeighting( "PointLight", *p );
		p->release();
	}
	std::cout << std::endl;

	{
		SpotLight* p = new SpotLight( 1800.0, Point3( 0, 0, -10 ), 20*DEG_TO_RAD, 45*DEG_TO_RAD, RISEPel( 1.0, 0.95, 0.85 ), true );
		TestPhotonPowerWeighting( "SpotLight 20/45", *p );
		p->release();
	}
	std::cout << std::endl;

	{
		SpotLight* p = new SpotLight( 1800.0, Point3( 0, 0, -10 ), 10*DEG_TO_RAD, 25*DEG_TO_RAD, RISEPel( 1.0, 0.95, 0.85 ), true );
		TestPhotonPowerWeighting( "SpotLight 10/25", *p );
		p->release();
	}
	std::cout << std::endl;

	{
		SpotLight* p = new SpotLight( 1000.0, Point3( 0, 0, -1 ), 60*DEG_TO_RAD, 120*DEG_TO_RAD, RISEPel( 1.0, 1.0, 1.0 ), true );
		TestPhotonPowerWeighting( "SpotLight 60/120", *p );
		p->release();
	}

	std::cout << std::endl;
	std::cout << "=== All LightExitanceTest tests passed! ===" << std::endl;
	return 0;
}
