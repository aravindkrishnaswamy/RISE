//////////////////////////////////////////////////////////////////////
//
//  TranslucentInitialContainmentTest.cpp - Regression guard for DL-46:
//    a camera/light origin already inside a closed TranslucentMaterial
//    object was never seeded with its membership, so the first
//    physical crossing was misclassified as an ENTRY instead of an
//    EXIT.
//
//  THE BUG THIS TEST GUARDS AGAINST
//
//    `IORStackSeeding::SeedFromPoint`'s probe only tracked materials
//    whose `GetSpecularInfo` reported `canRefract=true` (a real
//    dielectric with its own numeric IOR).  `TranslucentMaterial`
//    inherited `IMaterial`'s invalid/non-refracting default
//    `GetSpecularInfo`, so the probe silently skipped it: an origin
//    strictly inside a closed translucent object was never pushed onto
//    the seed stack, even though `TranslucentSPF::Scatter`/`ScatterNM`
//    classify entry vs. exit purely from `ior_stack.containsCurrent()`
//    -- exactly like a real refractor.  The first real hit on the
//    object's boundary therefore ran the ENTERING branch (front
//    reflection + a NEW push) instead of the EXITING branch (Beer
//    extinction + a pop), a wrong-lobe-family misclassification
//    parallel to the submerged-camera bug this whole seeding mechanism
//    exists to fix (docs/SUBMERGED_CAMERA_IOR_SEEDING.md).
//
//  THE FIX
//
//    `SpecularInfo` gained a `hasInterior` flag (SpecularInfo.h) for a
//    material that tracks its own containment like a refractor but is
//    NOT specular and has no distinct IOR of its own.
//    `TranslucentMaterial::GetSpecularInfo` now reports it.
//    `SeedFromPoint` accepts `canRefract || hasInterior`, and for a
//    `hasInterior`-only entry it re-pushes whatever IOR is already on
//    the stack (rather than a captured constant) -- matching
//    `TranslucentSPF::Scatter`'s own `push(ior_stack.top())` -- so
//    nesting a translucent object inside a distinct refractive
//    enclosure preserves that enclosure's numeric IOR exactly.
//
//  COVERAGE
//
//    A real scene (real `sphere_geometry` + `standard_object` +
//    `translucent_material`/`perfectrefractor_material`/
//    `perfectreflector_material`/`lambertian_material`, loaded via the
//    production CST loader) with:
//      - a translucent sphere sitting in AIR,
//      - a translucent sphere NESTED inside a distinct glass (ior 1.5)
//        shell,
//      - a perfect-reflector sphere and a Lambertian sphere as
//        negative controls (neither reports canRefract nor
//        hasInterior; must NOT be seeded -- confirms hasInterior is
//        additive and doesn't leak onto unrelated materials).
//
//    Sub-test 1 -- SeedFromPoint direct assertions at five positions
//      (in-air translucent interior, nested translucent interior,
//      clearly outside everything, inside the mirror, inside the
//      Lambertian sphere).
//    Sub-test 2 -- production SeedFromPoint followed by a REAL ray
//      cast (IObjectManager::IntersectRay, not a synthetic stub) from
//      each seeded interior point outward to the object's own real
//      geometric boundary, then the REAL TranslucentSPF::Scatter/
//      ScatterNM at that hit: the first physical crossing must be
//      classified as EXIT (Beer attenuation + a pop -- not an entry's
//      new push), and the popped stack must reveal exactly the
//      enclosing numeric IOR (1.0 for the in-air case, 1.5 for the
//      nested-in-glass case).
//    Sub-test 3 (P2-4, review round 3) -- an OPEN single-sided
//      translucent card is not an enclosure.  DL-46 made every
//      translucent object a probe participant, and translucent is the
//      material authors put on open sheets; a lone away-facing card
//      above the seed presented one "exit" and no entry, so the old
//      single-probe parity declared containment.  Programmatic meshes
//      (not the CST scene), with a CLOSED single-sided translucent box
//      in the same scene as the positive control.
//    Sub-test 4 (P2-4, review round 3) -- hair is never an enclosure.
//      `HairGeometry`'s geometric normal is RAY-DERIVED and it reports
//      `bGeomNormalOrientedToRay` unconditionally, so the double-sided
//      un-flip reads EXIT at every strand; two strands straddling the
//      seed would defeat sub-test 3's reverse-probe rule as well.  The
//      probe honours `bGeomNormalRayDerived` and skips such hits.
//
//  Author: Aravind Krishnaswamy (RISE debt-cleanup, slice `translucent`)
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <string>
#include <cmath>
#ifdef _WIN32
	#include <process.h>
	#define getpid _getpid
#else
	#include <unistd.h>
#endif

#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IScene.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/IORStack.h"
#include "../src/Library/Utilities/IORStackSeeding.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Intersection/RayIntersection.h"
#include "../src/Library/Interfaces/ISPF.h"
#include "../src/Library/Interfaces/IMaterial.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Managers/ObjectManager.h"
#include "../src/Library/Scene.h"
#include "../src/Library/Materials/TranslucentMaterial.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Geometry/HairGeometry.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

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

