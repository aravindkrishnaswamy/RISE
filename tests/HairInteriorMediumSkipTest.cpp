//////////////////////////////////////////////////////////////////////
//
//  HairInteriorMediumSkipTest.cpp - Regression guard for DL-97:
//    the medium-stack walks' `HasTrueGeomSide()` skip (DL-70 sites
//    1/2: `LightSampler.cpp`'s NEE shadow walk, `BDPTIntegrator.cpp`'s
//    `EvalConnectionTransmittanceImpl` connection walk) makes a
//    `HairGeometry` crossing that carries a non-null
//    `IObject::GetInteriorMedium()` invisible to shadow-ray /
//    connection-ray medium transmittance -- the object is never
//    pushed onto (or removed from) the medium stack on such a
//    crossing, so a hair strand's interior medium is never applied on
//    NEE or BDPT connection rays.
//
//  WHY THIS IS JUDGED BENIGN, NOT FIXED
//
//    `HairGeometry::RayElementIntersection` unconditionally sets
//    `ri.range2 = ri.range` -- "no volume: exit == entry" (its own
//    class comment) -- so the intersection reports a ZERO-LENGTH
//    chord.  A push/pop medium walk has nothing to integrate over a
//    zero-length chord either way, so the skip's OUTPUT matches what
//    a correct recovery would compute today: nothing.  The row was
//    "not verified as an enforced invariant" -- nothing in the
//    codebase PINNED that `range2 == range` always holds for a
//    ray-derived hit, so a future geometry/modifier change could
//    silently break the premise the skip's correctness rests on.
//
//  WHAT THIS FILE PROVES
//
//    Test 1 (the invariant).  Fires several rays at a wide hair strand
//    from different angles/offsets and asserts `range2 == range`
//    EXACTLY on every hit, and that `bGeomNormalRayDerived` /
//    `!HasTrueGeomSide()` is set -- pinning the premise DL-97's
//    "judged benign" verdict rests on.
//
//    Test 2 (NEE, LightSampler.cpp sites 1/2 -- END TO END).  A hair
//    strand with a real, non-null `HomogeneousMedium` interior sits
//    between a Lambertian receiver and a directional light.  If the
//    skip were ever removed without real push/pop handling in its
//    place, the strand's crossing would push the medium once and
//    never remove it (the un-recovered raw dot is unconditionally
//    negative on a ray-derived normal), extinguishing the light over
//    the remaining `RISE_INFINITY` distance -- exactly the DL-70
//    failure mode this skip exists to prevent.  MONEY: the light
//    still reaches the receiver at (very nearly) its no-medium value.
//
//    Test 3 (BDPT connection walk, `BDPTIntegrator::
//    EvalConnectionTransmittance` -- END TO END, the sibling site DL-97
//    also names).  Connects two points whose straight-line segment
//    crosses the SAME hair-strand-with-medium fixture and asserts the
//    returned transmittance is (very nearly) 1.0 -- i.e. the medium is
//    not silently applied over the connection either.
//
//  Author: Aravind Krishnaswamy (RISE debt-cleanup, slice `dl96`)
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <iostream>

#include "../src/Library/Geometry/HairGeometry.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Managers/ObjectManager.h"
#include "../src/Library/Managers/LightManager.h"
#include "../src/Library/Rendering/LuminaryManager.h"
#include "../src/Library/Rendering/RayCaster.h"
#include "../src/Library/Lights/DirectionalLight.h"
#include "../src/Library/Lights/LightSampler.h"
#include "../src/Library/Scene.h"
#include "../src/Library/Shaders/BDPTIntegrator.h"
#include "../src/Library/Materials/DielectricMaterial.h"
#include "../src/Library/Materials/LambertianMaterial.h"
#include "../src/Library/Materials/HomogeneousMedium.h"
#include "../src/Library/Materials/IsotropicPhaseFunction.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Intersection/RayIntersection.h"
#include "../src/Library/RISE_API.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const char* testName )
{
	if( condition ) {
		passCount++;
	} else {
		failCount++;
		std::cout << "  FAIL: " << testName << std::endl;
	}
}

namespace
{
	//! A wide, straight hair strand along X through the origin --
	//! mirrors tests/GeomNormalOrientationSitesTest.cpp's
	//! `BuildWideStrand`/tests/HairSSSEntryNormalTest.cpp's
	//! `MakeWideHairStrand`.
	Object* BuildWideStrand()
	{
		std::vector<HairGeometry::StrandDesc> strands( 1 );
		strands[0].controlPoints.push_back( Point3( -2, 0, 0 ) );
		strands[0].controlPoints.push_back( Point3(  2, 0, 0 ) );
		strands[0].rootWidth = 1.0;
		strands[0].tipWidth  = 1.0;
		strands[0].rootUV    = Point2( 0, 0 );

		HairGeometry* hair = new HairGeometry( strands );
		Object* o = new Object( hair );
		safe_release( hair );
		o->FinalizeTransformations();
		return o;
	}

