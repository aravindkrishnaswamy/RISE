//////////////////////////////////////////////////////////////////////
//
//  CsgProbeFloorTest.cpp
//
//  Regression coverage for the two CSG-LEVEL defects the adversarial
//  review of 4b141ad3 found in `SelfHitRootFloor` and its one consumer,
//  `CSGObject::AdoptCsgExitFacePayloadViaProbe`.  (The per-geometry
//  half of that review -- the torus deflation band and the SDF
//  Lipschitz divisor -- is pinned in tests/BoxGeometryTest.cpp's
//  `RunSelfHitRootFloorContract`, against the intersection routines
//  themselves; this file covers what only shows up once a COMPOSITE is
//  answering.)
//
//    P1-3 (infinite floor).  `Geometry::BoundingBoxRootFloor` sums the
//      bounding box's corner L1.  A collection with nothing in it --
//      an unbuilt `TriangleMeshGeometryIndexed` (null BVH), an empty
//      `BilinearPatchGeometry` -- reports the DEFAULT `BoundingBox()`,
//      whose corners are +-RISE_INFINITY (DBL_MAX), so the sum
//      OVERFLOWS and the floor came back +inf.  Through the probe that
//      became an infinite margin, a probe origin with a NaN component
//      (0 * inf on a zero direction component), and a
//      `range > maxAcceptRange` rejection that can never fire because
//      every comparison against NaN is false.  Fixed at BOTH layers:
//      the bbox helper falls back to the generic floor when the box is
//      not a real, built, finite one, and the probe returns false
//      (its own graceful fallback) on a non-finite floor or margin.
//
//    P2-1 (sibling inflation).  `CSGObject::SelfHitRootFloor` took the
//      max over BOTH operands unconditionally, so an operand that has
//      nothing to do with the face being re-hit set the composite's
//      floor.  Measured in Test 2 below: a triangle-mesh lobe parked 1e4
//      units from a unit box claims 3.008e-8 (correct FOR THAT MESH)
//      against the box's own 4.014e-12, and the parent reported the
//      mesh's -- 7494x too wide.  Through the probe's 2x margin and 10 %
//      slack that is a same-face acceptance window of ~1.3e-7, ~250x
//      WIDER than the 5e-10 decoy gap CsgSurfacePayloadTest Test 15
//      exists to bound.  Fixed by charging only operands whose surface
//      actually passes through the face at `localOrigin`, settled with a
//      short two-sided probe along the face normal.
//
//  Test map:
//    1. Unbuilt / empty collections report a FINITE, generic floor
//       (P1-3, geometry layer).  Red-proof: drop the `built` guard in
//       Geometry::BoundingBoxRootFloor and both rows report inf.
//    2. A nested CSG_UNION of a unit box and a far mesh reports the
//       BOX's own floor on the box's face, not the mesh's (P2-1).
//    3. The same composite still recovers the REAL exit face's payload
//       end to end through the CSG_SUBTRACTION exit-designated branch
//       -- i.e. the ownership filter tightened the floor without
//       breaking the probe it feeds.
//    4. A nested CSG_UNION containing an EMPTY mesh operand, driven
//       through the exit-designated branch: every composite field
//       finite (no NaN), and the payload is the real exit face's
//       (P1-3, end to end).
//
//  Style follows tests/CsgSurfacePayloadTest.cpp (`Check` / `Hit` /
//  oracle probes, plain counters, no framework).  Deliberately a
//  SEPARATE file rather than more cases in that one: these are
//  floor-and-probe mechanics, not surface-payload algebra.
//
//  Author: Claude Opus 4.8
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <iostream>

