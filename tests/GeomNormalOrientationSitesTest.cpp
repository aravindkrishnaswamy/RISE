//////////////////////////////////////////////////////////////////////
//
//  GeomNormalOrientationSitesTest.cpp - Regression guard for DL-70:
//    sign-sensitive `vGeomNormal` consumers that performed an
//    entry-vs-exit / front-vs-back test WITHOUT undoing the
//    orient-to-ray flip a double-sided triangle mesh (and
//    `ClippedPlaneGeometry` / `BezierPatchGeometry` on a back-face hit)
//    applies, recorded in
//    `RayIntersectionGeometric::bGeomNormalOrientedToRay`.
//
//  THE BUG PATTERN, IN ONE SENTENCE
//
//    On such a hit the reported `vGeomNormal` ALWAYS opposes the ray --
//    on a true entry AND on a true exit alike -- so
//    `Dot( vGeomNormal, rayDir ) < 0` is a constant `true` and every
//    consumer that used it as "am I entering this solid?" reads
//    "entering" at every crossing and never sees an exit.
//
//  THE FIX
//
//    `RayIntersectionGeometric::UnflippedGeomNormal()` /
//    `TrueGeomFacing()` / `HasTrueGeomSide()` -- the single shared
//    recovery `(oriented && !rayDerived) ? -vGeomNormal : vGeomNormal`,
//    with `HairGeometry`'s ray-DERIVED normal excluded (there is no real
//    surface side to recover, so such a consumer must skip).
//
//  WHAT EACH SUB-TEST PROVES
//
//    Sub-test 1 (record level -- the shared mechanism, on production
//      data).  A CONSISTENCY PIN, not a red-proof: it is green before
//      and after the site fixes, because it evaluates BOTH the defective
//      predicate form and its recovered twin side by side and asserts
//      which answer each gives.  It exists because five of the fourteen
//      DL-70 sites sit in functions no test can reach (two file-static
//      shadow-walk helpers, an anonymous-namespace SMS photon tracer,
//      the BSSRDF front-face gates buried in the PT/BDPT bounce loops,
//      and two shader ops that need a volume file on disk / a full
//      shading rig).  What it DOES establish, on records the production
//      geometry really produced from a real closed double-sided cube, a
//      single-sided cube, an analytic sphere and a hair strand: each of
//      those sites' predicate, spelled the pre-fix way, takes the wrong
//      branch at a true exit, and spelled the post-fix way takes the
//      right one -- and that both spellings agree on the single-sided
//      and analytic controls, so the fix cannot regress them.
//
//    Sub-test 2 (site 3, END-TO-END).  `RayCaster::CastShadowRayTransmittance`
//      through a CLOSED double-sided dielectric cube at normal
//      incidence.  The walk's `bEntering` selects the Fresnel (Ni, Nt)
//      pair; pre-fix the exit face reads "entering" too, so it computes
//      glass->glass (matched indices, F = 0, T = 1) instead of
//      glass->air, and the slab transmits ONE interface's worth instead
//      of two.  Money assertion: the closed-form (1-F)^2, which the
//      single-sided twin already satisfies.
//
//    Sub-test 3 (sites 1/2, END-TO-END).  `LightSampler::EvaluateDirectLighting`
//      Step 1 (a directional light) with a per-object interior medium
//      inside a CLOSED double-sided dielectric cube.  The medium-stack
//      shadow walk pushes on `ndotd < 0` and removes otherwise; pre-fix
//      the exit crossing PUSHES again, the stack is never emptied, and
//      the medium is then applied over the remaining `RISE_INFINITY`
//      distance to the light -- extinguishing it completely.  Money
//      assertion: the directional light survives at exactly
//      `exp(-sigma_a * chord)`, which the single-sided twin already
//      satisfies.  `BDPTIntegrator`'s connection-walk twin is the same
//      code shape on the same record and is fixed in the same commit.
//
//  Author: Aravind Krishnaswamy (RISE debt-cleanup, slice `dl70`)
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <iostream>

