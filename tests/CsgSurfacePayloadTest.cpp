//////////////////////////////////////////////////////////////////////
//
//  CsgSurfacePayloadTest.cpp
//
//  Regression coverage for a CSG hit-payload defect found in review:
//  CSGObject::IntersectRay starts most branches with an efficient
//  whole-record `ri = riObjA` (or `riObjB`) copy and then overrides
//  only the boundary-identifying fields (range/range2, vNormal[2],
//  vGeomNormal[2]) per the CSG algebra.  In CSG_INTERSECTION's
//  "outside both" branches, the algebra can attribute the reported
//  ENTRY boundary to the OTHER operand from the one `ri` started as
//  -- and prior to the fix, only vNormal / vGeomNormal / range were
//  re-pointed at that operand, leaving the AUXILIARY per-surface
//  payload (ptCoord, ptObjIntersec, and -- on geometries that
//  populate them -- derivatives / txFootprint / vColor / vTangent /
//  bShadingTangentFromGeometry) stuck on the wrong operand's values.
//  The result: a hit whose normal says "operand B" but whose UV /
//  object-space point says "operand A".
//
//  These tests exercise:
//    1. CSG_INTERSECTION, "A enters first, B enters while inside A"
//       -- the composite entry is wholly B's surface.
//    2. CSG_INTERSECTION, "B enters first, A enters while inside B"
//       -- the composite entry is wholly A's surface (the mirror
//       branch, a separate code path in CSGObject.cpp).
//    3. CSG_SUBTRACTION, the "inside A, outside B" branch -- the
//       visible boundary is the SUBTRACTED operand's (B's) surface.
//       This branch was already self-consistent (no cross-operand
//       bug), but is exactly the scenario item (a) of the task calls
//       for, so it is covered as a regression guard.
//    4. CSG_SUBTRACTION, the EXIT-designated boundary branch (the
//       composite's reported entry is operand B's EXIT face, not its
//       entry face) -- confirms the payload honestly stays B's
//       ENTRY-hit data (the only data any RISE geometry computes) and
//       is never contaminated by operand A.  Substitutes for the
//       "clear wire-edge info on an exit-designated boundary" ask in
//       the original defect report: RISE has no wire-edge /
//       bHasWireEdgeInfo field at all (grepped the whole tree, none
//       found), so there is nothing to clear -- this test instead
//       proves the exit-designated case never leaks the OTHER
//       operand's payload, which is the part of the invariant that
//       does apply here.
//
//  Note on scope: triangle meshes do not populate range2 / vNormal2
//  on `bComputeExitInfo` (TriangleMeshGeometryIndexed::IntersectRay
//  ignores that parameter entirely), so meshes cannot act as CSG
//  operands with correct entry/exit semantics in this codebase --
//  every CSG regression test here uses analytical primitives (box,
//  sphere), which is also what every existing CSG test in this repo
//  (CSGObjectIdentityTest.cpp, GeometricNormalPlumbingTest.cpp) does.
//  Consequently `derivatives.valid`, `bHasVertexColor`, `bHasTangent`,
//  and `bShadingTangentFromGeometry` are false for every hit exercised
//  here (box/sphere never set them) -- these are asserted to stay
//  false and consistent with a direct standalone intersection of the
//  owning operand, rather than exercised with true/non-default values.
//
//////////////////////////////////////////////////////////////////////

#include <cassert>
#include <cmath>
#include <iostream>