#include "../src/Library/Geometry/BilinearPatchGeometry.h"
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
	const Scalar kEps = 1e-6;

	bool Close( Scalar a, Scalar b, Scalar eps = kEps )
	{
		return std::fabs( a - b ) < eps;
	}

	bool Point2Close( const Point2& a, const Point2& b, Scalar eps = kEps )
	{
		return Close( a.x, b.x, eps ) && Close( a.y, b.y, eps );
	}

	bool PointClose( const Point3& a, const Point3& b, Scalar eps = kEps )
	{
		return Close( a.x, b.x, eps ) && Close( a.y, b.y, eps ) && Close( a.z, b.z, eps );
	}

	bool PointFinite( const Point3& p )
	{
		return std::isfinite( p.x ) && std::isfinite( p.y ) && std::isfinite( p.z );
	}

	bool VectorFinite( const Vector3& v )
	{
		return std::isfinite( v.x ) && std::isfinite( v.y ) && std::isfinite( v.z );
	}

	void Hit( IObject* pObj, const Ray& r, RayIntersection& ri )
	{
		ri.geometric.bHit = false;
		ri.geometric.range = RISE_INFINITY;
		ri.geometric.range2 = RISE_INFINITY;
		ri.geometric.ray = r;
		pObj->IntersectRay( ri, RISE_INFINITY, true, true, true );
	}

	// A single big triangle whose vertices all sit ~1e4 units out, i.e. a
	// bbox whose corner L1 is ~3e4 and whose RayTriangleIntersection gate is
	// therefore ~3e-8 -- four orders of magnitude above a unit box's.  Built
	// (DoneIndexedTriangles) so it is a genuine, legitimately-inflated
	// sibling, not the unbuilt/empty case Test 1 and Test 4 cover.
	TriangleMeshGeometryIndexed* MakeFarTriangle()
	{
		TriangleMeshGeometryIndexed* g = new TriangleMeshGeometryIndexed( true, true );
		g->BeginIndexedTriangles();
		g->AddVertex( Point3( 1.0e4, 1.0e4, 1.0e4 ) );
		g->AddVertex( Point3( 1.0e4 + 40.0, 1.0e4, 1.0e4 ) );
		g->AddVertex( Point3( 1.0e4, 1.0e4 + 40.0, 1.0e4 ) );
		g->AddTexCoord( Point2( 0, 0 ) );
		g->AddTexCoord( Point2( 1, 0 ) );
		g->AddTexCoord( Point2( 0, 1 ) );
		IndexedTriangle t;
		t.iVertices[0] = 0; t.iVertices[1] = 1; t.iVertices[2] = 2;
		t.iCoords[0]   = 0; t.iCoords[1]   = 1; t.iCoords[2]   = 2;
		g->AddIndexedTriangle( t );
		g->DoneIndexedTriangles();
		return g;
	}
}

//
// Test 1 (P1-3, geometry layer): a collection with NOTHING in it must
// still report a finite, generic floor.
//
// `BoundingBox()`'s default corners are +-RISE_INFINITY == DBL_MAX, so
// the corner-L1 sum the helper used to compute OVERFLOWED to +inf.  The
// two in-tree classes that can hand it that box are an indexed triangle
// mesh whose BVH was never built and a bilinear patch tree with no
// patches.  Both must now fall back to `NEARZERO * (1 + |o|_1)` -- the
// same value `IGeometry::SelfHitRootFloor` defaults to, which is exactly
// right when there are no primitives to gate.
//
void TestEmptyCollectionsReportFiniteFloor()
{
	std::cout << "Empty / unbuilt collections report a FINITE self-hit floor (P1-3)..." << std::endl;

	const Point3  o( 0.1, 0.05, -1.0 );
	const Vector3 d( 0.0, 0.0, 1.0 );
	const Vector3 n( 0.0, 0.0, -1.0 );
	const Scalar  generic = NEARZERO * ( 1.0 + std::fabs(o.x) + std::fabs(o.y) + std::fabs(o.z) );

	{
		// Never given BeginIndexedTriangles/DoneIndexedTriangles: no BVH.
		TriangleMeshGeometryIndexed* g = new TriangleMeshGeometryIndexed( true, true );
		const Scalar f = g->SelfHitRootFloor( o, d, n );
		Check( std::isfinite( f ), "Test1: unbuilt indexed mesh floor is FINITE (was +inf)" );
		Check( f > 0.0, "Test1: unbuilt indexed mesh floor is positive" );
		Check( Close( f, generic, generic * 1e-9 ), "Test1: unbuilt indexed mesh falls back to the generic floor" );
		safe_release( g );
	}

	{
		// No patches added, so its tree bounds nothing.
		BilinearPatchGeometry* g = new BilinearPatchGeometry( 4, 8, false );
		const Scalar f = g->SelfHitRootFloor( o, d, n );
		Check( std::isfinite( f ), "Test1: empty bilinear patch floor is FINITE (was +inf)" );
		Check( f > 0.0, "Test1: empty bilinear patch floor is positive" );
		Check( Close( f, generic, generic * 1e-9 ), "Test1: empty bilinear patch falls back to the generic floor" );
		safe_release( g );
	}
}