#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Geometry/HairGeometry.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Managers/ObjectManager.h"
#include "../src/Library/Managers/LightManager.h"
#include "../src/Library/Rendering/LuminaryManager.h"
#include "../src/Library/Rendering/RayCaster.h"
#include "../src/Library/Lights/DirectionalLight.h"
#include "../src/Library/Lights/LightSampler.h"
#include "../src/Library/Scene.h"
#include "../src/Library/Materials/DielectricMaterial.h"
#include "../src/Library/Materials/LambertianMaterial.h"
#include "../src/Library/Materials/HomogeneousMedium.h"
#include "../src/Library/Materials/IsotropicPhaseFunction.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Utilities/IORStack.h"
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

static void CheckClose( double got, double want, double tol, const char* testName )
{
	const bool ok = std::fabs( got - want ) <= tol;
	if( ok ) {
		passCount++;
	} else {
		failCount++;
		std::cout << "  FAIL: " << testName
			<< "  (got " << got << ", want " << want << " +/- " << tol << ")" << std::endl;
	}
}

namespace
{
	Vector3 AxisVector( int axis, Scalar value )
	{
		Vector3 v( 0, 0, 0 );
		if( axis == 0 ) v.x = value;
		else if( axis == 1 ) v.y = value;
		else v.z = value;
		return v;
	}

	//! A closed, axis-aligned cube spanning [-halfSize, halfSize]^3, built
	//! from six flat-shaded UNSHARED-vertex quads (24 vertices, 12
	//! triangles) so no face's authored normal can leak onto a neighbour
	//! through Phong interpolation.  `bDoubleSided` selects the geometry
	//! mode under test; everything else about the two cubes is identical,
	//! which is what makes the single-sided cube a valid control.
	//!
	//! (Mirrors tests/TranslucentDoubleSidedTest.cpp's `BuildDoubleSidedCube`,
	//! generalised over the flag and the size.)
	TriangleMeshGeometryIndexed* BuildCube( bool bDoubleSided, Scalar halfSize )
	{
		TriangleMeshGeometryIndexed* mesh = new TriangleMeshGeometryIndexed( bDoubleSided, false );
		mesh->BeginIndexedTriangles();

		unsigned int nextVertex = 0;
		auto addFace = [&]( int axis, Scalar sign )
		{
			const Vector3 outward = AxisVector( axis, sign );
			const int uAxis = (axis + 1) % 3;
			const int vAxis = (axis + 2) % 3;
			const unsigned int base = nextVertex;
			for( int su = -1; su <= 1; su += 2 ) {
				for( int sv = -1; sv <= 1; sv += 2 ) {
					Point3 p( 0, 0, 0 );
					p[axis] = sign * halfSize;
					p[uAxis] = static_cast<Scalar>( su ) * halfSize;
					p[vAxis] = static_cast<Scalar>( sv ) * halfSize;
					mesh->AddVertex( p );
					mesh->AddNormal( outward );
					mesh->AddTexCoord( Point2( 0, 0 ) );
				}
			}
			IndexedTriangle t1, t2;
			t1.iVertices[0] = base + 0; t1.iVertices[1] = base + 1; t1.iVertices[2] = base + 2;
			t2.iVertices[0] = base + 1; t2.iVertices[1] = base + 3; t2.iVertices[2] = base + 2;
			for( int k = 0; k < 3; k++ ) {
				t1.iNormals[k] = t1.iVertices[k]; t1.iCoords[k] = t1.iVertices[k];
				t2.iNormals[k] = t2.iVertices[k]; t2.iCoords[k] = t2.iVertices[k];
			}
			mesh->AddIndexedTriangle( t1 );
			mesh->AddIndexedTriangle( t2 );
			nextVertex += 4;
		};

		addFace( 0, +1 ); addFace( 0, -1 );
		addFace( 1, +1 ); addFace( 1, -1 );
		addFace( 2, +1 ); addFace( 2, -1 );

		mesh->DoneIndexedTriangles();
		return mesh;
	}

	//! Production intersection call (matches TranslucentDoubleSidedTest's
	//! `Hit`): front + back faces, exit info, real Object::IntersectRay.
	void Hit( IObject* pObj, const Ray& r, RayIntersection& ri )
	{
		ri.geometric.bHit = false;
		ri.geometric.range = RISE_INFINITY;
		ri.geometric.range2 = RISE_INFINITY;
		ri.geometric.ray = r;
		pObj->IntersectRay( ri, RISE_INFINITY, true, true, true );
	}

