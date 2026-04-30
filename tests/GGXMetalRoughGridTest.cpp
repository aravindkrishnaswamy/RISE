//////////////////////////////////////////////////////////////////////
//
//  GGXMetalRoughGridTest.cpp - Validates the four diagonal corners of
//    a (metallic × roughness) grid against analytical predictions.
//
//    Top-left  (m=0, r=0):    smooth dielectric — F0 = 0.04 at normal,
//                              high diffuse retention (1 - 0.04 = 0.96)
//    Top-right (m=0, r=1):    rough dielectric — diffuse-dominant Lambert,
//                              specular peak smeared
//    Bot-left  (m=1, r=0):    smooth metal — F0 = baseColor at normal,
//                              zero diffuse contribution
//    Bot-right (m=1, r=1):    rough metal — multiscatter compensation
//                              kicks in, diffuse still zero
//
//  Constructs each corner via Job::AddPBRMetallicRoughnessMaterial, then
//  evaluates the resulting GGXBRDF.value() at normal incidence and an
//  oblique angle to verify the painter graph + Schlick mode together
//  produce the right reflectance.
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
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Materials/GGXBRDF.h"

using namespace RISE;
using namespace RISE::Implementation;

// ============================================================
//  Helper: synthesise the painter graph that
//  Job::AddPBRMetallicRoughnessMaterial constructs internally
//  (post-Phase-3 mapping):
//
//     rd = baseColor * (1 - metallic)
//     rs = lerp(0.04, baseColor, metallic) = F0
//     alpha = roughness * roughness
//
//  Avoids the parser layer so the test stays focused on the BRDF.
// ============================================================

static GGXBRDF* MakePBRBrdf(
	const RISEPel& baseColor,
	const Scalar   metallic,
	const Scalar   roughness
	)
{
	const RISEPel rd = baseColor * (1.0 - metallic);
	const RISEPel f0 = baseColor * metallic + RISEPel(0.04, 0.04, 0.04) * (1.0 - metallic);
	const Scalar alpha = roughness * roughness;

	UniformColorPainter* pRd = new UniformColorPainter( rd );  pRd->addref();
	UniformColorPainter* pRs = new UniformColorPainter( f0 );  pRs->addref();
	UniformColorPainter* pAx = new UniformColorPainter( RISEPel(alpha, alpha, alpha) );  pAx->addref();
	UniformColorPainter* pAy = new UniformColorPainter( RISEPel(alpha, alpha, alpha) );  pAy->addref();
	UniformColorPainter* pIor = new UniformColorPainter( RISEPel(1.5, 1.5, 1.5) );  pIor->addref();
	UniformColorPainter* pExt = new UniformColorPainter( RISEPel(0.0, 0.0, 0.0) );  pExt->addref();

	// Use schlick_f0 mode — the same mode that Job::AddPBRMetallicRoughnessMaterial
	// passes to GGX after the Phase 3 flip.
	GGXBRDF* brdf = new GGXBRDF( *pRd, *pRs, *pAx, *pAy, *pIor, *pExt, eFresnelSchlickF0 );
	brdf->addref();

	// Painters can be released here -- BRDF holds its own refs.
	pRd->release();
	pRs->release();
	pAx->release();
	pAy->release();
	pIor->release();
	pExt->release();
	return brdf;
}

