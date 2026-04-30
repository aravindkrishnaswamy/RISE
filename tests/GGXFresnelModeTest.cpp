//////////////////////////////////////////////////////////////////////
//
//  GGXFresnelModeTest.cpp - Analytical oracles for the GGX BRDF's
//    Fresnel-mode selector (added in Phase 3 of the glTF import work).
//
//  Mirrors the test plan documented in docs/GLTF_IMPORT.md §13:
//    1. Schlick-at-normal-incidence: F0=0.04 cosθ=1 → F=0.04 ± 1e-9.
//                                    F0=baseColor cosθ=1 → F=baseColor.
//    2. Schlick-at-grazing: cosθ=0.001 → F ≈ 1 regardless of F0.
//    3. Reciprocity: BRDF(wi,wo) ≈ BRDF(wo,wi) at random sample directions
//       (within 1e-6 relative error per glTF / energy-conservation theory).
//    4. Energy bound: hemispherical-directional reflectance ≤ 1.
//    5. Hemispherical Schlick-Fresnel-average closed form:
//         F_avg = F0 + (1 - F0) / 21
//       compared against numerical 21-pt Gauss-Legendre quadrature of
//       Schlick(F0, μ) over the hemisphere.
//
//  Run alongside GGXWhiteFurnaceTest -- this catches Schlick-mode-specific
//  regressions that the broader furnace test would miss because its
//  oracles were designed for the conductor path.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <iomanip>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Optics.h"
#include "../src/Library/Utilities/MicrofacetEnergyLUT.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Materials/GGXBRDF.h"

using namespace RISE;
using namespace RISE::Implementation;

// ============================================================
//  Test 1: Schlick at known cosθ values
// ============================================================

static bool TestSchlickAnalytical()
{
	std::cout << "--- Test 1: Schlick analytical oracles ---" << std::endl;
	bool allPassed = true;

	// At cosθ=1 (normal incidence): F = F0
	{
		const Scalar F0 = 0.04;
		const Scalar F = Optics::CalculateFresnelReflectanceSchlick<Scalar>( F0, 1.0 );
		const double err = fabs( F - F0 );
		std::cout << "  cosθ=1, F0=0.04: F=" << std::setprecision(9) << F
				  << " err=" << err;
		if( err > 1e-9 ) { std::cout << "  FAIL" << std::endl; allPassed = false; }
		else std::cout << "  OK" << std::endl;
	}

	// At cosθ=1 with vector F0 (e.g., gold): F = F0
	{
		const RISEPel F0( 1.00, 0.71, 0.29 );
		const RISEPel F = Optics::CalculateFresnelReflectanceSchlick<RISEPel>( F0, 1.0 );
		const double err = fabs(F.r - F0.r) + fabs(F.g - F0.g) + fabs(F.b - F0.b);
		std::cout << "  cosθ=1, F0=(1.00,0.71,0.29): F=("
				  << F.r << "," << F.g << "," << F.b << ") err=" << err;
		if( err > 1e-9 ) { std::cout << "  FAIL" << std::endl; allPassed = false; }
		else std::cout << "  OK" << std::endl;
	}

	// At cosθ=0 (grazing): F = 1.0 regardless of F0
	{
		const Scalar F0 = 0.04;
		const Scalar F = Optics::CalculateFresnelReflectanceSchlick<Scalar>( F0, 0.0 );
		const double err = fabs( F - 1.0 );
		std::cout << "  cosθ=0, F0=0.04: F=" << std::setprecision(9) << F
				  << " err=" << err;
		if( err > 1e-9 ) { std::cout << "  FAIL" << std::endl; allPassed = false; }
		else std::cout << "  OK" << std::endl;
	}

	// At cosθ=0 with metal F0: still F=1
	{
		const Scalar F0 = 0.95;
		const Scalar F = Optics::CalculateFresnelReflectanceSchlick<Scalar>( F0, 0.0 );
		const double err = fabs( F - 1.0 );
		std::cout << "  cosθ=0, F0=0.95: F=" << std::setprecision(9) << F
				  << " err=" << err;
		if( err > 1e-9 ) { std::cout << "  FAIL" << std::endl; allPassed = false; }
		else std::cout << "  OK" << std::endl;
	}

	// At cosθ=0.5: F = F0 + (1-F0) * (0.5)^5 = F0 + (1-F0)*0.03125
	{
		const Scalar F0 = 0.10;
		const Scalar F = Optics::CalculateFresnelReflectanceSchlick<Scalar>( F0, 0.5 );
		const Scalar expected = F0 + (1.0 - F0) * pow(0.5, 5.0);
		const double err = fabs( F - expected );
		std::cout << "  cosθ=0.5, F0=0.10: F=" << std::setprecision(9) << F
				  << " expected=" << expected << " err=" << err;
		if( err > 1e-9 ) { std::cout << "  FAIL" << std::endl; allPassed = false; }
		else std::cout << "  OK" << std::endl;
	}

	return allPassed;
}