	//! One straight, wide hair strand along X through the origin (the
	//! `MakeWideHairStrand` pattern from tests/HairSSSEntryNormalTest.cpp),
	//! so a +Z ray near the origin hits it squarely.  `HairGeometry`
	//! fabricates its geometric normal FROM THE RAY and advertises that
	//! via `bGeomNormalRayDerived`.
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

	//! The RayCaster factory takes an IShader by reference; a
	//! shader with no ops is enough for the shadow / NEE paths under
	//! test here (neither ever calls Shade).
	IShader* MakeTrivialShader()
	{
		std::vector<IShaderOp*> noOps;
		IShader* pShader = 0;
		RISE_API_CreateStandardShader( &pShader, noOps );
		return pShader;
	}

	//! A clear dielectric: tau = 1 (no tint), the given IOR, and the
	//! documented delta pass-through `scattering` (CLAUDE.md: a
	//! dielectric's `scattering 0.0` is maximally DIFFUSE transmission;
	//! delta pass-through -- and therefore `clearTransmission`, which
	//! `CastShadowRayTransmittance` gates on -- is `scattering 1000000`).
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
}

//////////////////////////////////////////////////////////////////////
//  Sub-test 1: the shared mechanism, on records the production
//  geometry actually produced.
//////////////////////////////////////////////////////////////////////
static void TestRecordLevelRecovery()
{
	std::cout << "Sub-test 1: DL-70 predicate forms on real double-sided / single-sided / hair records"
		<< std::endl;

	TriangleMeshGeometryIndexed* gD = BuildCube( /*bDoubleSided*/true, 1.0 );
	Object* oD = new Object( gD );  safe_release( gD );
	oD->FinalizeTransformations();

	TriangleMeshGeometryIndexed* gS = BuildCube( /*bDoubleSided*/false, 1.0 );
	Object* oS = new Object( gS );  safe_release( gS );
	oS->FinalizeTransformations();

	SphereGeometry* gSph = new SphereGeometry( 1.0 );
	Object* oSph = new Object( gSph );  safe_release( gSph );
	oSph->FinalizeTransformations();

	RasterizerState rast = {0};

	// Two incidences: normal (tilt 0) and a genuine tilt, so the
	// classification is exercised away from the degenerate head-on case.
	// Both rays START INSIDE the cube travelling outward, i.e. every hit
	// below is a TRUE EXIT -- the case the flip makes indistinguishable
	// from an entry.
	struct Case { const char* name; Vector3 dir; };
	const Case cases[2] = {
		{ "tilt 0",  Vector3( 0, 0, 1 ) },
		{ "tilt 30", Vector3( 0, std::sin( 30.0 * PI / 180.0 ), std::cos( 30.0 * PI / 180.0 ) ) }
	};

	for( int c = 0; c < 2; c++ )
	{
		const Vector3 dir = Vector3Ops::Normalize( cases[c].dir );
		// Offset in x so the chord misses the quads' shared diagonal
		// (which runs corner-to-corner through (0,0) on every face).
		const Ray rayOut( Point3( 0.3, 0, -0.5 ), dir );

		RayIntersection riD( rayOut, rast );
		Hit( oD, rayOut, riD );
		RayIntersection riS( rayOut, rast );
		Hit( oS, rayOut, riS );

		char buf[256];

		std::snprintf( buf, sizeof(buf), "(%s) double-sided cube: the outward chord hits", cases[c].name );
		Check( riD.geometric.bHit, buf );
		std::snprintf( buf, sizeof(buf), "(%s) single-sided cube: the outward chord hits", cases[c].name );
		Check( riS.geometric.bHit, buf );
		if( !riD.geometric.bHit || !riS.geometric.bHit ) { continue; }

		// --- fixture sanity: the geometry really did flip, and the
		// single-sided control really did not. ---
		std::snprintf( buf, sizeof(buf), "(%s) fixture: double-sided hit reports bGeomNormalOrientedToRay", cases[c].name );
		Check( riD.geometric.bGeomNormalOrientedToRay, buf );
		std::snprintf( buf, sizeof(buf), "(%s) fixture: the flip is not ray-DERIVED (not hair)", cases[c].name );
		Check( !riD.geometric.bGeomNormalRayDerived, buf );
		std::snprintf( buf, sizeof(buf), "(%s) control: single-sided hit does NOT flip", cases[c].name );
		Check( !riS.geometric.bGeomNormalOrientedToRay, buf );

		// Ground truth, known independently of anything the record says:
		// the ray started inside the cube and is travelling outward, so
		// this hit is a TRUE EXIT and the true outward normal has a
		// POSITIVE dot with the ray direction.
		const Scalar trueFacing = riD.geometric.TrueGeomFacing( dir );
		const Scalar rawFacing  = Vector3Ops::Dot( riD.geometric.vGeomNormal, dir );

		std::snprintf( buf, sizeof(buf), "(%s) recovered facing is POSITIVE (a true exit)", cases[c].name );
		Check( trueFacing > 0, buf );
		std::snprintf( buf, sizeof(buf), "(%s) raw facing is NEGATIVE -- the defect DL-70 describes", cases[c].name );
		Check( rawFacing < 0, buf );
		std::snprintf( buf, sizeof(buf), "(%s) recovery is exactly the sign flip of the raw read", cases[c].name );
		CheckClose( trueFacing, -rawFacing, 1e-12, buf );

		// The single-sided control's raw read is already correct, and the
		// recovery is a no-op on it -- i.e. the fix cannot regress it.
		std::snprintf( buf, sizeof(buf), "(%s) control: raw facing already POSITIVE", cases[c].name );
		Check( Vector3Ops::Dot( riS.geometric.vGeomNormal, dir ) > 0, buf );
		std::snprintf( buf, sizeof(buf), "(%s) control: recovery is a no-op", cases[c].name );
		CheckClose( riS.geometric.TrueGeomFacing( dir ),
			Vector3Ops::Dot( riS.geometric.vGeomNormal, dir ), 1e-12, buf );

		// --- The six DL-70 predicate FORMS, verbatim, on this record. ---
		// Each pair is (what the site computed pre-fix, what it computes
		// now).  The "want" column is the fixture's ground truth above.

		// (1)/(2) LightSampler / BDPTIntegrator medium push-pop:
		//         `ndotd < 0` => push (entering), else remove (exiting).
		std::snprintf( buf, sizeof(buf), "(%s) form 1/2 medium push-pop: raw says PUSH on a true exit", cases[c].name );
		Check( rawFacing < 0, buf );
		std::snprintf( buf, sizeof(buf), "(%s) form 1/2 medium push-pop: recovered says REMOVE", cases[c].name );
		Check( !(trueFacing < 0), buf );

		// (3) RayCaster transmissive shadow: `bEntering = cosRaw < 0`.
		std::snprintf( buf, sizeof(buf), "(%s) form 3 bEntering: raw says ENTERING on a true exit", cases[c].name );
		Check( rawFacing < 0, buf );
		std::snprintf( buf, sizeof(buf), "(%s) form 3 bEntering: recovered says EXITING", cases[c].name );
		Check( !(trueFacing < 0), buf );

		// (4) DirectVolumeRenderingShader: `cosine = -Dot(n, dir)`,
		//     `cosine < NEARZERO` => leaving (shade the volume now).
		std::snprintf( buf, sizeof(buf), "(%s) form 4 DVR leaving-test: raw says NOT leaving", cases[c].name );
		Check( !(-rawFacing < NEARZERO), buf );
		std::snprintf( buf, sizeof(buf), "(%s) form 4 DVR leaving-test: recovered says LEAVING", cases[c].name );
		Check( -trueFacing < NEARZERO, buf );

		// (5) SMSPhotonMap `bEntering = cosI < 0` on a reflection vertex.
		//     Same scalar as form 3; kept as its own row so a future
		//     regression names the site it broke.
		std::snprintf( buf, sizeof(buf), "(%s) form 5 SMS bEntering: raw says ENTERING", cases[c].name );
		Check( rawFacing < 0, buf );
		std::snprintf( buf, sizeof(buf), "(%s) form 5 SMS bEntering: recovered says EXITING", cases[c].name );
		Check( !(trueFacing < 0), buf );

		// (6) BSSRDF front-face gate `cosInGeom = Dot(n, wo) > NEARZERO`,
		//     where `wo = -rayDir`.  A back-face (interior) hit must NOT
		//     take the BSSRDF branch.
		const Vector3 wo = -dir;
		std::snprintf( buf, sizeof(buf), "(%s) form 6 BSSRDF gate: raw ADMITS a back-face hit", cases[c].name );
		Check( Vector3Ops::Dot( riD.geometric.vGeomNormal, wo ) > NEARZERO, buf );
		std::snprintf( buf, sizeof(buf), "(%s) form 6 BSSRDF gate: recovered REJECTS it", cases[c].name );
		Check( !(riD.geometric.TrueGeomFacing( wo ) > NEARZERO), buf );

		// (7) TransparencyShaderOp `bOneSided` cull: `Dot(dir, n) > 0`
		//     => the ray came from behind, so discard the blend.
		std::snprintf( buf, sizeof(buf), "(%s) form 7 bOneSided cull: raw never fires", cases[c].name );
		Check( !(rawFacing > 0), buf );
		std::snprintf( buf, sizeof(buf), "(%s) form 7 bOneSided cull: recovered fires on the back face", cases[c].name );
		Check( trueFacing > 0, buf );
	}

	// --- Analytic-primitive control: a sphere never flips, so every one
	// of the forms above is unchanged by the fix on it. ---
	{
		const Vector3 dir( 0, 0, 1 );
		const Ray rayOut( Point3( 0.3, 0, -0.2 ), dir );
		RayIntersection ri( rayOut, rast );
		Hit( oSph, rayOut, ri );
		Check( ri.geometric.bHit, "(sphere) the outward chord hits" );
		if( ri.geometric.bHit ) {
			Check( !ri.geometric.bGeomNormalOrientedToRay,
				"(sphere) analytic primitive never sets bGeomNormalOrientedToRay" );
			CheckClose( ri.geometric.TrueGeomFacing( dir ),
				Vector3Ops::Dot( ri.geometric.vGeomNormal, dir ), 1e-12,
				"(sphere) recovery is a no-op on an analytic primitive" );
		}
	}

	oSph->release();
	oS->release();
	oD->release();
}

