//////////////////////////////////////////////////////////////////////
//
//  GeometricNormalPlumbingTest.cpp
//
//  Verifies that the recently-added `vGeomNormal` / `vGeomNormal2`
//  fields on RayIntersectionGeometric are populated correctly by
//  every plumbing site we touched:
//
//    1. Analytical primitives (sphere, box) — geometric == shading,
//       both at entry (`vGeomNormal`) and exit (`vGeomNormal2`).
//    2. Triangle mesh with smooth per-vertex normals — `vGeomNormal`
//       is the FLAT face normal, `vNormal` is the Phong-interpolated
//       shading normal; the two must differ by construction.
//    3. Object world-space transform — `vGeomNormal` / `vGeomNormal2`
//       transform with the same inverse-transpose as `vNormal`.
//    4. CSG_SUBTRACTION — the carved-out child contributes a flipped
//       `vGeomNormal` (sign matches the flipped `vNormal`).
//    5. CSG_INTERSECTION — entry follows the LATER child, exit
//       follows the EARLIER child; both `vGeomNormal[2]` follow the
//       same selection as `vNormal[2]`.
//    6. CSG_UNION exit promotion — when one child overlaps the other,
//       the composite exit `vGeomNormal2` follows the further child
//       (replaces the prior `-entryGeomNormal` heuristic).
//    7. CSG world-space transform — final `m_mxInvTranspose` step
//       applies to `vGeomNormal` / `vGeomNormal2` too.
//
//////////////////////////////////////////////////////////////////////
//
//  WHY THIS FILE USES A FAILURE TALLY AND NOT `assert`.
//  It was written assert-only, which on the project's own DOCUMENTED
//  WINDOWS PATH verifies NOTHING: build/cmake/rise-tests/CMakeLists.txt
//  does not override CMAKE_CXX_FLAGS_RELEASE, MSVC's default for that
//  config carries `/DNDEBUG`, and run_all_tests.ps1 defaults to
//  `-Config Release`.  Under NDEBUG every `assert` compiles to nothing,
//  so an INVERTED assertion still prints "Passed." and exits 0.
//
//  Worse than a silently-absent CHECK: several of the CSG tests build
//  their scene with `assert( csg->AssignObjects( a, b ) )` — a
//  SIDE-EFFECTING call sitting INSIDE the assert.  Under NDEBUG that
//  call never runs at all, so the composite is never assigned its
//  operands and every check downstream silently runs against an
//  unassigned CSG object instead of failing loudly.  Every
//  side-effecting call is therefore made on its own line here, with
//  only its RESULT checked.
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <iostream>
#include <vector>

#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Geometry/BoxGeometry.h"
#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"
#include "../src/Library/Intersection/RayIntersection.h"
#include "../src/Library/Objects/CSGObject.h"
#include "../src/Library/Objects/Object.h"

using namespace RISE;
using namespace RISE::Implementation;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const char* w ) { if( c ) ++g_pass; else { ++g_fail; std::printf( "  FAIL: %s\n", w ); } }

namespace
{
	const Scalar kEps = 1e-5;

	bool VecClose( const Vector3& a, const Vector3& b, Scalar eps = kEps )
	{
		return std::fabs( a.x - b.x ) < eps &&
			   std::fabs( a.y - b.y ) < eps &&
			   std::fabs( a.z - b.z ) < eps;
	}

	bool IsUnit( const Vector3& v, Scalar eps = kEps )
	{
		const Scalar m = Vector3Ops::SquaredModulus( v );
		return std::fabs( m - 1.0 ) < eps;
	}

	void Hit( IObject* pObj, const Ray& r, RayIntersection& ri,
			  bool bComputeExit = true )
	{
		ri.geometric.bHit = false;
		ri.geometric.range = RISE_INFINITY;
		ri.geometric.range2 = RISE_INFINITY;
		ri.geometric.ray = r;
		pObj->IntersectRay( ri, RISE_INFINITY, true, true, bComputeExit );
	}
}

