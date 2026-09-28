//////////////////////////////////////////////////////////////////////
//
//  SSSExteriorIndexInvarianceTest.cpp - Regression guard for DL-49:
//    the SSS boundary is an interface between the material and the
//    medium the ray arrived through, not between the material and air.
//
//  THE DEFECT
//
//    `SubSurfaceScatteringSPF`'s surface reflection has always priced
//    its Fresnel against `ior_stack.top()` -- the real exterior -- but
//    every OTHER boundary quantity of the same SSS event used the
//    material's absolute index against a hardcoded 1.0 (air): the
//    diffusion profiles' `FresnelTransmission` and the Sw normalization
//    built from `GetIOR`, `RandomWalkSSS::SampleExit`'s entry/exit Snell
//    refraction and exit Fresnel, the PT/BDPT random-walk entry coin,
//    the `BSSRDFEntryAdapters.h` NEE adapters (also used by BDPT's
//    zero-exitance sweep, DL-207), `PathVertexEval`'s BSSRDF-entry
//    re-evaluation, and the rough `SubSurfaceScatteringBSDF`.  An SSS
//    body immersed in a non-air medium therefore priced two different
//    interfaces within one boundary event, and a common scaling of the
//    exterior and interior indices -- which leaves every relative-index
//    physical quantity unchanged -- changed the image.
//
//  THE INVARIANT (reference-free)
//
//    Scale every index by the same factor s: exterior 1 -> s, interior
//    n -> s*n.  Snell directions, exact dielectric Fresnel and every
//    Schlick law of the RELATIVE index are unchanged, and the complete
//    SSS event carries no unmatched eta^2 (DL-04), so every weight, every
//    sampled direction and every rendered pixel must be unchanged.  This
//    needs no reference image and no closed form.
//
//  WHAT EACH PART PROVES
//
//    Part A -- deterministic, function level (exact to rounding):
//      A1  profile FresnelTransmission is scale invariant; matched index
//          (relative 1) transmits exactly 1 at normal incidence; a denser
//          exterior is totally reflected past its critical angle.
//      A2  Sw normalization integrates to exactly one over the exterior
//          cosine hemisphere at relative indices below, at, and above 1
//          (the eta < 1 closed form eta^2 * 20(1-F0)/21 is new in DL-49).
//      A3  both NEE entry adapters are scale invariant (RGB and NM) and,
//          under a DENSER exterior (1.33 inside 1.5), transmit nothing past
//          the critical angle and equal their scaled-down air twin.
//      A4  the rough SubSurfaceScatteringBSDF is scale invariant and is
//          exactly zero at a matched index (no interface, no reflection).
//      A5  BSSRDFSampling::SampleEntryPoint, seeded twins: identical
//          weights at (1, 1.33) and (1.5, 1.995), RGB and NM.
//      A6  RandomWalkSSS::SampleExit, seeded twins: identical weights,
//          exit points and continuation directions, RGB and NM.
//      A7  RandomWalkSSS::SampleExit at a MATCHED index with a vanishing
//          medium: the walk goes straight through (no refraction bend,
//          no exit Fresnel) and exits exactly on the incident ray's
//          chord through the sphere.
//      A8  PathVertexEval::EvalBSDFAtVertex{,NM} at a BSSRDF entry vertex
//          (the BDPT/VCM connection re-evaluation) is scale invariant and
//          honours `BDPTVertex::mediumIOR` under a denser exterior.
//    NOTE on A3/A8: for a relative index >= 1 the normalized Sw does not
//    depend on the index at all ((1-F0) cancels against c), so their
//    scale-invariance rows are consistency pins that pass before DL-49
//    too; the denser-exterior rows are the discriminating half.
//    Part B -- rendered, end to end:
//      An SSS sphere, a spherical area light and a pinhole camera inside a
//      black absorbing room, all in air versus all inside an ideal
//      non-reflecting enclosure of index 1.5 with every interior index
//      scaled by 1.5 (one relative index, two absolute scales).  PT RGB,
//      BDPT RGB and PT spectral; smooth diffusion (the subsurface event
//      alone), rough diffusion (adds the BSDF NEE lobe, whose pre-DL-49
//      error has the OPPOSITE sign and partially cancels in a mean -- so
//      it gets its own, tighter band), random walk, and "dense" rows with
//      the relative index below 1.  The enclosed/air ratio of the image
//      mean must be 1.  A Lambertian control row (no index anywhere) pins
//      that the enclosure itself changes nothing else.
//    Usage: [--unit-only] [--trials K (default 4)] [--only <label substring>]
//
//  Author: RISE debt-cleanup, slice `debt-dl49`
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#ifdef _WIN32
	#include <process.h>
	#define getpid _getpid
#else
	#include <unistd.h>
#endif

#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Interfaces/ICamera.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Math3D/Constants.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/BSSRDFSampling.h"
#include "../src/Library/Utilities/RandomWalkSSS.h"
#include "../src/Library/Utilities/SSSCoefficients.h"
#include "../src/Library/Utilities/PathVertexEval.h"
#include "../src/Library/Utilities/IORStack.h"
#include "../src/Library/Utilities/IORStackSeeding.h"
#include "../src/Library/Intersection/RayIntersection.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Painters/RGBScalarPainter.h"
#include "../src/Library/Materials/BurleyNormalizedDiffusionProfile.h"
#include "../src/Library/Materials/SubSurfaceScatteringMaterial.h"
#include "../src/Library/Materials/SubSurfaceScatteringBSDF.h"
#include "../src/Library/Materials/RandomWalkSSSMaterial.h"
#include "../src/Library/Materials/DonnerJensenSkinDiffusionProfile.h"
#include "../src/Library/Materials/MultipoleDiffusion.h"
#include "../src/Library/Shaders/SSS/PointSetOctree.h"
#include "../src/Library/Shaders/SSS/DiffusionApproximationExtinction.h"
#include "../src/Library/Shaders/BSSRDFEntryAdapters.h"
#include "../src/Library/Shaders/BDPTVertex.h"
#include "../src/Library/Utilities/Color/Color_Template.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE { bool RISE_CreateJobPriv( IJobPriv** ppi ); }

namespace
{
	int passCount = 0;
	int failCount = 0;

	void Check( const bool condition, const std::string& label )
	{
		if( condition ) {
			++passCount;
		} else {
			++failCount;
			std::cout << "  FAILED: " << label << std::endl;
		}
	}

	// Air configuration and its uniformly scaled twin (relative 1.33 both).
	const Scalar kAirExterior = 1.0;
	const Scalar kAirInterior = 1.33;
	const Scalar kScale = 1.5;
	const Scalar kScaledExterior = kAirExterior * kScale;	// 1.5
	const Scalar kScaledInterior = kAirInterior * kScale;	// 1.995

	bool RelClose( const Scalar a, const Scalar b, const Scalar tol )
	{
		if( !std::isfinite( a ) || !std::isfinite( b ) ) return false;
		return std::fabs( a - b ) <= tol * std::fmax( Scalar( 1e-300 ), std::fmax( std::fabs( a ), std::fabs( b ) ) )
			|| std::fabs( a - b ) <= Scalar( 1e-300 );
	}

	class TestSampler : public ISampler
	{
		RandomNumberGenerator rng;
	public:
		explicit TestSampler( const unsigned int seed ) : rng( seed ) {}
		Scalar Get1D() { return rng.CanonicalRandom(); }
		Point2 Get2D() { return Point2( Get1D(), Get1D() ); }
	};

	//! A front-face hit at the bottom of the unit sphere, ray travelling +Y.
	RayIntersectionGeometric MakeSurfaceRI( const Scalar exteriorIOR )
	{
		const Point3 point( 0, -1, 0 );
		const Vector3 normal( 0, -1, 0 );
		const Vector3 incoming( 0, 1, 0 );
		RayIntersectionGeometric ri(
			Ray( Point3Ops::mkPoint3( point, -incoming * 2.0 ), incoming ),
			nullRasterizerState );
		ri.bHit = true;
		ri.ptIntersection = point;
		ri.vNormal = normal;
		ri.vGeomNormal = normal;
		ri.onb.CreateFromW( normal );
		ri.ambientIOR = exteriorIOR;
		return ri;
	}

	//! An exterior-side direction at the given cosine against MakeSurfaceRI's normal.
	Vector3 DirectionForCosine( const Scalar cosine )
	{
		return Vector3( sqrt( 1.0 - cosine * cosine ), -cosine, 0 );
	}

	Object* MakeUnitSphere()
	{
		SphereGeometry* geometry = new SphereGeometry( 1.0 );
		geometry->addref();
		Object* object = new Object( geometry );
		object->addref();
		geometry->release();
		return object;
	}

	struct ProfileBundle
	{
		UniformScalarPainter* ior;
		RGBScalarPainter* absorption;
		RGBScalarPainter* scattering;
		BurleyNormalizedDiffusionProfile* profile;

		explicit ProfileBundle( const Scalar n )
		{
			ior = new UniformScalarPainter( n ); ior->addref();
			absorption = new RGBScalarPainter( 0.05, 0.10, 0.20 ); absorption->addref();
			scattering = new RGBScalarPainter( 1.0, 1.0, 1.0 ); scattering->addref();
			profile = new BurleyNormalizedDiffusionProfile( *ior, *absorption, *scattering, 0.0 );
			profile->addref();
		}
		~ProfileBundle()
		{
			profile->release(); ior->release(); absorption->release(); scattering->release();
		}
	};

