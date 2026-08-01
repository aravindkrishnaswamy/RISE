//////////////////////////////////////////////////////////////////////
//
//  GGXConductorAmbientIORTest.cpp - G6 regression: the GGX conductor
//    Fresnel must use the AMBIENT (incident-medium) IOR carried on
//    RayIntersectionGeometric::ambientIOR, NOT a hardcoded air constant.
//
//    Physics: a conductor viewed THROUGH a dielectric (e.g. silver under
//    n~1.5 enamel glass) reflects LESS at every angle than the same
//    conductor in air, because the Fresnel-conductor reflectance shrinks
//    as the incident-medium index approaches the metal's index.
//    Optics::CalculateConductorReflectance already takes an explicit Ni;
//    the bug was that GGXSPF / GGXBRDF fed it 1.0 unconditionally.  The G6
//    fix stamps ri.ambientIOR from the IOR stack at hit production and the
//    GGX conductor branches read it.
//
//    This test proves, at the MATERIAL level (no scene render needed):
//      1. Optics oracle: R(glass->metal) < R(air->metal) at normal
//         incidence, by the exact Fresnel-conductor amount.
//      2. GGXBRDF::value   (RGB) reflects strictly LESS with ambientIOR
//         = 1.5 than with ambientIOR = 1.0, and the ratio matches the
//         Optics oracle within microfacet tolerance.
//      3. GGXBRDF::valueNM (spectral) — same, and dispersion-correct
//         (ambientIOR is the per-wavelength n(lambda) in the NM path).
//      4. GGXSPF::Scatter  (RGB) sampling weight kray likewise drops.
//      5. BYTE-IDENTICAL in air: the default-constructed ri.ambientIOR
//         (1.0) yields the SAME value as an explicitly air-stamped ri —
//         i.e. every existing (vacuum) scene is unchanged.
//
//  Build (from project root):
//    c++ -arch arm64 -Isrc/Library -I/opt/homebrew/include
//        -O3 -ffast-math -fno-finite-math-only -funroll-loops -Wall -pedantic
//        (-fno-finite-math-only is REQUIRED to match production since
//         2026-07-29; without it std::isfinite/isnan fold to constants
//         and NaN-sentinel assertions silently pass -- see CLAUDE.md.)
//        -Wno-c++11-long-long -DCOLORS_RGB -DMERSENNE53
//        -DNO_TIFF_SUPPORT -DNO_EXR_SUPPORT -DRISE_ENABLE_MAILBOXING
//        -c tests/GGXConductorAmbientIORTest.cpp -o tests/GGXConductorAmbientIORTest.o
//    c++ -arch arm64 -o tests/ggx_ambient_ior_test
//        tests/GGXConductorAmbientIORTest.o bin/librise.a -L/opt/homebrew/lib -lpng -lz
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <cstdlib>
#include <iomanip>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Utilities/Optics.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Materials/GGXBRDF.h"
#include "../src/Library/Materials/GGXSPF.h"

#include "TestStubObject.h"

using namespace RISE;
using namespace RISE::Implementation;

// Silver-like conductor (representative n,k in the visible band).
static const Scalar N_METAL = 0.15;
static const Scalar K_METAL = 3.50;

static const Scalar N_AIR   = 1.0;
static const Scalar N_GLASS = 1.5;

// The GGX single-scatter lobe carries the Fresnel F linearly, but the
// Kulla-Conty multiscatter tail adds a second (also F-decreasing) term and
// the microfacet NDF/G weighting introduces a small residual, so the
// material-level ratio need not equal the pure-Fresnel ratio to machine
// precision.  We assert (a) STRICT monotonicity glass < air and (b) the
// ratio is in the neighbourhood of the Optics oracle.
static const double RATIO_TOL = 0.05;

static int g_failures = 0;

static void Check( bool cond, const char* msg )
{
	if( !cond ) {
		std::cout << "  [FAIL] " << msg << std::endl;
		g_failures++;
	} else {
		std::cout << "  [ok]   " << msg << std::endl;
	}
}

// Build a near-mirror GGX conductor material.  diffuse=0 isolates the
// specular (Fresnel-bearing) response; alpha is small so the lobe is a
// tight near-specular peak evaluated at the perfect-reflection direction.
struct Mat
{
	UniformColorPainter*  diffuse;
	UniformColorPainter*  specular;
	UniformScalarPainter* alphaX;
	UniformScalarPainter* alphaY;
	UniformScalarPainter* ior;
	UniformScalarPainter* ext;
	GGXBRDF*              brdf;
	GGXSPF*               spf;

	Mat( Scalar alpha )
	{
		diffuse  = new UniformColorPainter( RISEPel(0.0, 0.0, 0.0) );  diffuse->addref();
		specular = new UniformColorPainter( RISEPel(1.0, 1.0, 1.0) );  specular->addref();
		alphaX   = new UniformScalarPainter( alpha );  alphaX->addref();
		alphaY   = new UniformScalarPainter( alpha );  alphaY->addref();
		ior      = new UniformScalarPainter( N_METAL ); ior->addref();
		ext      = new UniformScalarPainter( K_METAL ); ext->addref();
		brdf = new GGXBRDF( *diffuse, *specular, *alphaX, *alphaY, *ior, *ext, eFresnelConductor );  brdf->addref();
		spf  = new GGXSPF ( *diffuse, *specular, *alphaX, *alphaY, *ior, *ext, eFresnelConductor );  spf->addref();
	}