//
// Test 1a: Sphere — vGeomNormal == vNormal at entry, exit symmetric.
//
void TestSphere_GeomEqualsShading()
{
	std::cout << "Sphere: geometric == shading at entry & exit..." << std::endl;

	SphereGeometry* pGeom = new SphereGeometry( 1.0 );
	Object* pObj = new Object( pGeom );
	safe_release( pGeom );
	pObj->FinalizeTransformations();

	// Ray hits +Z face at (0,0,1), exits at (0,0,-1).
	RayIntersection ri( Ray( Point3( 0, 0, 5 ), Vector3( 0, 0, -1 ) ),
						nullRasterizerState );
	Hit( pObj, ri.geometric.ray, ri );

	Check( ri.geometric.bHit, "Sphere: ray hits the sphere" );
	Check( IsUnit( ri.geometric.vNormal ), "Sphere: entry shading normal is unit length" );
	Check( IsUnit( ri.geometric.vGeomNormal ), "Sphere: entry geometric normal is unit length" );
	Check( IsUnit( ri.geometric.vNormal2 ), "Sphere: exit shading normal is unit length" );
	Check( IsUnit( ri.geometric.vGeomNormal2 ), "Sphere: exit geometric normal is unit length" );
	Check( VecClose( ri.geometric.vGeomNormal,  ri.geometric.vNormal ), "Sphere: entry vGeomNormal equals vNormal" );
	Check( VecClose( ri.geometric.vGeomNormal2, ri.geometric.vNormal2 ), "Sphere: exit vGeomNormal2 equals vNormal2" );
	// Sanity: entry and exit on opposite poles
	Check( VecClose( ri.geometric.vGeomNormal,  Vector3( 0, 0,  1 ) ), "Sphere: entry vGeomNormal is +Z pole" );
	Check( VecClose( ri.geometric.vGeomNormal2, Vector3( 0, 0, -1 ) ), "Sphere: exit vGeomNormal2 is -Z pole" );

	safe_release( pObj );
}

//
// Test 1b: Box — entry on +Z face, exit on -Z face; both geom == shading.
//
void TestBox_GeomEqualsShading()
{
	std::cout << "Box: geometric == shading at entry & exit..." << std::endl;

	// Unit cube centered at origin: half-extents (0.5, 0.5, 0.5)
	BoxGeometry* pGeom = new BoxGeometry( 1.0, 1.0, 1.0 );
	Object* pObj = new Object( pGeom );
	safe_release( pGeom );
	pObj->FinalizeTransformations();

	RayIntersection ri( Ray( Point3( 0, 0, 5 ), Vector3( 0, 0, -1 ) ),
						nullRasterizerState );
	Hit( pObj, ri.geometric.ray, ri );

	Check( ri.geometric.bHit, "Box: ray hits the box" );
	Check( VecClose( ri.geometric.vGeomNormal,  ri.geometric.vNormal ), "Box: entry vGeomNormal equals vNormal" );
	Check( VecClose( ri.geometric.vGeomNormal2, ri.geometric.vNormal2 ), "Box: exit vGeomNormal2 equals vNormal2" );
	Check( VecClose( ri.geometric.vGeomNormal,  Vector3( 0, 0,  1 ) ), "Box: entry vGeomNormal is +Z face" );
	Check( VecClose( ri.geometric.vGeomNormal2, Vector3( 0, 0, -1 ) ), "Box: exit vGeomNormal2 is -Z face" );

	safe_release( pObj );
}