//////////////////////////////////////////////////////////////////////
//  Sub-test 1b: HairGeometry -- the recovery must be SKIPPED, not
//  applied.
//////////////////////////////////////////////////////////////////////
static void TestHairIsSkipped()
{
	std::cout << "Sub-test 1b: HairGeometry reports a RAY-DERIVED normal (recovery must be skipped)"
		<< std::endl;

	Object* o = BuildWideStrand();

	RasterizerState rast = {0};
	const Ray ray( Point3( 0, 0, -2 ), Vector3( 0, 0, 1 ) );
	RayIntersection ri( ray, rast );
	Hit( o, ray, ri );

	Check( ri.geometric.bHit, "(hair) the strand is hit" );
	if( ri.geometric.bHit ) {
		Check( ri.geometric.bGeomNormalRayDerived,
			"(hair) white-box: HairGeometry sets bGeomNormalRayDerived" );
		Check( !ri.geometric.HasTrueGeomSide(),
			"(hair) HasTrueGeomSide() is false -- a which-side consumer must SKIP" );
		// And the recovery is deliberately the IDENTITY there, so a
		// consumer that forgets to skip gets the reported (ray-facing)
		// normal rather than a fabricated "always exiting" one.
		CheckClose( ri.geometric.TrueGeomFacing( ray.Dir() ),
			Vector3Ops::Dot( ri.geometric.vGeomNormal, ray.Dir() ), 1e-12,
			"(hair) UnflippedGeomNormal() is the identity on a ray-derived normal" );
	}

	o->release();
}