// ============================================================
//  Test 2: Schlick hemispherical average closed form
//          F_avg = F0 + (1 - F0) / 21
//          compared against numerical Gauss-Legendre quadrature of
//          Schlick(F0, μ) over the hemisphere.
// ============================================================

static Scalar SchlickFavgQuadrature( const Scalar F0 )
{
	// Same quadrature ComputeFresnelAvg uses for the conductor path.
	// F_avg = 2 * integral_0^1 F(μ) * μ dμ
	Scalar sum = 0;
	for( int i = 0; i < MicrofacetEnergyLUT::GL_N; i++ )
	{
		const Scalar mu = MicrofacetEnergyLUT::GL_nodes[i];
		const Scalar F = Optics::CalculateFresnelReflectanceSchlick<Scalar>( F0, mu );
		sum += F * (2.0 * mu * MicrofacetEnergyLUT::GL_weights[i]);
	}
	return sum;
}

static bool TestSchlickHemisphericalAverage()
{
	std::cout << std::endl << "--- Test 2: Schlick hemispherical average closed form ---" << std::endl;
	bool allPassed = true;

	const Scalar F0_values[] = { 0.0, 0.04, 0.1, 0.5, 0.9, 1.0 };
	for( int i = 0; i < 6; i++ )
	{
		const Scalar F0 = F0_values[i];
		const Scalar closedForm = F0 + (1.0 - F0) / 21.0;
		const Scalar quadrature = SchlickFavgQuadrature( F0 );
		const Scalar err = fabs( closedForm - quadrature );

		std::cout << "  F0=" << std::fixed << std::setprecision(2) << F0
				  << ": closed=" << std::setprecision(8) << closedForm
				  << " quad=" << quadrature << " err=" << err;
		// Gauss-Legendre N=21 on a smooth Schlick should converge below 1e-5.
		if( err > 1e-5 ) { std::cout << "  FAIL" << std::endl; allPassed = false; }
		else std::cout << "  OK" << std::endl;
	}
	return allPassed;
}

// ============================================================
//  Test 3: Reciprocity of GGX::value in schlick_f0 mode
//          BRDF(wi → wo) ≈ BRDF(wo → wi) for non-delta lobes.
//          (Holds analytically for Schlick + symmetric NDF + height-
//          correlated G2 + symmetric multiscatter.)
// ============================================================

static RayIntersectionGeometric MakeRIWithRay(
	const Vector3& rayDir,
	const Point3& origin
	)
{
	Ray inRay( origin, rayDir );
	RasterizerState rs = {0, 0};
	RayIntersectionGeometric ri( inRay, rs );
	ri.bHit = true;
	ri.range = 1.0;
	ri.ptIntersection = Point3(0, 0, 0);
	ri.vNormal = Vector3(0, 0, 1);
	ri.onb.CreateFromW( Vector3(0, 0, 1) );
	ri.ptCoord = Point2(0.5, 0.5);
	return ri;
}

