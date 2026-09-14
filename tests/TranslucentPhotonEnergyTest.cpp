//////////////////////////////////////////////////////////////////////
//
//  TranslucentPhotonEnergyTest.cpp - Regression guard for DL-39:
//    TranslucentPelPhotonTracer deposited ALL incoming photon power at
//    a translucent interior exit, regardless of the SPF's own Beer
//    extinction, because its deposit formula
//    `power*(1-accum_scattered)` only ever subtracted the kray of rays
//    that were actually re-traced (type eRayTranslucent/eRayReflection/
//    eRayRefraction) -- TranslucentSPF's diffuse exit ray is type
//    eRayDiffuse, which that filter never matches, so at scattering=0
//    (no backscatter ray either) `accum_scattered` stayed exactly 0 and
//    the deposit was `power*1`, silently discarding whatever the SPF
//    had already attenuated by distance*extinction.
//
//  THE FIX
//
//    Sum the DIFFUSE lobe's own kray directly (TranslucentSPF emits at
//    most one eRayDiffuse ray per Scatter() call) and deposit
//    `power*diffuse_kray` instead of `power*(1-accum_scattered)`.
//
//  RED-PROOF (recorded from a run against the pre-fix library; see the
//    fix commit message for the verbatim failing output)
//
//    scattering=0, extinction in {0, 0.1, 1}: pre-fix deposited power
//    was the FULL incoming power (1,1,1) on every extinction value --
//    identical for extinction=0 and extinction=1, which alone proves
//    extinction was not being consulted at all.  Expected (and the
//    fixed behaviour): incoming power * exp(-extinction*distance).
//
//  COVERAGE
//
//    Drives the REAL TranslucentPelPhotonTracer::TracePhoton (via a
//    thin public subclass exposing the protected method) through one
//    synthetic interior-segment hit (distance fixed at 1 by a
//    single-hit-then-miss IObjectManager stub, mirroring
//    TranslucentGuidedStackProbe.h's NextHitManager pattern), with a
//    REAL TranslucentMaterial/TranslucentSPF doing the actual scatter.
//    The recursive backscatter ray (when scattering>0) hits nothing on
//    its own recursive call, so the map's ONLY stored photon is the one
//    deposit call this fix touches -- isolating exactly the quantity
//    DL-39 is about.  A geometrically-aligned shading/geometric normal
//    keeps DL-45's independent geometric-horizon gate a no-op here (its
//    own coverage lives in TranslucentTiltedExitTest.cpp), so this test
//    is purely about the ENERGY the deposit call reports.
//
//    Sub-test 1 -- scattering=0, extinction in {0, 0.1, 1}: deposited
//      power must equal incoming power * exp(-extinction*distance),
//      not incoming power (the pre-fix value for every extinction).
//    Sub-test 2 -- scattering=0.3 (nonzero backscatter): deposited
//      power must equal incoming power * exp(-extinction*distance) *
//      (1-scattering); and absorbed_frac + deposited_frac + traced_frac
//      (all closed-form except deposited_frac, which is MEASURED from
//      the real map) must sum to exactly 1 per channel.
//
//  Author: Aravind Krishnaswamy (RISE debt-cleanup, slice `translucent`)
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/IORStack.h"
#include "../src/Library/Intersection/RayIntersection.h"
#include "../src/Library/Interfaces/IPainter.h"
#include "../src/Library/Interfaces/IScalarPainter.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Materials/TranslucentMaterial.h"
#include "../src/Library/PhotonMapping/TranslucentPelPhotonTracer.h"
#include "../src/Library/PhotonMapping/TranslucentPelPhotonMap.h"
#include "../src/Library/Managers/ObjectManager.h"
#include "../src/Library/Scene.h"

#include "TestStubObject.h"

using namespace RISE;
using namespace RISE::Implementation;

static int failed = 0;

#define EXPECT( cond, msg ) do { \
	if( !(cond) ) { \
		std::cout << "FAIL: " << __FILE__ << ":" << __LINE__ << " " << msg << std::endl; \
		failed++; \
	} \
} while(0)

#define EXPECT_NEAR_PEL( a, b, tol, msg ) do { \
	const RISEPel _a = (a), _b = (b); \
	const Scalar _d = r_max( r_max(fabs(_a[0]-_b[0]),fabs(_a[1]-_b[1])), fabs(_a[2]-_b[2]) ); \
	if( _d > (tol) ) { \
		std::cout << "FAIL: " << __FILE__ << ":" << __LINE__ << " " << msg \
			<< " (got (" << _a[0] << "," << _a[1] << "," << _a[2] << ")" \
			<< ", expected (" << _b[0] << "," << _b[1] << "," << _b[2] << "))" << std::endl; \
		failed++; \
	} else { \
		std::cout << "  ok: " << msg << " = (" << _a[0] << "," << _a[1] << "," << _a[2] << ")" << std::endl; \
	} \
} while(0)