	//////////////////////////////////////////////////////////////////
	// A1 -- profile FresnelTransmission
	//////////////////////////////////////////////////////////////////
	void TestProfileFresnel()
	{
		std::cout << "A1: profile FresnelTransmission is a function of the relative index" << std::endl;
		ProfileBundle air( kAirInterior ), scaled( kScaledInterior );
		const RayIntersectionGeometric riAir = MakeSurfaceRI( kAirExterior );
		const RayIntersectionGeometric riScaled = MakeSurfaceRI( kScaledExterior );
		const Scalar mus[] = { 1.0, 0.8, 0.5, 0.2, 0.05 };
		Scalar worst = 0;
		for( const Scalar mu : mus ) {
			const Scalar a = air.profile->FresnelTransmission( mu, riAir );
			const Scalar s = scaled.profile->FresnelTransmission( mu, riScaled );
			worst = std::fmax( worst, std::fabs( a - s ) / a );
			Check( RelClose( a, s, 1e-12 ), "A1: Ft(mu) at (1, 1.33) == Ft(mu) at (1.5, 1.995)" );
		}
		std::cout << "    worst |Ft_scaled - Ft_air| / Ft_air = " << worst << std::endl;

		// Matched index: no interface.  Schlick at F0 = 0 transmits exactly 1
		// at normal incidence (a pre-DL-49 build reads 0.96, the air value).
		ProfileBundle glass( 1.5 );
		const RayIntersectionGeometric riGlass = MakeSurfaceRI( 1.5 );
		const Scalar ftMatched = glass.profile->FresnelTransmission( 1.0, riGlass );
		std::cout << "    matched index (1.5 in 1.5) Ft(1) = " << std::setprecision( 12 ) << ftMatched << std::endl;
		Check( ftMatched == 1.0, "A1: matched relative index transmits exactly 1 at normal incidence" );

		// A denser exterior: 1.33 inside 1.5.  Critical exterior cosine is
		// sqrt(1 - (1.33/1.5)^2) = 0.4623; below it the interface reflects all.
		ProfileBundle water( 1.33 );
		const Scalar ftTIR = water.profile->FresnelTransmission( 0.3, riGlass );
		const Scalar ftIn = water.profile->FresnelTransmission( 0.9, riGlass );
		std::cout << "    1.33 inside 1.5: Ft(0.3) = " << ftTIR << "  Ft(0.9) = " << ftIn << std::endl;
		Check( ftTIR == 0.0, "A1: denser exterior totally reflects past the critical angle" );
		Check( ftIn > 0.9 && ftIn < 1.0, "A1: denser exterior transmits inside the critical cone" );

		// A record that never had an exterior stamped is air (stackless
		// fallback): identical to the explicit-air record, bit for bit.
		RayIntersectionGeometric riDefault = MakeSurfaceRI( 1.0 );
		riDefault.ambientIOR = RayIntersectionGeometric( Ray(), nullRasterizerState ).ambientIOR;
		Check( air.profile->FresnelTransmission( 0.37, riDefault ) ==
			air.profile->FresnelTransmission( 0.37, riAir ), "A1: stackless record falls back to air exactly" );
	}

	//////////////////////////////////////////////////////////////////
	// A2 -- Sw normalization at relative index below / at / above 1
	//////////////////////////////////////////////////////////////////
	template<typename Evaluate>
	Scalar IntegrateHemisphere( const Evaluate& evaluate )
	{
		// Midpoint rule in cos(theta).  For eta < 1 the integrand has a
		// square-root edge at the critical cosine; 2^18 bins bound that
		// quadrature error far below the 1e-5 tolerance used here.
		const int count = 1 << 18;
		Scalar sum = 0;
		for( int i = 0; i < count; ++i ) {
			const Scalar cosine = ( Scalar( i ) + 0.5 ) / count;
			sum += TWO_PI * cosine * evaluate( cosine );
		}
		return sum / count;
	}

	void TestSwNormalization()
	{
		std::cout << "A2: Sw integrates to one over the exterior hemisphere at every relative index" << std::endl;
		struct Case { Scalar interior, exterior; };
		const Case cases[] = { { 1.33, 1.5 }, { 1.5, 1.5 }, { 1.33, 1.0 }, { 1.995, 1.5 }, { 1.2, 2.0 } };
		for( const Case& c : cases ) {
			ProfileBundle bundle( c.interior );
			const RayIntersectionGeometric ri = MakeSurfaceRI( c.exterior );
			BSSRDFAdapters::BSSRDFEntryBSDF diffusion( bundle.profile, c.interior );
			BSSRDFAdapters::RandomWalkEntryBSDF walk( c.interior );
			const Scalar iDiffRGB = IntegrateHemisphere( [&]( Scalar mu ) { return diffusion.value( DirectionForCosine( mu ), ri )[0]; } );
			const Scalar iDiffNM = IntegrateHemisphere( [&]( Scalar mu ) { return diffusion.valueNM( DirectionForCosine( mu ), ri, 550.0 ); } );
			const Scalar iWalkRGB = IntegrateHemisphere( [&]( Scalar mu ) { return walk.value( DirectionForCosine( mu ), ri )[0]; } );
			const Scalar iWalkNM = IntegrateHemisphere( [&]( Scalar mu ) { return walk.valueNM( DirectionForCosine( mu ), ri, 550.0 ); } );
			std::cout << "    n_s=" << c.interior << " n_e=" << c.exterior << " eta=" << c.interior / c.exterior
				<< "  diffusion RGB/NM=" << std::setprecision( 9 ) << iDiffRGB << "/" << iDiffNM
				<< "  walk RGB/NM=" << iWalkRGB << "/" << iWalkNM << std::endl;
			const std::string tag = " (n_s=" + std::to_string( c.interior ) + ", n_e=" + std::to_string( c.exterior ) + ")";
			Check( std::fabs( iDiffRGB - 1.0 ) < 1e-5, "A2: BSSRDFEntryBSDF RGB unit integral" + tag );
			Check( std::fabs( iDiffNM - 1.0 ) < 1e-5, "A2: BSSRDFEntryBSDF NM unit integral" + tag );
			Check( std::fabs( iWalkRGB - 1.0 ) < 1e-5, "A2: RandomWalkEntryBSDF RGB unit integral" + tag );
			Check( std::fabs( iWalkNM - 1.0 ) < 1e-5, "A2: RandomWalkEntryBSDF NM unit integral" + tag );
		}
		// The eta < 1 closed form agrees with the eta >= 1 one at eta = 1.
		Check( std::fabs( BSSRDFSampling::SchlickTransmissionNormalization( 1.0 - 1e-12 ) -
			BSSRDFSampling::SchlickTransmissionNormalization( 1.0 ) ) < 1e-10,
			"A2: normalization is continuous across eta = 1" );
	}

	//////////////////////////////////////////////////////////////////
	// A3 -- NEE entry adapters, scale invariance
	//////////////////////////////////////////////////////////////////
	void TestAdapters()
	{
		std::cout << "A3: entry adapters are scale invariant" << std::endl;
		ProfileBundle air( kAirInterior ), scaled( kScaledInterior );
		const RayIntersectionGeometric riAir = MakeSurfaceRI( kAirExterior );
		const RayIntersectionGeometric riScaled = MakeSurfaceRI( kScaledExterior );
		BSSRDFAdapters::BSSRDFEntryBSDF dAir( air.profile, kAirInterior ), dScaled( scaled.profile, kScaledInterior );
		BSSRDFAdapters::RandomWalkEntryBSDF wAir( kAirInterior ), wScaled( kScaledInterior );
		Scalar worstD = 0, worstW = 0;
		for( const Scalar mu : { 0.95, 0.6, 0.3, 0.1 } ) {
			const Vector3 d = DirectionForCosine( mu );
			const Scalar a = dAir.value( d, riAir )[0], s = dScaled.value( d, riScaled )[0];
			const Scalar aNM = dAir.valueNM( d, riAir, 550 ), sNM = dScaled.valueNM( d, riScaled, 550 );
			const Scalar wa = wAir.value( d, riAir )[0], ws = wScaled.value( d, riScaled )[0];
			const Scalar waNM = wAir.valueNM( d, riAir, 550 ), wsNM = wScaled.valueNM( d, riScaled, 550 );
			worstD = std::fmax( worstD, std::fabs( a - s ) / a );
			worstW = std::fmax( worstW, std::fabs( wa - ws ) / wa );
			Check( RelClose( a, s, 1e-12 ) && RelClose( aNM, sNM, 1e-12 ), "A3: BSSRDFEntryBSDF RGB/NM scale invariant" );
			Check( RelClose( wa, ws, 1e-12 ) && RelClose( waNM, wsNM, 1e-12 ), "A3: RandomWalkEntryBSDF RGB/NM scale invariant" );
		}
		std::cout << "    worst relative difference: diffusion " << worstD << "  random walk " << worstW << std::endl;

		// For a relative index >= 1 the normalized Sw does not depend on the
		// index at all ((1-F0) cancels against c), so the checks above are a
		// CONSISTENCY PIN that passes before DL-49 too.  The discriminating
		// case is a DENSER exterior: 1.33 inside 1.5 has a critical exterior
		// cosine of sqrt(1 - (1.33/1.5)^2) = 0.4623, below which no light
		// crosses, so Sw must vanish there -- and must equal its own
		// scaled-down air twin (interior 1.33/1.5 in air) everywhere.
		ProfileBundle dense( 1.33 ), denseAir( 1.33 / 1.5 );
		const RayIntersectionGeometric riGlass = MakeSurfaceRI( 1.5 );
		BSSRDFAdapters::BSSRDFEntryBSDF dDense( dense.profile, 1.33 ), dDenseAir( denseAir.profile, 1.33 / 1.5 );
		BSSRDFAdapters::RandomWalkEntryBSDF wDense( 1.33 ), wDenseAir( 1.33 / 1.5 );
		const Vector3 grazing = DirectionForCosine( 0.3 );
		const Vector3 steep = DirectionForCosine( 0.9 );
		std::cout << "    1.33 inside 1.5: diffusion Sw(0.3)=" << dDense.value( grazing, riGlass )[0]
			<< " Sw(0.9)=" << dDense.value( steep, riGlass )[0]
			<< "  walk Sw(0.3)=" << wDense.value( grazing, riGlass )[0] << " Sw(0.9)=" << wDense.value( steep, riGlass )[0] << std::endl;
		Check( dDense.value( grazing, riGlass )[0] == 0.0 && dDense.valueNM( grazing, riGlass, 550 ) == 0.0,
			"A3: BSSRDFEntryBSDF RGB/NM: denser exterior, no transmission past the critical angle" );
		Check( wDense.value( grazing, riGlass )[0] == 0.0 && wDense.valueNM( grazing, riGlass, 550 ) == 0.0,
			"A3: RandomWalkEntryBSDF RGB/NM: denser exterior, no transmission past the critical angle" );
		for( const Scalar mu : { 0.95, 0.7, 0.5 } ) {
			const Vector3 d = DirectionForCosine( mu );
			Check( RelClose( dDense.value( d, riGlass )[0], dDenseAir.value( d, riAir )[0], 1e-12 ) &&
				dDense.value( d, riGlass )[0] > 0, "A3: BSSRDFEntryBSDF denser exterior equals its air twin" );
			Check( RelClose( wDense.value( d, riGlass )[0], wDenseAir.value( d, riAir )[0], 1e-12 ) &&
				wDense.value( d, riGlass )[0] > 0, "A3: RandomWalkEntryBSDF denser exterior equals its air twin" );
		}
	}

