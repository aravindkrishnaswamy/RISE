//////////////////////////////////////////////////////////////////////
//
//  TranslucentDoubleSidedTest.cpp - Regression guard for P2-1 (DL-45
//    follow-up) / P2-2 (DL-46 follow-up), 2026-09-13: a double-sided
//    triangle mesh flips `vGeomNormal` to face
//    whichever side a ray struck (TriangleMeshGeometry{,Indexed}::
//    IntersectRay), recording that in `bGeomNormalOrientedToRay`
//    (RayIntersectionGeometric.h).  Two consumers read `vGeomNormal`
//    as if it were unconditionally the object's TRUE, static outward
//    direction, without undoing that flip -- on a double-sided mesh
//    both effectively went blind.
//
//  THE BUGS THIS TEST GUARDS AGAINST
//
//    (a) `TranslucentSPF::Scatter`/`ScatterNM`'s DL-45 exit-lobe
//        geometric-horizon gate used raw `ri.vGeomNormal` as "the
//        object's actual outward direction ... which for real geometry
//        ... `ri.vGeomNormal` already reports unconditionally" -- false
//        on a double-sided mesh, where the gate becomes a no-op (or, in
//        this fixture's configuration, systematically validates the
//        WRONG hemisphere).
//
//    (b) `IORStackSeeding::SeedFromPoint`'s containment probe computed
//        `cosN = Dot(ri.geometric.vGeomNormal, probe.Dir())` directly.
//        On a double-sided mesh this is ALWAYS negative (the geometry
//        flips the normal to face the probe, on both a true entry and
//        a true exit), so `cosN > 0` (exit, parity++) never fires,
//        parity only ever decrements, and the object is never seeded --
//        DL-46's closure does not hold for a double-sided translucent
//        enclosure (e.g. a double-sided lampshade mesh).
//
//  THE FIX
//
//    Both sites now recover the TRUE, author-authored geometric normal
//    via the documented un-flip before using it:
//
//        oriented ? -vGeomNormal : vGeomNormal
//
//    (RayIntersectionGeometric.h's `bGeomNormalOrientedToRay` doc
//    comment).  Single-sided meshes and every analytical primitive
//    leave the flag false, so the recovery is a no-op for them.
//
//  COVERAGE
//
//    A real, closed, double-sided cube mesh (`TriangleMeshGeometryIndexed`,
//    12 triangles, flat per-face authored normals -- built programmatically,
//    not loaded from a file) with a real `TranslucentMaterial`.
//
//    Sub-test 1 (P2-1) -- the cube's +Z face carries a per-vertex
//    shading normal deliberately TILTED 60 degrees from the face's true
//    (flat) geometric plane normal (0,0,1) -- inside GlintModifier's
//    documented <=60 deg convention, and enough to make the mesh's
//    OWN vNormal/vGeomNormal double-sided flip decisions diverge for a
//    ray exiting at a shallow angle (see the derivation in this file's
//    header comment above `kExitDir`).  A real `Object`/`TranslucentMaterial`
//    hit from INSIDE the cube (`ior_stack` pre-seeded as already
//    containing the object, i.e. the "coming out the other side"
//    branch) drives `TranslucentSPF::Scatter`/`ScatterNM` many times;
//    every emitted diffuse exit direction is checked against the
//    fixture's INDEPENDENTLY KNOWN true outward normal (0,0,1) -- not
//    against anything the SPF itself reports.  Money assertion: 0
//    inward exits.  A regression of the P2-1 fix reproduces the
//    original bug report's "100% of exits point inward" for this
//    geometry (this file's fix-commit message carries the exact
//    pre-fix count).
//
//    Sub-test 2 (P2-2) -- a plain (untilted) double-sided cube in a
//    real `ObjectManager`/`Scene`, translucent material, probed with
//    the real `IORStackSeeding::SeedFromPoint` from the cube's center.
//    Money assertion: the cube is seeded (`stack.topObject() != 0`),
//    mirroring `TranslucentInitialContainmentTest`'s single-sided-mesh
//    assertion (A) but on a double-sided mesh, where DL-46's fix alone
//    does not hold.
//
//  Author: Aravind Krishnaswamy (RISE debt-cleanup, slice `translucent`)
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <iostream>