static std::string WriteSceneToTempFile( const char* sceneText, const char* tag )
{
	char path[512];
	std::snprintf( path, sizeof(path),
		"/tmp/translucent_initial_containment_%s_%d.RISEscene",
		tag, static_cast<int>(::getpid()) );

	std::ofstream ofs( path );
	if( !ofs.is_open() ) {
		return std::string();
	}
	ofs << sceneText;
	ofs.close();
	return std::string( path );
}

//////////////////////////////////////////////////////////////////////
//  Scene: a translucent sphere in air (radius 0.5, origin), a
//  translucent sphere (radius 0.5) NESTED inside a distinct glass
//  shell (radius 2.0, ior 1.5) both centered at z=6, a perfect
//  reflector sphere at z=12, and a Lambertian sphere at z=18 -- the
//  latter two are negative controls.
//////////////////////////////////////////////////////////////////////
static const char* kSceneText =
	"RISE ASCII SCENE 7\n"
	"\n"
	"film\n"
	"{\n"
	"\twidth 32\n"
	"\theight 32\n"
	"}\n"
	"\n"
	"pinhole_camera\n"
	"{\n"
	"\tlocation 0 0 -20\n"
	"\tlookat 0 0 0\n"
	"\tup 0 1 0\n"
	"\tfov 30.0\n"
	"}\n"
	"\n"
	"standard_shader\n"
	"{\n"
	"\tname global\n"
	"\tshaderop DefaultPathTracing\n"
	"}\n"
	"\n"
	"pathtracing_pel_rasterizer\n"
	"{\n"
	"\tsamples 1\n"
	"\toidn_denoise FALSE\n"
	"}\n"
	"\n"
	"uniformcolor_painter\n"
	"{\n"
	"\tname color_white\n"
	"\tcolor 1.0 1.0 1.0\n"
	"}\n"
	"\n"
	"scalar_painter\n"
	"{\n"
	"\tname pnt_ior_glass\n"
	"\tvalue 1.5\n"
	"}\n"
	"\n"
	"perfectrefractor_material\n"
	"{\n"
	"\tname mat_glass\n"
	"\trefractance color_white\n"
	"\tior pnt_ior_glass\n"
	"}\n"
	"\n"
	"translucent_material\n"
	"{\n"
	"\tname mat_trans\n"
	"\tref color_white\n"
	"\ttau color_white\n"
	"\text 0\n"
	"\tN 1.0\n"
	"\tscattering 0.3\n"
	"}\n"
	"\n"
	"perfectreflector_material\n"
	"{\n"
	"\tname mat_mirror\n"
	"\treflectance color_white\n"
	"}\n"
	"\n"
	"lambertian_material\n"
	"{\n"
	"\tname mat_lambert\n"
	"\treflectance color_white\n"
	"}\n"
	"\n"
	"sphere_geometry\n"
	"{\n"
	"\tname sph_small\n"
	"\tradius 0.5\n"
	"}\n"
	"\n"
	"sphere_geometry\n"
	"{\n"
	"\tname sph_big\n"
	"\tradius 2.0\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_trans_air\n"
	"\tgeometry sph_small\n"
	"\tposition 0 0 0\n"
	"\tmaterial mat_trans\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_glass_shell\n"
	"\tgeometry sph_big\n"
	"\tposition 0 0 6\n"
	"\tmaterial mat_glass\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_trans_nested\n"
	"\tgeometry sph_small\n"
	"\tposition 0 0 6\n"
	"\tmaterial mat_trans\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_mirror\n"
	"\tgeometry sph_small\n"
	"\tposition 0 0 12\n"
	"\tmaterial mat_mirror\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_lambert\n"
	"\tgeometry sph_small\n"
	"\tposition 0 0 18\n"
	"\tmaterial mat_lambert\n"
	"}\n";