namespace
{
	// Reports exactly ONE hit (a fixed distance of 1 along the incoming
	// ray, geometrically-aligned normal so DL-45's gate is a no-op) on
	// its first call, then reports a miss on every subsequent call --
	// terminating any recursive TracePhoton call (the backscatter ray)
	// without depositing anything further, so the map's stored contents
	// are exactly this test's target quantity.
	class SingleHitThenMissManager : public ObjectManager
	{
		const IObject& object;
		const IMaterial& material;
		mutable int callCount;
	public:
		SingleHitThenMissManager( const IObject& obj, const IMaterial& mat ) :
			ObjectManager( false, false, 4, 8 ), object( obj ), material( mat ), callCount( 0 )
		{}
		void IntersectRay( RayIntersection& ri, bool, bool, bool ) const override
		{
			if( callCount++ != 0 ) {
				ri.geometric.bHit = false;
				return;
			}
			const Vector3 normal( 0, 0, 1 );
			ri.geometric.bHit = true;
			ri.geometric.range = 1;
			ri.geometric.ptIntersection = ri.geometric.ray.PointAtLength( 1 );
			ri.geometric.vNormal = normal;
			ri.geometric.vGeomNormal = normal;
			ri.geometric.onb.CreateFromW( normal );
			ri.pObject = &object;
			ri.pMaterial = &material;
		}
	};

	// Exposes the protected TracePhoton so the test can drive it
	// directly, without needing a full luminaire-driven photon shoot.
	class TestTranslucentPelPhotonTracer : public TranslucentPelPhotonTracer
	{
	public:
		TestTranslucentPelPhotonTracer() :
			// Virtual inheritance (TranslucentPelPhotonTracer -> virtual
			// PhotonTracer<TranslucentPelPhotonMap>) requires the
			// MOST-DERIVED class to initialize the virtual base directly;
			// initializing it only via TranslucentPelPhotonTracer's own
			// initializer list below is not enough once this subclass
			// exists.
			PhotonTracer<TranslucentPelPhotonMap>(
				/*shootFromNonMeshLights*/ true, /*power_scale*/ 1.0,
				/*temporal_samples*/ 1, /*regenerate*/ false ),
			TranslucentPelPhotonTracer(
				/*maxR*/ 8, /*ext (global photon-extinction cutoff)*/ 1e-6,
				/*reflect*/ true, /*refract*/ true, /*direct_translucent*/ true,
				/*shootFromNonMeshLights*/ true, /*powerscale*/ 1.0,
				/*temporal_samples*/ 1, /*regenerate*/ false )
		{}

		void TestTrace(
			const Ray& ray, const RISEPel& power, const bool bFromTranslucent,
			TranslucentPelPhotonMap& map, const IORStack& ior_stack,
			const unsigned int depth ) const
		{
			// depth=1 (not 0) sidesteps TracePhoton's `depth==0` direct-hit
			// special case entirely -- this test is deliberately modeling
			// an INTERIOR segment, not a primary photon-light hit.
			TracePhoton( ray, power, bFromTranslucent, map, ior_stack, depth );
		}
	};

	// Exposes the single stored photon's power.  `vphotons` is
	// `protected` on PhotonMapCore<TranslucentPhoton>, so a subclass can
	// read it directly; TranslucentPelPhotonMap itself has no by-index
	// accessor (RadianceEstimate needs a real BSDF/surface, which this
	// energy-accounting test has no reason to fabricate).
	class InspectableTranslucentPelPhotonMap : public TranslucentPelPhotonMap
	{
	public:
		InspectableTranslucentPelPhotonMap( unsigned int maxPhotons ) :
			TranslucentPelPhotonMap( maxPhotons, 0 )
		{}
		unsigned int StoredCount() const { return static_cast<unsigned int>( vphotons.size() ); }
		RISEPel StoredPower( unsigned int i ) const { return vphotons[i].power; }
	};

	// Builds an IORStack whose current object (transObj) is ALREADY on
	// the stack -- i.e. the state a real photon has after having already
	// entered the translucent object, about to hit its exit boundary.
	IORStack MakeInsideStack( const IObject* transObj )
	{
		IORStack stack( 1.0 );
		stack.SetCurrentObject( transObj );
		stack.push( 1.0 );
		return stack;
	}