	//////////////////////////////////////////////////////////////////
	// A4 -- rough SubSurfaceScatteringBSDF
	//////////////////////////////////////////////////////////////////
	void TestRoughBSDF()
	{
		std::cout << "A4: rough SubSurfaceScatteringBSDF is a relative-index interface" << std::endl;
		UniformScalarPainter* nAir = new UniformScalarPainter( kAirInterior ); nAir->addref();
		UniformScalarPainter* nScaled = new UniformScalarPainter( kScaledInterior ); nScaled->addref();
		UniformScalarPainter* nGlass = new UniformScalarPainter( 1.5 ); nGlass->addref();
		SubSurfaceScatteringBSDF* bAir = new SubSurfaceScatteringBSDF( *nAir, 0.0, 0.3 ); bAir->addref();
		SubSurfaceScatteringBSDF* bScaled = new SubSurfaceScatteringBSDF( *nScaled, 0.0, 0.3 ); bScaled->addref();
		SubSurfaceScatteringBSDF* bGlass = new SubSurfaceScatteringBSDF( *nGlass, 0.0, 0.3 ); bGlass->addref();

		// The viewer ray arrives at 30 degrees; light from several directions.
		RayIntersectionGeometric riAir = MakeSurfaceRI( kAirExterior );
		const Vector3 view = Vector3Ops::Normalize( Vector3( -0.5, 0.866025403784, 0 ) );
		riAir.ray = Ray( Point3Ops::mkPoint3( riAir.ptIntersection, -view * 2.0 ), view );
		RayIntersectionGeometric riScaled = riAir; riScaled.ambientIOR = kScaledExterior;
		RayIntersectionGeometric riGlass = riAir; riGlass.ambientIOR = 1.5;
		Scalar worst = 0, maxGlass = 0, minAir = 1e300;
		for( const Scalar mu : { 0.9, 0.7, 0.5, 0.3 } ) {
			const Vector3 light = DirectionForCosine( mu );
			const Scalar a = bAir->value( light, riAir )[0], s = bScaled->value( light, riScaled )[0];
			const Scalar aNM = bAir->valueNM( light, riAir, 550 ), sNM = bScaled->valueNM( light, riScaled, 550 );
			worst = std::fmax( worst, std::fabs( a - s ) / a );
			minAir = std::fmin( minAir, a );
			Check( RelClose( a, s, 1e-12 ) && RelClose( aNM, sNM, 1e-12 ), "A4: rough BSDF RGB/NM scale invariant" );
			const Scalar g = bGlass->value( light, riGlass )[0], gNM = bGlass->valueNM( light, riGlass, 550 );
			maxGlass = std::fmax( maxGlass, std::fmax( g, gNM ) );
		}
		std::cout << "    worst relative difference " << worst << "; matched-index max value " << maxGlass
			<< " (air control min " << minAir << ")" << std::endl;
		Check( minAir > 0, "A4: (sanity) the air configuration reflects" );
		Check( maxGlass == 0.0, "A4: matched index has no interface reflection" );
		bAir->release(); bScaled->release(); bGlass->release();
		nAir->release(); nScaled->release(); nGlass->release();
	}

	//////////////////////////////////////////////////////////////////
	// A5 -- SampleEntryPoint seeded twins
	//////////////////////////////////////////////////////////////////
	void TestSampleEntryPointTwins()
	{
		std::cout << "A5: BSSRDFSampling::SampleEntryPoint seeded twins agree" << std::endl;
		Object* sphere = MakeUnitSphere();
		RGBScalarPainter* absorption = new RGBScalarPainter( 0.05, 0.10, 0.20 ); absorption->addref();
		RGBScalarPainter* scattering = new RGBScalarPainter( 1.0, 1.0, 1.0 ); scattering->addref();
		UniformScalarPainter* nAir = new UniformScalarPainter( kAirInterior ); nAir->addref();
		UniformScalarPainter* nScaled = new UniformScalarPainter( kScaledInterior ); nScaled->addref();
		SubSurfaceScatteringMaterial* mAir = new SubSurfaceScatteringMaterial( *nAir, *absorption, *scattering, 0.0, 0.0 ); mAir->addref();
		SubSurfaceScatteringMaterial* mScaled = new SubSurfaceScatteringMaterial( *nScaled, *absorption, *scattering, 0.0, 0.0 ); mScaled->addref();

		for( int spectral = 0; spectral < 2; ++spectral ) {
			const Scalar nm = spectral ? 550.0 : 0.0;
			TestSampler sa( 4100 + spectral ), ss( 4100 + spectral );
			const RayIntersectionGeometric riAir = MakeSurfaceRI( kAirExterior );
			const RayIntersectionGeometric riScaled = MakeSurfaceRI( kScaledExterior );
			int active = 0, agree = 0;
			Scalar worst = 0;
			for( int i = 0; i < 512; ++i ) {
				const BSSRDFSampling::SampleResult a = BSSRDFSampling::SampleEntryPoint( riAir, sphere, mAir, sa, nm );
				const BSSRDFSampling::SampleResult s = BSSRDFSampling::SampleEntryPoint( riScaled, sphere, mScaled, ss, nm );
				if( a.valid != s.valid ) continue;
				if( !a.valid ) { ++agree; continue; }
				++active;
				bool ok = true;
				if( spectral ) {
					ok = RelClose( a.weightNM, s.weightNM, 1e-9 ) && RelClose( a.weightSpatialNM, s.weightSpatialNM, 1e-9 );
					worst = std::fmax( worst, std::fabs( a.weightNM - s.weightNM ) / std::fmax( 1e-300, std::fabs( a.weightNM ) ) );
				} else {
					for( int c = 0; c < 3; ++c ) {
						ok = ok && RelClose( a.weight[c], s.weight[c], 1e-9 ) && RelClose( a.weightSpatial[c], s.weightSpatial[c], 1e-9 );
						worst = std::fmax( worst, std::fabs( a.weight[c] - s.weight[c] ) / std::fmax( 1e-300, std::fabs( a.weight[c] ) ) );
					}
				}
				if( ok ) ++agree;
			}
			std::cout << "    " << ( spectral ? "NM " : "RGB" ) << " active=" << active << "/512 agree=" << agree
				<< "/512 worst relative weight difference " << worst << std::endl;
			Check( active >= 64, std::string( "A5: (sanity) enough valid samples " ) + ( spectral ? "NM" : "RGB" ) );
			Check( agree == 512, std::string( "A5: every seeded twin agrees " ) + ( spectral ? "NM" : "RGB" ) );
		}
		mAir->release(); mScaled->release(); nAir->release(); nScaled->release();
		absorption->release(); scattering->release(); sphere->release();
	}