//////////////////////////////////////////////////////////////////////
//  Sub-test 1: direct SeedFromPoint assertions.
//////////////////////////////////////////////////////////////////////
static void TestSeedFromPoint( IScenePriv& scene )
{
	std::cout << "Sub-test 1: SeedFromPoint direct assertions" << std::endl;

	// (A) Inside the translucent sphere sitting in air: must be seeded,
	// with the enclosing (air) IOR of 1.0 preserved -- translucent has
	// no distinct IOR of its own.  RED-PROOF: pre-fix, this stack stayed
	// empty because GetSpecularInfo reported canRefract=false/invalid.
	{
		IORStack stack( 1.0 );
		IORStackSeeding::SeedFromPoint( stack, Point3(0,0,0), scene );
		const IObject* seeded = stack.topObject();
		Check( seeded != 0, "(A) translucent-in-air interior: something was seeded onto the stack" );
		if( seeded ) {
			stack.SetCurrentObject( seeded );
			Check( stack.containsCurrent() && std::fabs(stack.top() - 1.0) < 1e-9,
				"(A) translucent-in-air interior seeded with enclosing IOR 1.0" );
		}
	}

	// (B) Inside the translucent sphere NESTED inside the glass shell:
	// both objects must be seeded (the point is inside both), with the
	// glass's numeric IOR (1.5) preserved as top() once the translucent
	// object (no IOR of its own) is pushed on top of it.
	{
		IORStack stack( 1.0 );
		IORStackSeeding::SeedFromPoint( stack, Point3(0,0,6), scene );
		Check( std::fabs(stack.top() - 1.5) < 1e-9,
			"(B) translucent-nested-in-glass: enclosing glass IOR 1.5 preserved on top" );
	}

	// (C) Clearly outside everything: stack stays untouched.
	{
		IORStack stack( 1.0 );
		IORStackSeeding::SeedFromPoint( stack, Point3(10,10,-20), scene );
		Check( stack.topObject() == 0 && std::fabs(stack.top() - 1.0) < 1e-9,
			"(C) outside-everything: stack unaffected" );
	}

	// (D) Inside the pure reflector sphere: mirrors are not media and
	// must NOT be seeded (neither canRefract nor hasInterior) -- guards
	// against hasInterior leaking onto an unrelated material.
	{
		IORStack stack( 1.0 );
		IORStackSeeding::SeedFromPoint( stack, Point3(0,0,12), scene );
		Check( stack.topObject() == 0 && std::fabs(stack.top() - 1.0) < 1e-9,
			"(D) mirror interior: stack unaffected (mirrors are not media)" );
	}

	// (E) Inside the Lambertian sphere: same non-media control.
	{
		IORStack stack( 1.0 );
		IORStackSeeding::SeedFromPoint( stack, Point3(0,0,18), scene );
		Check( stack.topObject() == 0 && std::fabs(stack.top() - 1.0) < 1e-9,
			"(E) Lambertian interior: stack unaffected (not a medium)" );
	}
}

//////////////////////////////////////////////////////////////////////
//  Sub-test 2: seeded stack -> real ray cast -> real Scatter/ScatterNM
//  classifies the first crossing as EXIT and preserves the enclosing
//  IOR through the pop.
//////////////////////////////////////////////////////////////////////
static void CheckFirstCrossingIsExit(
	IScenePriv& scene, const Point3& seedPos, Scalar expectedEnclosingIOR,
	const char* label )
{
	IORStack stack( 1.0 );
	IORStackSeeding::SeedFromPoint( stack, seedPos, scene );

	// Cast a real ray from the seed point straight out along +Z to the
	// object's own real geometric boundary (radius 0.5 spheres all
	// centered at the seed's (x,y) -- +Z exits at distance 0.5).
	RayIntersection ri( Ray( seedPos, Vector3(0,0,1) ), nullRasterizerState );
	scene.GetObjects()->IntersectRay( ri, true, true, false );

	Check( ri.geometric.bHit && ri.pObject != 0 && ri.pMaterial != 0,
		( std::string(label) + ": real ray finds the object's boundary" ).c_str() );
	if( !ri.geometric.bHit || !ri.pObject || !ri.pMaterial ) {
		return;
	}

	stack.SetCurrentObject( ri.pObject );
	const bool seededContains = stack.containsCurrent();
	Check( seededContains,
		( std::string(label) + ": seeded stack already contains the object (exit, not entry)" ).c_str() );

	ISPF* pSPF = ri.pMaterial->GetSPF();
	Check( pSPF != 0, ( std::string(label) + ": material exposes an SPF" ).c_str() );
	if( !pSPF ) {
		return;
	}

	RandomNumberGenerator rng( 12345 );
	IndependentSampler sampler( rng );

	for( unsigned int spectral = 0; spectral < 2; spectral++ ) {
		ScatteredRayContainer scattered;
		if( spectral ) {
			pSPF->ScatterNM( ri.geometric, sampler, 550.0, scattered, stack );
		} else {
			pSPF->Scatter( ri.geometric, sampler, scattered, stack );
		}

		// The EXIT branch's diffuse ray carries a non-null ior_stack that
		// has POPPED the object (containsCurrent() false for it) and
		// reveals the enclosing medium.  The ENTRY branch's translucent
		// lobe carries a non-null ior_stack that has PUSHED the object
		// (containsCurrent() true) -- the pre-fix misclassification.
		bool sawPoppedExit = false;
		Scalar revealedIOR = -1;
		for( unsigned int i = 0; i < scattered.Count(); i++ ) {
			if( scattered[i].ior_stack != 0 ) {
				IORStack* s = scattered[i].ior_stack;
				s->SetCurrentObject( ri.pObject );
				if( !s->containsCurrent() ) {
					sawPoppedExit = true;
					revealedIOR = s->top();
				}
			}
		}
		const char* tag = spectral ? "NM" : "RGB";
		Check( sawPoppedExit,
			( std::string(label) + " " + tag + ": first crossing classified as EXIT (popped stack), not entry" ).c_str() );
		if( sawPoppedExit ) {
			Check( std::fabs(revealedIOR - expectedEnclosingIOR) < 1e-6,
				( std::string(label) + " " + tag + ": popped stack reveals the correct enclosing IOR" ).c_str() );
		}
	}
}