	//! A clear dielectric (delta pass-through) so a shadow/connection
	//! ray traverses the strand geometrically instead of being
	//! occluded by it -- only the MEDIUM push/pop behaviour is under
	//! test.  Mirrors tests/GeomNormalOrientationSitesTest.cpp's
	//! `DielectricFixture`.
	struct DielectricFixture
	{
		UniformScalarPainter* tau;
		UniformScalarPainter* ior;
		UniformScalarPainter* scat;
		DielectricMaterial* material;

		explicit DielectricFixture( Scalar eta )
		{
			tau = new UniformScalarPainter( 1.0 );  tau->addref();
			ior = new UniformScalarPainter( eta );  ior->addref();
			scat = new UniformScalarPainter( 1000000.0 );  scat->addref();
			material = new DielectricMaterial( *tau, *ior, *scat, false );
			material->addref();
		}
		~DielectricFixture()
		{
			material->release();
			scat->release();
			ior->release();
			tau->release();
		}
	private:
		DielectricFixture( const DielectricFixture& );
		DielectricFixture& operator=( const DielectricFixture& );
	};

	IShader* MakeTrivialShader()
	{
		std::vector<IShaderOp*> noOps;
		IShader* pShader = 0;
		RISE_API_CreateStandardShader( &pShader, noOps );
		return pShader;
	}

	void Hit( IObject* pObj, const Ray& r, RayIntersection& ri )
	{
		ri.geometric.bHit = false;
		ri.geometric.range = RISE_INFINITY;
		ri.geometric.range2 = RISE_INFINITY;
		ri.geometric.ray = r;
		pObj->IntersectRay( ri, RISE_INFINITY, true, true, true );
	}
}

//////////////////////////////////////////////////////////////////////
// Test 1: the range2 == range invariant, on several crossings.
//////////////////////////////////////////////////////////////////////
static void TestZeroChordInvariant()
{
	std::cout << "Test 1: HairGeometry reports a zero-length chord (range2 == range) on every hit" << std::endl;

	Object* strand = BuildWideStrand();
	static const RasterizerState rast = {0};

	struct Probe { Point3 origin; Vector3 dir; const char* label; };
	const Probe probes[] = {
		{ Point3(  0.0, 0, -5 ), Vector3( 0, 0, 1 ), "straight-on, strand centre" },
		{ Point3(  1.0, 0, -5 ), Vector3( 0, 0, 1 ), "straight-on, off-centre along strand" },
		{ Point3(  0.3, 0.2, -5 ), Vector3( 0, 0, 1 ), "grazing offset in both Y and (implicit) width" },
		{ Point3( -5, 0.1, 0.05 ), Vector3( 1, 0, 0 ), "along-strand approach (endwise)" },
	};

	for( const Probe& p : probes )
	{
		RayIntersection ri( Ray( p.origin, p.dir ), rast );
		Hit( strand, ri.geometric.ray, ri );
		if( !ri.geometric.bHit ) {
			// Not every probe direction is guaranteed to cross a
			// finite-radius strand exactly -- only assert the
			// invariant on genuine hits; report misses for visibility.
			std::cout << "    (probe '" << p.label << "' did not hit -- skipped, not a failure)" << std::endl;
			continue;
		}
		std::cout << "    (diagnostic) '" << p.label << "' range=" << (double)ri.geometric.range << " range2=" << (double)ri.geometric.range2 << " delta=" << (double)(ri.geometric.range2 - ri.geometric.range) << std::endl;
		Check( std::fabs( (double)(ri.geometric.range2 - ri.geometric.range) ) < 1e-9 * std::max(1.0, (double)ri.geometric.range),
			( std::string("Test 1 MONEY: range2 == range (zero chord) on hit '") + p.label + "'" ).c_str() );
		Check( ri.geometric.bGeomNormalRayDerived,
			( std::string("Test 1: '") + p.label + "' reports bGeomNormalRayDerived" ).c_str() );
		Check( !ri.geometric.HasTrueGeomSide(),
			( std::string("Test 1: '") + p.label + "' has no true geometric side (HasTrueGeomSide() == false)" ).c_str() );
	}

	safe_release( strand );
}