//////////////////////////////////////////////////////////////////////
//  Sub-test 2 (site 3): RayCaster::CastShadowRayTransmittance.
//////////////////////////////////////////////////////////////////////
static void TestTransmissiveShadowFresnelPair()
{
	std::cout << "Sub-test 2: transmissive-shadow Fresnel pair through a double-sided dielectric cube (site 3)"
		<< std::endl;

	const Scalar eta = 1.5;
	const Scalar F = ( (eta - 1.0) / (eta + 1.0) ) * ( (eta - 1.0) / (eta + 1.0) );
	const Scalar slabT = ( 1.0 - F ) * ( 1.0 - F );		// TWO interfaces

	for( int variant = 0; variant < 2; variant++ )
	{
		const bool bDoubleSided = ( variant == 1 );
		const char* label = bDoubleSided ? "double-sided" : "single-sided (control)";

		TriangleMeshGeometryIndexed* g = BuildCube( bDoubleSided, 1.0 );
		Object* o = new Object( g );  safe_release( g );
		o->FinalizeTransformations();

		DielectricFixture fx( eta );
		o->AssignMaterial( *fx.material );

		ObjectManager* manager = new ObjectManager( false, false, 4, 8 );
		manager->addref();
		manager->AddItem( o, "cube" );

		Scene* scene = new Scene();
		scene->addref();
		scene->SetObjectManager( manager );

		IShader* pShader = MakeTrivialShader();
		IRayCaster* pICaster = 0;
		RISE_API_CreateRayCaster( &pICaster, false, 10, *pShader, true );

		RayCaster* pCaster = dynamic_cast<RayCaster*>( pICaster );
		char buf[192];
		std::snprintf( buf, sizeof(buf), "(%s) ray caster created", label );
		Check( pCaster != 0, buf );

		if( pCaster )
		{
			pCaster->AttachScene( scene );
			pCaster->SetTransparentShadows( true );

			// Offset in x so the chord misses the quads' shared diagonal.
			const Ray ray( Point3( 0.3, 0, -3 ), Vector3( 0, 0, 1 ) );
			RISEPel T( 0, 0, 0 );
			const bool occluded = pCaster->CastShadowRayTransmittance( ray, 6.0 - 0.001, false, 0.0, T );

			std::snprintf( buf, sizeof(buf), "(%s) clear dielectric is not a full occluder", label );
			Check( !occluded, buf );
			std::snprintf( buf, sizeof(buf),
				"(%s) MONEY: transmittance == (1-F)^2 (BOTH interfaces read their real Fresnel pair)", label );
			CheckClose( T.r, slabT, 0.005 * slabT, buf );
		}

		safe_release( pICaster );
		safe_release( pShader );
		scene->release();
		manager->release();
		o->release();
	}
}