//
// Test 2: Triangle mesh with smooth Phong-interpolated normals tilted
// off the flat face normal — vGeomNormal must be the face normal,
// vNormal the (different) interpolated shading normal.
//
void TestMesh_GeomDistinctFromShading()
{
	std::cout << "Mesh: geometric (face) distinct from shading (Phong)..." << std::endl;

	// Single triangle in the y=0 plane (face normal = +Y).
	// Per-vertex normals tilted: each vertex has a normal
	// pulled toward an outward direction so the Phong-interpolated
	// shading normal at the centroid is (0,1,0) only by symmetry —
	// but at an off-center hit, it deviates from the face normal.
	TriangleMeshGeometryIndexed* pMesh = new TriangleMeshGeometryIndexed(
		true,    // double sided
		false    // do NOT use face normals — force Phong path
	);

	pMesh->BeginIndexedTriangles();

	pMesh->AddVertex( Point3( -1, 0, -1 ) );
	pMesh->AddVertex( Point3(  1, 0, -1 ) );
	pMesh->AddVertex( Point3(  0, 0,  1 ) );

	// Per-vertex normals — bent away from the (0,1,0) face normal
	// in different directions per vertex.  Pick directions whose
	// y-component is positive (so they orient consistently with
	// the face normal), and whose tangential components do NOT
	// cancel at the hit point we'll cast at.
	pMesh->AddNormal( Vector3Ops::Normalize( Vector3( -0.5,  1.0,  0.0 ) ) );
	pMesh->AddNormal( Vector3Ops::Normalize( Vector3(  0.5,  1.0,  0.0 ) ) );
	pMesh->AddNormal( Vector3Ops::Normalize( Vector3(  0.0,  1.0,  0.5 ) ) );

	pMesh->AddTexCoord( Point2( 0, 0 ) );
	pMesh->AddTexCoord( Point2( 1, 0 ) );
	pMesh->AddTexCoord( Point2( 0, 1 ) );

	IndexedTriangle t;
	t.iVertices[0] = 0; t.iVertices[1] = 1; t.iVertices[2] = 2;
	t.iNormals[0]  = 0; t.iNormals[1]  = 1; t.iNormals[2]  = 2;
	t.iCoords[0]   = 0; t.iCoords[1]   = 1; t.iCoords[2]   = 2;
	pMesh->AddIndexedTriangle( t );

	pMesh->DoneIndexedTriangles();

	Object* pObj = new Object( pMesh );
	safe_release( pMesh );
	pObj->FinalizeTransformations();

	// Hit the triangle off-center (so Phong interpolation differs
	// meaningfully from the face normal).
	RayIntersection ri( Ray( Point3( 0.3, 5, -0.2 ), Vector3( 0, -1, 0 ) ),
						nullRasterizerState );
	Hit( pObj, ri.geometric.ray, ri, false );

	Check( ri.geometric.bHit, "Mesh: ray hits the triangle" );
	Check( IsUnit( ri.geometric.vNormal ), "Mesh: shading normal is unit length" );
	Check( IsUnit( ri.geometric.vGeomNormal ), "Mesh: geometric normal is unit length" );

	// Geometric normal must be exactly the face normal (+Y).
	Check( VecClose( ri.geometric.vGeomNormal, Vector3( 0, 1, 0 ) ), "Mesh: geometric normal is the flat face normal (+Y)" );

	// Shading normal must DIFFER from the geometric normal at this
	// off-center sample (otherwise the test isn't meaningful).
	const Scalar diff = std::fabs( ri.geometric.vNormal.x - ri.geometric.vGeomNormal.x )
					  + std::fabs( ri.geometric.vNormal.z - ri.geometric.vGeomNormal.z );
	Check( diff > 1e-3, "Mesh: Phong shading normal differs from geometric normal at off-center hit" );

	safe_release( pObj );
}

//
// Test 3: Object world-space transform — vGeomNormal / vGeomNormal2
// transform with the inverse-transpose alongside vNormal / vNormal2.
//
void TestObject_TransformsGeomNormals()
{
	std::cout << "Object: transform applied to vGeomNormal & vGeomNormal2..." << std::endl;

	SphereGeometry* pGeom = new SphereGeometry( 1.0 );
	Object* pObj = new Object( pGeom );
	safe_release( pGeom );

	// Translate the sphere; rotation isn't strictly needed for the
	// invariance check (sphere is rotation-symmetric) but a non-axis-
	// aligned position guarantees we exercise the world-space code path.
	pObj->SetPosition( Point3( 3, 4, 5 ) );
	pObj->FinalizeTransformations();

	// Cast a ray straight at the sphere center along -Z.
	// Entry hits the +Z hemisphere; exit hits the -Z hemisphere.
	const Point3 center( 3, 4, 5 );
	RayIntersection ri( Ray( Point3( 3, 4, 10 ), Vector3( 0, 0, -1 ) ),
						nullRasterizerState );
	Hit( pObj, ri.geometric.ray, ri );

	Check( ri.geometric.bHit, "Object-transform: ray hits the translated sphere" );

	// Entry normal must point from the sphere center toward (3,4,6)
	// in world space → (0,0,+1).  Exit normal → (0,0,-1).
	Check( VecClose( ri.geometric.vGeomNormal,  Vector3( 0, 0,  1 ) ), "Object-transform: entry vGeomNormal is world +Z" );
	Check( VecClose( ri.geometric.vGeomNormal2, Vector3( 0, 0, -1 ) ), "Object-transform: exit vGeomNormal2 is world -Z" );

	// And of course they still equal the shading normals.
	Check( VecClose( ri.geometric.vGeomNormal,  ri.geometric.vNormal ), "Object-transform: entry vGeomNormal equals vNormal" );
	Check( VecClose( ri.geometric.vGeomNormal2, ri.geometric.vNormal2 ), "Object-transform: exit vGeomNormal2 equals vNormal2" );

	safe_release( pObj );
}