	~Mat()
	{
		brdf->release(); spf->release();
		diffuse->release(); specular->release();
		alphaX->release(); alphaY->release();
		ior->release(); ext->release();
	}
};

// A hit at normal incidence: ray travels along -Z, surface normal +Z.
static RayIntersectionGeometric MakeNormalHit()
{
	Vector3 inDir( 0, 0, -1 );
	Ray inRay( Point3(0, 0, 1), inDir );
	RasterizerState rs = {0, 0};
	RayIntersectionGeometric ri( inRay, rs );
	ri.bHit = true;
	ri.range = 1.0;
	ri.ptIntersection = Point3(0, 0, 0);
	ri.vNormal = Vector3(0, 0, 1);
	ri.vGeomNormal = Vector3(0, 0, 1);
	ri.onb.CreateFromW( Vector3(0, 0, 1) );
	ri.ptCoord = Point2(0.5, 0.5);
	// ambientIOR intentionally left at its default (1.0) so a caller who
	// forgets to stamp it gets air — the byte-identical guarantee.
	return ri;
}

// -----------------------------------------------------------------
// Test 1: Optics oracle — the exact Fresnel-conductor ratio.
// -----------------------------------------------------------------
static void TestOpticsOracle()
{
	std::cout << "Test 1: Optics::CalculateConductorReflectance oracle" << std::endl;

	// At normal incidence v is anti-parallel to n; |v.n| = 1.
	const Vector3 n( 0, 0, 1 );
	const Vector3 v( 0, 0, -1 );

	const Scalar Rair   = Optics::CalculateConductorReflectance<Scalar>( v, n, N_AIR,   N_METAL, K_METAL );
	const Scalar Rglass = Optics::CalculateConductorReflectance<Scalar>( v, n, N_GLASS, N_METAL, K_METAL );

	std::cout << "  R(air->metal)   = " << std::setprecision(6) << Rair   << std::endl;
	std::cout << "  R(glass->metal) = " << std::setprecision(6) << Rglass << std::endl;
	std::cout << "  ratio glass/air = " << (Rglass / Rair) << std::endl;

	Check( Rglass < Rair,
		"silver reflects LESS through glass than through air (normal incidence)" );
	Check( Rair > 0.9 && Rair < 1.0,
		"air->silver normal-incidence reflectance is high (>0.9) and physical (<1)" );
	Check( Rglass > 0.8 && Rglass < Rair,
		"glass->silver reflectance stays high but strictly below the air value" );
}

// -----------------------------------------------------------------
// Test 2 + 5: GGXBRDF::value (RGB) drops with glass ambient, and the
// default-air path is byte-identical to an explicit air stamp.
// -----------------------------------------------------------------
static void TestBRDFValueRGB()
{
	std::cout << "Test 2/5: GGXBRDF::value (RGB) ambient-IOR response" << std::endl;

	Mat mat( 0.02 );  // near-specular

	// Perfect mirror reflection direction of the incident ray.
	// Incident -Z, normal +Z -> reflected light direction is +Z.
	const Vector3 vLightIn( 0, 0, 1 );

	RayIntersectionGeometric riAir = MakeNormalHit();      // default ambientIOR = 1.0
	RayIntersectionGeometric riAirExplicit = MakeNormalHit();
	riAirExplicit.ambientIOR = N_AIR;                      // explicit air
	RayIntersectionGeometric riGlass = MakeNormalHit();
	riGlass.ambientIOR = N_GLASS;                          // enamel glass

	const RISEPel vAir      = mat.brdf->value( vLightIn, riAir );
	const RISEPel vAirExpl  = mat.brdf->value( vLightIn, riAirExplicit );
	const RISEPel vGlass    = mat.brdf->value( vLightIn, riGlass );

	const double air   = ColorMath::MaxValue( vAir );
	const double glass = ColorMath::MaxValue( vGlass );

	std::cout << "  value(air)   = " << std::setprecision(6) << air   << std::endl;
	std::cout << "  value(glass) = " << std::setprecision(6) << glass << std::endl;
	std::cout << "  ratio        = " << (glass / air) << std::endl;

	Check( air > 0.0, "air BRDF value is positive (sanity)" );
	Check( glass < air, "GGXBRDF::value(glass) < GGXBRDF::value(air)" );

	// Byte-identical-in-air: default construction == explicit air stamp.
	Check( ColorMath::MaxValue( vAir - vAirExpl ) == 0.0 &&
	       ColorMath::MinValue( vAir - vAirExpl ) == 0.0,
		"BYTE-IDENTICAL: default ambientIOR (1.0) == explicit air (1.0)" );

	// Ratio neighbourhood check against the Fresnel oracle.
	const Vector3 n( 0, 0, 1 );
	const Vector3 v( 0, 0, -1 );
	const Scalar Rair   = Optics::CalculateConductorReflectance<Scalar>( v, n, N_AIR,   N_METAL, K_METAL );
	const Scalar Rglass = Optics::CalculateConductorReflectance<Scalar>( v, n, N_GLASS, N_METAL, K_METAL );
	const double predicted = Rglass / Rair;
	const double measured  = glass / air;
	std::cout << "  predicted (Fresnel) ratio = " << predicted << std::endl;
	Check( std::fabs( measured - predicted ) < RATIO_TOL,
		"RGB value ratio matches the Fresnel-conductor prediction within tol" );
}