//
// Test 2 (P2-1): a composite must charge only the operand that OWNS the
// face, not whichever operand happens to have the widest gate.
//
// The reviewer's configuration: CSG_UNION of a unit box with a
// triangle-mesh lobe 1e4 units away.  Queried on the BOX's own -Z face,
// the composite used to report the MESH's 3.008e-8 (7494x the box's own
// 4.014e-12), which the probe then doubles and pads into a ~1.3e-7
// same-face acceptance window -- ~250x wider than the 5e-10 decoy gap
// CsgSurfacePayloadTest Test 15 bounds it against.
//
void TestSiblingDoesNotInflateCompositeFloor()
{
	std::cout << "CSG composite floor charges only the OWNING operand (P2-1)..." << std::endl;

	BoxGeometry* gBox = new BoxGeometry( 2.0, 2.0, 2.0 );   // half-extent 1, spans [-1,1]^3
	TriangleMeshGeometryIndexed* gFar = MakeFarTriangle();

	Object* box = new Object( gBox );
	Object* far = new Object( gFar );
	safe_release( gBox );
	safe_release( gFar );

	box->SetPosition( Point3( 0, 0, 0 ) );
	box->FinalizeTransformations();
	far->SetPosition( Point3( 0, 0, 0 ) );
	far->FinalizeTransformations();

	CSGObject* csg = new CSGObject( CSG_UNION );
	const bool assigned = csg->AssignObjects( box, far );
	Check( assigned, "Test2: composite takes box(A)/far-mesh(B) operands" );
	csg->FinalizeTransformations();

	// A point on the box's -Z face, and the probe geometry the exit-face
	// probe would hand the composite there: the ORIGINAL ray direction
	// (+Z, travelling out through that face) and the face's outward normal.
	const Point3  ptFace( 0.1, 0.05, -1.0 );
	const Vector3 dirIn( 0.0, 0.0, 1.0 );
	const Vector3 nOut( 0.0, 0.0, -1.0 );

	const Scalar floorBox    = box->SelfHitRootFloor( ptFace, dirIn, nOut );
	const Scalar floorFar    = far->SelfHitRootFloor( ptFace, dirIn, nOut );
	const Scalar floorNested = csg->SelfHitRootFloor( ptFace, dirIn, nOut );

	// Sanity: the two operands' own gates really are orders apart, so this
	// test is discriminating.
	Check( floorBox  < 1e-10, "Test2: (sanity) the box's own gate is ulp-scale" );
	Check( floorFar  > 1e-8,  "Test2: (sanity) the far mesh's own gate is ~3e-8, four orders wider" );
	Check( floorFar  > floorBox * 1000.0, "Test2: (sanity) the sibling gate is >1000x the box's" );

	// MONEY: the composite reports the BOX's floor -- the operand that owns
	// this face -- not the unrelated sibling's.
	Check( Close( floorNested, floorBox, floorBox * 1e-9 ),
		"Test2: MONEY ASSERTION -- composite floor equals the OWNING box's own floor" );
	Check( floorNested < 1e-10,
		"Test2: MONEY ASSERTION -- composite floor is NOT the far sibling's ~3e-8" );

	safe_release( csg );
	safe_release( box );
	safe_release( far );
}