//
// Test 4: CSG_SUBTRACTION — A minus B, ray pierces through the carved
// region.  The "entry into A-minus-B" surface is the FLIPPED B surface
// (we're now leaving the void where B was carved out, into A's solid).
// vGeomNormal must follow the same flip as vNormal.
//
void TestCSG_Subtraction_FlippedGeomNormal()
{
	std::cout << "CSG_SUBTRACTION: flipped vGeomNormal at carved entry..." << std::endl;

	// A: a large sphere at origin, radius 1.0
	// B: a smaller sphere offset on +Z, radius 0.6 — carves a chunk
	//    out of the +Z side of A.
	SphereGeometry* gA = new SphereGeometry( 1.0 );
	SphereGeometry* gB = new SphereGeometry( 0.6 );
	Object* oA = new Object( gA );
	Object* oB = new Object( gB );
	safe_release( gA );
	safe_release( gB );

	oA->SetPosition( Point3( 0, 0,  0.0 ) );
	oB->SetPosition( Point3( 0, 0,  0.7 ) );  // overlaps the +Z cap of A
	oA->FinalizeTransformations();
	oB->FinalizeTransformations();

	CSGObject* csg = new CSGObject( CSG_SUBTRACTION );
	const bool assigned = csg->AssignObjects( oA, oB );
	Check( assigned, "CSG-Subtraction: the composite takes both operands" );
	csg->FinalizeTransformations();

	// Ray enters from +Z along -Z, axis-aligned.  It enters A at z=+1,
	// hits B at z = 0.7+0.6 = 1.3 first → so it actually enters B
	// before A.  Re-cast from inside the carved overlap region: shoot
	// from above so we hit B's far face *inside* A.
	//
	// To get the "inside-A, exit-via-flipped-B-front" case, we cast
	// from inside A but outside B → start INSIDE A first.  Easiest:
	// fire from (0, 0, +0.95) toward -Z.  At z=+0.95 we're inside A
	// but inside the carve too (B reaches from 0.1 to 1.3 along Z).
	// So instead: fire from a point inside A but *not* inside B —
	// (0, 0.5, 0) → it's inside A (radius<=1), outside B (B is centered
	// at +0.7 in Z so its sphere doesn't reach (0, 0.5, 0)).
	//
	// Simplest: external ray that grazes through the carved hole.
	// Fire along -X from (+5, 0, 0.7) toward origin.  It enters A
	// at x=+1, y=0, z=0.7 ... but z=0.7 is on B's center plane, B has
	// radius 0.6 → the ray actually enters B first at x=+0.6, before
	// it would enter A.  In the SUBTRACTION case "inside B but not
	// inside A" matches the riObjB.range2==0 branch isn't quite right
	// either.  Use the "outside A, outside B" branch: ray that enters
	// A first, then enters B while inside A (the carved hole).
	//
	// Setup: ray from (-5, 0, 0.7) toward +X.  It enters A at x=-sqrt(1-0.49)=-0.714
	// (range to A entry).  Inside A, it then enters B at x=-0.6 (B carves
	// from x=-0.6 to x=+0.6 at z=0.7).  The composite (A - B) entry
	// surface at that point is FLIPPED B → -vNormalB.
	//
	// Per the SUBTRACTION code path "We hit bit as well" → first
	// branch riObjA.range < riObjB.range — that branch sets
	//   ri.geometric.vNormal2 = -riObjB.vNormal
	// (because it's the EXIT from A-minus-B at the START of the carve)
	// Hmm — that branch is the one where A's entry comes BEFORE B's,
	// and the "inside" segment ends at B's entry (range2 = riObjB.range,
	// vNormal2 = -riObjB.vNormal).  Let's check that case.
	{
		Ray r( Point3( -5, 0, 0.7 ), Vector3( 1, 0, 0 ) );
		RayIntersection ri( r, nullRasterizerState );
		Hit( csg, r, ri );
		Check( ri.geometric.bHit, "CSG-Subtraction: ray hits the composite (entry-A/exit-B case)" );

		// Entry: A's outer surface at x ≈ -0.714, normal ≈ (-x,0,0)
		// in object space, here world == object since CSG identity transform.
		// Just verify entry geom == shading.
		Check( VecClose( ri.geometric.vGeomNormal, ri.geometric.vNormal ), "CSG-Subtraction: entry vGeomNormal equals vNormal (A's outer surface)" );

		// Exit: when the ray hits B's near surface at x=-0.6,
		// composite (A-B) ends; the exit normal is -vNormal_B.
		// vNormal_B at that point = (-0.6, 0, 0)/0.6 = (-1, 0, 0),
		// so exit vNormal2 (and vGeomNormal2) = -(-1,0,0) = (+1, 0, 0).
		Check( VecClose( ri.geometric.vGeomNormal2, ri.geometric.vNormal2 ), "CSG-Subtraction: exit vGeomNormal2 equals vNormal2 (flipped B surface)" );
		Check( VecClose( ri.geometric.vGeomNormal2, Vector3( 1, 0, 0 ) ), "CSG-Subtraction: exit vGeomNormal2 is flipped B normal (+1,0,0)" );
	}

	// Now fire a ray that starts INSIDE the carved hole of A:
	// from (0, 0, 0.7) toward +X.  We're inside both A and B at start.
	// Per the SUBTRACTION first sub-case (inside both), the composite
	// boundary is hit when we exit B before exiting A:
	//   ri = riObjB; vNormal = -riObjB.vNormal (FLIPPED B entry)
	{
		Ray r( Point3( 0, 0, 0.7 ), Vector3( 1, 0, 0 ) );
		RayIntersection ri( r, nullRasterizerState );
		Hit( csg, r, ri );
		Check( ri.geometric.bHit, "CSG-Subtraction: ray hits the composite (inside-carve case)" );

		// We exit B at x = +0.6.  The B normal at (0.6, 0, 0) (object
		// space of B, which is offset to (0,0,0.7) in CSG space — but
		// here normal is from sphere center, which after offset is
		// still (1, 0, 0) on the local x-axis since the offset is in Z).
		// So composite vNormal = -B_normal = (-1, 0, 0).
		Check( VecClose( ri.geometric.vGeomNormal, ri.geometric.vNormal ), "CSG-Subtraction: entry vGeomNormal equals vNormal (flipped B, inside-carve case)" );
		Check( VecClose( ri.geometric.vGeomNormal, Vector3( -1, 0, 0 ) ), "CSG-Subtraction: entry vGeomNormal is flipped B normal (-1,0,0)" );
	}

	safe_release( csg );
	safe_release( oA );
	safe_release( oB );
}