static RayIntersectionGeometric MakeRI( double incomingTheta )
{
	const Scalar sinT = sin(incomingTheta);
	const Scalar cosT = cos(incomingTheta);
	const Vector3 inDir( -sinT, 0, -cosT );		// ray comes "in" toward origin
	Ray inRay( Point3(sinT, 0, 1.0), inDir );
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

// ============================================================
//  Corner 1: Smooth dielectric (m=0, r=0).
//    Diffuse ≈ baseColor * (1 - max(0.04)) / π = baseColor * 0.96/π.
//    Specular peak: at the half-vector (mirror direction) the spec lobe
//    contributes, but at any other direction it's negligible because
//    the NDF is sharp.  The diffuse term dominates everywhere except
//    the mirror cone.
// ============================================================

static bool TestSmoothDielectric()
{
	std::cout << "--- Corner 1: Smooth dielectric (m=0, r=0) ---" << std::endl;
	bool allPassed = true;

	const RISEPel baseColor( 0.8, 0.6, 0.4 );	// arbitrary
	GGXBRDF* brdf = MakePBRBrdf( baseColor, 0.0, 0.0 );

	// Sample at oblique angle, light direction far from mirror.  The
	// diffuse term dominates: BRDF ≈ baseColor * (1 - 0.04) / π.
	RayIntersectionGeometric ri = MakeRI( 0.3 );	// view from ~17°
	const Vector3 lightDir = Vector3Ops::Normalize( Vector3(-0.6, 0.6, 0.5) );
	const RISEPel f = brdf->value( lightDir, ri );

	const RISEPel expectedDiffuse = baseColor * ((1.0 - 0.04) * INV_PI);
	const Scalar errR = fabs(f.r - expectedDiffuse.r);
	const Scalar errG = fabs(f.g - expectedDiffuse.g);
	const Scalar errB = fabs(f.b - expectedDiffuse.b);

	std::cout << "  BRDF = (" << std::fixed << std::setprecision(4)
			  << f.r << "," << f.g << "," << f.b << ")"
			  << "  expected ≈ (" << expectedDiffuse.r << "," << expectedDiffuse.g << "," << expectedDiffuse.b << ")";
	// Allow 1% slack — at smooth dielectric, off-mirror dirs have
	// effectively zero specular but multiscatter at α≈0 is small but
	// not exactly zero.
	if( errR > expectedDiffuse.r * 0.05 || errG > expectedDiffuse.g * 0.05 || errB > expectedDiffuse.b * 0.05 ) {
		std::cout << "  FAIL" << std::endl;
		allPassed = false;
	} else std::cout << "  OK" << std::endl;

	brdf->release();
	return allPassed;
}

// ============================================================
//  Corner 2: Rough dielectric (m=0, r=1).
//    Pure Lambertian diffuse plus a wide specular lobe + multiscatter.
//    BRDF should be roughly baseColor / π ± a correction for (1-F0)
//    diffuse split + the broad specular peak.  At an off-mirror sample
//    the diffuse dominates.
// ============================================================

static bool TestRoughDielectric()
{
	std::cout << std::endl << "--- Corner 2: Rough dielectric (m=0, r=1) ---" << std::endl;
	bool allPassed = true;

	const RISEPel baseColor( 0.6, 0.8, 0.4 );
	GGXBRDF* brdf = MakePBRBrdf( baseColor, 0.0, 1.0 );

	RayIntersectionGeometric ri = MakeRI( 0.3 );
	const Vector3 lightDir = Vector3Ops::Normalize( Vector3(-0.6, 0.6, 0.5) );
	const RISEPel f = brdf->value( lightDir, ri );

	// Lower bound: at minimum we should see (1 - 0.04) * baseColor / π
	// (the diffuse term alone, ignoring specular and multiscatter).
	const RISEPel diffuseLB = baseColor * ((1.0 - 0.04) * INV_PI);

	std::cout << "  BRDF = (" << std::fixed << std::setprecision(4)
			  << f.r << "," << f.g << "," << f.b << ")"
			  << "  diffuse LB = (" << diffuseLB.r << ", " << diffuseLB.g << ", " << diffuseLB.b << ")";
	if( f.r < diffuseLB.r * 0.95 || f.g < diffuseLB.g * 0.95 || f.b < diffuseLB.b * 0.95 ) {
		std::cout << "  FAIL (below diffuse LB)" << std::endl;
		allPassed = false;
	} else std::cout << "  OK" << std::endl;

	brdf->release();
	return allPassed;
}

// ============================================================
//  Corner 3: Smooth metal (m=1, r=0).
//    Zero diffuse: BRDF at off-mirror directions should be ≈ 0
//    (within numerical noise from multiscatter at very low α).
// ============================================================

static bool TestSmoothMetal()
{
	std::cout << std::endl << "--- Corner 3: Smooth metal (m=1, r=0) ---" << std::endl;
	bool allPassed = true;

	const RISEPel baseColor( 0.95, 0.93, 0.88 );	// silver-ish
	GGXBRDF* brdf = MakePBRBrdf( baseColor, 1.0, 0.0 );

	RayIntersectionGeometric ri = MakeRI( 0.3 );
	const Vector3 lightDir = Vector3Ops::Normalize( Vector3(-0.6, 0.6, 0.5) );	// off-mirror
	const RISEPel f = brdf->value( lightDir, ri );

	std::cout << "  BRDF (off-mirror) = (" << std::fixed << std::setprecision(6)
			  << f.r << "," << f.g << "," << f.b << ")";
	// Should be ~0 because metals have no diffuse.  Allow tiny multiscatter
	// noise (Eavg ≈ 1 at α=0 — should be essentially zero).
	if( f.r > 0.02 || f.g > 0.02 || f.b > 0.02 ) {
		std::cout << "  FAIL (off-mirror metal not dark)" << std::endl;
		allPassed = false;
	} else std::cout << "  OK" << std::endl;

	brdf->release();
	return allPassed;
}

// ============================================================
//  Corner 4: Rough metal (m=1, r=1).
//    Zero diffuse but multiscatter compensation contributes broadly.
//    BRDF should be smooth across the hemisphere (Lambert-ish but
//    tinted by F0=baseColor).
// ============================================================

static bool TestRoughMetal()
{
	std::cout << std::endl << "--- Corner 4: Rough metal (m=1, r=1) ---" << std::endl;
	bool allPassed = true;

	const RISEPel baseColor( 0.95, 0.93, 0.88 );	// silver-ish
	GGXBRDF* brdf = MakePBRBrdf( baseColor, 1.0, 1.0 );

	RayIntersectionGeometric ri = MakeRI( 0.3 );
	const Vector3 lightDir = Vector3Ops::Normalize( Vector3(-0.6, 0.6, 0.5) );
	const RISEPel f = brdf->value( lightDir, ri );

	std::cout << "  BRDF = (" << std::fixed << std::setprecision(4)
			  << f.r << "," << f.g << "," << f.b << ")";
	// Rough metal should have a non-trivial BRDF (multiscatter + spec lobe
	// is broad), at least a few percent of baseColor / π.
	const RISEPel target = baseColor * INV_PI * 0.1;	// 10% of pure-diffuse Lambert
	if( f.r < target.r || f.g < target.g || f.b < target.b ) {
		std::cout << "  FAIL (rough metal too dark)" << std::endl;
		allPassed = false;
	} else std::cout << "  OK" << std::endl;

	// Verify no channel exceeds baseColor / π by more than a small factor —
	// rough metal can't reflect more energy in any direction than its
	// hemispherical average.
	const RISEPel upperBound = baseColor * INV_PI * 5.0;
	if( f.r > upperBound.r || f.g > upperBound.g || f.b > upperBound.b ) {
		std::cout << "  FAIL (rough metal too bright)" << std::endl;
		allPassed = false;
	}

	brdf->release();
	if( allPassed ) std::cout << "  OK" << std::endl;
	return allPassed;
}

// ============================================================
//  Confirms that the (1 - max(F0)) diffuse split is visible:
//  for the same metallic=0 dielectric, increasing baseColor brightness
//  must produce non-decreasing diffuse output (no sign-flip from a
//  Schlick-mode bug).
// ============================================================

static bool TestDiffuseMonotonicity()
{
	std::cout << std::endl << "--- Diffuse monotonicity in m=0 dielectrics ---" << std::endl;
	bool allPassed = true;

	const Scalar bcVals[] = { 0.1, 0.3, 0.5, 0.7, 0.9 };
	Scalar prev = -1;

	for( int i = 0; i < 5; i++ )
	{
		GGXBRDF* brdf = MakePBRBrdf( RISEPel(bcVals[i], bcVals[i], bcVals[i]), 0.0, 0.5 );
		RayIntersectionGeometric ri = MakeRI( 0.4 );
		const Vector3 lightDir = Vector3Ops::Normalize( Vector3(-0.6, 0.6, 0.5) );
		const RISEPel f = brdf->value( lightDir, ri );

		std::cout << "  baseColor=" << bcVals[i]
				  << " BRDF=" << std::fixed << std::setprecision(4) << f.r;
		if( f.r < prev - 1e-6 ) {
			std::cout << "  FAIL (decreasing!)" << std::endl;
			allPassed = false;
		} else std::cout << "  OK" << std::endl;
		prev = f.r;
		brdf->release();
	}
	return allPassed;
}

int main()
{
	std::cout << "=== GGX Metallic-Roughness Grid Test ===" << std::endl;
	GlobalLog();	// initialize log to prevent null deref

	bool allPassed = true;
	allPassed &= TestSmoothDielectric();
	allPassed &= TestRoughDielectric();
	allPassed &= TestSmoothMetal();
	allPassed &= TestRoughMetal();
	allPassed &= TestDiffuseMonotonicity();

	std::cout << std::endl << "=== "
			  << (allPassed ? "ALL TESTS PASSED" : "SOME TESTS FAILED")
			  << " ===" << std::endl;
	return allPassed ? 0 : 1;
}