static void TestFirstCrossing( IScenePriv& scene )
{
	std::cout << "Sub-test 2: seeded stack -> real Scatter/ScatterNM first-crossing classification" << std::endl;

	CheckFirstCrossingIsExit( scene, Point3(0,0,0), 1.0, "in-air" );
	CheckFirstCrossingIsExit( scene, Point3(0,0,6), 1.5, "nested-in-glass" );
}

//////////////////////////////////////////////////////////////////////
//  Sub-test 3 (P2-4, review round 3): OPEN translucent geometry must
//  NOT be mistaken for an enclosure.
//
//  `SeedFromPoint`'s per-object exit-vs-entry parity is a containment
//  test only for a CLOSED manifold.  DL-46 made every
//  `translucent_material` object a probe participant -- and translucent
//  is exactly the material authors put on OPEN sheets: a leaf, a
//  curtain, a lampshade panel, a paper card.  A single-sided card above
//  the seed point with its normal pointing UP presents one "exit"
//  crossing to the +Z probe and nothing else, so parity reads +1 and
//  the seed is falsely declared inside it.  The camera's very first hit
//  on that card then runs `TranslucentSPF`'s EXIT branch -- Beer
//  extinction plus a pop of an IOR that was never pushed -- instead of
//  the entry branch.
//
//  Note the card must be a MESH to reach this: a single-sided
//  `ClippedPlaneGeometry` culls the back-face hit outright
//  (ClippedPlaneGeometry.cpp, `isBackFaceHit ? (bHitBackFaces &&
//  bDoubleSided) : bHitFrontFaces`), so the probe never sees it at all.
//  A single-sided TRIANGLE MESH does report the hit (the probe asks for
//  both face directions); it simply does not flip the normal.
//
//  The fix requires positive parity along the probe AND its reverse.
//  Sub-test 3 pins both halves: the open card is rejected, and a real
//  CLOSED single-sided translucent box in the same scene is still
//  seeded (so the rule did not just disable seeding).
//////////////////////////////////////////////////////////////////////
namespace
{
	//! Open, single-sided quad card (2 triangles) in the plane
	//! z = `zPlane`, spanning [-4,4]^2, authored normal along `normalZ`.
	TriangleMeshGeometryIndexed* BuildOpenCard( Scalar zPlane, Scalar normalZ )
	{
		TriangleMeshGeometryIndexed* mesh = new TriangleMeshGeometryIndexed( /*bDoubleSided*/false, false );
		mesh->BeginIndexedTriangles();
		const Vector3 nrm( 0, 0, normalZ );
		for( int sx = -1; sx <= 1; sx += 2 ) {
			for( int sy = -1; sy <= 1; sy += 2 ) {
				mesh->AddVertex( Point3( 4.0*sx, 4.0*sy, zPlane ) );
				mesh->AddNormal( nrm );
				mesh->AddTexCoord( Point2( 0, 0 ) );
			}
		}
		IndexedTriangle t1, t2;
		t1.iVertices[0] = 0; t1.iVertices[1] = 1; t1.iVertices[2] = 2;
		t2.iVertices[0] = 1; t2.iVertices[1] = 3; t2.iVertices[2] = 2;
		for( int k = 0; k < 3; k++ ) {
			t1.iNormals[k] = t1.iVertices[k]; t1.iCoords[k] = t1.iVertices[k];
			t2.iNormals[k] = t2.iVertices[k]; t2.iCoords[k] = t2.iVertices[k];
		}
		mesh->AddIndexedTriangle( t1 );
		mesh->AddIndexedTriangle( t2 );
		mesh->DoneIndexedTriangles();
		return mesh;
	}

	//! Closed, single-sided, axis-aligned box [-1,1]^3 centred at
	//! (0,0,`zCentre`): the positive control for the same rule.
	TriangleMeshGeometryIndexed* BuildClosedBox( Scalar zCentre )
	{
		TriangleMeshGeometryIndexed* mesh = new TriangleMeshGeometryIndexed( /*bDoubleSided*/false, false );
		mesh->BeginIndexedTriangles();
		unsigned int next = 0;
		for( int axis = 0; axis < 3; axis++ ) {
			for( int sgn = -1; sgn <= 1; sgn += 2 ) {
				Vector3 outward( 0, 0, 0 );
				if( axis == 0 ) outward.x = sgn; else if( axis == 1 ) outward.y = sgn; else outward.z = sgn;
				const int uAxis = (axis + 1) % 3;
				const int vAxis = (axis + 2) % 3;
				const unsigned int base = next;
				for( int su = -1; su <= 1; su += 2 ) {
					for( int sv = -1; sv <= 1; sv += 2 ) {
						Point3 p( 0, 0, 0 );
						p[axis] = sgn;
						p[uAxis] = static_cast<Scalar>( su );
						p[vAxis] = static_cast<Scalar>( sv );
						p.z += zCentre;
						mesh->AddVertex( p );
						mesh->AddNormal( outward );
						mesh->AddTexCoord( Point2( 0, 0 ) );
					}
				}
				IndexedTriangle t1, t2;
				t1.iVertices[0] = base+0; t1.iVertices[1] = base+1; t1.iVertices[2] = base+2;
				t2.iVertices[0] = base+1; t2.iVertices[1] = base+3; t2.iVertices[2] = base+2;
				for( int k = 0; k < 3; k++ ) {
					t1.iNormals[k] = t1.iVertices[k]; t1.iCoords[k] = t1.iVertices[k];
					t2.iNormals[k] = t2.iVertices[k]; t2.iCoords[k] = t2.iVertices[k];
				}
				mesh->AddIndexedTriangle( t1 );
				mesh->AddIndexedTriangle( t2 );
				next += 4;
			}
		}
		mesh->DoneIndexedTriangles();
		return mesh;
	}
}