	// Runs one deposit trial with the given extinction/scattering and
	// returns the map's single stored photon's power.
	RISEPel RunOneDeposit( Scalar extinction, Scalar scattering )
	{
		UniformColorPainter* front = new UniformColorPainter( RISEPel( 0.2, 0.2, 0.2 ) );  front->addref();
		UniformColorPainter* trans = new UniformColorPainter( RISEPel( 0.4, 0.4, 0.4 ) );  trans->addref();
		UniformScalarPainter* ext = new UniformScalarPainter( extinction );  ext->addref();
		UniformScalarPainter* phongN = new UniformScalarPainter( 1.0 );  phongN->addref();
		UniformScalarPainter* scat = new UniformScalarPainter( scattering );  scat->addref();

		TranslucentMaterial* material = new TranslucentMaterial( *front, *trans, *ext, *phongN, *scat );
		material->addref();

		StubObject* transObj = new StubObject();  transObj->addref();

		SingleHitThenMissManager* manager = new SingleHitThenMissManager( *transObj, *material );
		manager->addref();
		Scene* scene = new Scene();
		scene->addref();
		scene->SetObjectManager( manager );

		TestTranslucentPelPhotonTracer* tracer = new TestTranslucentPelPhotonTracer();
		tracer->addref();
		tracer->AttachScene( scene );

		InspectableTranslucentPelPhotonMap map( 16 );

		const Ray primaryRay( Point3( 0, 0, 1 ), Vector3( 0, 0, -1 ) );
		const RISEPel incomingPower( 1, 1, 1 );
		IORStack stack = MakeInsideStack( transObj );

		tracer->TestTrace( primaryRay, incomingPower, /*bFromTranslucent*/ true, map, stack, /*depth*/ 1 );

		EXPECT( map.StoredCount() == 1, "exactly one photon deposited at the interior-exit hit" );

		RISEPel stored( 0, 0, 0 );
		if( map.StoredCount() >= 1 ) {
			stored = map.StoredPower( 0 );
		}

		manager->release();
		scene->release();
		tracer->release();
		material->release();
		transObj->release();
		scat->release();
		phongN->release();
		ext->release();
		trans->release();
		front->release();

		return stored;
	}
}

static void TestExtinctionSweep()
{
	std::cout << "Sub-test 1: scattering=0, extinction sweep" << std::endl;

	const Scalar extinctions[] = { 0.0, 0.1, 1.0 };
	for( unsigned int i = 0; i < 3; i++ ) {
		const Scalar extinction = extinctions[i];
		const RISEPel deposited = RunOneDeposit( extinction, 0.0 );
		const Scalar beer = std::exp( -extinction * 1.0 );
		const RISEPel expected( beer, beer, beer );
		std::cout << "  extinction=" << extinction << std::endl;
		EXPECT_NEAR_PEL( deposited, expected, 1e-9,
			"deposited power == incoming * exp(-extinction*distance)" );
		// Red-proof-shape guard: the pre-fix formula deposited FULL
		// incoming power (1,1,1) regardless of extinction.  Assert we do
		// NOT match that for any extinction whose Beer term is < 1.
		if( beer < 1.0 - 1e-9 ) {
			const Scalar d = r_max( r_max(fabs(deposited[0]-1.0),fabs(deposited[1]-1.0)), fabs(deposited[2]-1.0) );
			EXPECT( d > 1e-6, "deposited power is NOT the unattenuated full incoming power (DL-39 red-proof shape)" );
		}
	}
}

static void TestBackscatterBalance()
{
	std::cout << "Sub-test 2: nonzero backscatter energy balance" << std::endl;

	const Scalar extinction = 0.5;
	const Scalar scattering = 0.3;
	const RISEPel deposited = RunOneDeposit( extinction, scattering );

	const Scalar beer = std::exp( -extinction * 1.0 );
	const Scalar depositedFrac = beer * ( 1.0 - scattering );
	const Scalar tracedFrac    = beer * scattering;
	const Scalar absorbedFrac  = 1.0 - beer;

	const RISEPel expectedDeposit( depositedFrac, depositedFrac, depositedFrac );
	EXPECT_NEAR_PEL( deposited, expectedDeposit, 1e-9,
		"deposited power == incoming * exp(-extinction*distance) * (1-scattering)" );

	// Energy balance: absorbed + deposited + traced-away == incoming
	// power, exactly, per channel.  depositedFrac here is the MEASURED
	// quantity (via `deposited`, asserted above); the identity below
	// re-derives the same close form the SPF documents (DL-01/DL-02)
	// for the two terms this test does not otherwise observe, so the
	// overall check is that the REAL deposit fix is consistent with the
	// documented Beer/scattering split, not a tautology about constants
	// chosen independently of the code under test.
	const Scalar total = absorbedFrac + ( deposited[0] ) + tracedFrac;
	EXPECT_NEAR_PEL( RISEPel(total,total,total), RISEPel(1,1,1), 1e-9,
		"absorbed_frac + deposited_frac(measured) + traced_frac == 1 (energy balance)" );
}

int main()
{
	GlobalLog();

	TestExtinctionSweep();
	TestBackscatterBalance();

	std::cout << std::endl;
	if( failed == 0 ) {
		std::cout << "ALL TESTS PASSED" << std::endl;
		return 0;
	} else {
		std::cout << failed << " CHECK(S) FAILED" << std::endl;
		return 1;
	}
}