//
// Test 5: CSG_INTERSECTION — entry follows the LATER child, exit
// follows the EARLIER child; vGeomNormal must follow vNormal.
//
void TestCSG_Intersection_GeomFollowsShading()
{
	std::cout << "CSG_INTERSECTION: vGeomNormal follows entry/exit child..." << std::endl;

	// Two unit spheres separated along X, overlapping at the origin
	// region.  A at (-0.5,0,0), B at (+0.5,0,0).
	SphereGeometry* gA = new SphereGeometry( 1.0 );
	SphereGeometry* gB = new SphereGeometry( 1.0 );
	Object* oA = new Object( gA );
	Object* oB = new Object( gB );
	safe_release( gA );
	safe_release( gB );

	oA->SetPosition( Point3( -0.5, 0, 0 ) );
	oB->SetPosition( Point3( +0.5, 0, 0 ) );
	oA->FinalizeTransformations();
	oB->FinalizeTransformations();

	CSGObject* csg = new CSGObject( CSG_INTERSECTION );
	const bool assigned = csg->AssignObjects( oA, oB );
	Check( assigned, "CSG-Intersection: the composite takes both operands" );
	csg->FinalizeTransformations();

	// Ray from -X along +X, straight through the origin.
	// Enters A at x=-1.5, exits A at x=+0.5 (A center -0.5, radius 1).
	// Enters B at x=-0.5, exits B at x=+1.5.
	// CSG_INTERSECTION:
	//   entry = max(A.entry, B.entry) = -0.5  → from B's entry surface
	//   exit  = min(A.exit, B.exit)  = +0.5   → from A's exit surface
	Ray r( Point3( -10, 0, 0 ), Vector3( 1, 0, 0 ) );
	RayIntersection ri( r, nullRasterizerState );
	Hit( csg, r, ri );
	Check( ri.geometric.bHit, "CSG-Intersection: ray hits the composite" );

	// Entry world-space point: (-0.5, 0, 0) → on B's surface.
	// B's center is (0.5, 0, 0); normal = (-0.5 - 0.5, 0, 0) / 1 = (-1, 0, 0).
	Check( VecClose( ri.geometric.vGeomNormal, ri.geometric.vNormal ), "CSG-Intersection: entry vGeomNormal equals vNormal (later child, B)" );
	Check( VecClose( ri.geometric.vGeomNormal, Vector3( -1, 0, 0 ) ), "CSG-Intersection: entry vGeomNormal is B's surface normal" );

	// Exit world-space point: (+0.5, 0, 0) → on A's surface.
	// A's center is (-0.5, 0, 0); normal = (0.5 - (-0.5), 0, 0) / 1 = (+1, 0, 0).
	Check( VecClose( ri.geometric.vGeomNormal2, ri.geometric.vNormal2 ), "CSG-Intersection: exit vGeomNormal2 equals vNormal2 (earlier child, A)" );
	Check( VecClose( ri.geometric.vGeomNormal2, Vector3( 1, 0, 0 ) ), "CSG-Intersection: exit vGeomNormal2 is A's surface normal" );

	safe_release( csg );
	safe_release( oA );
	safe_release( oB );
}