#include "../src/Library/Geometry/BoxGeometry.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Intersection/RayIntersection.h"
#include "../src/Library/Objects/CSGObject.h"
#include "../src/Library/Objects/Object.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	const Scalar kEps = 1e-6;

	bool Close( Scalar a, Scalar b, Scalar eps = kEps )
	{
		return std::fabs( a - b ) < eps;
	}

	bool VecClose( const Vector3& a, const Vector3& b, Scalar eps = kEps )
	{
		return Close( a.x, b.x, eps ) && Close( a.y, b.y, eps ) && Close( a.z, b.z, eps );
	}

	bool PointClose( const Point3& a, const Point3& b, Scalar eps = kEps )
	{
		return Close( a.x, b.x, eps ) && Close( a.y, b.y, eps ) && Close( a.z, b.z, eps );
	}

	bool Point2Close( const Point2& a, const Point2& b, Scalar eps = kEps )
	{
		return Close( a.x, b.x, eps ) && Close( a.y, b.y, eps );
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

//
// Test 1: CSG_INTERSECTION, "A enters first, B enters while inside A".
// Outer box (A) is entered first; a small inner box (B) is nested
// well inside A's span, so the composite's ENTRY boundary is wholly
// B's surface.  `ri` starts life as a whole copy of A's record, so
// prior to the fix, ptCoord / ptObjIntersec stayed A's while vNormal
// switched to B's -- a mixed-surface hit.
//
void TestIntersection_AEntersFirst_EntryIsWhollyB()
{
	std::cout << "CSG_INTERSECTION: A enters first, entry payload wholly B's..." << std::endl;

	BoxGeometry* gOuter = new BoxGeometry( 4.0, 4.0, 4.0 );   // half-extent 2
	BoxGeometry* gInner = new BoxGeometry( 0.6, 0.6, 0.6 );   // half-extent 0.3
	Object* outer = new Object( gOuter );
	Object* inner = new Object( gInner );
	safe_release( gOuter );
	safe_release( gInner );

	outer->SetPosition( Point3( 0, 0, 0 ) );
	inner->SetPosition( Point3( 0, 0, -0.5 ) );   // nested well inside outer's [-2,2] span
	outer->FinalizeTransformations();
	inner->FinalizeTransformations();

	// Off-center ray so box UV isn't the degenerate (0.5, 0.5) center point.
	Ray r( Point3( 0.1, 0.05, -10 ), Vector3( 0, 0, 1 ) );

	CSGObject* csg = new CSGObject( CSG_INTERSECTION );
	assert( csg->AssignObjects( outer, inner ) );   // pObjectA = outer, pObjectB = inner
	csg->FinalizeTransformations();

	RayIntersection ri( r, nullRasterizerState );
	Hit( csg, r, ri );
	assert( ri.geometric.bHit );

	// Reference: intersect the SAME inner-box object directly, standalone.
	RayIntersection refInner( r, nullRasterizerState );
	Hit( inner, r, refInner );
	assert( refInner.geometric.bHit );

	// Reference: intersect the outer box too, to build a negative check.
	RayIntersection refOuter( r, nullRasterizerState );
	Hit( outer, r, refOuter );
	assert( refOuter.geometric.bHit );

	// Sanity: the two boxes really do produce distinct UV / object-space
	// points at this ray (otherwise the test wouldn't be discriminating).
	assert( !Point2Close( refInner.geometric.ptCoord, refOuter.geometric.ptCoord ) );
	assert( !PointClose( refInner.geometric.ptObjIntersec, refOuter.geometric.ptObjIntersec ) );

	// The composite entry must be wholly B's (inner's) surface.
	assert( VecClose( ri.geometric.vNormal, refInner.geometric.vNormal ) );
	assert( VecClose( ri.geometric.vGeomNormal, refInner.geometric.vGeomNormal ) );
	assert( Point2Close( ri.geometric.ptCoord, refInner.geometric.ptCoord ) );
	assert( PointClose( ri.geometric.ptObjIntersec, refInner.geometric.ptObjIntersec ) );
	assert( ri.geometric.derivatives.valid == refInner.geometric.derivatives.valid );
	assert( ri.geometric.bHasVertexColor == refInner.geometric.bHasVertexColor );
	assert( ri.geometric.bHasTangent == refInner.geometric.bHasTangent );

	// Negative check: NOT A's (outer's) UV / object-space point -- this is
	// exactly what the bug produced (B's normal paired with A's payload).
	assert( !Point2Close( ri.geometric.ptCoord, refOuter.geometric.ptCoord ) );
	assert( !PointClose( ri.geometric.ptObjIntersec, refOuter.geometric.ptObjIntersec ) );

	safe_release( csg );
	safe_release( outer );
	safe_release( inner );
	std::cout << "  Passed." << std::endl;
}

//
// Test 2: CSG_INTERSECTION, "B enters first, A enters while inside B"
// -- the mirror branch (a distinct code path in CSGObject.cpp).  Same
// physical geometry as Test 1, but the operand roles are swapped when
// assigned to the CSG object, so the SAME nested-box scenario now
// enters first as pObjectB and second as pObjectA, exercising the
// second cross-operand fix site.
//
void TestIntersection_BEntersFirst_EntryIsWhollyA()
{
	std::cout << "CSG_INTERSECTION: B enters first, entry payload wholly A's..." << std::endl;

	BoxGeometry* gOuter = new BoxGeometry( 4.0, 4.0, 4.0 );
	BoxGeometry* gInner = new BoxGeometry( 0.6, 0.6, 0.6 );
	Object* outer = new Object( gOuter );
	Object* inner = new Object( gInner );
	safe_release( gOuter );
	safe_release( gInner );

	outer->SetPosition( Point3( 0, 0, 0 ) );
	inner->SetPosition( Point3( 0, 0, -0.5 ) );
	outer->FinalizeTransformations();
	inner->FinalizeTransformations();

	Ray r( Point3( 0.1, 0.05, -10 ), Vector3( 0, 0, 1 ) );

	CSGObject* csg = new CSGObject( CSG_INTERSECTION );
	// Roles swapped vs Test 1: pObjectA = inner (enters second physically,
	// but is now the FIRST-listed operand), pObjectB = outer.  Outer still
	// enters first physically (its near face is farther out), so this
	// exercises the "B enters first" branch instead of "A enters first".
	assert( csg->AssignObjects( inner, outer ) );
	csg->FinalizeTransformations();

	RayIntersection ri( r, nullRasterizerState );
	Hit( csg, r, ri );
	assert( ri.geometric.bHit );

	RayIntersection refInner( r, nullRasterizerState );
	Hit( inner, r, refInner );
	assert( refInner.geometric.bHit );

	RayIntersection refOuter( r, nullRasterizerState );
	Hit( outer, r, refOuter );
	assert( refOuter.geometric.bHit );

	// The composite entry must still be wholly the INNER box's surface
	// (it's the operand that enters second / is nested), regardless of
	// which CSG operand slot (A or B) it was assigned to.
	assert( VecClose( ri.geometric.vNormal, refInner.geometric.vNormal ) );
	assert( VecClose( ri.geometric.vGeomNormal, refInner.geometric.vGeomNormal ) );
	assert( Point2Close( ri.geometric.ptCoord, refInner.geometric.ptCoord ) );
	assert( PointClose( ri.geometric.ptObjIntersec, refInner.geometric.ptObjIntersec ) );

	// Negative check: NOT the outer box's payload.
	assert( !Point2Close( ri.geometric.ptCoord, refOuter.geometric.ptCoord ) );
	assert( !PointClose( ri.geometric.ptObjIntersec, refOuter.geometric.ptObjIntersec ) );

	safe_release( csg );
	safe_release( outer );
	safe_release( inner );
	std::cout << "  Passed." << std::endl;
}

//
// Test 3: CSG_SUBTRACTION, item (a) of the defect report -- "a
// CSG_SUBTRACTION of two [primitives] where the visible boundary is
// the SUBTRACTED operand's surface".  Ray origin starts INSIDE A
// (sphere) but outside B (a smaller sphere embedded in A), so the
// composite boundary is B's surface (flipped normal, carving a void
// out of A).  This branch was already self-consistent in the
// pre-fix code (no cross-operand override existed here), so this is
// primarily a regression guard confirming the fix didn't disturb it.
//
void TestSubtraction_VisibleBoundaryIsSubtractedOperand()
{
	std::cout << "CSG_SUBTRACTION: visible boundary wholly the subtracted operand's..." << std::endl;

	SphereGeometry* gA = new SphereGeometry( 2.0 );
	SphereGeometry* gB = new SphereGeometry( 0.5 );
	Object* oA = new Object( gA );
	Object* oB = new Object( gB );
	safe_release( gA );
	safe_release( gB );

	oA->SetPosition( Point3( 0, 0, 0 ) );
	oB->SetPosition( Point3( 0, 0, 0.8 ) );   // fully embedded in A (0.8+0.5 < 2.0)
	oA->FinalizeTransformations();
	oB->FinalizeTransformations();

	// Ray origin INSIDE A (dist 1.5 < 2), OUTSIDE B (dist 2.3 > 0.5).
	Ray r( Point3( 0, 0, -1.5 ), Vector3( 0, 0, 1 ) );

	CSGObject* csg = new CSGObject( CSG_SUBTRACTION );
	assert( csg->AssignObjects( oA, oB ) );
	csg->FinalizeTransformations();

	RayIntersection ri( r, nullRasterizerState );
	Hit( csg, r, ri );
	assert( ri.geometric.bHit );

	// Reference: standalone B at the same ray (unflipped).
	RayIntersection refB( r, nullRasterizerState );
	Hit( oB, r, refB );
	assert( refB.geometric.bHit );

	RayIntersection refA( r, nullRasterizerState );
	Hit( oA, r, refA );
	assert( refA.geometric.bHit );

	// Sanity: A and B produce distinct UV / object-space points here.
	assert( !PointClose( refA.geometric.ptObjIntersec, refB.geometric.ptObjIntersec ) );

	// The composite boundary is B's surface, normal flipped (we're
	// leaving A's solid into the void B carved out of it).
	assert( VecClose( ri.geometric.vNormal, Vector3( -refB.geometric.vNormal.x, -refB.geometric.vNormal.y, -refB.geometric.vNormal.z ) ) );
	assert( VecClose( ri.geometric.vGeomNormal, Vector3( -refB.geometric.vGeomNormal.x, -refB.geometric.vGeomNormal.y, -refB.geometric.vGeomNormal.z ) ) );
	// UV and object-space point are NOT sign-flipped -- they're B's own.
	assert( Point2Close( ri.geometric.ptCoord, refB.geometric.ptCoord ) );
	assert( PointClose( ri.geometric.ptObjIntersec, refB.geometric.ptObjIntersec ) );

	// Negative check: not A's payload.
	assert( !PointClose( ri.geometric.ptObjIntersec, refA.geometric.ptObjIntersec ) );

	safe_release( csg );
	safe_release( oA );
	safe_release( oB );
	std::cout << "  Passed." << std::endl;
}

//
// Test 4: CSG_SUBTRACTION, the EXIT-designated boundary branch --
// the composite's reported ENTRY is operand B's EXIT face (B carves
// into the near part of A; the visible near wall of A-minus-B is
// where B's far/exit face sits).  No geometry in RISE computes a
// second UV / object-space payload for an exit hit, so the payload
// honestly stays B's ENTRY-hit data.  This test's job is to prove
// that payload is B's (not A's) -- i.e. the CSG algebra never
// silently swaps in the WRONG OPERAND's data for this exit-designated
// case, even though it is (by documented, unavoidable, limitation)
// the wrong FACE of the right operand.
//
void TestSubtraction_ExitDesignatedBoundary_NoWrongOperandLeak()
{
	std::cout << "CSG_SUBTRACTION: exit-designated boundary stays B's (never A's)..." << std::endl;

	BoxGeometry* gA = new BoxGeometry( 6.0, 6.0, 6.0 );   // half-extent 3
	BoxGeometry* gB = new BoxGeometry( 4.0, 4.0, 4.0 );   // half-extent 2
	Object* oA = new Object( gA );
	Object* oB = new Object( gB );
	safe_release( gA );
	safe_release( gB );

	oA->SetPosition( Point3( 0, 0, 0 ) );      // spans z in [-3, 3]
	oB->SetPosition( Point3( 0, 0, -4 ) );     // spans z in [-6, -2]: overlaps A's near wall
	oA->FinalizeTransformations();
	oB->FinalizeTransformations();

	Ray r( Point3( 0.1, 0.05, -10 ), Vector3( 0, 0, 1 ) );

	CSGObject* csg = new CSGObject( CSG_SUBTRACTION );
	assert( csg->AssignObjects( oA, oB ) );
	csg->FinalizeTransformations();

	RayIntersection ri( r, nullRasterizerState );
	Hit( csg, r, ri );
	assert( ri.geometric.bHit );

	RayIntersection refA( r, nullRasterizerState );
	Hit( oA, r, refA );
	assert( refA.geometric.bHit );

	RayIntersection refB( r, nullRasterizerState );
	Hit( oB, r, refB );
	assert( refB.geometric.bHit );

	// Sanity: confirm we actually hit the exit-designated branch, i.e.
	// the composite range lands on B's EXIT (range2), not B's entry
	// (range) nor A's entry.
	assert( Close( ri.geometric.range, refB.geometric.range2, 1e-3 ) );
	assert( !Close( ri.geometric.range, refB.geometric.range, 1e-3 ) );
	assert( !Close( ri.geometric.range, refA.geometric.range, 1e-3 ) );

	// Normal is B's EXIT normal, flipped.
	assert( VecClose( ri.geometric.vNormal, Vector3( -refB.geometric.vNormal2.x, -refB.geometric.vNormal2.y, -refB.geometric.vNormal2.z ) ) );
	assert( VecClose( ri.geometric.vGeomNormal, Vector3( -refB.geometric.vGeomNormal2.x, -refB.geometric.vGeomNormal2.y, -refB.geometric.vGeomNormal2.z ) ) );

	// Payload (ptCoord / ptObjIntersec) is honestly B's ENTRY-face data
	// (the only data any geometry computes) -- NOT A's, at either A's
	// entry or any other A-owned value.
	assert( Point2Close( ri.geometric.ptCoord, refB.geometric.ptCoord ) );
	assert( PointClose( ri.geometric.ptObjIntersec, refB.geometric.ptObjIntersec ) );
	assert( !Point2Close( ri.geometric.ptCoord, refA.geometric.ptCoord ) );
	assert( !PointClose( ri.geometric.ptObjIntersec, refA.geometric.ptObjIntersec ) );

	safe_release( csg );
	safe_release( oA );
	safe_release( oB );
	std::cout << "  Passed." << std::endl;
}

int main()
{
	TestIntersection_AEntersFirst_EntryIsWhollyB();
	TestIntersection_BEntersFirst_EntryIsWhollyA();
	TestSubtraction_VisibleBoundaryIsSubtractedOperand();
	TestSubtraction_ExitDesignatedBoundary_NoWrongOperandLeak();
	std::cout << "\nAll CsgSurfacePayloadTest cases passed." << std::endl;
	return 0;
}
