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
//      the real map) must sum to exactly 1 per channel.  NOTE: this
//      balance check is not independent evidence of a second, distinct
//      property -- deposited_frac IS beer*(1-scattering) by the SAME
//      construction Sub-test 1 already checks (Beer term), so the sum
//      is algebraically implied once Sub-test 1's per-channel formula
//      is confirmed; it is included as a redundant cross-check of the
//      arithmetic, not as coverage of a code path Sub-test 1 misses.
//    Sub-test 3 (P2-2, review round 3) -- the deposit rule is SCOPED to
//      the translucent exit.  `TranslucentPelPhotonMap::RadianceEstimate`
//      is the Jensen estimator (`sum(power_i) * brdf.value(...) /
//      (pi*r^2)`), so the STORED quantity must be the flux ARRIVING at
//      the surface -- the gather applies that surface's own BSDF itself.
//      Depositing the diffuse lobe's kray is correct only where that
//      kray is a TRANSPORT attenuation, i.e. at a translucent interior
//      exit (Beer * (1-scattering)).  A Lambertian wall's eRayDiffuse
//      kray is instead its ALBEDO (LambertianSPF.cpp), so depositing it
//      would make the gather read `power * albedo^2`; TranslucentSPF's
//      own ENTERING branch has the same shape (its diffuse kray is
//      `pRefFront`, a reflectance).  A round-2 revision of this test
//      asserted the albedo-weighted deposit as "an accuracy
//      improvement"; that was wrong, and this sub-test now asserts the
//      full arriving power at the Lambertian wall. That historical scope
//      control had no non-diffuse continuation. DL280 adds a mixed Phong
//      receiver: subtracting traced specular power was still wrong there,
//      since the gather itself applies the query BSDF. All ordinary
//      receivers now store full incoming power.
//
//    DL239 additionally prices the real deposit-to-gather response for
//      tilted frames. Tagged exit packets already contain Beer*(1-s), so
//      their gather uses the clipped cosine exit law, not front reflectance.
//      Ordinary incident packets retain their direction and use the BSDF.
//      Exact tagged roundtrip and legacy failure/no-mutation are covered.
//
//  Author: Aravind Krishnaswamy (RISE debt-cleanup, slice `translucent`)
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <vector>
#include <chrono>
#include <filesystem>
#include "../src/Library/Job.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Utilities/MemoryBuffer.h"
#include "../src/Library/Utilities/Color/ColorUtils.h"

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/IORStack.h"
#include "../src/Library/Intersection/RayIntersection.h"
#include "../src/Library/Interfaces/IPainter.h"
#include "../src/Library/Interfaces/IScalarPainter.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Materials/TranslucentMaterial.h"
#include "../src/Library/Materials/LambertianMaterial.h"
#include "../src/Library/Materials/IsotropicPhongMaterial.h"
#include "../src/Library/PhotonMapping/TranslucentPelPhotonTracer.h"
#include "../src/Library/PhotonMapping/TranslucentPelPhotonMap.h"
#include "../src/Library/Managers/ObjectManager.h"
#include "../src/Library/Scene.h"

#include "TestStubObject.h"

using namespace RISE;
using namespace RISE::Implementation;

static int failed = 0;
static int checks = 0;

#define EXPECT( cond, msg ) do { \
	checks++; \
	if( !(cond) ) { \
		std::cout << "FAIL: " << __FILE__ << ":" << __LINE__ << " " << msg << std::endl; \
		failed++; \
	} \
} while(0)