//
// Test 6: CSG_UNION exit promotion — two overlapping spheres, ray
// enters inside one and the union exit is on the OTHER (further)
// sphere.  vGeomNormal2 must come from that further child, NOT from
// a fabricated `-entryGeomNormal`.
//
void TestCSG_Union_ExitPromotion()
{
	std::cout << "CSG_UNION: exit promotion uses real child geom normal..." << std::endl;

	// A at (-0.5,0,0) radius 1, B at (+0.5,0,0) radius 1.
	// Same overlap as the intersection test.
	SphereGeometry* gA = new SphereGeometry( 1.0 );
	SphereGeometry* gB = new SphereGeometry( 1.0 );
	Object* oA = new Object( gA );
	Object* oB = new Object( gB );
	safe_release( gA );
	safe_release( gB );

	oA->SetPosition( Point3( -0.5, 0, 0 ) );
	oB->SetPosition( Point3( +0.5, 0, 0 ) );
	oA->FinalizeTransformations();
	oB->FinalizeTransformations();

	CSGObject* csg = new CSGObject( CSG_UNION );
	const bool assigned = csg->AssignObjects( oA, oB );
	Check( assigned, "CSG-Union-exit: the composite takes both operands" );
	csg->FinalizeTransformations();

	// Ray from -X along +X, same as intersection test.
	// Enters A at -1.5, exits A at +0.5.
	// Enters B at -0.5, exits B at +1.5.
	// UNION: entry = min(A.entry, B.entry) = -1.5 (A), exit = max(A.exit, B.exit) = +1.5 (B).
	Ray r( Point3( -10, 0, 0 ), Vector3( 1, 0, 0 ) );
	RayIntersection ri( r, nullRasterizerState );
	Hit( csg, r, ri );
	Check( ri.geometric.bHit, "CSG-Union-exit: ray hits the composite (axis-aligned case)" );

	// Entry: A's near pole at (-1.5, 0, 0) → normal (-1, 0, 0).
	Check( VecClose( ri.geometric.vGeomNormal, ri.geometric.vNormal ), "CSG-Union-exit: entry vGeomNormal equals vNormal (A's near pole)" );
	Check( VecClose( ri.geometric.vGeomNormal, Vector3( -1, 0, 0 ) ), "CSG-Union-exit: entry vGeomNormal is A's near-pole normal" );

	// Exit: B's far pole at (+1.5, 0, 0).  B center (+0.5,0,0),
	// normal = (1.5 - 0.5, 0, 0) / 1 = (+1, 0, 0).
	// CRITICAL: this is the case where the prior code fabricated
	// `-entryGeomNormal` = (+1,0,0) which would coincidentally match,
	// so make this case more discriminating: aim the ray off-axis
	// so the "fabricated" answer would be wrong.
	Check( VecClose( ri.geometric.vGeomNormal2, ri.geometric.vNormal2 ), "CSG-Union-exit: exit vGeomNormal2 equals vNormal2 (B's far pole)" );

	// Now an off-axis ray to confirm the geom normal isn't just an
	// echo of the entry normal.
	{
		Ray r2( Point3( -10, 0.3, 0 ), Vector3( 1, 0, 0 ) );
		RayIntersection ri2( r2, nullRasterizerState );
		Hit( csg, r2, ri2 );
		Check( ri2.geometric.bHit, "CSG-Union-exit: ray hits the composite (off-axis case)" );

		// Entry on A: world point (xA, 0.3, 0) where xA = -0.5 - sqrt(1 - 0.09).
		const Scalar dx = std::sqrt( 1.0 - 0.09 );
		const Scalar xA_entry = -0.5 - dx;
		const Vector3 nA_entry = Vector3Ops::Normalize(
			Vector3( xA_entry - (-0.5), 0.3, 0 ) );
		Check( VecClose( ri2.geometric.vGeomNormal, ri2.geometric.vNormal ), "CSG-Union-exit: off-axis entry vGeomNormal equals vNormal" );
		Check( VecClose( ri2.geometric.vGeomNormal, nA_entry, 1e-4 ), "CSG-Union-exit: off-axis entry vGeomNormal matches computed A entry normal" );

		// Exit on B: world point (xB, 0.3, 0) where xB = +0.5 + sqrt(1 - 0.09).
		const Scalar xB_exit = +0.5 + dx;
		const Vector3 nB_exit = Vector3Ops::Normalize(
			Vector3( xB_exit - 0.5, 0.3, 0 ) );

		Check( VecClose( ri2.geometric.vGeomNormal2, ri2.geometric.vNormal2 ), "CSG-Union-exit: off-axis exit vGeomNormal2 equals vNormal2" );
		Check( VecClose( ri2.geometric.vGeomNormal2, nB_exit, 1e-4 ), "CSG-Union-exit: off-axis exit vGeomNormal2 matches computed B exit normal" );

		// Negative check: confirm the prior buggy `-entryGeomNormal`
		// answer would have been WRONG here.
		const Vector3 fabricated = Vector3( -nA_entry.x, -nA_entry.y, -nA_entry.z );
		Check( !VecClose( ri2.geometric.vGeomNormal2, fabricated, 1e-3 ), "CSG-Union-exit: exit vGeomNormal2 is NOT the fabricated -entryGeomNormal value" );
	}

	safe_release( csg );
	safe_release( oA );
	safe_release( oB );
}