	//////////////////////////////////////////////////////////////////
	// A6 -- RandomWalkSSS::SampleExit seeded twins
	//////////////////////////////////////////////////////////////////
	void TestRandomWalkTwins()
	{
		std::cout << "A6: RandomWalkSSS::SampleExit seeded twins agree" << std::endl;
		Object* sphere = MakeUnitSphere();
		const RISEPel sigmaA( 0.05, 0.1, 0.2 );
		const RISEPel sigmaS( 2.0, 2.0, 2.0 );
		RISEPel sigmaT;
		SSSCoefficients::FromCoefficients( sigmaA, sigmaS, sigmaT );
		// An oblique arrival, so the entry refraction bends.
		RayIntersectionGeometric riAir = MakeSurfaceRI( kAirExterior );
		const Vector3 dir = Vector3Ops::Normalize( Vector3( 0.6, 0.8, 0 ) );
		riAir.ray = Ray( Point3Ops::mkPoint3( riAir.ptIntersection, -dir * 2.0 ), dir );
		RayIntersectionGeometric riScaled = riAir; riScaled.ambientIOR = kScaledExterior;

		for( int spectral = 0; spectral < 2; ++spectral ) {
			const Scalar nm = spectral ? 550.0 : 0.0;
			TestSampler sa( 5200 + spectral ), ss( 5200 + spectral );
			int active = 0, agree = 0;
			Scalar worst = 0;
			for( int i = 0; i < 512; ++i ) {
				const BSSRDFSampling::SampleResult a = RandomWalkSSS::SampleExit(
					riAir, sphere, sigmaA, sigmaS, sigmaT, 0.0, kAirInterior, 256, sa, nm );
				const BSSRDFSampling::SampleResult s = RandomWalkSSS::SampleExit(
					riScaled, sphere, sigmaA, sigmaS, sigmaT, 0.0, kScaledInterior, 256, ss, nm );
				if( a.valid != s.valid ) continue;
				if( !a.valid ) { ++agree; continue; }
				++active;
				bool ok = Vector3Ops::Magnitude( Vector3Ops::mkVector3( a.entryPoint, s.entryPoint ) ) < 1e-7 &&
					Vector3Ops::Dot( a.scatteredRay.Dir(), s.scatteredRay.Dir() ) > 1.0 - 1e-10;
				if( spectral ) {
					ok = ok && RelClose( a.weightNM, s.weightNM, 1e-7 ) && RelClose( a.weightSpatialNM, s.weightSpatialNM, 1e-7 );
					worst = std::fmax( worst, std::fabs( a.weightNM - s.weightNM ) / std::fmax( 1e-300, std::fabs( a.weightNM ) ) );
				} else {
					for( int c = 0; c < 3; ++c ) {
						ok = ok && RelClose( a.weight[c], s.weight[c], 1e-7 );
						worst = std::fmax( worst, std::fabs( a.weight[c] - s.weight[c] ) / std::fmax( 1e-300, std::fabs( a.weight[c] ) ) );
					}
				}
				if( ok ) ++agree;
			}
			std::cout << "    " << ( spectral ? "NM " : "RGB" ) << " active=" << active << "/512 agree=" << agree
				<< "/512 worst relative weight difference " << worst << std::endl;
			Check( active >= 64, std::string( "A6: (sanity) enough valid walks " ) + ( spectral ? "NM" : "RGB" ) );
			// Walks with hundreds of bounces can amplify last-bit Snell/Fresnel
			// rounding into a different branch; allow a handful of such
			// divergences, but a systematic index error diverges almost all.
			Check( agree >= 500, std::string( "A6: seeded twins agree " ) + ( spectral ? "NM" : "RGB" ) );
		}
		sphere->release();
	}

	//////////////////////////////////////////////////////////////////
	// A7 -- matched-index random walk goes straight through
	//////////////////////////////////////////////////////////////////
	void TestRandomWalkMatchedIndex()
	{
		std::cout << "A7: matched-index random walk exits on the incident chord" << std::endl;
		Object* sphere = MakeUnitSphere();
		// A vanishing medium: the sampled free flight is astronomically longer
		// than the sphere, so the walk exits at its first boundary hit.
		const RISEPel sigmaA( 1e-9, 1e-9, 1e-9 );
		const RISEPel sigmaS( 1e-9, 1e-9, 1e-9 );
		RISEPel sigmaT;
		SSSCoefficients::FromCoefficients( sigmaA, sigmaS, sigmaT );
		const Scalar offsets[] = { 0.0, 0.3, 0.6, 0.85 };
		for( int spectral = 0; spectral < 2; ++spectral ) {
			int valid = 0, straight = 0;
			Scalar worst = 0;
			TestSampler sampler( 6300 + spectral );
			for( const Scalar x : offsets ) {
				// Ray travelling +Y at lateral offset x, entering the sphere's
				// bottom and leaving at its top.
				const Point3 entry( x, -sqrt( 1.0 - x * x ), 0 );
				const Point3 expectedExit( x, sqrt( 1.0 - x * x ), 0 );
				RayIntersectionGeometric ri( Ray( Point3( x, -3, 0 ), Vector3( 0, 1, 0 ) ), nullRasterizerState );
				ri.bHit = true;
				ri.ptIntersection = entry;
				const Vector3 n( entry.x, entry.y, entry.z );
				ri.vNormal = n;
				ri.vGeomNormal = n;
				ri.onb.CreateFromW( n );
				ri.ambientIOR = 1.5;
				for( int i = 0; i < 16; ++i ) {
					const BSSRDFSampling::SampleResult r = RandomWalkSSS::SampleExit(
						ri, sphere, sigmaA, sigmaS, sigmaT, 0.0, 1.5, 64, sampler, spectral ? 550.0 : 0.0 );
					if( !r.valid ) continue;
					++valid;
					// SampleExit offsets the exit point outward by BSSRDF_RAY_EPSILON.
					const Scalar miss = Vector3Ops::Magnitude( Vector3Ops::mkVector3( r.entryPoint, expectedExit ) );
					worst = std::fmax( worst, miss );
					if( miss < 1e-5 ) ++straight;
				}
			}
			std::cout << "    " << ( spectral ? "NM " : "RGB" ) << " valid=" << valid << "/64 straight=" << straight
				<< " worst exit-point miss " << worst << std::endl;
			Check( valid == 64, std::string( "A7: every matched-index walk exits (no exit Fresnel) " ) + ( spectral ? "NM" : "RGB" ) );
			Check( straight == valid && valid > 0, std::string( "A7: every walk exits on the incident chord (no refraction bend) " ) + ( spectral ? "NM" : "RGB" ) );
		}
		sphere->release();
	}

	//////////////////////////////////////////////////////////////////
	// A8 -- PathVertexEval at a BSSRDF entry vertex (BDPT/VCM re-evaluation)
	//////////////////////////////////////////////////////////////////
	BDPTVertex MakeEntryVertex( const IMaterial* pMaterial, const Scalar mediumIOR )
	{
		BDPTVertex v;
		v.type = BDPTVertex::SURFACE;
		v.position = Point3( 0, -1, 0 );
		v.normal = Vector3( 0, -1, 0 );
		v.geomNormal = Vector3( 0, -1, 0 );
		v.onb.CreateFromW( v.normal );
		v.pMaterial = pMaterial;
		v.isBSSRDFEntry = true;
		v.isConnectible = true;
		v.mediumIOR = mediumIOR;
		return v;
	}

	void TestPathVertexEval()
	{
		std::cout << "A8: PathVertexEval BSSRDF-entry re-evaluation is scale invariant" << std::endl;
		RGBScalarPainter* absorption = new RGBScalarPainter( 0.05, 0.10, 0.20 ); absorption->addref();
		RGBScalarPainter* scattering = new RGBScalarPainter( 1.0, 1.0, 1.0 ); scattering->addref();
		UniformScalarPainter* nAir = new UniformScalarPainter( kAirInterior ); nAir->addref();
		UniformScalarPainter* nScaled = new UniformScalarPainter( kScaledInterior ); nScaled->addref();
		SubSurfaceScatteringMaterial* dAir = new SubSurfaceScatteringMaterial( *nAir, *absorption, *scattering, 0.0, 0.0 ); dAir->addref();
		SubSurfaceScatteringMaterial* dScaled = new SubSurfaceScatteringMaterial( *nScaled, *absorption, *scattering, 0.0, 0.0 ); dScaled->addref();
		RandomWalkSSSMaterial* wAir = new RandomWalkSSSMaterial( *nAir, *absorption, *scattering, 0.0, 0.0, 64 ); wAir->addref();
		RandomWalkSSSMaterial* wScaled = new RandomWalkSSSMaterial( *nScaled, *absorption, *scattering, 0.0, 0.0, 64 ); wScaled->addref();

		const BDPTVertex vdAir = MakeEntryVertex( dAir, kAirExterior ), vdScaled = MakeEntryVertex( dScaled, kScaledExterior );
		const BDPTVertex vwAir = MakeEntryVertex( wAir, kAirExterior ), vwScaled = MakeEntryVertex( wScaled, kScaledExterior );
		const Vector3 wo( 0, -1, 0 );
		Scalar worstD = 0, worstW = 0;
		for( const Scalar mu : { 0.95, 0.6, 0.3, 0.1 } ) {
			const Vector3 wi = DirectionForCosine( mu );
			const Scalar a = PathVertexEval::EvalBSDFAtVertex( vdAir, wi, wo )[0];
			const Scalar s = PathVertexEval::EvalBSDFAtVertex( vdScaled, wi, wo )[0];
			const Scalar aNM = PathVertexEval::EvalBSDFAtVertexNM( vdAir, wi, wo, 550.0 );
			const Scalar sNM = PathVertexEval::EvalBSDFAtVertexNM( vdScaled, wi, wo, 550.0 );
			const Scalar wa = PathVertexEval::EvalBSDFAtVertex( vwAir, wi, wo )[0];
			const Scalar ws = PathVertexEval::EvalBSDFAtVertex( vwScaled, wi, wo )[0];
			const Scalar waNM = PathVertexEval::EvalBSDFAtVertexNM( vwAir, wi, wo, 550.0 );
			const Scalar wsNM = PathVertexEval::EvalBSDFAtVertexNM( vwScaled, wi, wo, 550.0 );
			worstD = std::fmax( worstD, std::fabs( a - s ) / a );
			worstW = std::fmax( worstW, std::fabs( wa - ws ) / wa );
			Check( a > 0 && wa > 0, "A8: (sanity) entry vertex evaluates a positive Sw" );
			Check( RelClose( a, s, 1e-12 ) && RelClose( aNM, sNM, 1e-12 ), "A8: diffusion entry vertex RGB/NM scale invariant" );
			Check( RelClose( wa, ws, 1e-12 ) && RelClose( waNM, wsNM, 1e-12 ), "A8: random-walk entry vertex RGB/NM scale invariant" );
		}
		std::cout << "    worst relative difference: diffusion " << worstD << "  random walk " << worstW << std::endl;

		// As in A3, the discriminating case is a denser exterior -- and here
		// it is what pins `BDPTVertex::mediumIOR` as the exterior source for
		// the connection re-evaluation (1.33 inside 1.5: nothing crosses
		// below the critical exterior cosine 0.4623).
		UniformScalarPainter* nDense = new UniformScalarPainter( 1.33 ); nDense->addref();
		SubSurfaceScatteringMaterial* dDense = new SubSurfaceScatteringMaterial( *nDense, *absorption, *scattering, 0.0, 0.0 ); dDense->addref();
		RandomWalkSSSMaterial* wDense = new RandomWalkSSSMaterial( *nDense, *absorption, *scattering, 0.0, 0.0, 64 ); wDense->addref();
		const BDPTVertex vdDense = MakeEntryVertex( dDense, 1.5 ), vwDense = MakeEntryVertex( wDense, 1.5 );
		const Vector3 grazing = DirectionForCosine( 0.3 );
		const Scalar dg = PathVertexEval::EvalBSDFAtVertex( vdDense, grazing, wo )[0];
		const Scalar dgNM = PathVertexEval::EvalBSDFAtVertexNM( vdDense, grazing, wo, 550.0 );
		const Scalar wg = PathVertexEval::EvalBSDFAtVertex( vwDense, grazing, wo )[0];
		const Scalar wgNM = PathVertexEval::EvalBSDFAtVertexNM( vwDense, grazing, wo, 550.0 );
		const Scalar ds = PathVertexEval::EvalBSDFAtVertex( vdDense, DirectionForCosine( 0.9 ), wo )[0];
		std::cout << "    1.33 inside 1.5 (mediumIOR): diffusion Sw(0.3) RGB/NM=" << dg << "/" << dgNM
			<< " Sw(0.9)=" << ds << "  walk Sw(0.3) RGB/NM=" << wg << "/" << wgNM << std::endl;
		Check( dg == 0.0 && dgNM == 0.0, "A8: diffusion entry vertex honours mediumIOR (no transmission past the critical angle)" );
		Check( wg == 0.0 && wgNM == 0.0, "A8: random-walk entry vertex honours mediumIOR (no transmission past the critical angle)" );
		Check( ds > 0, "A8: (sanity) denser exterior still transmits inside the critical cone" );
		dDense->release(); wDense->release(); nDense->release();

		dAir->release(); dScaled->release(); wAir->release(); wScaled->release();
		nAir->release(); nScaled->release(); absorption->release(); scattering->release();
	}