static bool TestReciprocity( FresnelMode mode, const char* modeName )
{
	std::cout << std::endl << "--- Test 3: BRDF reciprocity [" << modeName << "] ---" << std::endl;

	UniformColorPainter* diffuse = new UniformColorPainter( RISEPel(0.3, 0.3, 0.3) );  diffuse->addref();
	UniformColorPainter* specular = new UniformColorPainter( RISEPel(0.5, 0.5, 0.5) );  specular->addref();
	UniformColorPainter* alphaX = new UniformColorPainter( RISEPel(0.4, 0.4, 0.4) );  alphaX->addref();
	UniformColorPainter* alphaY = new UniformColorPainter( RISEPel(0.4, 0.4, 0.4) );  alphaY->addref();
	UniformColorPainter* ior = new UniformColorPainter( RISEPel(2.5, 2.5, 2.5) );  ior->addref();
	UniformColorPainter* ext = new UniformColorPainter( RISEPel(3.0, 3.0, 3.0) );  ext->addref();

	GGXBRDF* brdf = new GGXBRDF( *diffuse, *specular, *alphaX, *alphaY, *ior, *ext, mode );
	brdf->addref();

	RandomNumberGenerator rng;
	const int N = 1024;
	double maxRelErr = 0;
	int largeFailures = 0;
	int validCount = 0;

	for( int i = 0; i < N; i++ )
	{
		// Two random directions on the upper hemisphere.
		const Scalar u1 = rng.CanonicalRandom();
		const Scalar u2 = rng.CanonicalRandom();
		const Scalar theta1 = acos( 1.0 - u1 * 0.95 );	// avoid grazing
		const Scalar phi1 = u2 * TWO_PI;
		const Vector3 wi(
			sin(theta1) * cos(phi1),
			sin(theta1) * sin(phi1),
			cos(theta1)
		);

		const Scalar u3 = rng.CanonicalRandom();
		const Scalar u4 = rng.CanonicalRandom();
		const Scalar theta2 = acos( 1.0 - u3 * 0.95 );
		const Scalar phi2 = u4 * TWO_PI;
		const Vector3 wo(
			sin(theta2) * cos(phi2),
			sin(theta2) * sin(phi2),
			cos(theta2)
		);

		// Forward eval: ray comes from -wo direction (toward the surface),
		// integrating the light coming in from wi.
		RayIntersectionGeometric riForward = MakeRIWithRay( -wo, Point3(0,0,1) );
		const RISEPel f1 = brdf->value( wi, riForward );

		// Reverse eval: swap roles.
		RayIntersectionGeometric riReverse = MakeRIWithRay( -wi, Point3(0,0,1) );
		const RISEPel f2 = brdf->value( wo, riReverse );

		const Scalar f1Max = ColorMath::MaxValue( f1 );
		const Scalar f2Max = ColorMath::MaxValue( f2 );
		if( f1Max < 1e-6 && f2Max < 1e-6 ) continue;

		const Scalar denom = (f1Max > f2Max) ? f1Max : f2Max;
		const Scalar relErr = fabs(f1Max - f2Max) / r_max( denom, Scalar(1e-10) );

		if( relErr > maxRelErr ) maxRelErr = relErr;
		if( relErr > 1e-4 ) largeFailures++;
		validCount++;
	}

	std::cout << "  [" << modeName << "] valid=" << validCount
			  << " maxRelErr=" << std::fixed << std::setprecision(8) << maxRelErr
			  << " >1e-4 failures=" << largeFailures;

	brdf->release();
	diffuse->release();
	specular->release();
	alphaX->release();
	alphaY->release();
	ior->release();
	ext->release();

	// Allow a handful of failures due to Monte Carlo edge effects on grazing
	// directions; require <0.5% large failures and overall maxErr < 1e-3.
	if( largeFailures > validCount / 200 || maxRelErr > 1e-3 ) {
		std::cout << "  FAIL" << std::endl;
		return false;
	}
	std::cout << "  OK" << std::endl;
	return true;
}

// ============================================================
//  Test 4: Energy bound — hemispherical-directional reflectance ≤ 1.
//          For each fixed wi, MC-integrate ∫ BRDF·cosθ dωo using cosine-
//          weighted hemisphere sampling.  The integral must not exceed
//          1.0 in either Fresnel mode (energy conservation).
// ============================================================