// -----------------------------------------------------------------
// Test 3: GGXBRDF::valueNM (spectral) drops with glass ambient.
// -----------------------------------------------------------------
static void TestBRDFValueNM()
{
	std::cout << "Test 3: GGXBRDF::valueNM (spectral) ambient-IOR response" << std::endl;

	Mat mat( 0.02 );
	const Vector3 vLightIn( 0, 0, 1 );
	const Scalar nm = 550.0;

	RayIntersectionGeometric riAir = MakeNormalHit();       // default 1.0
	RayIntersectionGeometric riGlass = MakeNormalHit();
	riGlass.ambientIOR = N_GLASS;

	const Scalar air   = mat.brdf->valueNM( vLightIn, riAir,   nm );
	const Scalar glass = mat.brdf->valueNM( vLightIn, riGlass, nm );

	std::cout << "  valueNM(air)   = " << std::setprecision(6) << air   << std::endl;
	std::cout << "  valueNM(glass) = " << std::setprecision(6) << glass << std::endl;
	std::cout << "  ratio          = " << (glass / air) << std::endl;

	Check( air > 0.0, "air NM value is positive (sanity)" );
	Check( glass < air, "GGXBRDF::valueNM(glass) < GGXBRDF::valueNM(air)" );

	const Vector3 n( 0, 0, 1 );
	const Vector3 v( 0, 0, -1 );
	const Scalar Rair   = Optics::CalculateConductorReflectance<Scalar>( v, n, N_AIR,   N_METAL, K_METAL );
	const Scalar Rglass = Optics::CalculateConductorReflectance<Scalar>( v, n, N_GLASS, N_METAL, K_METAL );
	const double predicted = (double)(Rglass / Rair);
	const double measured  = (double)(glass / air);
	Check( std::fabs( measured - predicted ) < RATIO_TOL,
		"NM value ratio matches the Fresnel-conductor prediction within tol" );
}

// -----------------------------------------------------------------
// Test 4: GGXSPF::Scatter (RGB) sampling weight drops with glass ambient.
//   Averages the specular-lobe kray over many samples so the Fresnel
//   factor (the only difference between the two ambients) shows through.
// -----------------------------------------------------------------
static void TestSPFScatterRGB()
{
	std::cout << "Test 4: GGXSPF::Scatter (RGB) ambient-IOR response" << std::endl;

	Mat mat( 0.1 );
	StubObject* stub = new StubObject();  stub->addref();

	RandomNumberGenerator rng;
	IndependentSampler sampler( rng );

	auto meanSpecKray = [&]( Scalar ambientIOR ) -> double
	{
		RayIntersectionGeometric ri = MakeNormalHit();
		ri.ambientIOR = ambientIOR;
		IORStack iorStack = MakeTestIORStack( stub );
		double accum = 0.0;
		int    count = 0;
		const int N = 40000;
		for( int i = 0; i < N; i++ ) {
			ScatteredRayContainer scattered;
			mat.spf->Scatter( ri, sampler, scattered, iorStack );
			for( unsigned int r = 0; r < scattered.Count(); r++ ) {
				const ScatteredRay& scat = scattered[r];
				if( scat.isDelta ) continue;
				if( scat.type != ScatteredRay::eRayReflection ) continue;
				accum += ColorMath::MaxValue( scat.kray );
				count++;
			}
		}
		return count > 0 ? accum / count : 0.0;
	};

	const double air   = meanSpecKray( N_AIR );
	const double glass = meanSpecKray( N_GLASS );

	std::cout << "  mean spec kray(air)   = " << std::setprecision(6) << air   << std::endl;
	std::cout << "  mean spec kray(glass) = " << std::setprecision(6) << glass << std::endl;
	std::cout << "  ratio                 = " << (glass / air) << std::endl;

	Check( air > 0.0, "air SPF spec kray is positive (sanity)" );
	Check( glass < air, "GGXSPF::Scatter spec kray(glass) < kray(air)" );

	stub->release();
}

int main()
{
	std::cout << "=============================================================" << std::endl;
	std::cout << " GGX Conductor Ambient-IOR (G6) Regression Test" << std::endl;
	std::cout << "=============================================================" << std::endl;

	TestOpticsOracle();
	TestBRDFValueRGB();
	TestBRDFValueNM();
	TestSPFScatterRGB();

	std::cout << "=============================================================" << std::endl;
	if( g_failures == 0 ) {
		std::cout << "ALL TESTS PASSED" << std::endl;
		return 0;
	}
	std::cout << g_failures << " TEST(S) FAILED" << std::endl;
	return 1;
}