	//////////////////////////////////////////////////////////////////
	// C -- DL-291: precomputed / authored boundary conditions
	//////////////////////////////////////////////////////////////////

	//! A Donner-Jensen multipole profile with the given layer indices and
	//! the chunk defaults for every other parameter.
	struct SkinBundle
	{
		UniformScalarPainter* p[9];
		DonnerJensenSkinDiffusionProfile* profile;
		SkinBundle( const Scalar iorEpidermis, const Scalar iorDermis )
		{
			const Scalar v[9] = { 0.02, 0.5, 0.002, 0.001, 0.005, 0.025, iorEpidermis, iorDermis, 0.7 };
			for( int i = 0; i < 9; ++i ) { p[i] = new UniformScalarPainter( v[i] ); p[i]->addref(); }
			profile = new DonnerJensenSkinDiffusionProfile( *p[0], *p[1], *p[2], *p[3], *p[4], *p[5], *p[6], *p[7], *p[8] );
			profile->addref();
		}
		~SkinBundle()
		{
			profile->release();
			for( int i = 0; i < 9; ++i ) p[i]->release();
		}
	};

	//! Largest relative difference between two profiles over their whole
	//! public surface (RGB and NM evaluation, radius sampling, radius pdf).
	Scalar WorstSkinProfileDifference(
		const DonnerJensenSkinDiffusionProfile& a, const RayIntersectionGeometric& riA,
		const DonnerJensenSkinDiffusionProfile& b, const RayIntersectionGeometric& riB )
	{
		Scalar worst = 0;
		auto accumulate = [&worst]( const Scalar x, const Scalar y ) {
			const Scalar scale = std::fmax( std::fabs( x ), std::fabs( y ) );
			if( !std::isfinite( x ) || !std::isfinite( y ) ) { worst = INFINITY; return; }
			if( scale > 0 ) worst = std::fmax( worst, std::fabs( x - y ) / scale );
		};
		const Scalar radii[] = { 0.0, 0.002, 0.01, 0.03, 0.08, 0.2 };
		const Scalar wavelengths[] = { 405.0, 480.0, 555.0, 633.0, 695.0 };
		const Scalar us[] = { 0.02, 0.3, 0.6, 0.9, 0.995 };
		for( const Scalar r : radii ) {
			const RISEPel ea = a.EvaluateProfile( r, riA ), eb = b.EvaluateProfile( r, riB );
			for( int c = 0; c < 3; ++c ) accumulate( ea[c], eb[c] );
			for( const Scalar nm : wavelengths ) accumulate( a.EvaluateProfileNM( r, riA, nm ), b.EvaluateProfileNM( r, riB, nm ) );
			if( r > 0 ) for( int c = 0; c < 3; ++c ) accumulate( a.PdfRadius( r, c, riA ), b.PdfRadius( r, c, riB ) );
		}
		for( const Scalar u : us ) for( int c = 0; c < 3; ++c ) accumulate( a.SampleRadius( u, c, riA ), b.SampleRadius( u, c, riB ) );
		// The sampler's entry-point cutoff follows the fit's active set.
		accumulate( a.GetMaximumDistanceForErrorAt( 1e-4, riA ), b.GetMaximumDistanceForErrorAt( 1e-4, riB ) );
		return worst;
	}

	void TestSkinMultipoleRelativeIndex()
	{
		std::cout << "C1: Donner-Jensen multipole Rd is a function of the RELATIVE layer indices" << std::endl;
		// The multipole's boundary term A = (1+Fdr)/(1-Fdr) of each slab is
		// evaluated against the medium surrounding the stack; before DL-291
		// the profile baked it once, at construction, against air.  Each
		// case compares a body seen through exterior n_e with a SECOND body
		// whose authored indices are the first one's divided by n_e, seen
		// in air: one relative problem, two absolute scales.
		struct Case { const char* what; Scalar epi, derm, ext; Scalar tol; };
		const Case cases[] = {
			// Common scaling by 1.5 (the Part B enclosure).  2.1/1.5 is not
			// exactly 1.4 in binary, so a rounding-level tolerance.
			{ "scaled (2.1, 2.07) in 1.5 vs (1.4, 1.38) in air", 2.1, 2.07, 1.5, 1e-9 },
			// Matched epidermis: relative index exactly 1 -- the boundary
			// constant must be EXACTLY the air value of a relative-1 body.
			{ "matched (1.4, 1.38) in 1.4 vs (1, 1.38/1.4) in air", 1.4, 1.38, 1.4, 0.0 },
			// Denser exterior (eta < 1, the Fdr fit's other branch).
			{ "dense (1.33, 1.31) in 1.5 vs (1.33/1.5, 1.31/1.5) in air", 1.33, 1.31, 1.5, 0.0 },
		};
		for( const Case& c : cases ) {
			SkinBundle immersed( c.epi, c.derm );
			SkinBundle reference( c.epi / c.ext, c.derm / c.ext );
			const Scalar worst = WorstSkinProfileDifference(
				*immersed.profile, MakeSurfaceRI( c.ext ), *reference.profile, MakeSurfaceRI( 1.0 ) );
			std::cout << "    " << c.what << ": worst relative difference " << std::setprecision( 6 ) << worst << std::endl;
			Check( worst <= c.tol, std::string( "C1: " ) + c.what );
		}

		// Discrimination: the SAME body is priced differently by a different
		// exterior (otherwise the cases above would pass vacuously).
		SkinBundle skin( 1.4, 1.38 );
		const Scalar moved = WorstSkinProfileDifference( *skin.profile, MakeSurfaceRI( 1.33 ), *skin.profile, MakeSurfaceRI( 1.0 ) );
		std::cout << "    same body, water vs air: worst relative difference " << moved << std::endl;
		Check( moved > 1e-2, "C1: the exterior index changes the multipole profile" );

		// In air the profile is the constructor's table, bit for bit: a
		// stackless record (default ambientIOR) and an explicit 1.0 agree.
		RayIntersectionGeometric riDefault = MakeSurfaceRI( 1.0 );
		riDefault.ambientIOR = RayIntersectionGeometric( Ray(), nullRasterizerState ).ambientIOR;
		Check( WorstSkinProfileDifference( *skin.profile, riDefault, *skin.profile, MakeSurfaceRI( 1.0 ) ) == 0.0,
			"C1: stackless record reads the air table exactly" );
		Check( skin.profile->GetMaximumDistanceForErrorAt( 1e-4, MakeSurfaceRI( 1.0 ) ) == skin.profile->GetMaximumDistanceForError( 1e-4 ),
			"C1: in air the entry-point cutoff is the constructor's, exactly" );

		// The NNLS fit's active set moves with the exterior, so the cutoff
		// must too: at the default melanin a 1.2-epidermis body's widest
		// active Gaussian in water has ~2.8x the variance of its air fit.
		SkinBundle thin( 1.2, 1.2 * ( 1.38 / 1.4 ) );
		const Scalar cutAir = thin.profile->GetMaximumDistanceForErrorAt( 1e-4, MakeSurfaceRI( 1.0 ) );
		const Scalar cutWater = thin.profile->GetMaximumDistanceForErrorAt( 1e-4, MakeSurfaceRI( 1.33 ) );
		std::cout << "    entry-point cutoff (1.2 body, default melanin): air " << cutAir << "  water " << cutWater << std::endl;
		Check( cutWater > cutAir * 1.2, "C1: the entry-point cutoff follows the exterior's fit" );

		// A record whose exterior differs only in the last bit is a
		// different key; a repeat of an exterior already seen is served
		// from the cache and is identical to its first evaluation.
		const Scalar first = skin.profile->EvaluateProfile( 0.01, MakeSurfaceRI( 1.33 ) )[1];
		const Scalar again = skin.profile->EvaluateProfile( 0.01, MakeSurfaceRI( 1.33 ) )[1];
		Check( first == again, "C1: a repeated exterior evaluates identically" );

		// The Egan-Hilgeman diffuse-Fresnel fit's two branches meet at
		// eta = 1 (so an exterior crossing the interior index is continuous).
		Check( std::fabs( ComputeFdr( 1.0 - 1e-12 ) - ComputeFdr( 1.0 ) ) < 1e-9,
			"C1: diffuse Fresnel fit is continuous across eta = 1" );
	}