//
// Test 7: CSG world-space transform — geom normals must be in WORLD
// space after the final m_mxInvTranspose step inside CSGObject, AND
// they must track vNormal / vNormal2 through that transform.
//
// We use a 30°-about-Y rotation expressed in RADIANS (Transformable's
// SetOrientation takes radians; the scene parser converts degrees on
// the way in).  The exact world-space normal direction depends on
// matrix layout convention, so we don't assert specific (x,y,z) values
// — instead we assert two convention-independent invariants:
//   (a) post-transform |vGeomNormal| = 1 and |vGeomNormal2| = 1
//   (b) vGeomNormal == vNormal and vGeomNormal2 == vNormal2  (the
//       whole point of the plumbing — they must agree on analytical
//       primitives at every stage of the pipeline)
//
void TestCSG_WorldTransform()
{
	std::cout << "CSG: vGeomNormal world-transformed alongside vNormal..." << std::endl;

	const Scalar kHalfPi = 1.5707963267948966;
	const Scalar kThirtyDeg = kHalfPi / 3.0;

	// Two spheres (UNION) along local X, then rotate about Y.
	SphereGeometry* gA = new SphereGeometry( 1.0 );
	SphereGeometry* gB = new SphereGeometry( 1.0 );
	Object* oA = new Object( gA );
	Object* oB = new Object( gB );
	safe_release( gA );
	safe_release( gB );

	oA->SetPosition( Point3( -0.5, 0, 0 ) );
	oB->SetPosition( Point3( +0.5, 0, 0 ) );
	oA->FinalizeTransformations();
	oB->FinalizeTransformations();

	// First: CSG with NO transform — record the un-transformed geom
	// normals as a sanity baseline.
	{
		CSGObject* csgNoTx = new CSGObject( CSG_UNION );
		const bool noTxAssigned = csgNoTx->AssignObjects( oA, oB );
		Check( noTxAssigned, "CSG-WorldTransform: (baseline) the no-transform composite takes both operands" );
		csgNoTx->FinalizeTransformations();

		Ray r( Point3( -10, 0.3, 0 ), Vector3( 1, 0, 0 ) );
		RayIntersection ri( r, nullRasterizerState );
		Hit( csgNoTx, r, ri );
		Check( ri.geometric.bHit, "CSG-WorldTransform: (baseline) ray hits the untransformed composite" );
		Check( IsUnit( ri.geometric.vGeomNormal ), "CSG-WorldTransform: (baseline) vGeomNormal is unit length" );
		Check( IsUnit( ri.geometric.vGeomNormal2 ), "CSG-WorldTransform: (baseline) vGeomNormal2 is unit length" );
		Check( VecClose( ri.geometric.vGeomNormal,  ri.geometric.vNormal ), "CSG-WorldTransform: (baseline) entry vGeomNormal equals vNormal" );
		Check( VecClose( ri.geometric.vGeomNormal2, ri.geometric.vNormal2 ), "CSG-WorldTransform: (baseline) exit vGeomNormal2 equals vNormal2" );
		safe_release( csgNoTx );
	}

	// Now: CSG with a 30°-about-Y rotation applied to the composite.
	// The geometric and shading normals MUST stay in lockstep through
	// the m_mxInvTranspose world-transform step.
	{
		CSGObject* csgTx = new CSGObject( CSG_UNION );
		const bool txAssigned = csgTx->AssignObjects( oA, oB );
		Check( txAssigned, "CSG-WorldTransform: the rotated composite takes both operands" );
		csgTx->SetOrientation( Vector3( 0, kThirtyDeg, 0 ) );
		csgTx->FinalizeTransformations();

		// Cast along world +X at y=0.3.  Even after the rotation the
		// ray will intersect the rotated UNION since it passes through
		// the origin region at y=0.3, well inside both rotated spheres.
		Ray r( Point3( -10, 0.3, 0 ), Vector3( 1, 0, 0 ) );
		RayIntersection ri( r, nullRasterizerState );
		Hit( csgTx, r, ri );
		Check( ri.geometric.bHit, "CSG-WorldTransform: ray hits the rotated composite" );

		// Convention-independent claims:
		Check( IsUnit( ri.geometric.vGeomNormal ), "CSG-WorldTransform: rotated vGeomNormal is unit length" );
		Check( IsUnit( ri.geometric.vGeomNormal2 ), "CSG-WorldTransform: rotated vGeomNormal2 is unit length" );
		Check( VecClose( ri.geometric.vGeomNormal,  ri.geometric.vNormal  ), "CSG-WorldTransform: rotated entry vGeomNormal equals vNormal" );
		Check( VecClose( ri.geometric.vGeomNormal2, ri.geometric.vNormal2 ), "CSG-WorldTransform: rotated exit vGeomNormal2 equals vNormal2" );

		// The transform is non-identity (we rotated 30°), so the geom
		// normal is no longer along world ±X.  Confirm the rotation
		// actually moved it (not a no-op transform).
		const Scalar offAxis =
			std::fabs( ri.geometric.vGeomNormal.y ) +
			std::fabs( ri.geometric.vGeomNormal.z );
		Check( offAxis > 1e-3 ||
				std::fabs( std::fabs( ri.geometric.vGeomNormal.x ) - 1.0 ) > 1e-3,
			   "CSG-WorldTransform: rotation actually moved vGeomNormal off world ±X (non-identity transform)" );

		safe_release( csgTx );
	}

	safe_release( oA );
	safe_release( oB );
}