static void TestOpenTranslucentGeometryIsNotAnEnclosure()
{
	std::cout << "Sub-test 3: open single-sided translucent card is not containment (P2-4)" << std::endl;

	UniformColorPainter* front = new UniformColorPainter( RISEPel( 0.5, 0.5, 0.5 ) );  front->addref();
	UniformColorPainter* tau = new UniformColorPainter( RISEPel( 0.5, 0.5, 0.5 ) );  tau->addref();
	UniformScalarPainter* ext = new UniformScalarPainter( 0.0 );  ext->addref();
	UniformScalarPainter* phongN = new UniformScalarPainter( 1.0 );  phongN->addref();
	UniformScalarPainter* scat = new UniformScalarPainter( 0.3 );  scat->addref();
	TranslucentMaterial* material = new TranslucentMaterial( *front, *tau, *ext, *phongN, *scat );
	material->addref();

	// Card at z = +3 with its normal pointing +Z (away from the seed at
	// the origin, i.e. the "one lone exit crossing" configuration).
	TriangleMeshGeometryIndexed* cardGeom = BuildOpenCard( 3.0, +1.0 );
	Object* card = new Object( cardGeom );
	safe_release( cardGeom );
	card->FinalizeTransformations();
	card->AssignMaterial( *material );

	// Closed single-sided translucent box centred at z = 20: positive
	// control for the same rule, in the same scene.
	TriangleMeshGeometryIndexed* boxGeom = BuildClosedBox( 20.0 );
	Object* box = new Object( boxGeom );
	safe_release( boxGeom );
	box->FinalizeTransformations();
	box->AssignMaterial( *material );

	ObjectManager* manager = new ObjectManager( false, false, 4, 8 );
	manager->addref();
	manager->AddItem( card, "open_translucent_card" );
	manager->AddItem( box, "closed_translucent_box" );

	Scene* scene = new Scene();
	scene->addref();
	scene->SetObjectManager( manager );

	// Fixture sanity: the +Z probe really does read the card as an
	// "exit" crossing, i.e. this fixture reproduces the configuration
	// the rule exists to reject (rather than passing vacuously because
	// the probe never sees the card at all).
	{
		RayIntersection ri( Ray( Point3(0,0,0), Vector3(0,0,1) ), nullRasterizerState );
		scene->GetObjects()->IntersectRay( ri, true, true, false );
		Check( ri.geometric.bHit && ri.pObject == card,
			"(F) fixture sanity: the +Z probe does hit the open card from below" );
		if( ri.geometric.bHit ) {
			const Vector3 trueN = ri.geometric.bGeomNormalOrientedToRay
				? -ri.geometric.vGeomNormal : ri.geometric.vGeomNormal;
			Check( Vector3Ops::Dot( trueN, Vector3(0,0,1) ) > 0,
				"(F) fixture sanity: that hit reads as an EXIT crossing (recovered normal along the probe)" );
		}
	}

	// (G) The seed point below the open card must NOT be seeded.
	{
		IORStack stack( 1.0 );
		IORStackSeeding::SeedFromPoint( stack, Point3(0,0,0), *scene );
		Check( stack.topObject() == 0 && std::fabs( stack.top() - 1.0 ) < 1e-9,
			"(G) P2-4 money assertion: a point merely BELOW an open translucent card is not seeded" );
	}

	// (H) The closed box still is -- the rule must not have simply
	// disabled seeding.
	{
		IORStack stack( 1.0 );
		IORStackSeeding::SeedFromPoint( stack, Point3(0,0,20), *scene );
		Check( stack.topObject() == box,
			"(H) positive control: a point inside the CLOSED translucent box is still seeded" );
	}

	manager->release();
	scene->release();
	card->release();
	box->release();
	material->release();
	scat->release();
	phongN->release();
	ext->release();
	tau->release();
	front->release();
}