//////////////////////////////////////////////////////////////////////
//  Sub-test 3 (sites 1/2): the NEE shadow walk's medium stack.
//////////////////////////////////////////////////////////////////////
static void TestShadowWalkMediumStack()
{
	std::cout << "Sub-test 3: NEE shadow-walk medium stack through a double-sided medium cube (sites 1/2)"
		<< std::endl;

	const Scalar eta = 1.5;
	const Scalar F = ( (eta - 1.0) / (eta + 1.0) ) * ( (eta - 1.0) / (eta + 1.0) );
	const Scalar slabT = ( 1.0 - F ) * ( 1.0 - F );
	const Scalar sigma_a = 0.25;
	const Scalar chord = 2.0;							// the cube spans z in [-1, 1]
	const Scalar mediumT = std::exp( -sigma_a * chord );

	for( int variant = 0; variant < 2; variant++ )
	{
		const bool bDoubleSided = ( variant == 1 );
		const char* label = bDoubleSided ? "double-sided" : "single-sided (control)";
		char buf[224];

		// --- the medium-bearing dielectric cube ---
		TriangleMeshGeometryIndexed* g = BuildCube( bDoubleSided, 1.0 );
		Object* oCube = new Object( g );  safe_release( g );
		oCube->FinalizeTransformations();

		DielectricFixture fx( eta );
		oCube->AssignMaterial( *fx.material );

		IsotropicPhaseFunction* phase = new IsotropicPhaseFunction();
		phase->addref();
		HomogeneousMedium* medium = new HomogeneousMedium(
			RISEPel( sigma_a, sigma_a, sigma_a ), RISEPel( 0, 0, 0 ), *phase );
		medium->addref();
		oCube->AssignInteriorMedium( *medium );

		ObjectManager* manager = new ObjectManager( false, false, 4, 8 );
		manager->addref();
		manager->AddItem( oCube, "medium_cube" );

		// --- a directional light travelling along +Z (direction is FROM
		// the surface TO the light, per SCENE_CONVENTIONS) ---
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
		std::snprintf( buf, sizeof(buf), "(%s) ray caster created", label );
		Check( pCaster != 0, buf );

		if( pCaster )
		{
			pCaster->AttachScene( scene );
			pCaster->SetTransparentShadows( true );

			LightSampler* sampler = new LightSampler();
			sampler->addref();
			LuminaryManager::LuminariesList noLuminaries;
			sampler->Prepare( *scene, noLuminaries );

			// Shading point: a Lambertian (albedo 1) receiver facing the
			// light, at z = -3, so the shadow ray crosses the whole cube.
			UniformColorPainter* white = new UniformColorPainter( RISEPel( 1, 1, 1 ) );
			white->addref();
			LambertianMaterial* lam = new LambertianMaterial( *white );
			lam->addref();

			RasterizerState rast = {0};
			// The receiver sits at z = -3 facing +Z, i.e. toward the
			// directional light at +Z infinity, with the medium cube
			// (z in [-1, 1]) squarely between the two.  The viewing ray
			// arrives from the +Z side as well (through the cube), so
			// `LambertianBRDF`'s geometric-horizon gate sees light and
			// viewer on the SAME side of the normal and the BRDF is
			// nonzero -- the only property of this record sub-test 3
			// depends on besides `ptIntersection`.
			RayIntersectionGeometric ri( Ray( Point3( 0.3, 0, 5 ), Vector3( 0, 0, -1 ) ), rast );
			ri.bHit = true;
			ri.ptIntersection = Point3( 0.3, 0, -3 );
			ri.vNormal = Vector3( 0, 0, 1 );
			ri.vGeomNormal = Vector3( 0, 0, 1 );
			ri.onb.CreateFromW( ri.vNormal );
			ri.range = 8.0;

			RandomNumberGenerator rng;
			IndependentSampler isampler( rng );

			const RISEPel L = sampler->EvaluateDirectLighting(
				ri, *lam->GetBSDF(), lam, *pCaster, isampler,
				/*pShadingObject*/0, /*pMedium*/0, /*isVolumeScatter*/false,
				/*pMediumObject*/0 );

			// Reference: the same evaluation with the cube's medium
			// removed entirely is `L_clear`; with the medium it must be
			// `L_clear * exp(-sigma_a * chord)` -- the medium is active
			// for the chord and NOWHERE ELSE.  Measuring `L_clear` live
			// (rather than deriving it) keeps the light's own radiometric
			// constants out of the assertion.
			Object* oClear = new Object( BuildCube( bDoubleSided, 1.0 ) );
			oClear->FinalizeTransformations();
			oClear->AssignMaterial( *fx.material );
			ObjectManager* mgrClear = new ObjectManager( false, false, 4, 8 );
			mgrClear->addref();
			mgrClear->AddItem( oClear, "clear_cube" );
			Scene* sceneClear = new Scene();
			sceneClear->addref();
			sceneClear->SetObjectManager( mgrClear );
			sceneClear->SetLightManager( lights );

			IRayCaster* pIClear = 0;
			RISE_API_CreateRayCaster( &pIClear, false, 10, *pShader, true );
			RayCaster* pClear = dynamic_cast<RayCaster*>( pIClear );
			RISEPel Lclear( 0, 0, 0 );
			if( pClear ) {
				pClear->AttachScene( sceneClear );
				pClear->SetTransparentShadows( true );
				LightSampler* samplerClear = new LightSampler();
				samplerClear->addref();
				samplerClear->Prepare( *sceneClear, noLuminaries );
				Lclear = samplerClear->EvaluateDirectLighting(
					ri, *lam->GetBSDF(), lam, *pClear, isampler,
					0, 0, false, 0 );
				samplerClear->release();
			}

			std::snprintf( buf, sizeof(buf),
				"(%s) fixture sanity: the clear (no-medium) reference is lit, and ~ (1-F)^2 of unobstructed", label );
			Check( Lclear.r > 0.1 * slabT, buf );

			std::snprintf( buf, sizeof(buf),
				"(%s) MONEY: the medium attenuates by exactly exp(-sigma_a*chord) over the CHORD, not beyond it", label );
			CheckClose( L.r, Lclear.r * mediumT, 0.01 * Lclear.r * mediumT, buf );

			safe_release( pIClear );
			sceneClear->release();
			mgrClear->release();
			oClear->release();

			lam->release();
			white->release();
			sampler->release();
		}

		safe_release( pICaster );
		safe_release( pShader );
		scene->release();
		lights->release();
		light->release();
		manager->release();
		medium->release();
		phase->release();
		oCube->release();
	}
}