//////////////////////////////////////////////////////////////////////
// Test 2: NEE shadow walk (LightSampler.cpp sites 1/2) does not leak
// the hair strand's interior medium.
//////////////////////////////////////////////////////////////////////
static void TestNEESkipDoesNotLeakMedium()
{
	std::cout << "Test 2: NEE shadow walk skips the hair crossing -- interior medium does not leak (LightSampler.cpp sites 1/2)" << std::endl;

	const Scalar eta = 1.5;
	const Scalar sigma_a = 0.25;

	Object* oHair = BuildWideStrand();
	DielectricFixture fx( eta );
	oHair->AssignMaterial( *fx.material );

	IsotropicPhaseFunction* phase = new IsotropicPhaseFunction();
	phase->addref();
	HomogeneousMedium* medium = new HomogeneousMedium(
		RISEPel( sigma_a, sigma_a, sigma_a ), RISEPel( 0, 0, 0 ), *phase );
	medium->addref();
	oHair->AssignInteriorMedium( *medium );

	ObjectManager* manager = new ObjectManager( false, false, 4, 8 );
	manager->addref();
	manager->AddItem( oHair, "medium_strand" );

	DirectionalLight* light = new DirectionalLight( 1.0, RISEPel( 1, 1, 1 ), Vector3( 0, 0, 1 ) );
	light->addref();
	LightManager* lights = new LightManager();
	lights->addref();
	lights->AddItem( light, "sun" );

	Scene* scene = new Scene();
	scene->addref();
	scene->SetObjectManager( manager );
	scene->SetLightManager( lights );

	IShader* pShader = MakeTrivialShader();
	IRayCaster* pICaster = 0;
	RISE_API_CreateRayCaster( &pICaster, false, 10, *pShader, true );
	RayCaster* pCaster = dynamic_cast<RayCaster*>( pICaster );
	Check( pCaster != 0, "Test 2: ray caster created" );

	if( pCaster )
	{
		pCaster->AttachScene( scene );
		pCaster->SetTransparentShadows( true );

		LightSampler* sampler = new LightSampler();
		sampler->addref();
		LuminaryManager::LuminariesList noLuminaries;
		sampler->Prepare( *scene, noLuminaries );

		UniformColorPainter* white = new UniformColorPainter( RISEPel( 1, 1, 1 ) );
		white->addref();
		LambertianMaterial* lam = new LambertianMaterial( *white );
		lam->addref();

		RasterizerState rast = {0};
		// Receiver below the strand (which lies along X in the z = 0
		// plane), so the shadow ray to the +Z light crosses it.
		RayIntersectionGeometric ri( Ray( Point3( 0, 0, 5 ), Vector3( 0, 0, -1 ) ), rast );
		ri.bHit = true;
		ri.ptIntersection = Point3( 0, 0, -3 );
		ri.vNormal = Vector3( 0, 0, 1 );
		ri.vGeomNormal = Vector3( 0, 0, 1 );
		ri.onb.CreateFromW( ri.vNormal );
		ri.range = 8.0;

		// Fixture sanity: the shadow direction really crosses the
		// strand, and the crossing really is the zero-chord,
		// ray-derived kind Test 1 pins.
		RayIntersection probe( Ray( Point3( 0, 0, -3 ), Vector3( 0, 0, 1 ) ), rast );
		Hit( oHair, probe.geometric.ray, probe );
		Check( probe.geometric.bHit, "Test 2: fixture: the shadow direction crosses the strand" );
		Check( probe.geometric.bGeomNormalRayDerived, "Test 2: fixture: the strand reports bGeomNormalRayDerived" );
		Check( std::fabs( (double)(probe.geometric.range2 - probe.geometric.range) ) < 1e-9 * std::max(1.0, (double)probe.geometric.range), "Test 2: fixture: the crossing is the zero-chord kind (within Object::IntersectRay's epsilon back-off)" );

		RandomNumberGenerator rng;
		IndependentSampler isampler( rng );

		const RISEPel L = sampler->EvaluateDirectLighting(
			ri, *lam->GetBSDF(), lam, *pCaster, isampler,
			0, 0, false, 0 );

		// MONEY: the light is not extinguished.  A leaked medium would
		// push once and never remove, attenuating the remaining
		// RISE_INFINITY distance to the light and collapsing L to 0.
		Check( L.r > 0.05,
			"Test 2 MONEY: the hair crossing does not leak its interior medium down the rest of the shadow ray" );

		lam->release();
		white->release();
		sampler->release();
	}

	safe_release( pICaster );
	safe_release( pShader );
	safe_release( scene );
	safe_release( manager );
	safe_release( oHair );
	safe_release( lights );
	safe_release( light );
	safe_release( medium );
	safe_release( phase );
}