//////////////////////////////////////////////////////////////////////
//  Sub-test 4 (P2-4, review round 3): a hair strand's RAY-DERIVED
//  geometric normal must not be read as a surface facing.
//
//  `HairGeometry` fabricates `vGeomNormal = Nflat` from the RAY
//  direction and the strand tangent (a 1-D curve has no two-sided
//  surface), and sets `bGeomNormalOrientedToRay = true`
//  UNCONDITIONALLY.  Un-flipping that -- the recovery the probe now
//  performs for double-sided meshes -- yields a direction that always
//  faces AWAY from the ray, so EVERY strand the probe crosses tallies
//  as an "exit".  Two strands straddling the seed point therefore make
//  BOTH probe directions read positive parity, defeating sub-test 3's
//  reverse-probe rule as well.
//
//  Today this is only reachable if the hair carries a trackable
//  material (`HairMaterial::GetSpecularInfo` reports nothing, so hair
//  is skipped for that reason instead) -- this fixture assigns a
//  translucent material to make the hazard reachable and pin the
//  structural guard: `bGeomNormalRayDerived` (RayIntersectionGeometric.h)
//  tells the probe to skip such a hit outright.
//////////////////////////////////////////////////////////////////////
static void TestHairIsNotAnEnclosure()
{
	std::cout << "Sub-test 4: hair's ray-derived geometric normal is not a containment crossing (P2-4)" << std::endl;

	UniformColorPainter* front = new UniformColorPainter( RISEPel( 0.5, 0.5, 0.5 ) );  front->addref();
	UniformColorPainter* tau = new UniformColorPainter( RISEPel( 0.5, 0.5, 0.5 ) );  tau->addref();
	UniformScalarPainter* ext = new UniformScalarPainter( 0.0 );  ext->addref();
	UniformScalarPainter* phongN = new UniformScalarPainter( 1.0 );  phongN->addref();
	UniformScalarPainter* scat = new UniformScalarPainter( 0.3 );  scat->addref();
	TranslucentMaterial* material = new TranslucentMaterial( *front, *tau, *ext, *phongN, *scat );
	material->addref();

	// Two straight strands along X, one above (+Z) and one below (-Z) the
	// origin -- so BOTH probe directions cross one, and sub-test 3's
	// reverse-probe rule alone would not save us.
	std::vector<HairGeometry::StrandDesc> descs( 2 );
	for( int i = 0; i < 2; i++ ) {
		const Scalar z = ( i == 0 ) ? 3.0 : -3.0;
		descs[i].controlPoints.push_back( Point3( -4, 0, z ) );
		descs[i].controlPoints.push_back( Point3(  4, 0, z ) );
		descs[i].rootWidth = 0.5;
		descs[i].tipWidth  = 0.5;
		descs[i].rootUV    = Point2( 0.5, 0.5 );
	}
	HairGeometry* hairGeom = new HairGeometry( descs );
	Object* hair = new Object( hairGeom );
	safe_release( hairGeom );
	hair->FinalizeTransformations();
	hair->AssignMaterial( *material );

	ObjectManager* manager = new ObjectManager( false, false, 4, 8 );
	manager->addref();
	manager->AddItem( hair, "hair_curtain" );

	Scene* scene = new Scene();
	scene->addref();
	scene->SetObjectManager( manager );

	// Fixture sanity (white box): the probe really does hit a strand, and
	// the hit really does advertise a ray-derived geometric normal whose
	// un-flip reads as an "exit" in BOTH directions.
	for( int s = 0; s < 2; s++ ) {
		const Vector3 dir( 0, 0, s ? -1.0 : 1.0 );
		RayIntersection ri( Ray( Point3(0,0,0), dir ), nullRasterizerState );
		scene->GetObjects()->IntersectRay( ri, true, true, false );
		Check( ri.geometric.bHit && ri.pObject == hair,
			s ? "(I) fixture sanity: the -Z probe hits a strand" : "(I) fixture sanity: the +Z probe hits a strand" );
		if( ri.geometric.bHit ) {
			Check( ri.geometric.bGeomNormalRayDerived,
				"(I) fixture sanity: the hair hit reports bGeomNormalRayDerived" );
			const Vector3 trueN = ri.geometric.bGeomNormalOrientedToRay
				? -ri.geometric.vGeomNormal : ri.geometric.vGeomNormal;
			Check( Vector3Ops::Dot( trueN, dir ) > 0,
				"(I) fixture sanity: the un-flip would read this crossing as an EXIT" );
		}
	}

	// (J) Money assertion: hair is never containment.
	{
		IORStack stack( 1.0 );
		IORStackSeeding::SeedFromPoint( stack, Point3(0,0,0), *scene );
		Check( stack.topObject() == 0 && std::fabs( stack.top() - 1.0 ) < 1e-9,
			"(J) P2-4 money assertion: a point between two hair strands is not seeded" );
	}

	manager->release();
	scene->release();
	hair->release();
	material->release();
	scat->release();
	phongN->release();
	ext->release();
	tau->release();
	front->release();
}