//
// Test 8: copy ctor / operator= round-trip the new fields.
//
void TestRayIntersectionGeometric_CopySemantics()
{
	std::cout << "RayIntersectionGeometric: copy-ctor & op= preserve vGeomNormal[2]..." << std::endl;

	RayIntersectionGeometric a( Ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) ),
								nullRasterizerState );
	a.vNormal       = Vector3( 1, 0, 0 );
	a.vNormal2      = Vector3( 0, 1, 0 );
	a.vGeomNormal   = Vector3( 0, 0, 1 );
	a.vGeomNormal2  = Vector3( 1, 1, 0 );

	// Copy ctor
	RayIntersectionGeometric b( a );
	Check( VecClose( b.vGeomNormal,  Vector3( 0, 0, 1 ) ), "RayIntersectionGeometric-copy: copy-ctor preserves vGeomNormal" );
	Check( VecClose( b.vGeomNormal2, Vector3( 1, 1, 0 ) ), "RayIntersectionGeometric-copy: copy-ctor preserves vGeomNormal2" );

	// op=
	RayIntersectionGeometric c( Ray( Point3(0,0,0), Vector3(1,0,0) ),
								nullRasterizerState );
	c = a;
	Check( VecClose( c.vGeomNormal,  Vector3( 0, 0, 1 ) ), "RayIntersectionGeometric-copy: operator= preserves vGeomNormal" );
	Check( VecClose( c.vGeomNormal2, Vector3( 1, 1, 0 ) ), "RayIntersectionGeometric-copy: operator= preserves vGeomNormal2" );
}

int main()
{
	TestRayIntersectionGeometric_CopySemantics();
	TestSphere_GeomEqualsShading();
	TestBox_GeomEqualsShading();
	TestMesh_GeomDistinctFromShading();
	TestObject_TransformsGeomNormals();
	TestCSG_Subtraction_FlippedGeomNormal();
	TestCSG_Intersection_GeomFollowsShading();
	TestCSG_Union_ExitPromotion();
	TestCSG_WorldTransform();
	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