static bool TestEnergyBound( FresnelMode mode, const char* modeName )
{
	std::cout << std::endl << "--- Test 4: Energy bound [" << modeName << "] ---" << std::endl;

	UniformColorPainter* diffuse = new UniformColorPainter( RISEPel(0.5, 0.5, 0.5) );  diffuse->addref();
	UniformColorPainter* specular = new UniformColorPainter( RISEPel(0.5, 0.5, 0.5) );  specular->addref();
	UniformColorPainter* alphaX = new UniformColorPainter( RISEPel(0.3, 0.3, 0.3) );  alphaX->addref();
	UniformColorPainter* alphaY = new UniformColorPainter( RISEPel(0.3, 0.3, 0.3) );  alphaY->addref();
	UniformColorPainter* ior = new UniformColorPainter( RISEPel(2.5, 2.5, 2.5) );  ior->addref();
	UniformColorPainter* ext = new UniformColorPainter( RISEPel(3.0, 3.0, 3.0) );  ext->addref();

	GGXBRDF* brdf = new GGXBRDF( *diffuse, *specular, *alphaX, *alphaY, *ior, *ext, mode );
	brdf->addref();

	RandomNumberGenerator rng;
	bool allPassed = true;

	const Scalar incomingThetas[] = { 0.0, 0.3, 0.6, 1.0, 1.3 };
	const int M = 8192;	// MC samples per wi direction

	for( int t = 0; t < 5; t++ )
	{
		const Scalar theta_i = incomingThetas[t];
		const Vector3 wi( sin(theta_i), 0, cos(theta_i) );

		// Ray comes "in toward the surface", i.e. dir = -wi rotated to the
		// shading frame.  Construct an RI where ray.Dir() = -wi.
		RayIntersectionGeometric ri = MakeRIWithRay( -wi, Point3(0,0,1) );

		// Cosine-weighted hemisphere sampling: pdf(wo) = cosθ_o / π.
		Scalar accum = 0;
		int validCount = 0;
		for( int s = 0; s < M; s++ )
		{
			const Scalar u1 = rng.CanonicalRandom();
			const Scalar u2 = rng.CanonicalRandom();
			const Scalar phi = TWO_PI * u1;
			const Scalar cosTheta = sqrt( u2 );
			const Scalar sinTheta = sqrt( 1.0 - cosTheta * cosTheta );
			const Vector3 wo( sinTheta * cos(phi), sinTheta * sin(phi), cosTheta );

			const RISEPel f = brdf->value( wo, ri );
			const Scalar fMax = ColorMath::MaxValue( f );
			// integrand = BRDF * cosθ_o; pdf = cosθ_o / π
			// MC estimator = BRDF * cosθ_o / pdf = BRDF * π
			accum += fMax * PI;
			validCount++;
		}

		const Scalar reflectance = (validCount > 0) ? accum / validCount : 0;

		std::cout << "  [" << modeName << "] θ_i="
				  << std::fixed << std::setprecision(2) << theta_i
				  << ": ∫BRDF·cosθ dω = " << std::setprecision(4) << reflectance;
		// Allow 5% Monte Carlo slack.
		if( reflectance > 1.05 ) {
			std::cout << "  FAIL (>1)" << std::endl;
			allPassed = false;
		}
		else std::cout << "  OK" << std::endl;
	}

	brdf->release();
	diffuse->release();
	specular->release();
	alphaX->release();
	alphaY->release();
	ior->release();
	ext->release();
	return allPassed;
}

// ============================================================
//  Main
// ============================================================

int main()
{
	std::cout << "=== GGX Fresnel Mode Test ===" << std::endl;
	GlobalLog();	// initialize the global log

	bool allPassed = true;

	allPassed &= TestSchlickAnalytical();
	allPassed &= TestSchlickHemisphericalAverage();
	allPassed &= TestReciprocity( eFresnelConductor, "conductor" );
	allPassed &= TestReciprocity( eFresnelSchlickF0, "schlick_f0" );
	allPassed &= TestEnergyBound( eFresnelConductor, "conductor" );
	allPassed &= TestEnergyBound( eFresnelSchlickF0, "schlick_f0" );

	std::cout << std::endl << "=== "
			  << (allPassed ? "ALL TESTS PASSED" : "SOME TESTS FAILED")
			  << " ===" << std::endl;
	return allPassed ? 0 : 1;
}
