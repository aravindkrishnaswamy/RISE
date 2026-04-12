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
#include <iomanip>

#include "../src/Library/Utilities/Math3D/Math3D.h"
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

static const int    MC_SAMPLES       = 4000000;
static const double EXITANCE_TOL     = 0.01;    // 1% relative tolerance
static const double PDF_TOL          = 0.01;    // 1% tolerance for PDF integral
static const int    PHOTON_SAMPLES   = 100000;

// ============================================================
//  Helpers
// ============================================================

/// Generate a uniformly distributed direction on the unit sphere
/// using two uniform random numbers in [0,1).
static Vector3 UniformSphereDirection( const double u1, const double u2 )
{
	const double cosTheta = 1.0 - 2.0 * u1;
	const double sinTheta = sqrt( 1.0 - cosTheta * cosTheta );
	const double phi = TWO_PI * u2;
	return Vector3( cos(phi)*sinTheta, sin(phi)*sinTheta, cosTheta );
}

/// Monte Carlo estimate of integral of emittedRadiance over the
/// full sphere.  Each sample contributes emittedRadiance(dir) * 4pi / N.
static RISEPel MonteCarloExitance( const ILight& light, const int N )
{
	RISEPel sum( 0, 0, 0 );
	RandomNumberGenerator rng;

	for( int i = 0; i < N; i++ )
	{
		const Vector3 dir = UniformSphereDirection( rng.CanonicalRandom(), rng.CanonicalRandom() );
		sum = sum + light.emittedRadiance( dir );
	}

	return sum * (FOUR_PI / Scalar(N));
}

/// Monte Carlo estimate of integral of pdfDirection over the
/// full sphere.  Should equal 1.0 for a normalized PDF.
static double MonteCarloPdfIntegral( const ILightPriv& light, const int N )
{
	double sum = 0;
	RandomNumberGenerator rng;

	for( int i = 0; i < N; i++ )
	{
		const Vector3 dir = UniformSphereDirection( rng.CanonicalRandom(), rng.CanonicalRandom() );
		sum += light.pdfDirection( dir );
	}

	return sum * FOUR_PI / double(N);
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
	const RISEPel mcExitance = MonteCarloExitance( light, MC_SAMPLES );

	assert( CheckRelative( mcExitance[0], exitance[0], EXITANCE_TOL, "PointLight R" ) );
	assert( CheckRelative( mcExitance[1], exitance[1], EXITANCE_TOL, "PointLight G" ) );
	assert( CheckRelative( mcExitance[2], exitance[2], EXITANCE_TOL, "PointLight B" ) );

	// Verify analytical value: exitance = color * energy * 4pi
	const RISEPel expected = color * energy * FOUR_PI;
	assert( CheckRelative( exitance[0], expected[0], 1e-10, "PointLight analytical R" ) );
	assert( CheckRelative( exitance[1], expected[1], 1e-10, "PointLight analytical G" ) );
	assert( CheckRelative( exitance[2], expected[2], 1e-10, "PointLight analytical B" ) );

	// pdfDirection should integrate to 1
	const double pdfInt = MonteCarloPdfIntegral( light, MC_SAMPLES );
	assert( CheckRelative( pdfInt, 1.0, PDF_TOL, "PointLight PDF integral" ) );

	std::cout << "  exitance: (" << exitance[0] << ", " << exitance[1] << ", " << exitance[2] << ")" << std::endl;
	std::cout << "  MC check: (" << mcExitance[0] << ", " << mcExitance[1] << ", " << mcExitance[2] << ")" << std::endl;
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
	const RISEPel mcExitance = MonteCarloExitance( light, MC_SAMPLES );

	std::cout << "  exitance: (" << exitance[0] << ", " << exitance[1] << ", " << exitance[2] << ")" << std::endl;
	std::cout << "  MC check: (" << mcExitance[0] << ", " << mcExitance[1] << ", " << mcExitance[2] << ")" << std::endl;

	assert( CheckRelative( mcExitance[0], exitance[0], EXITANCE_TOL, "SpotLight R" ) );
	assert( CheckRelative( mcExitance[1], exitance[1], EXITANCE_TOL, "SpotLight G" ) );
	assert( CheckRelative( mcExitance[2], exitance[2], EXITANCE_TOL, "SpotLight B" ) );

	// pdfDirection should integrate to 1
	const double pdfInt = MonteCarloPdfIntegral( light, MC_SAMPLES );
	std::cout << "  PDF integral: " << pdfInt << std::endl;
	assert( CheckRelative( pdfInt, 1.0, PDF_TOL, "SpotLight PDF integral" ) );

	// All photons should land within the outer half-cone
	const Scalar halfOuter = outer / 2.0;
	const Vector3 dir = pLight->emissionDirection();
	RandomNumberGenerator rng;
	int outsideCone = 0;
	for( int i = 0; i < PHOTON_SAMPLES; i++ )
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
		std::cout << "  FAIL: " << outsideCone << "/" << PHOTON_SAMPLES
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
		const RISEPel mcExitance = MonteCarloExitance( *pLight, MC_SAMPLES );

		std::cout << "  narrow (2/5): exitance=" << exitance[0]
				  << " MC=" << mcExitance[0] << std::endl;
		assert( CheckRelative( mcExitance[0], exitance[0], EXITANCE_TOL, "SpotLight narrow" ) );
		pLight->release();
	}

	// Case 3: wide spot (inner=60, outer=120)
	{
		const Scalar inner = 60.0 * DEG_TO_RAD;
		const Scalar outer = 120.0 * DEG_TO_RAD;
		SpotLight* pLight = new SpotLight( energy, Point3( 0, 0, -1 ), inner, outer, color, true );

		const RISEPel exitance = pLight->radiantExitance();
		const RISEPel mcExitance = MonteCarloExitance( *pLight, MC_SAMPLES );

		std::cout << "  wide (60/120): exitance=" << exitance[0]
				  << " MC=" << mcExitance[0] << std::endl;
		assert( CheckRelative( mcExitance[0], exitance[0], EXITANCE_TOL, "SpotLight wide" ) );
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
	const int N = MC_SAMPLES;

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

int main()
{
	std::cout << "=== LightExitanceTest ===" << std::endl;
	std::cout << "MC samples: " << MC_SAMPLES << std::endl;
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