	//! Counts how the octree hands the IOR stack to the BSDF.
	class StackRecordingBSDF : public virtual IBSDF, public virtual Reference
	{
	public:
		mutable unsigned int withStack = 0, withoutStack = 0;
		RISEPel value( const Vector3&, const RayIntersectionGeometric& ) const override { ++withoutStack; return RISEPel( 1, 1, 1 ); }
		Scalar valueNM( const Vector3&, const RayIntersectionGeometric&, const Scalar ) const override { ++withoutStack; return 1; }
		RISEPel valueStateful( const Vector3&, const RayIntersectionGeometric&, const IORStack* s ) const override
		{
			if( s ) ++withStack; else ++withoutStack;
			return RISEPel( 1, 1, 1 );
		}
	protected:
		virtual ~StackRecordingBSDF() {}
	};

	class UnitExtinction : public virtual ISubSurfaceExtinctionFunction, public virtual Reference
	{
	public:
		mutable unsigned int exteriorMismatches = 0;
		Scalar expectedExterior = 1.0;
		Scalar GetMaximumDistanceForError( const Scalar ) const override { return RISE_INFINITY; }
		RISEPel ComputeTotalExtinction( const Scalar ) const override { ++exteriorMismatches; return RISEPel( 1, 1, 1 ); }
		RISEPel ComputeTotalExtinctionForExterior( const Scalar, const Scalar exteriorIOR ) const override
		{
			if( exteriorIOR != expectedExterior ) ++exteriorMismatches;
			return RISEPel( 1, 1, 1 );
		}
	protected:
		virtual ~UnitExtinction() {}
	};

	void TestPointSetOctreeStackForwarding()
	{
		std::cout << "C2: the point-set octree hands the live IOR stack to the BSDF at every depth" << std::endl;
		// DL-223 plumbed `const IORStack*` through PointSetOctree::Evaluate so
		// a stateful BSDF (translucent_material, the material both shipped
		// `multiplybsdf TRUE` scenes use) is priced on the right side; the
		// recursion into child nodes dropped it, so every node below the
		// root fell back to the stackless evaluation.
		PointSetOctree::PointSet points;
		BoundingBox bbox( Point3( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY ), Point3( -RISE_INFINITY, -RISE_INFINITY, -RISE_INFINITY ) );
		RandomNumberGenerator rng( 291 );
		for( int i = 0; i < 2000; ++i ) {
			PointSetOctree::SamplePoint sp;
			sp.ptPosition = Point3( rng.CanonicalRandom(), rng.CanonicalRandom(), rng.CanonicalRandom() );
			sp.irrad = RISEPel( 1, 1, 1 );
			points.push_back( sp );
			bbox.Include( sp.ptPosition );
		}
		bbox.EnsureBoxHasVolume();
		PointSetOctree tree( bbox, 4 );
		Check( tree.AddElements( points, 8 ), "C2: (setup) octree built" );
		StackRecordingBSDF* bsdf = new StackRecordingBSDF(); bsdf->addref();
		UnitExtinction* ext = new UnitExtinction(); ext->addref();
		IORStack stack( 1.0 );
		ext->expectedExterior = 1.33;
		RISEPel c( 0, 0, 0 );
		tree.Evaluate( c, Point3( 0.5, 0.5, 0.5 ), *ext, 0.001, bsdf, MakeSurfaceRI( 1.33 ), &stack, 1.33 );
		std::cout << "    BSDF evaluations with stack " << bsdf->withStack << ", without " << bsdf->withoutStack
			<< "; profile evaluations at the wrong exterior " << ext->exteriorMismatches << std::endl;
		Check( bsdf->withStack == 2000, "C2: every sample point priced with the live stack" );
		Check( bsdf->withoutStack == 0, "C2: no evaluation fell back to the stackless value" );
		Check( ext->exteriorMismatches == 0, "C2: every profile evaluation received the exterior index" );
		bsdf->release(); ext->release();
	}

	//! Internal diffuse Fresnel reflectance of a boundary with relative
	//! index `eta` (inside / outside): 2 * integral F(mu) mu dmu over the
	//! INSIDE hemisphere, exact unpolarized dielectric Fresnel, total
	//! internal reflection past the critical angle.  Independent of every
	//! polynomial fit in the library (the reference the fits approximate).
	Scalar NumericInternalFdr( const Scalar eta )
	{
		const int count = 1 << 16;
		Scalar sum = 0;
		for( int i = 0; i < count; ++i ) {
			const Scalar cosI = ( Scalar( i ) + 0.5 ) / count;
			const Scalar sinT2 = eta * eta * ( 1.0 - cosI * cosI );
			Scalar F = 1.0;
			if( sinT2 < 1.0 ) {
				const Scalar cosT = std::sqrt( 1.0 - sinT2 );
				const Scalar rs = ( eta * cosI - cosT ) / ( eta * cosI + cosT );
				const Scalar rp = ( eta * cosT - cosI ) / ( eta * cosT + cosI );
				F = 0.5 * ( rs * rs + rp * rp );
			}
			sum += 2.0 * F * cosI;
		}
		return sum / count;
	}

	void TestLegacyDipoleRelativeIndex()
	{
		std::cout << "C3: legacy dipole (diffusion_approximation_sss_shaderop) prices the relative index" << std::endl;
		// The chunk's `ior` is the material's index against air; the
		// dipole's boundary term A = (1+Fdr)/(1-Fdr) is a function of the
		// index RELATIVE to the medium the body sits in.
		const RISEPel absorption( 0.05, 0.1, 0.2 ), scattering( 2, 2, 2 );
		DiffusionApproximationExtinction* body = new DiffusionApproximationExtinction( absorption, scattering, 1.3, 0.0, 0.01 );
		body->addref();
		struct Case { const char* what; Scalar exterior; };
		const Case cases[] = { { "air", 1.0 }, { "water", 1.33 }, { "matched (1.3 in 1.3)", 1.3 }, { "denser glass", 1.5 } };
		const Scalar distances[] = { 0.0, 0.0005, 0.002, 0.01, 0.05 };
		for( const Case& c : cases ) {
			DiffusionApproximationExtinction* twin = new DiffusionApproximationExtinction( absorption, scattering, 1.3 / c.exterior, 0.0, 0.01 );
			twin->addref();
			Scalar worst = 0;
			for( const Scalar d : distances ) {
				const RISEPel a = body->ComputeTotalExtinctionForExterior( d, c.exterior );
				const RISEPel b = twin->ComputeTotalExtinction( d );
				for( int k = 0; k < 3; ++k ) worst = std::fmax( worst, std::fabs( a[k] - b[k] ) / std::fmax( 1e-300, std::fabs( b[k] ) ) );
			}
			std::cout << "    " << c.what << ": worst relative difference to the relative-index twin in air " << worst << std::endl;
			Check( worst == 0.0, std::string( "C3: exterior " ) + c.what + " == relative-index twin in air, exactly" );
			twin->release();
		}
		// The boundary term itself against the Fresnel integral BELOW
		// relative index 1 (a body less dense than its exterior -- only
		// reachable since DL-291).  The twins above cannot see a wrong fit:
		// both sides evaluate the same function.  Egan-Hilgeman's eta >= 1
		// polynomial goes NEGATIVE here (-0.051 at 1.3 in 1.33, -0.375 at
		// 1.3 in 1.5); the eta < 1 fit is within ~1e-3 of the integral.
		const Scalar belowOne[] = { 1.3 / 1.33, 1.3 / 1.5, 0.75 };
		for( const Scalar nu : belowOne ) {
			const Scalar Fnum = NumericInternalFdr( nu );
			const Scalar Anum = ( 1.0 + Fnum ) / ( 1.0 - Fnum );
			const Scalar Aused = DiffusionApproximationExtinction::BoundaryA( nu );
			const Scalar Amultipole = ( 1.0 + ComputeFdr( nu ) ) / ( 1.0 - ComputeFdr( nu ) );
			std::cout << "    relative index " << nu << ": numeric Fdr " << Fnum << "  A numeric " << Anum
				<< "  legacy dipole A " << Aused << "  multipole A " << Amultipole << std::endl;
			Check( std::fabs( Aused - Anum ) < 0.01 * Anum, "C3: legacy dipole boundary term below relative index 1 matches the Fresnel integral" );
			Check( std::fabs( Amultipole - Anum ) < 0.01 * Anum, "C3: multipole boundary term below relative index 1 matches the Fresnel integral" );
		}
		// The dipole's two fits meet at relative index 1 (to the fits' own 1e-4).
		Check( std::fabs( DiffusionApproximationExtinction::BoundaryA( 1.0 - 1e-12 ) - DiffusionApproximationExtinction::BoundaryA( 1.0 ) ) < 1e-3,
			"C3: legacy dipole boundary term continuous across relative index 1" );

		// In air the exterior-aware entry is the constructor's value, bit for bit.
		Check( body->ComputeTotalExtinctionForExterior( 0.003, 1.0 )[1] == body->ComputeTotalExtinction( 0.003 )[1],
			"C3: air reads the constructor's dipole exactly" );
		// Discrimination: a non-air exterior does move the profile.
		Check( body->ComputeTotalExtinctionForExterior( 0.003, 1.33 )[1] != body->ComputeTotalExtinction( 0.003 )[1],
			"C3: the exterior index changes the dipole" );
		body->release();
	}