#define EXPECT_NEAR_PEL( a, b, tol, msg ) do { \
	checks++; \
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
		Scalar tilt;
	public:
		SingleHitThenMissManager( const IObject& obj, const IMaterial& mat, Scalar angle = 0 ) :
			ObjectManager( false, false, 4, 8 ), object( obj ), material( mat ), callCount( 0 ), tilt( angle )
		{}
		void Reset() const { callCount = 0; }
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
			ri.geometric.vNormal = Vector3( sin(tilt), 0, cos(tilt) );
			ri.geometric.vGeomNormal = normal;
			ri.geometric.onb.CreateFromW( ri.geometric.vNormal );
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
		bool AllPayloads( const Vector3& wi, bool exit ) const {
			for(const auto& p:vphotons) if(p.diffuseExit!=exit || p.incomingDirection.x!=wi.x || p.incomingDirection.y!=wi.y || p.incomingDirection.z!=wi.z) return false;
			return !vphotons.empty();
		}
	};

	// DL-39 P3 follow-up: reports TWO hits -- the translucent interior
	// segment first (matching SingleHitThenMissManager), then whatever
	// SECOND object/material the caller supplies (the recursive
	// TracePhoton call driven by the translucent object's own
	// eRayTranslucent backscatter ray) -- then misses forever, exactly
	// like the single-hit manager.  Both hits report the same aligned,
	// untilted normal so neither DL-45's exit gate nor the Lambertian
	// SPF's own geometric-horizon gate is in play here; this fixture is
	// purely about what TracePhoton deposits at a NON-translucent
	// surface reached through an eRayTranslucent-typed ray, not about
	// direction validity.
	class TwoHitThenMissManager : public ObjectManager
	{
		const IObject& object1;
		const IMaterial& material1;
		const IObject& object2;
		const IMaterial& material2;
		mutable int callCount;
	public:
		TwoHitThenMissManager( const IObject& obj1, const IMaterial& mat1,
			const IObject& obj2, const IMaterial& mat2 ) :
			ObjectManager( false, false, 4, 8 ),
			object1( obj1 ), material1( mat1 ), object2( obj2 ), material2( mat2 ),
			callCount( 0 )
		{}
		void IntersectRay( RayIntersection& ri, bool, bool, bool ) const override
		{
			const int call = callCount++;
			if( call > 1 ) {
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
			ri.pObject = ( call == 0 ) ? &object1 : &object2;
			ri.pMaterial = ( call == 0 ) ? &material1 : &material2;
		}
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

	// P2-2 (review round 3): DL-39's "sum the diffuse lobe's own kray"
	// rule is SCOPED to the translucent interior exit -- the one place
	// that kray is a transport attenuation rather than a reflectance.
	// `TracePhoton` re-fetches `ri.pMaterial->GetSPF()` at EVERY hit, so
	// when the translucent object's own eRayTranslucent-typed backscatter
	// ray (which carries `bFromTranslucent=true` into its recursive call)
	// goes on to hit a completely different, non-translucent surface, the
	// deposit there must still be the flux ARRIVING at that surface,
	// because `TranslucentPelPhotonMap::RadianceEstimate` multiplies by
	// the surface's own BSDF at gather time.  A Lambertian wall's
	// eRayDiffuse kray IS its albedo (LambertianSPF.cpp), so depositing
	// it would make the gather read `power * albedo^2`.
	//
	// This sub-test exercises that directly: a fully-backscattering
	// (scattering=1, extinction=0) translucent object hands its ENTIRE
	// incoming power to the backscatter ray, which then hits a real
	// LambertianMaterial; the single photon stored at THAT hit must equal
	// the full arriving power, independent of the wall's colour.  The
	// wall's albedo is deliberately non-grey (0.7,0.5,0.3) so an
	// albedo-weighted deposit is distinguishable per channel.
	RISEPel RunLambertianWallDeposit( const RISEPel& wallAlbedo )
	{
		UniformColorPainter* front = new UniformColorPainter( RISEPel( 0.2, 0.2, 0.2 ) );  front->addref();
		UniformColorPainter* trans = new UniformColorPainter( RISEPel( 0.4, 0.4, 0.4 ) );  trans->addref();
		UniformScalarPainter* ext = new UniformScalarPainter( 0.0 );  ext->addref();
		UniformScalarPainter* phongN = new UniformScalarPainter( 1.0 );  phongN->addref();
		UniformScalarPainter* scat = new UniformScalarPainter( 1.0 );  scat->addref();

		TranslucentMaterial* transMaterial = new TranslucentMaterial( *front, *trans, *ext, *phongN, *scat );
		transMaterial->addref();

		UniformColorPainter* wallReflectance = new UniformColorPainter( wallAlbedo );  wallReflectance->addref();
		LambertianMaterial* wallMaterial = new LambertianMaterial( *wallReflectance );
		wallMaterial->addref();

		StubObject* transObj = new StubObject();  transObj->addref();
		StubObject* wallObj = new StubObject();  wallObj->addref();

		TwoHitThenMissManager* manager = new TwoHitThenMissManager(
			*transObj, *transMaterial, *wallObj, *wallMaterial );
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

		// Two hits happen (translucent interior segment, then the
		// Lambertian wall through its backscatter ray), but only ONE
		// deposit call stores a photon: `TranslucentPelPhotonMap::Store`
		// silently drops a zero-power deposit (`MaxValue(power)<=0`,
		// TranslucentPelPhotonMap.cpp), and the interior segment's OWN
		// deposit is exactly zero here -- it IS a translucent exit, so it
		// deposits its exit lobe's transport weight
		// `Beer * (1-scattering)`, and scattering=1 leaves that at 0.  So
		// the map's one stored photon is the Lambertian wall's, which
		// takes the non-exit branch (full arriving power).
		EXPECT( map.StoredCount() == 1,
			"exactly one non-zero photon deposited (the Lambertian-wall hit; the interior segment's own deposit is zero and silently dropped)" );

		RISEPel wallStored( 0, 0, 0 );
		if( map.StoredCount() >= 1 ) {
			wallStored = map.StoredPower( 0 );
		}

		manager->release();
		scene->release();
		tracer->release();
		transMaterial->release();
		wallMaterial->release();
		transObj->release();
		wallObj->release();
		scat->release();
		phongN->release();
		ext->release();
		trans->release();
		front->release();
		wallReflectance->release();

		return wallStored;
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

static void TestLambertianWallDeposit()
{
	std::cout << "Sub-test 3: deposit at a non-translucent surface reached via eRayTranslucent (P2-2)" << std::endl;

	const RISEPel wallAlbedo( 0.7, 0.5, 0.3 );
	const RISEPel deposited = RunLambertianWallDeposit( wallAlbedo );

	// The Jensen gather at this wall multiplies the stored power by the
	// wall's own BSDF, so the stored power must be the ARRIVING flux --
	// here the full (1,1,1) the fully-backscattering translucent object
	// handed on, NOT that times the wall's albedo.
	EXPECT_NEAR_PEL( deposited, RISEPel( 1, 1, 1 ), 1e-9,
		"Lambertian wall reached via the translucent object's own backscatter ray deposits the FULL arriving power" );
	// Red-proof-shape guard: a deposit that had been weighted by the
	// wall's own albedo would read (0.7,0.5,0.3) here, and the gather
	// would then square it.
	{
		const Scalar d = r_max( r_max(fabs(deposited[0]-wallAlbedo[0]),fabs(deposited[1]-wallAlbedo[1])), fabs(deposited[2]-wallAlbedo[2]) );
		EXPECT( d > 1e-6, "deposited power is NOT albedo-weighted (P2-2 red-proof shape)" );
	}
}

// DL239: drive actual deposits through the public gather. The two packet
// meanings have different responses: ordinary incident flux receives the
// query BSDF; an exit packet already paid Beer*(1-s) and represents a
// clipped-cosine re-emission lobe, not another front-reflection interaction.
static void TestDirectionalGathers()
{
 for( bool exit : {false,true} ) for( double angle : {0.,30.,60.} ) {
  const double tilt=angle*PI/180;
  auto* front=new UniformColorPainter(RISEPel(.2,.4,.7));
  auto* transmission=new UniformColorPainter(RISEPel(.4));
  auto* extinction=new UniformScalarPainter(.5);
  auto* exponent=new UniformScalarPainter(1.);
  auto* scattering=new UniformScalarPainter(.3);
  auto* trans=new TranslucentMaterial(*front,*transmission,*extinction,*exponent,*scattering);
  auto* wall=new LambertianMaterial(*front);
  IMaterial* material=exit?static_cast<IMaterial*>(trans):static_cast<IMaterial*>(wall);
  auto* object=new StubObject();
  auto* manager=new SingleHitThenMissManager(*object,*material,tilt);
  auto* scene=new Scene();scene->SetObjectManager(manager);
  auto* tracer=new TestTranslucentPelPhotonTracer();tracer->AttachScene(scene);
  InspectableTranslucentPelPhotonMap map(2602);
  // The interior packet arrives outward, opposite the ordinary-wall case.
  const Vector3 wi=exit?Vector3(0,0,-1):Vector3(.6,0,.8);
  std::vector<double> distances;
  for(int y=-25;y<=25;++y)for(int x=-25;x<=25;++x){
   const Point3 p(x*.01,y*.01,0);
   manager->Reset();IORStack stack=exit?MakeInsideStack(object):IORStack(1.0);
   const Ray incoming(Point3(p.x+wi.x,p.y+wi.y,p.z+wi.z),-wi);
   tracer->TestTrace(incoming,RISEPel(1.),true,map,stack,1);
   const double d2=p.x*p.x+p.y*p.y;if(d2<.04)distances.push_back(d2);
  }
  EXPECT(map.StoredCount()==2601,"directional gather has every live tracer deposit");
  EXPECT(map.AllPayloads(wi,exit),"every live producer records exact incident direction and response kind");
  EXPECT(!map.Store(RISEPel(99.),Point3(9,9,9)),"legacy directionless Store fails explicitly with spare capacity");
  EXPECT(map.StoredCount()==2601 && map.AllPayloads(wi,exit),"legacy directionless Store leaves existing packets unchanged");
  map.Balance();map.SetGatherParams(.2,.05,10,400,nullptr);
  auto* serialized=new MemoryBuffer();map.Serialize(*serialized);const unsigned serializedBytes=serialized->getCurPos();serialized->seek(IBuffer::START,0);
  InspectableTranslucentPelPhotonMap restored(0);const bool loaded=restored.DeserializeChecked(*serialized);
  EXPECT(loaded && restored.StoredCount()==2601,"tagged translucent map roundtrip retains every packet");
  EXPECT(restored.AllPayloads(wi,exit),"tagged roundtrip retains exact direction and kind");
  auto* truncated=new MemoryBuffer(serialized->Pointer(),serializedBytes-1,false);
  EXPECT(!restored.DeserializeChecked(*truncated),"truncated tagged map reports failure");
  EXPECT(restored.StoredCount()==2601 && restored.AllPayloads(wi,exit),"truncated tagged load retains prior packet field");
  truncated->release();
  std::sort(distances.begin(),distances.end());const double density=400/(PI*distances[399]);
  for(double view:{0.,70.,150.}){
   const double v=view*PI/180;const Vector3 wo(sin(v),0,cos(v));
   RayIntersectionGeometric query(Ray(Point3(0,0,1),-wo),nullRasterizerState);
   query.ptIntersection=Point3(0,0,0);query.vNormal=Vector3(sin(tilt),0,cos(tilt));query.vGeomNormal=Vector3(0,0,1);query.onb.CreateFromW(query.vNormal);
   RISEPel got;map.RadianceEstimate(got,query,*material->GetBSDF());
   RISEPel roundtrip;restored.RadianceEstimate(roundtrip,query,*material->GetBSDF());
   EXPECT_NEAR_PEL(roundtrip,got,1e-12,"tagged roundtrip preserves directional query response");
   RISEPel expected(0.);
   const double incomingResponse=fabs(Vector3Ops::Dot(query.vNormal,wi)/wi.z);
   if(exit){
    // Independently cancel p_exit(wo) * adjoint / |Ng.wo|.
    // p_exit = max(Ns.wo,0)/(pi * (1+Ns.Ng)/2), clipped to Ng.wo>0.
    if(wo.z>0 && Vector3Ops::Dot(query.vNormal,wo)>0)
     expected=RISEPel(density*exp(-.5)*.7*incomingResponse/(PI*((1+cos(tilt))*.5)));
   }else expected=material->GetBSDF()->value(wi,query)*(density*incomingResponse);
   for(int c=0;c<3;++c){
    const bool ok=std::isfinite(got[c])&&fabs(got[c]-expected[c])<1e-9*std::max(1.,fabs(expected[c]));
    ++checks;if(!ok)++failed;
    std::printf("%s DL239 translucent gather kind=%s tilt=%.17g view=%.17g channel=%d got=%.17g expected=%.17g density=%.17g\n",ok?"PASS":"FAIL",exit?"exit-lobe":"incident-flux",angle,view,c,got[c],expected[c],density);
   }
  }
  // Real Job load: unsupported old directionless data must not replace
  // an installed, populated directional map. The new format must load.
  if(angle==0){
   auto* job=new Job();job->GetScene()->SetTranslucentPelMap(&map);
   auto* old=new MemoryBuffer(512);old->setUInt(1);old->setUInt(0);old->setDouble(.04);old->setDouble(.05);old->setUInt(0);old->setUInt(1);old->setDouble(1.);BoundingBox(Point3(-1,-1,-1),Point3(1,1,1)).Serialize(*old);old->setUInt(1);Point3Ops::Serialize(Point3(0,0,0),*old);old->setUChar(0);ColorUtils::SerializeRGBPel(RISEPel(1.),*old);
   const auto path=std::filesystem::temp_directory_path()/("rise_dl239_trans_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".pmap");
   EXPECT(old->DumpToFileToCursor(path.string().c_str()),"legacy translucent fixture written");
   EXPECT(!job->LoadTranslucentPelPhotonmap(path.string().c_str()),"legacy directionless file reports unsupported reconstruction");
   EXPECT(job->GetScene()->GetTranslucentPelMap()==&map && map.StoredCount()==2601,"failed legacy load preserves installed valid map");
   EXPECT(serialized->DumpToFileToCursor(path.string().c_str()),"new tagged fixture written");
   EXPECT(job->LoadTranslucentPelPhotonmap(path.string().c_str()),"Job loads tagged directional map");
   EXPECT(job->GetScene()->GetTranslucentPelMapMutable()->NumStored()==2601,"Job replacement retains full tagged population");
   std::filesystem::remove(path);old->release();job->release();
  }
  serialized->release();
  tracer->release();scene->release();manager->release();object->release();wall->release();trans->release();front->release();transmission->release();extinction->release();exponent->release();scattering->release();
 }
}

// A sampled glossy continuation is not a subtraction from the incident
// flux used by a photon-density BSDF estimate at this same surface.
static void TestMixedMaterialIncidentDeposit()
{
 auto* diffuse=new UniformColorPainter(RISEPel(.2,.3,.4));
 auto* glossy=new UniformColorPainter(RISEPel(.3,.2,.1));
 auto* exponent=new UniformScalarPainter(4.);
 auto* material=new IsotropicPhongMaterial(*diffuse,*glossy,*exponent);
 auto* object=new StubObject();auto* manager=new SingleHitThenMissManager(*object,*material);
 auto* scene=new Scene();scene->SetObjectManager(manager);
 auto* tracer=new TestTranslucentPelPhotonTracer();tracer->AttachScene(scene);
 InspectableTranslucentPelPhotonMap map(32);
 for(int i=0;i<32;++i){manager->Reset();IORStack stack(1.0);tracer->TestTrace(Ray(Point3(i*.01,0,1),Vector3(0,0,-1)),RISEPel(1.),true,map,stack,1);}
 EXPECT(map.StoredCount()==32,"mixed diffuse/glossy receiver stores every arriving packet");
 for(unsigned i=0;i<map.StoredCount();++i)for(int c=0;c<3;++c){const double got=map.StoredPower(i)[c];const bool ok=std::isfinite(got)&&fabs(got-1.)<1e-12;++checks;if(!ok)++failed;std::printf("%s DL280 actual mixed-material incident packet sample=%u channel=%d got=%.17g expected=1\n",ok?"PASS":"FAIL",i,c,got);}
 tracer->release();scene->release();manager->release();object->release();material->release();diffuse->release();glossy->release();exponent->release();
}

int main()
{
	GlobalLog();

	TestExtinctionSweep();
	TestBackscatterBalance();
	TestLambertianWallDeposit();
	TestDirectionalGathers();
	TestMixedMaterialIncidentDeposit();

	std::cout << std::endl;
	std::cout << "TranslucentPhotonEnergyTest: " << checks << " checks, " << failed << " failures" << std::endl;
	if( failed == 0 ) {
		std::cout << "ALL TESTS PASSED" << std::endl;
		return 0;
	} else {
		std::cout << failed << " CHECK(S) FAILED" << std::endl;
		return 1;
	}
}