//////////////////////////////////////////////////////////////////////
//  Sub-test 5 (DL-76): a SINGLE Object built from two disjoint open
//  pieces straddling the seed on opposite sides of one axis, each
//  piece's normal facing AWAY from the seed, must not be mistaken for
//  an enclosure -- even though the per-object reverse-probe rule
//  (sub-test 3 / P2-4) sees the SAME `pObj` register a positive-parity
//  "exit" in BOTH probe directions.
//
//  THE RESIDUAL THIS TEST GUARDS AGAINST
//
//    P2-4's rule requires positive parity along a probe AND its
//    reverse, keyed by object identity.  A single mesh `Object` with
//    TWO disjoint quads -- one above the seed at z=+3 with normal +Z
//    (away from the seed), one below at z=-3 with normal -Z (also away
//    from the seed) -- presents exactly the pattern the rule accepts:
//    the +Z probe crosses the top quad (an "exit" for that pObj), and
//    the -Z probe crosses the BOTTOM quad (also an "exit" for the SAME
//    pObj, since both quads belong to one mesh/Object).  Both
//    directions report positive parity for the identical object
//    pointer, so the P2-4 confirmation alone falsely declares the seed
//    contained -- even though the seed sits in open air between two
//    unconnected sheets.
//
//  THE FIX (this slice)
//
//    `IsConfirmedAlongAxis` (IORStackSeeding.h) extends the vote to the
//    two OTHER principal axes (X, Y) for whichever object the Z pair
//    already accepts.  Both blades lie entirely in their own z=const
//    plane spanning a BOUNDED x/y extent, so a probe along X or Y from
//    the origin travels through z=0 forever and crosses neither blade
//    -- that axis reports zero parity for the object, and the
//    unanimity vote rejects it.  A genuinely closed enclosure (the
//    positive control reused from sub-test 3, a closed box) agrees
//    along every axis by construction and is unaffected.
//////////////////////////////////////////////////////////////////////
namespace
{
	//! ONE mesh containing TWO disjoint open quads: one at z=+zAbs with
	//! normal +Z (away from a seed at the origin), one at z=-zAbs with
	//! normal -Z (also away from the seed) -- the two-blade counterexample
	//! to the per-object reverse-probe rule.
	TriangleMeshGeometryIndexed* BuildTwoDisjointAwayFacingBlades( Scalar zAbs )
	{
		TriangleMeshGeometryIndexed* mesh = new TriangleMeshGeometryIndexed( /*bDoubleSided*/false, false );
		mesh->BeginIndexedTriangles();
		unsigned int next = 0;
		for( int blade = 0; blade < 2; blade++ ) {
			const Scalar z = ( blade == 0 ) ? zAbs : -zAbs;
			const Scalar normalZ = ( blade == 0 ) ? 1.0 : -1.0;	// away from the origin either way
			const Vector3 nrm( 0, 0, normalZ );
			const unsigned int base = next;
			for( int sx = -1; sx <= 1; sx += 2 ) {
				for( int sy = -1; sy <= 1; sy += 2 ) {
					mesh->AddVertex( Point3( 4.0*sx, 4.0*sy, z ) );
					mesh->AddNormal( nrm );
					mesh->AddTexCoord( Point2( 0, 0 ) );
				}
			}
			IndexedTriangle t1, t2;
			t1.iVertices[0] = base+0; t1.iVertices[1] = base+1; t1.iVertices[2] = base+2;
			t2.iVertices[0] = base+1; t2.iVertices[1] = base+3; t2.iVertices[2] = base+2;
			for( int k = 0; k < 3; k++ ) {
				t1.iNormals[k] = t1.iVertices[k]; t1.iCoords[k] = t1.iVertices[k];
				t2.iNormals[k] = t2.iVertices[k]; t2.iCoords[k] = t2.iVertices[k];
			}
			mesh->AddIndexedTriangle( t1 );
			mesh->AddIndexedTriangle( t2 );
			next += 4;
		}
		mesh->DoneIndexedTriangles();
		return mesh;
	}
}