#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Managers/ObjectManager.h"
#include "../src/Library/Scene.h"
#include "../src/Library/Materials/TranslucentMaterial.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Utilities/IORStack.h"
#include "../src/Library/Utilities/IORStackSeeding.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Intersection/RayIntersection.h"

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
	//! Vector3 has no mutable operator[] (Point3 does) -- build an
	//! axis-aligned vector by direct member assignment instead.
	Vector3 AxisVector( int axis, Scalar value )
	{
		Vector3 v( 0, 0, 0 );
		if( axis == 0 ) v.x = value;
		else if( axis == 1 ) v.y = value;
		else v.z = value;
		return v;
	}

	//! Builds a closed, axis-aligned, double-sided unit cube ([-1,1]^3)
	//! out of 6 flat-shaded, UNSHARED-vertex quads (24 vertices, 12
	//! triangles) -- no vertex is shared between faces, so each face's
	//! authored normal cannot leak (via Phong interpolation) onto its
	//! neighbours.  Every face except +Z gets its own true, untilted
	//! flat outward normal.  The +Z face's authored per-vertex normal
	//! is tilted `tiltDegrees` off its true (0,0,1) geometric plane
	//! normal, toward -X -- i.e. authored normal
	//! (-sin(tilt), 0, cos(tilt)) -- uniformly across all 4 of its
	//! vertices (no Phong variation ACROSS the face itself, isolating
	//! the shading-vs-geometric divergence this fixture exists to
	//! create from any unrelated per-vertex interpolation effect).
	//! `tiltDegrees=0` gives a plain, untilted cube (sub-test 2's needs).
	TriangleMeshGeometryIndexed* BuildDoubleSidedCube( Scalar tiltDegrees )
	{
		TriangleMeshGeometryIndexed* mesh = new TriangleMeshGeometryIndexed( /*bDoubleSided*/true, false );
		mesh->BeginIndexedTriangles();

		// axis: 0=X,1=Y,2=Z.  sign: +1 or -1.  Emits one quad (4 vertices,
		// 2 triangles) for the face at that axis/sign, normal defaulting
		// to the true flat outward direction unless overridden.
		unsigned int nextVertex = 0;
		auto addFace = [&]( int axis, Scalar sign, const Vector3* overrideNormal )
		{
			const Vector3 outward = AxisVector( axis, sign );

			const int uAxis = (axis + 1) % 3;
			const int vAxis = (axis + 2) % 3;

			const unsigned int base = nextVertex;
			for( int su = -1; su <= 1; su += 2 ) {
				for( int sv = -1; sv <= 1; sv += 2 ) {
					Point3 p( 0, 0, 0 );
					p[axis] = sign;
					p[uAxis] = static_cast<Scalar>( su );
					p[vAxis] = static_cast<Scalar>( sv );
					mesh->AddVertex( p );
					mesh->AddNormal( overrideNormal ? *overrideNormal : outward );
					mesh->AddTexCoord( Point2( 0, 0 ) );
				}
			}
			// Corner order from the loop above: (-1,-1), (-1,1), (1,-1), (1,1)
			// in (u,v).  Triangle winding is irrelevant here -- the geometry
			// self-corrects vGeomNormal's sign to match the authored vNormal
			// (TriangleMeshGeometryIndexedSpecializations.h), which is the
			// whole point of authoring vNormal directly instead of relying
			// on winding.
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

		const Scalar tiltRad = tiltDegrees * PI / 180.0;
		Vector3 tiltedPlusZNormal( -std::sin(tiltRad), 0, std::cos(tiltRad) );

		addFace( 0, +1, 0 );
		addFace( 0, -1, 0 );
		addFace( 1, +1, 0 );
		addFace( 1, -1, 0 );
		addFace( 2, +1, tiltDegrees != 0 ? &tiltedPlusZNormal : 0 );
		addFace( 2, -1, 0 );

		mesh->DoneIndexedTriangles();
		return mesh;
	}

	//! Standard test-harness intersection call (matches
	//! GeometryShadingTangentTest.cpp's `Hit`): reset the scratch
	//! record, stamp the ray, run the full production Object::IntersectRay
	//! (front+back faces, exit info) -- this is what populates
	//! `ri.geometric.onb` from the (possibly double-sided-flipped)
	//! `ri.geometric.vNormal`, exactly like a real render.
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
//  Sub-test 1 (P2-1): TranslucentSPF exit gate on a double-sided,
//  tilted-face mesh.
//////////////////////////////////////////////////////////////////////
static void TestExitGateOnDoubleSidedMesh()
{
	std::cout << "Sub-test 1: TranslucentSPF exit gate on a double-sided tilted mesh face (P2-1)" << std::endl;

	TriangleMeshGeometryIndexed* g = BuildDoubleSidedCube( /*tiltDegrees*/ 60.0 );
	Object* o = new Object( g );
	safe_release( g );
	o->FinalizeTransformations();

	UniformColorPainter* front = new UniformColorPainter( RISEPel( 0.5, 0.5, 0.5 ) );  front->addref();
	UniformColorPainter* trans = new UniformColorPainter( RISEPel( 0.5, 0.5, 0.5 ) );  trans->addref();
	UniformScalarPainter* ext = new UniformScalarPainter( 0.0 );  ext->addref();
	UniformScalarPainter* phongN = new UniformScalarPainter( 1.0 );  phongN->addref();
	UniformScalarPainter* scat = new UniformScalarPainter( 0.0 );  scat->addref();
	TranslucentMaterial* material = new TranslucentMaterial( *front, *trans, *ext, *phongN, *scat );
	material->addref();
	o->AssignMaterial( *material );

	// Ray from inside the cube toward the +Z face at a 45-degree angle
	// (see the file header derivation): origin offset in -x so the hit
	// point stays well inside the face's [-1,1]^2 extent, direction
	// tilted 45 degrees off +Z toward -X.  Chosen so the mesh's OWN
	// vNormal flip (against the 60-degree-tilted authored normal) and
	// vGeomNormal flip (against the true (0,0,1) plane normal) decisions
	// DIVERGE -- exactly the configuration where P2-1's stale
	// "vGeomNormal is unconditionally outward" assumption breaks:
	// vGeomNormal flips (a real geometric exit) while vNormal does not.
	const Scalar d = 1.0 / std::sqrt( 2.0 );
	const Ray ray( Point3( 0.3, 0, 0 ), Vector3( -d, 0, d ) );

	RayIntersection ri( ray, nullRasterizerState );
	Hit( o, ray, ri );

	Check( ri.geometric.bHit, "ray hits the tilted +Z face" );
	Check( ri.geometric.bGeomNormalOrientedToRay,
		"fixture sanity: this hit's vGeomNormal WAS flipped by the double-sided mesh" );

	if( ri.geometric.bHit ) {
		const Vector3 trueOutward( 0, 0, 1 );

		IORStack stack( 1.0 );
		stack.SetCurrentObject( o );
		stack.push( 1.0 );
		Check( stack.containsCurrent(), "fixture starts in the 'coming out the other side' (interior) state" );

		RandomNumberGenerator rng( 777 );
		IndependentSampler sampler( rng );

		const int kTrials = 8192;
		int emitted = 0, inward = 0;
		for( int i = 0; i < kTrials; i++ ) {
			ScatteredRayContainer scattered;
			material->GetSPF()->Scatter( ri.geometric, sampler, scattered, stack );
			for( unsigned int j = 0; j < scattered.Count(); j++ ) {
				if( scattered[j].type != ScatteredRay::eRayDiffuse ) continue;
				emitted++;
				if( Vector3Ops::Dot( scattered[j].ray.Dir(), trueOutward ) <= 0 ) {
					inward++;
				}
			}
		}
		std::cout << "  emitted=" << emitted << "/" << kTrials << " inward=" << inward << std::endl;
		Check( emitted > 0, "at least some exit lobes are emitted (not vacuously suppressed)" );
		Check( inward == 0, "P2-1 money assertion: 0 emitted exit directions are geometrically inward" );

		// NM twin, same fixture, same money assertion.
		int emittedNM = 0, inwardNM = 0;
		for( int i = 0; i < kTrials; i++ ) {
			ScatteredRayContainer scattered;
			material->GetSPF()->ScatterNM( ri.geometric, sampler, 550.0, scattered, stack );
			for( unsigned int j = 0; j < scattered.Count(); j++ ) {
				if( scattered[j].type != ScatteredRay::eRayDiffuse ) continue;
				emittedNM++;
				if( Vector3Ops::Dot( scattered[j].ray.Dir(), trueOutward ) <= 0 ) {
					inwardNM++;
				}
			}
		}
		std::cout << "  NM emitted=" << emittedNM << "/" << kTrials << " inward=" << inwardNM << std::endl;
		Check( emittedNM > 0, "NM: at least some exit lobes are emitted (not vacuously suppressed)" );
		Check( inwardNM == 0, "P2-1 NM money assertion: 0 emitted exit directions are geometrically inward" );
	}

	o->release();
	material->release();
	scat->release();
	phongN->release();
	ext->release();
	trans->release();
	front->release();
}

//////////////////////////////////////////////////////////////////////
//  Sub-test 2 (P2-2): IORStackSeeding on a plain double-sided mesh.
//////////////////////////////////////////////////////////////////////
static void TestSeedingOnDoubleSidedMesh()
{
	std::cout << "Sub-test 2: IORStackSeeding on a double-sided (untilted) translucent mesh (P2-2)" << std::endl;

	TriangleMeshGeometryIndexed* g = BuildDoubleSidedCube( /*tiltDegrees*/ 0.0 );
	Object* o = new Object( g );
	safe_release( g );
	o->FinalizeTransformations();

	UniformColorPainter* front = new UniformColorPainter( RISEPel( 0.5, 0.5, 0.5 ) );  front->addref();
	UniformColorPainter* trans = new UniformColorPainter( RISEPel( 0.5, 0.5, 0.5 ) );  trans->addref();
	UniformScalarPainter* ext = new UniformScalarPainter( 0.0 );  ext->addref();
	UniformScalarPainter* phongN = new UniformScalarPainter( 1.0 );  phongN->addref();
	UniformScalarPainter* scat = new UniformScalarPainter( 0.3 );  scat->addref();
	TranslucentMaterial* material = new TranslucentMaterial( *front, *trans, *ext, *phongN, *scat );
	material->addref();
	o->AssignMaterial( *material );

	ObjectManager* manager = new ObjectManager( false, false, 4, 8 );
	manager->addref();
	manager->AddItem( o, "double_sided_translucent_cube" );

	Scene* scene = new Scene();
	scene->addref();
	scene->SetObjectManager( manager );

	// (A) The origin (cube center) is seeded -- mirrors
	// TranslucentInitialContainmentTest's assertion (A), but on a
	// double-sided mesh, where DL-46 alone (without this P2-2 fix)
	// leaves the stack empty because `cosN` never reads positive.
	{
		IORStack stack( 1.0 );
		IORStackSeeding::SeedFromPoint( stack, Point3( 0, 0, 0 ), *scene );
		Check( stack.topObject() != 0,
			"P2-2 money assertion: origin inside the double-sided translucent cube is seeded" );
		if( stack.topObject() ) {
			stack.SetCurrentObject( stack.topObject() );
			Check( stack.containsCurrent() && std::fabs( stack.top() - 1.0 ) < 1e-9,
				"seeded with the enclosing (air) IOR 1.0 -- translucent has no distinct IOR of its own" );
		}
	}

	// (B) Clearly outside: stack stays untouched (negative control,
	// mirrors TranslucentInitialContainmentTest's (C)).
	{
		IORStack stack( 1.0 );
		IORStackSeeding::SeedFromPoint( stack, Point3( 10, 10, 10 ), *scene );
		Check( stack.topObject() == 0, "outside-everything: stack unaffected" );
	}

	manager->release();
	scene->release();
	o->release();
	material->release();
	scat->release();
	phongN->release();
	ext->release();
	trans->release();
	front->release();
}

int main()
{
	GlobalLog();

	std::cout << "TranslucentDoubleSidedTest: P2-1/P2-2 double-sided vGeomNormal recovery" << std::endl;

	TestExitGateOnDoubleSidedMesh();
	TestSeedingOnDoubleSidedMesh();

	std::cout << std::endl << "Passed: " << passCount << std::endl << "Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