//////////////////////////////////////////////////////////////////////
//  Sub-test 4 (sites 1/2, the RAY-DERIVED half): a hair strand
//  carrying an interior medium must not leak that medium down the rest
//  of the shadow ray.
//
//  `HairGeometry` fabricates its geometric normal from the ray, so BOTH
//  the raw read AND the un-flip recovery report "entering" at every
//  strand the shadow ray crosses: the medium is pushed once per strand
//  and never removed, and is then applied over the whole remaining
//  (here infinite) distance to the light.  A 1-D curve has no interior
//  for a medium to occupy, so the walk skips such a hit outright --
//  the same rule `IORStackSeeding::SeedFromPoint`'s containment probe
//  follows (`bGeomNormalRayDerived`).
//////////////////////////////////////////////////////////////////////
static void TestShadowWalkSkipsRayDerivedNormals()
{
	std::cout << "Sub-test 4: the shadow walk skips a RAY-DERIVED (hair) crossing (sites 1/2)"
		<< std::endl;

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
	Check( pCaster != 0, "(hair medium) ray caster created" );

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
		// Same receiver geometry as sub-test 3, moved below the strand
		// (which lies along X in the z = 0 plane) so the shadow ray to
		// the +Z light crosses it.
		RayIntersectionGeometric ri( Ray( Point3( 0, 0, 5 ), Vector3( 0, 0, -1 ) ), rast );
		ri.bHit = true;
		ri.ptIntersection = Point3( 0, 0, -3 );
		ri.vNormal = Vector3( 0, 0, 1 );
		ri.vGeomNormal = Vector3( 0, 0, 1 );
		ri.onb.CreateFromW( ri.vNormal );
		ri.range = 8.0;

		RandomNumberGenerator rng;
		IndependentSampler isampler( rng );

		const RISEPel L = sampler->EvaluateDirectLighting(
			ri, *lam->GetBSDF(), lam, *pCaster, isampler,
			0, 0, false, 0 );

		// Fixture sanity: the strand really is crossed, and really does
		// report a ray-derived normal.
		RayIntersection probe( Ray( Point3( 0, 0, -3 ), Vector3( 0, 0, 1 ) ), rast );
		Hit( oHair, probe.geometric.ray, probe );
		Check( probe.geometric.bHit, "(hair medium) fixture: the shadow direction crosses the strand" );
		Check( probe.geometric.bGeomNormalRayDerived,
			"(hair medium) fixture: the strand reports bGeomNormalRayDerived" );

		// MONEY: the light survives.  With the crossing tallied (either
		// spelling -- raw or recovered), the medium is pushed and never
		// removed, so it attenuates the remaining RISE_INFINITY distance
		// to the light and `L` collapses to 0.
		Check( L.r > 0.05,
			"(hair medium) MONEY: a ray-derived crossing does not leak its interior medium down the rest of the shadow ray" );

		lam->release();
		white->release();
		sampler->release();
	}

	safe_release( pICaster );
	safe_release( pShader );
	scene->release();
	lights->release();
	light->release();
	manager->release();
	medium->release();
	phase->release();
	oHair->release();
}

int main()
{
	std::cout << "GeomNormalOrientationSitesTest (DL-70)" << std::endl;
	std::cout << "======================================" << std::endl;

	TestRecordLevelRecovery();
	TestHairIsSkipped();
	TestTransmissiveShadowFresnelPair();
	TestShadowWalkMediumStack();
	TestShadowWalkSkipsRayDerivedNormals();

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << "  Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