static void TestTwoDisjointOpenSurfacesOfSameObjectIsNotAnEnclosure()
{
	std::cout << "Sub-test 5: two disjoint away-facing blades of the SAME object is not containment (DL-76)" << std::endl;

	UniformColorPainter* front = new UniformColorPainter( RISEPel( 0.5, 0.5, 0.5 ) );  front->addref();
	UniformColorPainter* tau = new UniformColorPainter( RISEPel( 0.5, 0.5, 0.5 ) );  tau->addref();
	UniformScalarPainter* ext = new UniformScalarPainter( 0.0 );  ext->addref();
	UniformScalarPainter* phongN = new UniformScalarPainter( 1.0 );  phongN->addref();
	UniformScalarPainter* scat = new UniformScalarPainter( 0.3 );  scat->addref();
	TranslucentMaterial* material = new TranslucentMaterial( *front, *tau, *ext, *phongN, *scat );
	material->addref();

	TriangleMeshGeometryIndexed* bladesGeom = BuildTwoDisjointAwayFacingBlades( 3.0 );
	Object* blades = new Object( bladesGeom );
	safe_release( bladesGeom );
	blades->FinalizeTransformations();
	blades->AssignMaterial( *material );

	// Closed single-sided translucent box centred at z = 20 (reused
	// pattern from sub-test 3): positive control in the SAME scene,
	// confirming the extra X/Y votes did not just disable seeding.
	TriangleMeshGeometryIndexed* boxGeom = BuildClosedBox( 20.0 );
	Object* box = new Object( boxGeom );
	safe_release( boxGeom );
	box->FinalizeTransformations();
	box->AssignMaterial( *material );

	ObjectManager* manager = new ObjectManager( false, false, 4, 8 );
	manager->addref();
	manager->AddItem( blades, "two_disjoint_blades" );
	manager->AddItem( box, "closed_translucent_box" );

	Scene* scene = new Scene();
	scene->addref();
	scene->SetObjectManager( manager );

	// Fixture sanity: BOTH the +Z and -Z probes really do hit the SAME
	// object (`blades`), each reading its crossing as an "exit" --
	// exactly the pattern that fools the Z-only reverse-probe rule, so
	// this fixture is a genuine red-proof against that rule alone.
	for( int s = 0; s < 2; s++ ) {
		const Vector3 dir( 0, 0, s ? -1.0 : 1.0 );
		RayIntersection ri( Ray( Point3(0,0,0), dir ), nullRasterizerState );
		scene->GetObjects()->IntersectRay( ri, true, true, false );
		Check( ri.geometric.bHit && ri.pObject == blades,
			s ? "(K) fixture sanity: the -Z probe hits the bottom blade" : "(K) fixture sanity: the +Z probe hits the top blade" );
		if( ri.geometric.bHit ) {
			const Vector3 trueN = ri.geometric.bGeomNormalOrientedToRay
				? -ri.geometric.vGeomNormal : ri.geometric.vGeomNormal;
			Check( Vector3Ops::Dot( trueN, dir ) > 0,
				"(K) fixture sanity: that hit reads as an EXIT crossing in both directions" );
		}
	}
	// X and Y probes must cross NEITHER blade (both lie in a bounded
	// x/y extent within their own z=const plane) -- confirming the
	// extra votes actually have signal to reject on, not a probe that
	// happens to graze something else.
	{
		const Vector3 axes[2] = { Vector3(1,0,0), Vector3(0,1,0) };
		for( int a = 0; a < 2; a++ ) {
			for( int s = -1; s <= 1; s += 2 ) {
				RayIntersection ri( Ray( Point3(0,0,0), axes[a] * static_cast<Scalar>(s) ), nullRasterizerState );
				scene->GetObjects()->IntersectRay( ri, true, true, false );
				Check( !( ri.geometric.bHit && ri.pObject == blades ),
					"(K) fixture sanity: an X/Y probe from the origin does not cross either blade" );
			}
		}
	}

	// (L) Money assertion: the seed between two disjoint away-facing
	// blades of the same object is not seeded.
	{
		IORStack stack( 1.0 );
		IORStackSeeding::SeedFromPoint( stack, Point3(0,0,0), *scene );
		Check( stack.topObject() == 0 && std::fabs( stack.top() - 1.0 ) < 1e-9,
			"(L) DL-76 money assertion: a point between two disjoint away-facing blades "
			"of the SAME object is not seeded" );
	}

	// (M) Positive control: the closed box in the same scene is still seeded.
	{
		IORStack stack( 1.0 );
		IORStackSeeding::SeedFromPoint( stack, Point3(0,0,20), *scene );
		Check( stack.topObject() == box,
			"(M) positive control: a point inside the CLOSED translucent box is still seeded "
			"(the extra X/Y votes did not just disable seeding)" );
	}

	manager->release();
	scene->release();
	blades->release();
	box->release();
	material->release();
	scat->release();
	phongN->release();
	ext->release();
	tau->release();
	front->release();
}

int main()
{
	GlobalLog();

	std::cout << "TranslucentInitialContainmentTest: DL-46 initial-containment seeding" << std::endl;

	const std::string scenePath = WriteSceneToTempFile( kSceneText, "scene" );
	if( scenePath.empty() ) {
		Check( false, "scene write" );
	} else {
		IJobPriv* pJob = nullptr;
		if( !RISE_CreateJobPriv( &pJob ) || !pJob ) {
			Check( false, "job create" );
		} else if( !pJob->LoadAsciiSceneViaCst( scenePath.c_str() ) ) {
			Check( false, "scene load" );
			safe_release( pJob );
		} else {
			IScenePriv* pScene = pJob->GetScene();
			if( !pScene ) {
				Check( false, "scene get" );
			} else {
				TestSeedFromPoint( *pScene );
				TestFirstCrossing( *pScene );
					TestOpenTranslucentGeometryIsNotAnEnclosure();
					TestHairIsNotAnEnclosure();
					TestTwoDisjointOpenSurfacesOfSameObjectIsNotAnEnclosure();
			}
			safe_release( pJob );
		}
		std::remove( scenePath.c_str() );
	}

	std::cout << std::endl << "Passed: " << passCount << std::endl << "Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