//
// Test 3 (P2-1, end to end): tightening the floor must not break the
// probe that consumes it.
//
// Same shape as CsgSurfacePayloadTest Tests 4/11/14/15 -- a
// CSG_SUBTRACTION whose carve operand is a nested CSG_UNION, driven
// through the exit-designated branch -- with the far-mesh sibling from
// Test 2 riding along in the union.  The composite's recovered payload
// must be the REAL exit face's, oracled by an independent direct probe
// of the carving lobe from outside.
//
void TestOwnershipFilterKeepsProbeWorking()
{
	std::cout << "CSG_SUBTRACTION exit probe still recovers the real face with a far sibling present (P2-1)..." << std::endl;

	BoxGeometry* gA = new BoxGeometry( 6.0, 6.0, 6.0 );          // half-extent 3, spans z in [-3,3]
	BoxGeometry* gRealLobe = new BoxGeometry( 4.0, 4.0, 4.0 );   // half-extent 2
	TriangleMeshGeometryIndexed* gFar = MakeFarTriangle();

	Object* oA = new Object( gA );
	Object* realLobe = new Object( gRealLobe );
	Object* far = new Object( gFar );
	safe_release( gA );
	safe_release( gRealLobe );
	safe_release( gFar );

	oA->SetPosition( Point3( 0, 0, 0 ) );
	oA->FinalizeTransformations();
	realLobe->SetPosition( Point3( 0, 0, -4 ) );    // spans z in [-6,-2]: overlaps A's near wall
	realLobe->FinalizeTransformations();
	far->SetPosition( Point3( 0, 0, 0 ) );
	far->FinalizeTransformations();

	CSGObject* nestedB = new CSGObject( CSG_UNION );
	Check( nestedB->AssignObjects( realLobe, far ), "Test3: nestedB takes realLobe/far-mesh operands" );
	nestedB->FinalizeTransformations();

	CSGObject* outerCsg = new CSGObject( CSG_SUBTRACTION );
	Check( outerCsg->AssignObjects( oA, nestedB ), "Test3: outerCsg takes oA/nestedB operands" );
	outerCsg->FinalizeTransformations();

	Ray r( Point3( 0.1, 0.05, -10 ), Vector3( 0, 0, 1 ) );
	RayIntersection ri( r, nullRasterizerState );
	Hit( outerCsg, r, ri );
	Check( ri.geometric.bHit, "Test3: (control) ray hits the composite" );

	// Ground truth: the carve lobe's exit face at z = -2, probed directly
	// from outside -- independent of any production probe code.
	Ray probeRefReal( Point3( 0.1, 0.05, -1.9 ), Vector3( 0, 0, -1 ) );
	RayIntersection refRealExit( probeRefReal, nullRasterizerState );
	Hit( realLobe, probeRefReal, refRealExit );
	Check( refRealExit.geometric.bHit, "Test3: (control) direct probe hits realLobe's exit face" );

	// The WRONG answer a missed probe falls back to: the carve lobe's ENTRY
	// face (z = -6), which is what `dst` already carries.
	Ray probeEntry( Point3( 0.1, 0.05, -10 ), Vector3( 0, 0, 1 ) );
	RayIntersection refEntry( probeEntry, nullRasterizerState );
	Hit( realLobe, probeEntry, refEntry );
	Check( refEntry.geometric.bHit, "Test3: (control) direct probe hits realLobe's entry face" );
	Check( !Point2Close( refRealExit.geometric.ptCoord, refEntry.geometric.ptCoord ),
		"Test3: (sanity) exit/entry ptCoord are distinct" );

	Check( Point2Close( ri.geometric.ptCoord, refRealExit.geometric.ptCoord ),
		"Test3: MONEY ASSERTION -- composite ptCoord matches the REAL exit face" );
	Check( PointClose( ri.geometric.ptObjIntersec, refRealExit.geometric.ptObjIntersec ),
		"Test3: MONEY ASSERTION -- composite ptObjIntersec matches the REAL exit face" );

	safe_release( outerCsg );
	safe_release( oA );
	safe_release( nestedB );
	safe_release( realLobe );
	safe_release( far );
}