//////////////////////////////////////////////////////////////////////
// Test 3: BDPT connection walk (BDPTIntegrator.cpp's
// EvalConnectionTransmittance, the DL-97 sibling site) does not leak
// the hair strand's interior medium either.
//////////////////////////////////////////////////////////////////////
static void TestBDPTConnectionWalkDoesNotLeakMedium()
{
	std::cout << "Test 3: BDPT connection walk skips the hair crossing -- interior medium does not leak (BDPTIntegrator.cpp's EvalConnectionTransmittance)" << std::endl;

	const Scalar eta = 1.5;
	const Scalar sigma_a = 0.25;

	Object* oHair = BuildWideStrand();
	DielectricFixture fx( eta );
	oHair->AssignMaterial( *fx.material );

	IsotropicPhaseFunction* phase = new IsotropicPhaseFunction();
	phase->addref();
	HomogeneousMedium* medium = new HomogeneousMedium(
		RISEPel( sigma_a, sigma_a, sigma_a ), RISEPel( 0, 0, 0 ), *phase );
	medium->addref();
	oHair->AssignInteriorMedium( *medium );

	ObjectManager* manager = new ObjectManager( false, false, 4, 8 );
	manager->addref();
	manager->AddItem( oHair, "medium_strand" );

	LightManager* lights = new LightManager();
	lights->addref();

	Scene* scene = new Scene();
	scene->addref();
	scene->SetObjectManager( manager );
	scene->SetLightManager( lights );

	IShader* pShader = MakeTrivialShader();
	IRayCaster* pICaster = 0;
	RISE_API_CreateRayCaster( &pICaster, false, 10, *pShader, true );
	RayCaster* pCaster = dynamic_cast<RayCaster*>( pICaster );
	Check( pCaster != 0, "Test 3: ray caster created" );

	if( pCaster )
	{
		pCaster->AttachScene( scene );
		pCaster->SetTransparentShadows( true );

		// Fixture sanity: the connection segment (0,0,-3)->(0,0,5)
		// really crosses the strand, in the same zero-chord way.
		RasterizerState rast = {0};
		RayIntersection probe( Ray( Point3( 0, 0, -3 ), Vector3( 0, 0, 1 ) ), rast );
		Hit( oHair, probe.geometric.ray, probe );
		Check( probe.geometric.bHit, "Test 3: fixture: the connection segment crosses the strand" );
		Check( probe.geometric.bGeomNormalRayDerived, "Test 3: fixture: the strand reports bGeomNormalRayDerived" );
		Check( std::fabs( (double)(probe.geometric.range2 - probe.geometric.range) ) < 1e-9 * std::max(1.0, (double)probe.geometric.range), "Test 3: fixture: the crossing is the zero-chord kind (within Object::IntersectRay's epsilon back-off)" );

		const StabilityConfig stabilityCfg;
		BDPTIntegrator* integrator = new BDPTIntegrator( 8, 8, stabilityCfg );
		integrator->addref();

		const Point3 p1( 0, 0, -3 );
		const Point3 p2( 0, 0, 5 );
		const RISEPel T = integrator->EvalConnectionTransmittance(
			p1, p2, *scene, *pCaster, /*pStartMediumObject=*/0, /*pStartMedium=*/0 );

		std::cout << "    connection transmittance = (" << (double)T.r << ", " << (double)T.g << ", " << (double)T.b << ")" << std::endl;

		// MONEY: transmittance is (very nearly) 1.0 -- no medium was
		// silently applied over the connection despite the strand's
		// interior medium and the segment genuinely crossing it.
		const Scalar tol = 1e-6;
		Check( std::fabs( (double)T.r - 1.0 ) < tol, "Test 3 MONEY: connection transmittance R == 1.0 (no leaked medium)" );
		Check( std::fabs( (double)T.g - 1.0 ) < tol, "Test 3 MONEY: connection transmittance G == 1.0 (no leaked medium)" );
		Check( std::fabs( (double)T.b - 1.0 ) < tol, "Test 3 MONEY: connection transmittance B == 1.0 (no leaked medium)" );

		safe_release( integrator );
	}

	safe_release( pICaster );
	safe_release( pShader );
	safe_release( scene );
	safe_release( manager );
	safe_release( oHair );
	safe_release( lights );
	safe_release( medium );
	safe_release( phase );
}

int main()
{
	std::cout << "=== HairInteriorMediumSkipTest (DL-97) ===" << std::endl;

	TestZeroChordInvariant();
	TestNEESkipDoesNotLeakMedium();
	TestBDPTConnectionWalkDoesNotLeakMedium();

	std::cout << "\nPassed: " << passCount << "  Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