	//////////////////////////////////////////////////////////////////
	// Part B -- rendered scale invariance
	//////////////////////////////////////////////////////////////////
	class CapturingRasterizerOutput
		: public virtual IRasterizerOutput
		, public virtual Reference
	{
	public:
		std::vector<RISEColor> pixels;
		CapturingRasterizerOutput() {}
	protected:
		virtual ~CapturingRasterizerOutput() {}
	public:
		virtual void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}
		virtual void OutputImage( const IRasterImage& image, const Rect*, const unsigned int ) override
		{
			pixels.resize( size_t( image.GetWidth() ) * image.GetHeight() );
			for( unsigned int y = 0; y < image.GetHeight(); y++ )
				for( unsigned int x = 0; x < image.GetWidth(); x++ )
					pixels[size_t( y ) * image.GetWidth() + x] = image.GetPEL( x, y );
		}
	};

	enum class Model { Lambertian, Diffusion, DiffusionRough, RandomWalk, SkinMultipole, LegacyDipole, LegacySkinOp };
	enum class Integrator { PT, BDPT, PTSpectral, PixelPel };

	const char* ModelName( Model m )
	{
		switch( m ) {
		case Model::Lambertian: return "lambertian_control";
		case Model::Diffusion: return "diffusion_smooth";
		case Model::DiffusionRough: return "diffusion_rough";
		case Model::RandomWalk: return "random_walk";
		case Model::SkinMultipole: return "skin_multipole";
		case Model::LegacyDipole: return "legacy_dipole_op";
		case Model::LegacySkinOp: return "legacy_skin_op";
		}
		return "unknown";
	}
	const char* IntegratorName( Integrator i )
	{
		switch( i ) {
		case Integrator::PT: return "PT";
		case Integrator::BDPT: return "BDPT";
		case Integrator::PTSpectral: return "PT-spectral";
		case Integrator::PixelPel: return "pixelpel";
		}
		return "unknown";
	}

	//! exterior == 1 builds the air scene; exterior > 1 wraps camera, light
	//! and subject in an ideal enclosure of that index.
	std::string BuildScene( Model model, Integrator integrator, Scalar exterior, Scalar interior, unsigned int samples )
	{
		std::ostringstream s;
		s << std::setprecision( 17 );
		s << "RISE ASCII SCENE 7\n";
		s << "film\n{\n\twidth 32\n\theight 32\n}\n\n";
		s << "pinhole_camera\n{\n\tlocation 0 0 4.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 30.0\n}\n\n";
		s << "uniformcolor_painter\n{\n\tname white\n\tcolor 1 1 1\n}\n\n";
		s << "uniformcolor_painter\n{\n\tname black\n\tcolor 0 0 0\n}\n\n";
		s << "lambertian_material\n{\n\tname black_base\n\treflectance black\n}\n\n";
		s << "lambertian_luminaire_material\n{\n\tname lum\n\texitance white\n\tmaterial black_base\n\tscale 6\n}\n\n";
		const Scalar nS = interior;
		const bool scaled = exterior != 1.0;
		switch( model ) {
		case Model::Lambertian:
			s << "uniformcolor_painter\n{\n\tname albedo\n\tcolor 0.7 0.6 0.5\n}\n\n"
			  << "lambertian_material\n{\n\tname subject\n\treflectance albedo\n}\n\n";
			break;
		case Model::Diffusion:
		case Model::DiffusionRough:
			// Smooth isolates the subsurface event (the only surface term is
			// the SPF's delta reflection, relative-index-correct before and
			// after DL-49).  Rough adds the SubSurfaceScatteringBSDF NEE
			// lobe, whose pre-DL-49 error has the OPPOSITE sign to the
			// subsurface one (air Fresnel over-reflects in the enclosure while
			// the air-index profile under-transmits), so the two partially
			// cancel in a whole-image mean -- which is why each gets a row.
			s << "subsurfacescattering_material\n{\n\tname subject\n\tior " << nS
			  << "\n\tabsorption 0.05 0.1 0.2\n\tscattering 2\n\tg 0\n\troughness "
			  << ( model == Model::DiffusionRough ? "0.3" : "0" ) << "\n}\n\n";
			break;
		case Model::RandomWalk:
			s << "randomwalk_sss_material\n{\n\tname subject\n\tior " << nS
			  << "\n\tabsorption 0.05 0.1 0.2\n\tscattering 2\n\tg 0\n\troughness 0\n\tmax_bounces 256\n}\n\n";
			break;
		case Model::SkinMultipole:
			// DL-291: the Donner-Jensen two-layer multipole.  `interior` is the
			// epidermis index; the dermis keeps the chunk defaults' ratio
			// (1.38/1.4), so both layers scale with the enclosure.  Smooth,
			// so the only surface term is the relative-index SPF reflection.
			s << "donner_jensen_skin_bssrdf_material\n{\n\tname subject\n\tior_epidermis " << nS
			  << "\n\tior_dermis " << nS * ( 1.38 / 1.4 ) << "\n\troughness 0\n}\n\n";
			break;
		case Model::LegacyDipole:
		case Model::LegacySkinOp:
			// DL-291: the legacy point-set shader-ops (pixelpel only).  The
			// subject is a white Lambertian whose shader is the SSS op; the
			// op's irradiance-capture shader is plain direct lighting, and
			// the op's authored index is the material's absolute index.
			s << "lambertian_material\n{\n\tname subject\n\treflectance white\n}\n\n"
			  << "standard_shader\n{\n\tname sss_irrad\n\tshaderop DefaultDirectLighting\n}\n\n";
			if( model == Model::LegacyDipole ) {
				s << "diffusion_approximation_sss_shaderop\n{\n\tname sss_op\n\tnumpoints 4000\n\tirrad_scale 1000\n"
				  << "\tgeometric_scale 0.01\n\tscattering 2 2 2\n\tabsorption 0.05 0.1 0.2\n\tior " << nS
				  << "\n\tg 0\n\tshader sss_irrad\n}\n\n";
			} else {
				s << "donner_jensen_skin_sss_shaderop\n{\n\tname sss_op\n\tnumpoints 4000\n\tirrad_scale 1\n"
				  << "\tior_epidermis " << nS << "\n\tior_dermis " << nS * ( 1.38 / 1.4 ) << "\n\tshader sss_irrad\n}\n\n";
			}
			s << "standard_shader\n{\n\tname sss_shader\n\tshaderop sss_op\n}\n\n";
			break;
		}
		s << "sphere_geometry\n{\n\tname subject_geo\n\tradius 1\n}\n\n";
		s << "standard_object\n{\n\tname subject_obj\n\tgeometry subject_geo\n\tmaterial subject\n"
		  << ( ( model == Model::LegacyDipole || model == Model::LegacySkinOp ) ? "\tshader sss_shader\n" : "" ) << "}\n\n";
		s << "sphere_geometry\n{\n\tname light_geo\n\tradius 0.4\n}\n\n";
		s << "standard_object\n{\n\tname light_obj\n\tgeometry light_geo\n\tmaterial lum\n\tposition 2 2.5 2.5\n}\n\n";
		// A black absorbing room around camera, light and subject, on BOTH
		// sides of the comparison: every path that leaves the neighbourhood
		// dies on it.  Without it the enclosure's inner wall totally
		// internally reflects grazing escapes back in (1.5 -> air), which
		// adds real indirect light that the air scene does not have -- a
		// confound the Lambertian control row measured at +46% under PT.
		// A non-refracting material, so it plays no part in IOR seeding.
		s << "sphere_geometry\n{\n\tname room_geo\n\tradius 20\n}\n\n";
		s << "standard_object\n{\n\tname room\n\tgeometry room_geo\n\tmaterial black_base\n}\n\n";
		if( scaled ) {
			// An ideal NON-reflecting index enclosure (the
			// SSSRadianceScalingTest idiom) outside the black room: no path
			// ever reaches its wall, so its only effect is the exterior index
			// the IOR stack is seeded with at the camera and the light.
			s << "perfectrefractor_material\n{\n\tname enclosure_mat\n\tior " << exterior << "\n\trefractance white\n}\n\n"
			  << "box_geometry\n{\n\tname enclosure_geo\n\twidth 60\n\theight 60\n\tdepth 60\n}\n\n"
			  << "standard_object\n{\n\tname enclosure\n\tgeometry enclosure_geo\n\tmaterial enclosure_mat\n}\n\n";
		}
		s << "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n";
		switch( integrator ) {
		case Integrator::PT:
			s << "pathtracing_pel_rasterizer\n{\n\tsamples " << samples
			  << "\n\trr_min_depth 8\n\tpixel_filter box\n\toidn_denoise FALSE\n\tpathguiding FALSE\n\tadaptive_max_samples 0\n}\n\n";
			break;
		case Integrator::BDPT:
			s << "bdpt_pel_rasterizer\n{\n\tmax_eye_depth 6\n\tmax_light_depth 6\n\tsamples " << samples
			  << "\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";
			break;
		case Integrator::PTSpectral:
			s << "pathtracing_spectral_rasterizer\n{\n\tsamples " << samples
			  << "\n\trr_min_depth 8\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";
			break;
		case Integrator::PixelPel:
			s << "pixelpel_rasterizer\n{\n\tsamples " << samples
			  << "\n\tmax_recursion 4\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";
			break;
		}
		s << "file_rasterizeroutput\n{\n\tpattern rendered/dl49_unused\n\ttype EXR\n\tbpp 32\n\tcolor_space Rec709RGB_Linear\n}\n";
		return s.str();
	}

	std::string WriteScene( const std::string& text, const std::string& tag )
	{
		char path[512];
		std::snprintf( path, sizeof( path ), "/tmp/dl49_invariance_%s_%d.RISEscene", tag.c_str(), static_cast<int>( ::getpid() ) );
		std::ofstream ofs( path );
		if( !ofs.is_open() ) return std::string();
		ofs << text;
		ofs.close();
		return std::string( path );
	}

	//! Renders once; returns the RGB-sum image mean, or a negative value on failure.
	//! On the first render of each configuration also verifies the camera's
	//! seeded exterior index (the enclosure really is the exterior).
	double RenderMean( const std::string& path, unsigned int seed, Scalar expectedExterior, bool checkSeed, const std::string& label )
	{
		IJobPriv* job = nullptr;
		if( !RISE_CreateJobPriv( &job ) || !job ) return -1;
		if( !job->LoadAsciiSceneViaCst( path.c_str() ) ) { safe_release( job ); return -1; }
		if( checkSeed ) {
			IORStack stack( 1.0 );
			IORStackSeeding::SeedFromPoint( stack, Point3( 0, 0, 4.5 ), *job->GetScene() );
			Check( std::fabs( stack.top() - expectedExterior ) < 1e-12, label + ": camera's seeded exterior index" );
		}
		job->RemoveRasterizerOutputs();
		CapturingRasterizerOutput* cap = new CapturingRasterizerOutput();
		GlobalLog()->PrintNew( cap, __FILE__, __LINE__, "dl49 capture" );
		job->GetRasterizer()->AddRasterizerOutput( cap );
		std::srand( seed );
		const bool rendered = job->Rasterize();
		double mean = -1;
		if( rendered && !cap->pixels.empty() ) {
			double sum = 0;
			bool finite = true;
			for( const RISEColor& c : cap->pixels ) {
				const double v = ( c.base.r + c.base.g + c.base.b ) * c.a;
				if( !std::isfinite( v ) ) { finite = false; break; }
				sum += v;
			}
			if( finite ) mean = sum / double( cap->pixels.size() );
		}
		safe_release( cap );
		safe_release( job );
		return mean;
	}

	struct Stats { double mean, sd; };
	Stats Summarize( const std::vector<double>& v )
	{
		double m = 0;
		for( double x : v ) m += x;
		m /= double( v.size() );
		double ss = 0;
		for( double x : v ) ss += ( x - m ) * ( x - m );
		return Stats{ m, v.size() > 1 ? std::sqrt( ss / double( v.size() - 1 ) ) : 0.0 };
	}

	void TestRenderedInvariance( const unsigned int trials, const std::string& only )
	{
		std::cout << "B: rendered scale invariance, air (1, n) vs enclosed (1.5, 1.5 n), n=" << trials << " per side" << std::endl;
		// Each row compares an air scene (exterior 1, interior airInterior)
		// with the same scene enclosed at `exterior` with interior
		// airInterior * exterior: one relative index, two absolute scales.
		struct Row { Model model; Integrator integrator; unsigned int samples; double band; Scalar airInterior; Scalar exterior; };
		// The "dense" rows put the relative index BELOW 1 (a 1.33 body inside
		// 1.5 glass, twinned with 1.33/1.5 in air): Schlick then runs at the
		// transmitted cosine with total reflection past the critical angle,
		// the one regime where Sw itself (not just Ft) depends on the index,
		// so these rows are what pin BDPTVertex::mediumIOR on the entry
		// vertices that PathVertexEval re-evaluates.
		const Scalar kDense = 1.33 / 1.5;
		// Bands: several times the measured sd of the ratio at these sample
		// counts and far below the pre-DL-49 deviations of the same rows
		// (both recorded in docs/DL49_SSS_EXTERIOR_INDEX.md).  BDPT renders
		// are deterministic for a fixed libc seed, and the pairs share one,
		// so the BDPT diffusion rows read exactly 1 after the fix.
		const Row rows[] = {
			{ Model::Lambertian,     Integrator::PT,         16,  0.02,  kAirInterior, kScale },
			{ Model::Diffusion,      Integrator::PT,         64,  0.02,  kAirInterior, kScale },
			{ Model::DiffusionRough, Integrator::PT,         64,  0.01,  kAirInterior, kScale },
			{ Model::RandomWalk,     Integrator::PT,         64,  0.04,  kAirInterior, kScale },
			{ Model::Diffusion,      Integrator::BDPT,       32,  0.02,  kAirInterior, kScale },
			{ Model::DiffusionRough, Integrator::BDPT,       32,  0.006, kAirInterior, kScale },
			{ Model::RandomWalk,     Integrator::BDPT,       128, 0.10,  kAirInterior, kScale },
			{ Model::Diffusion,      Integrator::PTSpectral, 64,  0.04,  kAirInterior, kScale },
			{ Model::RandomWalk,     Integrator::PTSpectral, 64,  0.05,  kAirInterior, kScale },
			{ Model::Diffusion,      Integrator::PT,         256, 0.008, kDense,       kScale },
			{ Model::Diffusion,      Integrator::BDPT,       32,  0.005, kDense,       kScale },
			{ Model::RandomWalk,     Integrator::PT,         64,  0.04,  kDense,       kScale },
			// DL-291 rows (bands set from measured sd; see
			// docs/DL49_SSS_EXTERIOR_INDEX.md section 10).  The skin BDPT
			// row's independent-sides sd is 0.032 / 0.019 / 0.0099 at
			// 32 / 128 / 512 spp (BDPT fireflies through the multipole),
			// and in the FULL suite a BDPT thread race decorrelates the
			// pair, so its band is 3 sd at 128 spp -- the pre-fix 0.89 is
			// still ~6 sd outside it.
			{ Model::SkinMultipole,  Integrator::PT,         64,  0.02,  1.4,          kScale },
			{ Model::SkinMultipole,  Integrator::BDPT,       128, 0.06,  1.4,          kScale },
			{ Model::SkinMultipole,  Integrator::PTSpectral, 256, 0.04,  1.4,          kScale },
			{ Model::SkinMultipole,  Integrator::PT,         64,  0.02,  1.33 / 1.5,   kScale },
			{ Model::LegacyDipole,   Integrator::PixelPel,   4,   0.03,  1.3,          kScale },
			{ Model::LegacyDipole,   Integrator::PixelPel,   4,   0.03,  1.3 / 1.5,    kScale },
			{ Model::LegacySkinOp,   Integrator::PixelPel,   4,   0.02,  1.4,          kScale },
		};
		unsigned int seed = 49000;
		for( const Row& row : rows ) {
			const std::string label = std::string( "B: " ) + ModelName( row.model ) +
				( row.airInterior < 1.0 ? "_dense" : "" ) + "/" + IntegratorName( row.integrator );
			if( !only.empty() && label.find( only ) == std::string::npos ) continue;
			const std::string airPath = WriteScene( BuildScene( row.model, row.integrator, 1.0, row.airInterior, row.samples ), "air" );
			const std::string scaledPath = WriteScene( BuildScene( row.model, row.integrator, row.exterior,
				row.airInterior * row.exterior, row.samples ), "scaled" );
			Check( !airPath.empty() && !scaledPath.empty(), label + ": scene files written" );
			std::vector<double> air, scaled;
			bool allValid = true;
			// Interleave the two sides so machine-load drift cannot bias the
			// ratio, and give each pair the SAME libc seed (common random
			// numbers: the invariance says the two sides are the same
			// function, so correlating their noise only tightens the ratio;
			// the independent-sides sd printed below is then conservative).
			for( unsigned int t = 0; t < trials; ++t ) {
				const unsigned int pairSeed = seed++;
				const double a = RenderMean( airPath, pairSeed, 1.0, t == 0, label + " air" );
				const double s = RenderMean( scaledPath, pairSeed, row.exterior, t == 0, label + " enclosed" );
				if( !( a > 0 ) || !( s > 0 ) ) allValid = false;
				air.push_back( a );
				scaled.push_back( s );
			}
			std::remove( airPath.c_str() );
			std::remove( scaledPath.c_str() );
			Check( allValid, label + ": every render finite and non-black" );
			if( !allValid ) continue;
			const Stats sa = Summarize( air ), ss = Summarize( scaled );
			const double ratio = ss.mean / sa.mean;
			const double ratioSd = ratio * std::sqrt( ( sa.sd / sa.mean ) * ( sa.sd / sa.mean ) / trials +
				( ss.sd / ss.mean ) * ( ss.sd / ss.mean ) / trials );
			std::cout << std::setprecision( 6 ) << "    " << label.substr( 3 )
				<< " spp=" << row.samples << ": air " << sa.mean << " +/- " << sa.sd
				<< "  enclosed " << ss.mean << " +/- " << ss.sd
				<< "  ratio " << ratio << " +/- " << ratioSd << " (band " << row.band << ")" << std::endl;
			Check( std::fabs( ratio - 1.0 ) < row.band, label + ": enclosed/air image mean ratio within band of 1" );
		}
	}
}

int main( int argc, char** argv )
{
	unsigned int trials = 4;
	bool unitOnly = false;
	std::string only;
	for( int i = 1; i < argc; ++i ) {
		const std::string a( argv[i] );
		if( a == "--unit-only" ) unitOnly = true;
		else if( a == "--only" && i + 1 < argc ) only = argv[++i];
		else if( a == "--trials" && i + 1 < argc ) trials = static_cast<unsigned int>( std::atoi( argv[++i] ) );
	}
	if( trials < 2 ) trials = 2;

	std::cout << "=== DL-49 SSS exterior-index invariance ===" << std::endl;
	TestProfileFresnel();
	TestSwNormalization();
	TestAdapters();
	TestRoughBSDF();
	TestSampleEntryPointTwins();
	TestRandomWalkTwins();
	TestRandomWalkMatchedIndex();
	TestPathVertexEval();
	TestSkinMultipoleRelativeIndex();
	TestPointSetOctreeStackForwarding();
	TestLegacyDipoleRelativeIndex();
	if( !unitOnly ) {
		TestRenderedInvariance( trials, only );
	}
	std::cout << "=== " << passCount << " passed, " << failCount << " failed ===" << std::endl;
	return failCount == 0 ? 0 : 1;
}