//
// Test 4 (P1-3, end to end): an EMPTY mesh riding in a nested CSG operand
// must not poison the exit-face probe.
//
// Before the fix the empty mesh's +inf floor propagated up through
// `CSGObject::SelfHitRootFloor` (max over both operands, unconditionally),
// made the probe's margin infinite, and gave the probe origin a NaN
// component -- after which the `range > maxAcceptRange` rejection could
// never fire, because every comparison against NaN is false.  Two layers
// now stop it: the bbox helper never returns a non-finite floor (Test 1),
// and the probe refuses a non-finite floor or margin outright.
//
// Assertions are BOTH halves of "not garbage": every composite field is
// finite, AND the payload is the real exit face's rather than something
// a NaN-poisoned probe happened to accept.
//
void TestEmptyMeshOperandDoesNotPoisonProbe()
{
	std::cout << "CSG_SUBTRACTION exit probe survives an EMPTY mesh operand in the carve union (P1-3)..." << std::endl;

	BoxGeometry* gA = new BoxGeometry( 6.0, 6.0, 6.0 );
	BoxGeometry* gRealLobe = new BoxGeometry( 4.0, 4.0, 4.0 );
	// Never built: no BVH, default (+-DBL_MAX) bounding box.
	TriangleMeshGeometryIndexed* gEmpty = new TriangleMeshGeometryIndexed( true, true );

	Object* oA = new Object( gA );
	Object* realLobe = new Object( gRealLobe );
	Object* emptyMesh = new Object( gEmpty );
	safe_release( gA );
	safe_release( gRealLobe );
	safe_release( gEmpty );

	oA->SetPosition( Point3( 0, 0, 0 ) );
	oA->FinalizeTransformations();
	realLobe->SetPosition( Point3( 0, 0, -4 ) );
	realLobe->FinalizeTransformations();
	emptyMesh->SetPosition( Point3( 0, 0, 0 ) );
	emptyMesh->FinalizeTransformations();

	CSGObject* nestedB = new CSGObject( CSG_UNION );
	Check( nestedB->AssignObjects( realLobe, emptyMesh ), "Test4: nestedB takes realLobe/empty-mesh operands" );
	nestedB->FinalizeTransformations();

	// The composite floor itself must already be finite.
	const Scalar nestedFloor = nestedB->SelfHitRootFloor(
		Point3( 0.1, 0.05, -2.0 ), Vector3( 0, 0, 1 ), Vector3( 0, 0, -1 ) );
	Check( std::isfinite( nestedFloor ), "Test4: nested composite floor is FINITE with an empty mesh operand" );
	Check( nestedFloor > 0.0 && nestedFloor < 1e-10, "Test4: nested composite floor is the real lobe's ulp-scale gate" );

	CSGObject* outerCsg = new CSGObject( CSG_SUBTRACTION );
	Check( outerCsg->AssignObjects( oA, nestedB ), "Test4: outerCsg takes oA/nestedB operands" );
	outerCsg->FinalizeTransformations();

	Ray r( Point3( 0.1, 0.05, -10 ), Vector3( 0, 0, 1 ) );
	RayIntersection ri( r, nullRasterizerState );
	Hit( outerCsg, r, ri );
	Check( ri.geometric.bHit, "Test4: (control) ray hits the composite" );

	// No NaN / inf anywhere in the reported hit.
	Check( std::isfinite( ri.geometric.range ), "Test4: composite range is finite (no NaN)" );
	Check( std::isfinite( ri.geometric.range2 ), "Test4: composite range2 is finite (no NaN)" );
	Check( PointFinite( ri.geometric.ptIntersection ), "Test4: composite ptIntersection is finite (no NaN)" );
	Check( PointFinite( ri.geometric.ptObjIntersec ), "Test4: composite ptObjIntersec is finite (no NaN)" );
	Check( std::isfinite( ri.geometric.ptCoord.x ) && std::isfinite( ri.geometric.ptCoord.y ),
		"Test4: composite ptCoord is finite (no NaN)" );
	Check( VectorFinite( ri.geometric.vNormal ), "Test4: composite vNormal is finite (no NaN)" );
	Check( VectorFinite( ri.geometric.vGeomNormal ), "Test4: composite vGeomNormal is finite (no NaN)" );

	// And the payload is the REAL exit face's, not whatever a poisoned
	// probe accepted.
	Ray probeRefReal( Point3( 0.1, 0.05, -1.9 ), Vector3( 0, 0, -1 ) );
	RayIntersection refRealExit( probeRefReal, nullRasterizerState );
	Hit( realLobe, probeRefReal, refRealExit );
	Check( refRealExit.geometric.bHit, "Test4: (control) direct probe hits realLobe's exit face" );
	Check( Point2Close( ri.geometric.ptCoord, refRealExit.geometric.ptCoord ),
		"Test4: MONEY ASSERTION -- composite ptCoord matches the REAL exit face" );
	Check( PointClose( ri.geometric.ptObjIntersec, refRealExit.geometric.ptObjIntersec ),
		"Test4: MONEY ASSERTION -- composite ptObjIntersec matches the REAL exit face" );

	safe_release( outerCsg );
	safe_release( oA );
	safe_release( nestedB );
	safe_release( realLobe );
	safe_release( emptyMesh );
}

int main()
{
	TestEmptyCollectionsReportFiniteFloor();
	TestSiblingDoesNotInflateCompositeFloor();
	TestOwnershipFilterKeepsProbeWorking();
	TestEmptyMeshOperandDoesNotPoisonProbe();

	std::printf( "\nCsgProbeFloorTest: %d passed, %d failed\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
