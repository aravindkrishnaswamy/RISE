//////////////////////////////////////////////////////////////////////
//
//  CsgSurfacePayloadTest.cpp
//
//  Regression coverage for a family of CSG defects found in review
//  (P1-a, P1-b, P1-c, P2-d, P2-e -- see CSGObject.cpp for the full
//  per-defect commentary at each fix site):
//
//    P1-a (bindings): the two CSG_INTERSECTION "outside both"
//      cross-operand branches re-adopt the GEOMETRIC payload
//      (ptCoord / derivatives / ...) from the operand that owns the
//      reported boundary via AdoptCsgSurfacePayload, but left the
//      RayIntersection-level BINDING pointers (pMaterial / pModifier /
//      pShader / pRadianceMap) on the FIRST-copied operand.  Fixed via
//      a sibling helper, AdoptCsgSurfaceBindings.
//    P1-b (subtraction branch order): the SUBTRACTION "outside both"
//      block tested the two overlap conditions BEFORE disjointness,
//      so a B interval that never overlaps A could still satisfy one
//      of them by accident -- a phantom surface either extended past
//      A's real exit (B wholly after A) or planted at B's back face in
//      empty space (B wholly before A).  Fixed by testing disjointness
//      first.
//    P1-c (shadow ranges): IntersectRay_IntersectionOnly called both
//      children with bComputeExitInfo=FALSE, so range2 stayed in
//      CHILD-LOCAL raw parametric units instead of the CSG-local frame
//      the switch statement compares it in -- wrong by exactly the
//      child's own scale factor.  Fixed by passing bComputeExitInfo=
//      true, matching the main IntersectRay.
//    P2-d (frame promotion): CSGObject::IntersectRay's world-promotion
//      tail transformed normals / points but never promoted surface
//      derivatives (dpdu/dpdv/dndu/dndv) or the tangent frame
//      (vTangent/bitangentSign) from the CSG's local frame to world --
//      copied child-frame values shipped un-promoted.  Fixed by
//      mirroring Object::IntersectRay's own promotion block.
//    P2-e (exit-face payload): branches that report an operand's EXIT
//      face as the composite's boundary (SUBTRACTION's carve wall, the
//      two UNION inside-operand-exit branches) previously left that
//      operand's ENTRY-hit payload in place -- "honestly the wrong
//      face of the right operand" -- because no RISE geometry computes
//      a second payload set for an exit hit.  Fixed with a reverse
//      probe (AdoptCsgExitFacePayloadViaProbe): a short ray fired from
//      just past the exit point, direction reversed, re-intersects the
//      SAME operand alone and lands on the exact same face from
//      outside, with a fully-stamped entry payload for THAT face.
//
//  Test map:
//    1. CSG_INTERSECTION, "A enters first, B enters while inside A"
//       -- the composite entry is wholly B's surface (P1-a geometric
//       payload half, regression guard).
//    2. CSG_INTERSECTION, "B enters first, A enters while inside B"
//       -- the mirror branch (P1-a geometric payload half, regression
//       guard).
//    3. CSG_SUBTRACTION, the "inside A, outside B" branch -- the
//       visible boundary is the SUBTRACTED operand's (B's) surface.
//       Already self-consistent pre-fix; regression guard.
//    4. CSG_SUBTRACTION, the EXIT-designated boundary branch -- the
//       composite's payload must now be the REAL exit-face data,
//       recovered via the P2-e reverse probe (verified against an
//       independent direct probe of the same face from outside).
//    5. CSG_INTERSECTION cross-operand branches: ri.pMaterial follows
//       the OWNING operand, not the operand `ri` started as a
//       whole-record copy of (P1-a bindings half).
//    6. CSG_SUBTRACTION, B disjoint from A on BOTH sides (wholly
//       before, wholly after) -- composite must equal A alone (P1-b).
//    7. IntersectRay_IntersectionOnly under a scaled CSG operand --
//       must agree with the always-correct IntersectRay verdict, not
//       leak a shadow due to a child-local/CSG-local unit mismatch
//       (P1-c).
//    8. CSG_UNION under a CSG-level rotation: a triangle-mesh operand's
//       dpdu/dpdv/dndu/dndv must match a standalone Object wrapping
//       the same mesh with the identical rotation applied directly
//       (P2-d).
//
//  Note on scope: triangle meshes do not populate range2 / vNormal2
//  on `bComputeExitInfo` (TriangleMeshGeometryIndexed::IntersectRay
//  ignores that parameter entirely), so meshes cannot act as CSG
//  operands with correct entry/exit semantics in this codebase --
//  every CSG regression test here that exercises entry/exit algebra
//  uses analytical primitives (box, sphere), matching every existing
//  CSG test in this repo (CSGObjectIdentityTest.cpp,
//  GeometricNormalPlumbingTest.cpp).  Test 8 (P2-d) is the one
//  exception -- it deliberately places its mesh operand where the
//  OTHER operand is never hit, so no entry/exit algebra is exercised,
//  only the simple `ri = riObjA` passthrough plus the CSG's own tail
//  promotion.  Consequently `derivatives.valid`, `bHasVertexColor`,
//  `bHasTangent`, and `bShadingTangentFromGeometry` are false for
//  every hit in tests 1-7 (box/sphere never set them); test 8's
//  hand-built mesh sets `derivatives.valid` but not `bHasTangent` (that
//  requires a glTF-imported TANGENT array, out of scope here), so
//  P2-d's vTangent/bitangentSign promotion half is exercised by
//  inspection of CSGObject.cpp rather than a dedicated test.
//
//
//  WHY THIS FILE USES A FAILURE TALLY AND NOT `assert` (2026-08-17).
//  Same NDEBUG hazard as CSGObjectIdentityTest.cpp: build/cmake/rise-tests/
//  CMakeLists.txt never overrides CMAKE_CXX_FLAGS_RELEASE, MSVC's Release
//  default carries `/DNDEBUG`, and run_all_tests.ps1 defaults to
//  `-Config Release` -- so on that path every `assert` in this file used
//  to compile to nothing and its 150+ checks verified NOTHING.
//
//  Worse: this file's setup calls `csg->AssignObjects(...)` -- a
//  SIDE-EFFECTING call that actually builds the CSG composite's operand
//  assignment -- and roughly fifteen of those calls sat INSIDE
//  `assert(...)`.  Under NDEBUG the composite was never assigned and
//  every check that followed ran against an unassigned CSG object instead
//  of failing loudly.  Every such call is now made on its own line, with
//  only its RESULT checked, so the setup always runs regardless of build
//  configuration.
//
//  Style follows CSGObjectIdentityTest.cpp / CstSourceInstanceTest: a
//  counted Check(), a printed tally, and a non-zero exit on any failure --
//  exactly what run_all_tests.{sh,ps1} judge.
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <iostream>

#include "../src/Library/Functions/ConstantFunctions.h"
#include "../src/Library/Geometry/BoxGeometry.h"
#include "../src/Library/Geometry/CylinderGeometry.h"
#include "../src/Library/Geometry/SDFGeometry.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Geometry/TriangleMeshGeometry.h"
#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"
#include "../src/Library/Intersection/RayIntersection.h"
#include "../src/Library/Materials/LambertianMaterial.h"
#include "../src/Library/Objects/CSGObject.h"
#include "../src/Library/Utilities/SurfaceCurvature.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Painters/UniformColorPainter.h"

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
	const bool assigned = csg->AssignObjects( outer, inner );   // pObjectA = outer, pObjectB = inner
	Check( assigned, "Test1: composite takes outer(A)/inner(B) operands" );
	csg->FinalizeTransformations();

	RayIntersection ri( r, nullRasterizerState );
	Hit( csg, r, ri );
	Check( ri.geometric.bHit, "Test1: (control) ray hits the composite at all" );

	// Reference: intersect the SAME inner-box object directly, standalone.
	RayIntersection refInner( r, nullRasterizerState );
	Hit( inner, r, refInner );
	Check( refInner.geometric.bHit, "Test1: (control) ray hits standalone inner box" );

	// Reference: intersect the outer box too, to build a negative check.
	RayIntersection refOuter( r, nullRasterizerState );
	Hit( outer, r, refOuter );
	Check( refOuter.geometric.bHit, "Test1: (control) ray hits standalone outer box" );

	// Sanity: the two boxes really do produce distinct UV / object-space
	// points at this ray (otherwise the test wouldn't be discriminating).
	Check( !Point2Close( refInner.geometric.ptCoord, refOuter.geometric.ptCoord ), "Test1: (sanity) inner/outer ptCoord are distinct" );
	Check( !PointClose( refInner.geometric.ptObjIntersec, refOuter.geometric.ptObjIntersec ), "Test1: (sanity) inner/outer ptObjIntersec are distinct" );

	// The composite entry must be wholly B's (inner's) surface.
	Check( VecClose( ri.geometric.vNormal, refInner.geometric.vNormal ), "Test1: composite vNormal matches inner's" );
	Check( VecClose( ri.geometric.vGeomNormal, refInner.geometric.vGeomNormal ), "Test1: composite vGeomNormal matches inner's" );
	Check( Point2Close( ri.geometric.ptCoord, refInner.geometric.ptCoord ), "Test1: composite ptCoord matches inner's" );
	Check( PointClose( ri.geometric.ptObjIntersec, refInner.geometric.ptObjIntersec ), "Test1: composite ptObjIntersec matches inner's" );
	Check( ri.geometric.derivatives.valid == refInner.geometric.derivatives.valid, "Test1: composite derivatives.valid matches inner's" );
	Check( ri.geometric.bHasVertexColor == refInner.geometric.bHasVertexColor, "Test1: composite bHasVertexColor matches inner's" );
	Check( ri.geometric.bHasTangent == refInner.geometric.bHasTangent, "Test1: composite bHasTangent matches inner's" );

	// Negative check: NOT A's (outer's) UV / object-space point -- this is
	// exactly what the bug produced (B's normal paired with A's payload).
	Check( !Point2Close( ri.geometric.ptCoord, refOuter.geometric.ptCoord ), "Test1: composite ptCoord is NOT outer's (the P1-a bug)" );
	Check( !PointClose( ri.geometric.ptObjIntersec, refOuter.geometric.ptObjIntersec ), "Test1: composite ptObjIntersec is NOT outer's (the P1-a bug)" );

	safe_release( csg );
	safe_release( outer );
	safe_release( inner );
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
	const bool assigned = csg->AssignObjects( inner, outer );
	Check( assigned, "Test2: composite takes inner(A)/outer(B) operands (roles swapped)" );
	csg->FinalizeTransformations();

	RayIntersection ri( r, nullRasterizerState );
	Hit( csg, r, ri );
	Check( ri.geometric.bHit, "Test2: (control) ray hits the composite at all" );

	RayIntersection refInner( r, nullRasterizerState );
	Hit( inner, r, refInner );
	Check( refInner.geometric.bHit, "Test2: (control) ray hits standalone inner box" );

	RayIntersection refOuter( r, nullRasterizerState );
	Hit( outer, r, refOuter );
	Check( refOuter.geometric.bHit, "Test2: (control) ray hits standalone outer box" );

	// The composite entry must still be wholly the INNER box's surface
	// (it's the operand that enters second / is nested), regardless of
	// which CSG operand slot (A or B) it was assigned to.
	Check( VecClose( ri.geometric.vNormal, refInner.geometric.vNormal ), "Test2: composite vNormal matches inner's" );
	Check( VecClose( ri.geometric.vGeomNormal, refInner.geometric.vGeomNormal ), "Test2: composite vGeomNormal matches inner's" );
	Check( Point2Close( ri.geometric.ptCoord, refInner.geometric.ptCoord ), "Test2: composite ptCoord matches inner's" );
	Check( PointClose( ri.geometric.ptObjIntersec, refInner.geometric.ptObjIntersec ), "Test2: composite ptObjIntersec matches inner's" );

	// Negative check: NOT the outer box's payload.
	Check( !Point2Close( ri.geometric.ptCoord, refOuter.geometric.ptCoord ), "Test2: composite ptCoord is NOT outer's" );
	Check( !PointClose( ri.geometric.ptObjIntersec, refOuter.geometric.ptObjIntersec ), "Test2: composite ptObjIntersec is NOT outer's" );

	safe_release( csg );
	safe_release( outer );
	safe_release( inner );
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
	const bool assigned = csg->AssignObjects( oA, oB );
	Check( assigned, "Test3: composite takes A/B operands" );
	csg->FinalizeTransformations();

	RayIntersection ri( r, nullRasterizerState );
	Hit( csg, r, ri );
	Check( ri.geometric.bHit, "Test3: (control) ray hits the composite at all" );

	// Reference: standalone B at the same ray (unflipped).
	RayIntersection refB( r, nullRasterizerState );
	Hit( oB, r, refB );
	Check( refB.geometric.bHit, "Test3: (control) ray hits standalone B" );

	RayIntersection refA( r, nullRasterizerState );
	Hit( oA, r, refA );
	Check( refA.geometric.bHit, "Test3: (control) ray hits standalone A" );

	// Sanity: A and B produce distinct UV / object-space points here.
	Check( !PointClose( refA.geometric.ptObjIntersec, refB.geometric.ptObjIntersec ), "Test3: (sanity) A/B ptObjIntersec are distinct" );

	// The composite boundary is B's surface, normal flipped (we're
	// leaving A's solid into the void B carved out of it).
	Check( VecClose( ri.geometric.vNormal, Vector3( -refB.geometric.vNormal.x, -refB.geometric.vNormal.y, -refB.geometric.vNormal.z ) ), "Test3: composite vNormal is B's normal, flipped" );
	Check( VecClose( ri.geometric.vGeomNormal, Vector3( -refB.geometric.vGeomNormal.x, -refB.geometric.vGeomNormal.y, -refB.geometric.vGeomNormal.z ) ), "Test3: composite vGeomNormal is B's geom normal, flipped" );
	// UV and object-space point are NOT sign-flipped -- they're B's own.
	Check( Point2Close( ri.geometric.ptCoord, refB.geometric.ptCoord ), "Test3: composite ptCoord matches B's (unflipped)" );
	Check( PointClose( ri.geometric.ptObjIntersec, refB.geometric.ptObjIntersec ), "Test3: composite ptObjIntersec matches B's (unflipped)" );

	// Negative check: not A's payload.
	Check( !PointClose( ri.geometric.ptObjIntersec, refA.geometric.ptObjIntersec ), "Test3: composite ptObjIntersec is NOT A's" );

	safe_release( csg );
	safe_release( oA );
	safe_release( oB );
}

//
// Test 4 (P2-e): CSG_SUBTRACTION, the EXIT-designated boundary branch
// -- the composite's reported ENTRY is operand B's EXIT face (B carves
// into the near part of A; the visible near wall of A-minus-B is
// where B's far/exit face sits).  Before P2-e, no geometry in RISE
// computed a second UV / object-space payload for an exit hit, so the
// payload honestly stayed B's ENTRY-hit data -- the wrong FACE of the
// right operand.  P2-e recovers the REAL exit-face payload via a
// reverse probe.  This test verifies the composite's payload now
// matches an INDEPENDENT direct probe of B's exit face from the
// outside (a hand-built "reverse" ray, not production code) -- and
// specifically does NOT match B's entry-face data anymore.
//
void TestSubtraction_ExitDesignatedBoundary_ProbedPayloadMatchesRealFace()
{
	std::cout << "CSG_SUBTRACTION: exit-designated boundary payload is the REAL face (P2-e probe)..." << std::endl;

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
	const bool assigned = csg->AssignObjects( oA, oB );
	Check( assigned, "Test4 (P2-e): composite takes A/B operands" );
	csg->FinalizeTransformations();

	RayIntersection ri( r, nullRasterizerState );
	Hit( csg, r, ri );
	Check( ri.geometric.bHit, "Test4 (P2-e): (control) ray hits the composite at all" );

	RayIntersection refA( r, nullRasterizerState );
	Hit( oA, r, refA );
	Check( refA.geometric.bHit, "Test4 (P2-e): (control) ray hits standalone A" );

	// B's ENTRY-face payload (the pre-P2-e "honestly wrong face" answer)
	// -- used below only as a NEGATIVE reference.
	RayIntersection refBEntry( r, nullRasterizerState );
	Hit( oB, r, refBEntry );
	Check( refBEntry.geometric.bHit, "Test4 (P2-e): (control) ray hits standalone B" );

	// Sanity: confirm we actually hit the exit-designated branch, i.e.
	// the composite range lands on B's EXIT (range2), not B's entry
	// (range) nor A's entry.
	Check( Close( ri.geometric.range, refBEntry.geometric.range2, 1e-3 ), "Test4 (P2-e): (sanity) composite range == B's exit range (range2)" );
	Check( !Close( ri.geometric.range, refBEntry.geometric.range, 1e-3 ), "Test4 (P2-e): (sanity) composite range != B's entry range" );
	Check( !Close( ri.geometric.range, refA.geometric.range, 1e-3 ), "Test4 (P2-e): (sanity) composite range != A's entry range" );

	// Normal is B's EXIT normal, flipped -- untouched by the P2-e probe
	// (the branch's own normal fields are authoritative; the probe only
	// ever supplies the AUXILIARY payload, never range/vNormal/vGeomNormal).
	Check( VecClose( ri.geometric.vNormal, Vector3( -refBEntry.geometric.vNormal2.x, -refBEntry.geometric.vNormal2.y, -refBEntry.geometric.vNormal2.z ) ), "Test4 (P2-e): composite vNormal is B's exit normal, flipped" );
	Check( VecClose( ri.geometric.vGeomNormal, Vector3( -refBEntry.geometric.vGeomNormal2.x, -refBEntry.geometric.vGeomNormal2.y, -refBEntry.geometric.vGeomNormal2.z ) ), "Test4 (P2-e): composite vGeomNormal is B's exit geom normal, flipped" );

	// P2-e reference: probe B's EXIT face DIRECTLY from the outside --
	// an INDEPENDENT ray starting just past world z=-2 (B's +Z face)
	// heading back in -Z, exactly what a direct render ray hitting that
	// face from the empty region beyond it would see.  This is a
	// hand-built test oracle, not a call into the production probe.
	Ray probeRef( Point3( 0.1, 0.05, -1.9 ), Vector3( 0, 0, -1 ) );
	RayIntersection refBExit( probeRef, nullRasterizerState );
	Hit( oB, probeRef, refBExit );
	Check( refBExit.geometric.bHit, "Test4 (P2-e): (control) direct probe hits B's exit face" );

	// Sanity: the probe reference actually lands on a DIFFERENT UV than
	// B's entry face (box UV mapping is per-face) -- otherwise this
	// test isn't discriminating between "entry payload" and "real exit
	// payload".
	Check( !Point2Close( refBExit.geometric.ptCoord, refBEntry.geometric.ptCoord ), "Test4 (P2-e): (sanity) exit-face ptCoord differs from entry-face ptCoord" );

	// The composite's payload must be the REAL exit-face data (P2-e),
	// not B's entry-face data (the pre-fix "honestly wrong face").
	Check( Point2Close( ri.geometric.ptCoord, refBExit.geometric.ptCoord ), "Test4 (P2-e): composite ptCoord matches the REAL exit-face probe" );
	Check( PointClose( ri.geometric.ptObjIntersec, refBExit.geometric.ptObjIntersec ), "Test4 (P2-e): composite ptObjIntersec matches the REAL exit-face probe" );
	Check( !Point2Close( ri.geometric.ptCoord, refBEntry.geometric.ptCoord ), "Test4 (P2-e): composite ptCoord is NOT B's entry-face data" );
	Check( !PointClose( ri.geometric.ptObjIntersec, refBEntry.geometric.ptObjIntersec ), "Test4 (P2-e): composite ptObjIntersec is NOT B's entry-face data" );

	// Never A's payload either.
	Check( !Point2Close( ri.geometric.ptCoord, refA.geometric.ptCoord ), "Test4 (P2-e): composite ptCoord is NOT A's" );
	Check( !PointClose( ri.geometric.ptObjIntersec, refA.geometric.ptObjIntersec ), "Test4 (P2-e): composite ptObjIntersec is NOT A's" );

	safe_release( csg );
	safe_release( oA );
	safe_release( oB );
}

//
// Test 5 (P1-a, bindings half): CSG_INTERSECTION cross-operand branches
// must adopt the RayIntersection-LEVEL material/modifier/shader/
// radiance-map bindings from the operand that OWNS the reported
// boundary, not the operand `ri` started as a whole-record copy of.
// Assign DISTINCT materials to A and B and confirm the composite's
// ri.pMaterial always matches the surface-owning operand, in both the
// "A enters first" and "B enters first" branches (mirrors Tests 1/2).
//
void TestIntersection_CrossOperandMaterialBindingFollowsOwner()
{
	std::cout << "CSG_INTERSECTION: cross-operand material binding follows the owning operand (P1-a)..." << std::endl;

	UniformColorPainter* painterA = new UniformColorPainter( RISEPel( 1, 0, 0 ) );
	painterA->addref();
	UniformColorPainter* painterB = new UniformColorPainter( RISEPel( 0, 1, 0 ) );
	painterB->addref();
	LambertianMaterial* matA = new LambertianMaterial( *painterA );
	matA->addref();
	LambertianMaterial* matB = new LambertianMaterial( *painterB );
	matB->addref();

	Ray r( Point3( 0.1, 0.05, -10 ), Vector3( 0, 0, 1 ) );

	// -- Case 1: A enters first, B enters while inside A (composite
	//    entry wholly B's surface -- see Test 1). --
	{
		BoxGeometry* gOuter = new BoxGeometry( 4.0, 4.0, 4.0 );
		BoxGeometry* gInner = new BoxGeometry( 0.6, 0.6, 0.6 );
		Object* outer = new Object( gOuter );
		Object* inner = new Object( gInner );
		safe_release( gOuter );
		safe_release( gInner );

		outer->AssignMaterial( *matA );
		inner->AssignMaterial( *matB );

		outer->SetPosition( Point3( 0, 0, 0 ) );
		inner->SetPosition( Point3( 0, 0, -0.5 ) );
		outer->FinalizeTransformations();
		inner->FinalizeTransformations();

		CSGObject* csg = new CSGObject( CSG_INTERSECTION );
		const bool assigned = csg->AssignObjects( outer, inner );   // A = outer/matA, B = inner/matB
		Check( assigned, "Test5 case1: composite takes outer(A)/inner(B) operands" );

		csg->FinalizeTransformations();

		RayIntersection ri( r, nullRasterizerState );
		Hit( csg, r, ri );
		Check( ri.geometric.bHit, "Test5 case1: (control) ray hits the composite at all" );

		// Composite entry is wholly B's (inner's) surface -- material
		// must be B's (matB), NOT A's (matA, the operand `ri` started
		// life as a whole-record copy of -- the exact P1-a bug).
		Check( ri.pMaterial == matB, "Test5 case1: composite ri.pMaterial == matB (owning operand)" );
		Check( ri.pMaterial != matA, "Test5 case1: composite ri.pMaterial != matA (the P1-a bug)" );

		safe_release( csg );
		safe_release( outer );
		safe_release( inner );
	}

	// -- Case 2: B enters first, A enters while inside B (the mirror
	//    branch -- see Test 2). --
	{
		BoxGeometry* gOuter = new BoxGeometry( 4.0, 4.0, 4.0 );
		BoxGeometry* gInner = new BoxGeometry( 0.6, 0.6, 0.6 );
		Object* outer = new Object( gOuter );
		Object* inner = new Object( gInner );
		safe_release( gOuter );
		safe_release( gInner );

		outer->AssignMaterial( *matA );
		inner->AssignMaterial( *matB );

		outer->SetPosition( Point3( 0, 0, 0 ) );
		inner->SetPosition( Point3( 0, 0, -0.5 ) );
		outer->FinalizeTransformations();
		inner->FinalizeTransformations();

		CSGObject* csg = new CSGObject( CSG_INTERSECTION );
		// Roles swapped: pObjectA = inner/matB, pObjectB = outer/matA.
		const bool assigned = csg->AssignObjects( inner, outer );
		Check( assigned, "Test5 case2: composite takes inner(A)/outer(B) operands (roles swapped)" );

		csg->FinalizeTransformations();

		RayIntersection ri( r, nullRasterizerState );
		Hit( csg, r, ri );
		Check( ri.geometric.bHit, "Test5 case2: (control) ray hits the composite at all" );

		// Composite entry is still wholly the inner box's surface --
		// material must be matB regardless of which CSG slot it's in.
		Check( ri.pMaterial == matB, "Test5 case2: composite ri.pMaterial == matB regardless of CSG slot" );
		Check( ri.pMaterial != matA, "Test5 case2: composite ri.pMaterial != matA" );

		safe_release( csg );
		safe_release( outer );
		safe_release( inner );
	}

	safe_release( matA );
	safe_release( matB );
	safe_release( painterA );
	safe_release( painterB );
}

//
// Test 6 (P1-b): CSG_SUBTRACTION's "outside both, overlapping" block
// tested disjointness LAST, so a B interval that never overlaps A's
// could still satisfy one of the two overlap conditions by accident:
//   (i)  B wholly BEFORE A -- satisfied the OLD "B enters first, exits
//        before A" condition (B.range2 < A.range2 trivially, since B
//        never even reaches A) -- phantom surface at B's back face in
//        empty space.
//   (ii) B wholly AFTER A -- satisfied the OLD "A enters first"
//        condition (A.range < B.range trivially) -- extended phantom
//        interval reaching out to a B that never touches A.
// After the fix, both directions must report a composite hit IDENTICAL
// to A alone (the disjointness test now excludes both overlap
// branches up front).
//
void TestSubtraction_DisjointBothSides_CompositeIsAAlone()
{
	std::cout << "CSG_SUBTRACTION: disjoint B (before and after A) -> composite == A alone (P1-b)..." << std::endl;

	BoxGeometry* gA = new BoxGeometry( 4.0, 4.0, 4.0 );   // half-extent 2, spans z in [-2,2]
	Object* oA = new Object( gA );
	safe_release( gA );
	oA->SetPosition( Point3( 0, 0, 0 ) );
	oA->FinalizeTransformations();

	Ray r( Point3( 0.1, 0.05, -10 ), Vector3( 0, 0, 1 ) );

	RayIntersection refA( r, nullRasterizerState );
	Hit( oA, r, refA );
	Check( refA.geometric.bHit, "Test6: (control) ray hits standalone A" );

	// -- (i) B wholly BEFORE A: half-extent 0.5 box spanning z in
	//    [-5.5,-4.5], well before A's entry at z=-2. --
	{
		BoxGeometry* gB = new BoxGeometry( 1.0, 1.0, 1.0 );
		Object* oB = new Object( gB );
		safe_release( gB );
		oB->SetPosition( Point3( 0, 0, -5.0 ) );
		oB->FinalizeTransformations();

		CSGObject* csg = new CSGObject( CSG_SUBTRACTION );
		const bool assigned = csg->AssignObjects( oA, oB );
		Check( assigned, "Test6 (i) B-before-A: composite takes A/B operands" );
		csg->FinalizeTransformations();

		RayIntersection ri( r, nullRasterizerState );
		Hit( csg, r, ri );
		Check( ri.geometric.bHit, "Test6 (i) B-before-A: (control) ray hits the composite at all" );

		Check( Close( ri.geometric.range, refA.geometric.range ), "Test6 (i) B-before-A: composite range == A's range" );
		Check( Close( ri.geometric.range2, refA.geometric.range2 ), "Test6 (i) B-before-A: composite range2 == A's range2" );
		Check( VecClose( ri.geometric.vNormal, refA.geometric.vNormal ), "Test6 (i) B-before-A: composite vNormal == A's vNormal" );
		Check( VecClose( ri.geometric.vGeomNormal, refA.geometric.vGeomNormal ), "Test6 (i) B-before-A: composite vGeomNormal == A's vGeomNormal" );
		Check( Point2Close( ri.geometric.ptCoord, refA.geometric.ptCoord ), "Test6 (i) B-before-A: composite ptCoord == A's ptCoord" );

		safe_release( csg );
		safe_release( oB );
	}

	// -- (ii) B wholly AFTER A: half-extent 0.5 box spanning z in
	//    [4.5,5.5], well after A's exit at z=2. --
	{
		BoxGeometry* gB = new BoxGeometry( 1.0, 1.0, 1.0 );
		Object* oB = new Object( gB );
		safe_release( gB );
		oB->SetPosition( Point3( 0, 0, 5.0 ) );
		oB->FinalizeTransformations();

		CSGObject* csg = new CSGObject( CSG_SUBTRACTION );
		const bool assigned = csg->AssignObjects( oA, oB );
		Check( assigned, "Test6 (ii) B-after-A: composite takes A/B operands" );
		csg->FinalizeTransformations();

		RayIntersection ri( r, nullRasterizerState );
		Hit( csg, r, ri );
		Check( ri.geometric.bHit, "Test6 (ii) B-after-A: (control) ray hits the composite at all" );

		Check( Close( ri.geometric.range, refA.geometric.range ), "Test6 (ii) B-after-A: composite range == A's range" );
		Check( Close( ri.geometric.range2, refA.geometric.range2 ), "Test6 (ii) B-after-A: composite range2 == A's range2" );
		Check( VecClose( ri.geometric.vNormal, refA.geometric.vNormal ), "Test6 (ii) B-after-A: composite vNormal == A's vNormal" );
		Check( VecClose( ri.geometric.vGeomNormal, refA.geometric.vGeomNormal ), "Test6 (ii) B-after-A: composite vGeomNormal == A's vGeomNormal" );
		Check( Point2Close( ri.geometric.ptCoord, refA.geometric.ptCoord ), "Test6 (ii) B-after-A: composite ptCoord == A's ptCoord" );

		safe_release( csg );
		safe_release( oB );
	}

	safe_release( oA );
}

//
// Test 7 (P1-c): IntersectRay_IntersectionOnly must express BOTH
// children's ranges in a common (CSG-local / caller) frame.  Before
// the fix, children were called with bComputeExitInfo=FALSE, so
// range2 stayed in the CHILD's own RAW LOCAL parametric units -- wrong
// by exactly the child's scale factor whenever that operand carries a
// non-unit scale.  Construct operand A scaled 10x (world span
// [-20,20]) and operand B unscaled (world span [-2,2], wholly inside
// A) in a CSG_INTERSECTION: the composite intersection is exactly B's
// span, entered at world range ~98.  Before the fix, A's WRONG
// (10x-too-small) local range2 (~12) failed BOTH "outside both"
// overlap comparisons (98 <= 12 is false, and the mirrored check is
// false too), so IntersectRay_IntersectionOnly fell through to
// `return false` -- a shadow LEAK (the occluder was skipped entirely).
// After the fix, A's range2 correctly reads ~120 (world units) and the
// composite intersection is detected.
//
void TestIntersectionOnly_ScaledOperandNoShadowLeak()
{
	std::cout << "IntersectRay_IntersectionOnly: scaled CSG operand doesn't leak shadow (P1-c)..." << std::endl;

	BoxGeometry* gA = new BoxGeometry( 4.0, 4.0, 4.0 );   // half-extent 2
	BoxGeometry* gB = new BoxGeometry( 4.0, 4.0, 4.0 );   // half-extent 2
	Object* oA = new Object( gA );
	Object* oB = new Object( gB );
	safe_release( gA );
	safe_release( gB );

	oA->SetPosition( Point3( 0, 0, 0 ) );
	oA->SetScale( 10.0 );                  // world span [-20,20]
	oA->FinalizeTransformations();

	oB->SetPosition( Point3( 0, 0, 0 ) );   // world span [-2,2], wholly inside A
	oB->FinalizeTransformations();

	CSGObject* csg = new CSGObject( CSG_INTERSECTION );
	const bool assigned = csg->AssignObjects( oA, oB );
	Check( assigned, "Test7 (P1-c): composite takes A/B operands" );
	csg->FinalizeTransformations();

	Ray r( Point3( 0, 0, -100 ), Vector3( 0, 0, 1 ) );
	const Scalar dHowFar = 200.0;

	// Ground truth: the full IntersectRay (always exit-info-correct)
	// says the composite IS hit, well within dHowFar (entry ~98).
	RayIntersection ri( r, nullRasterizerState );
	Hit( csg, r, ri );
	Check( ri.geometric.bHit, "Test7 (P1-c): (control) full IntersectRay hits the composite" );
	Check( ri.geometric.range < dHowFar, "Test7 (P1-c): (control) hit range is within dHowFar" );
	Check( ri.geometric.range > 90.0 && ri.geometric.range < 100.0, "Test7 (P1-c): (control) hit range is ~98 (B's span)" );

	// IntersectRay_IntersectionOnly must agree.
	const bool occluded = csg->IntersectRay_IntersectionOnly( r, dHowFar, true, true );
	Check( occluded, "Test7 (P1-c): MONEY ASSERTION -- IntersectionOnly agrees, no shadow leak from scaled operand" );

	safe_release( csg );
	safe_release( oA );
	safe_release( oB );
}

namespace
{
	// Builds a single, off-axis, non-degenerate triangle for the P2-d
	// derivatives test.  Face-normal mode (no per-vertex normals
	// needed) keeps the test focused on dpdu/dpdv/dndu/dndv frame
	// promotion rather than Phong shading-normal interpolation detail.
	// TexCoords are required regardless of face-normal mode --
	// DoneIndexedTriangles() unconditionally indexes into pCoords.
	TriangleMeshGeometryIndexed* BuildDerivativeTestMesh()
	{
		TriangleMeshGeometryIndexed* pMesh = new TriangleMeshGeometryIndexed(
			true,    // double sided
			true );  // use face normals

		pMesh->BeginIndexedTriangles();

		pMesh->AddVertex( Point3( -1, 0, -1 ) );
		pMesh->AddVertex( Point3(  1, 0, -1 ) );
		pMesh->AddVertex( Point3(  0, 0,  1 ) );

		pMesh->AddTexCoord( Point2( 0, 0 ) );
		pMesh->AddTexCoord( Point2( 1, 0 ) );
		pMesh->AddTexCoord( Point2( 0, 1 ) );

		IndexedTriangle t;
		t.iVertices[0] = 0; t.iVertices[1] = 1; t.iVertices[2] = 2;
		t.iCoords[0]   = 0; t.iCoords[1]   = 1; t.iCoords[2]   = 2;
		pMesh->AddIndexedTriangle( t );

		pMesh->DoneIndexedTriangles();
		return pMesh;
	}
}

//
// Test 8 (P2-d): CSGObject::IntersectRay's world-promotion tail must
// promote surface derivatives (dpdu/dpdv/dndu/dndv) from the CSG's own
// local frame to world space, exactly like Object::IntersectRay does
// for a plain (non-CSG) object -- mirroring the SAME normal/point
// promotion CSGObject already performs.  Build a CSG_UNION of a
// triangle-mesh operand A and a sphere operand B placed far enough
// away that it is NEVER hit (so the switch takes the simplest
// `ri = riObjA` passthrough, with no entry/exit algebra in play), then
// apply a rotation to the CSG OBJECT ITSELF (not to A).  The result
// must match a STANDALONE Object wrapping an identical mesh with the
// SAME rotation applied directly.
//
void TestUnion_TransformedCsgDerivativesMatchStandaloneRotated()
{
	std::cout << "CSG_UNION: transformed-CSG dpdu matches standalone rotated operand (P2-d)..." << std::endl;

	TriangleMeshGeometryIndexed* pMeshA = BuildDerivativeTestMesh();
	Object* a = new Object( pMeshA );
	safe_release( pMeshA );
	a->FinalizeTransformations();   // A itself carries NO extra transform

	// B: placed far enough away that it is NEVER hit by the test ray,
	// even after the CSG's own rotation is applied to it too.
	SphereGeometry* gB = new SphereGeometry( 1.0 );
	Object* b = new Object( gB );
	safe_release( gB );
	b->SetPosition( Point3( 10000, 10000, 10000 ) );
	b->FinalizeTransformations();

	CSGObject* csg = new CSGObject( CSG_UNION );
	const bool assigned = csg->AssignObjects( a, b );
	Check( assigned, "Test8 (P2-d): composite takes mesh(A)/far-sphere(B) operands" );
	const Scalar angle = 0.6981317007977318;   // ~40 degrees, radians
	csg->RotateObjectZAxis( angle );
	csg->FinalizeTransformations();

	// World-space test ray: the same local-frame ray that hits the
	// UN-rotated triangle off-center (see GeometricNormalPlumbingTest's
	// TestMesh_GeomDistinctFromShading), rotated by the identical Z
	// rotation so it still lands on the (now-rotated) triangle.
	const Matrix4 rot = Matrix4Ops::ZRotation( angle );
	const Point3 localOrigin( 0.3, 5, -0.2 );
	const Vector3 localDir( 0, -1, 0 );
	const Ray r(
		Point3Ops::Transform( rot, localOrigin ),
		Vector3Ops::Transform( rot, localDir ) );

	RayIntersection ri( r, nullRasterizerState );
	Hit( csg, r, ri );
	Check( ri.geometric.bHit, "Test8 (P2-d): (control) ray hits the rotated CSG" );
	Check( ri.geometric.derivatives.valid, "Test8 (P2-d): (control) composite derivatives.valid" );

	// Reference: a STANDALONE Object wrapping an independently-built
	// (but geometrically identical) copy of the same triangle, with the
	// IDENTICAL rotation applied DIRECTLY (no CSG nesting at all).
	TriangleMeshGeometryIndexed* pMeshRef = BuildDerivativeTestMesh();
	Object* ref = new Object( pMeshRef );
	safe_release( pMeshRef );
	ref->RotateObjectZAxis( angle );
	ref->FinalizeTransformations();

	RayIntersection refRi( r, nullRasterizerState );
	Hit( ref, r, refRi );
	Check( refRi.geometric.bHit, "Test8 (P2-d): (control) ray hits the standalone rotated reference" );
	Check( refRi.geometric.derivatives.valid, "Test8 (P2-d): (control) reference derivatives.valid" );

	// Sanity: dpdu is actually non-degenerate, so this comparison is
	// discriminating (a zero vector would trivially "match" whether or
	// not promotion happened).
	Check( Vector3Ops::SquaredModulus( refRi.geometric.derivatives.dpdu ) > 1e-6, "Test8 (P2-d): (sanity) reference dpdu is non-degenerate" );

	Check( VecClose( ri.geometric.derivatives.dpdu, refRi.geometric.derivatives.dpdu, 1e-4 ), "Test8 (P2-d): composite dpdu matches standalone rotated reference" );
	Check( VecClose( ri.geometric.derivatives.dpdv, refRi.geometric.derivatives.dpdv, 1e-4 ), "Test8 (P2-d): composite dpdv matches standalone rotated reference" );
	Check( VecClose( ri.geometric.derivatives.dndu, refRi.geometric.derivatives.dndu, 1e-4 ), "Test8 (P2-d): composite dndu matches standalone rotated reference" );
	Check( VecClose( ri.geometric.derivatives.dndv, refRi.geometric.derivatives.dndv, 1e-4 ), "Test8 (P2-d): composite dndv matches standalone rotated reference" );

	safe_release( csg );
	safe_release( a );
	safe_release( b );
	safe_release( ref );
}

//
// Test 9 (P1, direction-independent factor): CSGObject::IntersectRay_
// IntersectionOnly's world-to-local distance factor must be the
// magnitude of the TRANSFORMED RAY DIRECTION, not an arbitrary +X-axis
// probe.  Apply a NON-UNIFORM stretch to the CSG object itself (sx=0.01,
// sy=1, sz=100) and fire a shadow-style query along world +Z -- a
// direction the old +X-based factor scales completely wrong.  Ground
// truth (the always-correct full IntersectRay, called with dHowFar=
// RISE_INFINITY per the `Hit()` helper) puts the occluder's entry at
// world range ~100; querying IntersectRay_IntersectionOnly with
// dHowFar=90 (a "light" CLOSER than the occluder) must report NOT
// occluded.  The old +X-axis factor (=100, wildly wrong for a +Z ray
// here) inflates dHowFar2 enough that the occluder's local-frame range
// still passes the check, reporting a false shadow.
//
void TestIntersectionOnly_NonUniformScale_DirectionTrueFactor()
{
	std::cout << "IntersectRay_IntersectionOnly: non-uniform CSG scale, direction-true factor (P1, item 1)..." << std::endl;

	BoxGeometry* gA = new BoxGeometry( 4.0, 4.0, 4.0 );   // half-extent 2
	Object* oA = new Object( gA );
	safe_release( gA );
	oA->SetPosition( Point3( 0, 0, 0 ) );
	oA->FinalizeTransformations();

	// Far-away second operand -- never hit.
	SphereGeometry* gB = new SphereGeometry( 1.0 );
	Object* oB = new Object( gB );
	safe_release( gB );
	oB->SetPosition( Point3( 100000, 100000, 100000 ) );
	oB->FinalizeTransformations();

	CSGObject* csg = new CSGObject( CSG_UNION );
	const bool assigned = csg->AssignObjects( oA, oB );
	Check( assigned, "Test9 (P1 item1): composite takes A(box)/B(far sphere) operands" );
	// Non-uniform stretch on the CSG object itself: tiny in X, huge in Z.
	// World span of A (local half-extent 2, box[-2,2]) becomes
	// x in [-0.02,0.02], y in [-2,2], z in [-200,200].
	csg->SetStretch( Vector3( 0.01, 1.0, 100.0 ) );
	csg->FinalizeTransformations();

	// Ray travels along world +Z -- NOT the axis the old buggy factor
	// probed (+X).
	Ray r( Point3( 0, 0, -300 ), Vector3( 0, 0, 1 ) );

	// Ground truth: full IntersectRay (always direction-correct -- it
	// never scales dHowFar at all, and Hit() always passes RISE_INFINITY)
	// puts the entry at world z=-200, i.e. range ~100 from z=-300.
	RayIntersection ri( r, nullRasterizerState );
	Hit( csg, r, ri );
	Check( ri.geometric.bHit, "Test9 (P1 item1): (control) full IntersectRay hits the composite" );
	Check( ri.geometric.range > 95.0 && ri.geometric.range < 105.0, "Test9 (P1 item1): (control) hit range is ~100 (world z=-200 entry)" );

	// A "light" at world distance 90 is CLOSER than the occluder's entry
	// (~100) -- the occluder must NOT report as blocking it.
	const Scalar dHowFarTest = 90.0;
	const bool occluded = csg->IntersectRay_IntersectionOnly( r, dHowFarTest, true, true );
	Check( !occluded, "Test9 (P1 item1): MONEY ASSERTION -- not falsely occluded under non-uniform scale w/ direction-true factor" );

	safe_release( csg );
	safe_release( oA );
	safe_release( oB );
}

//
// Test 10 (P2, item 2): CSGObject::IntersectRay's final onb-construction
// block must honour bShadingTangentFromGeometry exactly like
// Object::IntersectRay does, instead of always calling CreateFromW.
// Build a flat heightfield SDF disk (SDFGeometry heightfield mode,
// scale=0.0 -- an exactly-flat z=0 surface, so the shading normal is
// world +Z everywhere on it: the same degenerate "cartesian_disk" case
// the flag exists for, see the flag's set site in SDFGeometry.cpp) both
// standalone and as one operand of a CSG_UNION whose other operand is
// never hit (forcing the simplest `ri = riObjA` passthrough into the
// CSG's own final promotion block, same pattern as Test 8/P2-d).  The
// CSG hit's onb must match the standalone hit's onb exactly.
//
void TestUnion_CsgHonoursShadingTangentFromGeometry()
{
	std::cout << "CSG_UNION: onb honours bShadingTangentFromGeometry through CSG (P2, item 2)..." << std::endl;

	ConstantFunction2D* fieldStandalone = new ConstantFunction2D( 0.0 );
	SDFGeometry* gStandalone = new SDFGeometry( fieldStandalone, 2.0, 0.0, 256, 0.0, 8 );
	safe_release( fieldStandalone );

	ConstantFunction2D* fieldOperand = new ConstantFunction2D( 0.0 );
	SDFGeometry* gOperand = new SDFGeometry( fieldOperand, 2.0, 0.0, 256, 0.0, 8 );
	safe_release( fieldOperand );

	Object* standalone = new Object( gStandalone );
	safe_release( gStandalone );
	standalone->FinalizeTransformations();

	Object* aOperand = new Object( gOperand );
	safe_release( gOperand );
	aOperand->FinalizeTransformations();

	// Far-away second operand -- never hit (same trick as Test 8/P2-d).
	SphereGeometry* gB = new SphereGeometry( 1.0 );
	Object* farB = new Object( gB );
	safe_release( gB );
	farB->SetPosition( Point3( 10000, 10000, 10000 ) );
	farB->FinalizeTransformations();

	CSGObject* csg = new CSGObject( CSG_UNION );
	const bool assigned = csg->AssignObjects( aOperand, farB );
	Check( assigned, "Test10 (P2 item2): composite takes aOperand(A)/farB(B) operands" );
	csg->FinalizeTransformations();

	Ray r( Point3( 0.3, 0.2, 5.0 ), Vector3( 0, 0, -1 ) );

	RayIntersection ri( r, nullRasterizerState );
	Hit( csg, r, ri );
	Check( ri.geometric.bHit, "Test10 (P2 item2): (control) ray hits the composite" );
	Check( ri.geometric.bShadingTangentFromGeometry, "Test10 (P2 item2): (control) composite bShadingTangentFromGeometry set" );

	RayIntersection refRi( r, nullRasterizerState );
	Hit( standalone, r, refRi );
	Check( refRi.geometric.bHit, "Test10 (P2 item2): (control) ray hits the standalone reference" );
	Check( refRi.geometric.bShadingTangentFromGeometry, "Test10 (P2 item2): (control) reference bShadingTangentFromGeometry set" );

	// Sanity: the flag really does route to the WORLD-X-PROJECTED branch
	// (CreateFromWU), not the default CreateFromW axis -- for an exact +Z
	// normal, CreateFromW picks U=(-1,0,0) (cross(W,canonicalV)), while
	// the geometry-aware branch picks U=(+1,0,0) (world-X projected into
	// the normal plane, which for n=(0,0,1) is just world-X itself).  If
	// these coincided the test wouldn't discriminate.
	Check( VecClose( refRi.geometric.onb.u(), Vector3( 1, 0, 0 ) ), "Test10 (P2 item2): (sanity) reference onb.u() is world-X-projected" );
	Check( !VecClose( refRi.geometric.onb.u(), Vector3( -1, 0, 0 ) ), "Test10 (P2 item2): (sanity) reference onb.u() is NOT the CreateFromW default" );

	Check( VecClose( ri.geometric.onb.u(), refRi.geometric.onb.u() ), "Test10 (P2 item2): composite onb.u() matches standalone reference" );
	Check( VecClose( ri.geometric.onb.v(), refRi.geometric.onb.v() ), "Test10 (P2 item2): composite onb.v() matches standalone reference" );
	Check( VecClose( ri.geometric.onb.w(), refRi.geometric.onb.w() ), "Test10 (P2 item2): composite onb.w() matches standalone reference" );

	safe_release( csg );
	safe_release( aOperand );
	safe_release( farB );
	safe_release( standalone );
}

//
// Test 11 (P2, item 3): the exit-face reverse probe's margin must be
// derived from the OPERAND's own world bounding-box diagonal, not the
// camera range, or a long-range shadow/exit query can overshoot past a
// DIFFERENT lobe of a multi-lobe operand and adopt the wrong face's
// payload.  `nestedB` is a CSG_UNION of two disjoint box "lobes":
// `realLobe` (matching the P2-e test's own B geometry, spans z in
// [-6,-2], carving A's near wall) and a `decoyLobe` just 0.3 units
// beyond the real exit face (z in [-1.7,-1.3]).  From far away the outer
// SUBTRACTION only ever sees `nestedB` report the nearer, disjoint
// `realLobe` interval (union's own "outside both, disjoint" algebra) --
// the decoy is invisible to the outer entry/exit algebra and reachable
// ONLY by the reverse probe.  At a LONG camera range, the OLD margin
// (exitRangeCsgLocal * 1e-6) grows past the 0.3-unit gap and the probe's
// short search finds the decoy INSTEAD of the real exit face; the NEW
// diagonal-derived margin stays camera-independent and finds the real
// face regardless of range.
//
void TestSubtraction_ExitProbe_DoesNotOvershootToADifferentLobe()
{
	std::cout << "CSG_SUBTRACTION: exit-probe margin doesn't overshoot to a different lobe at long camera range (P2, item 3)..." << std::endl;

	BoxGeometry* gA = new BoxGeometry( 6.0, 6.0, 6.0 );          // half-extent 3, spans z in [-3,3]
	BoxGeometry* gRealLobe = new BoxGeometry( 4.0, 4.0, 4.0 );   // half-extent (2,2,2)
	BoxGeometry* gDecoyLobe = new BoxGeometry( 3.6, 3.6, 0.4 );  // half-extent (1.8,1.8,0.2) -- narrower in X/Y than realLobe so their ptCoord mappings differ too

	Object* oA = new Object( gA );
	Object* realLobe = new Object( gRealLobe );
	Object* decoyLobe = new Object( gDecoyLobe );
	safe_release( gA );
	safe_release( gRealLobe );
	safe_release( gDecoyLobe );

	oA->SetPosition( Point3( 0, 0, 0 ) );
	oA->FinalizeTransformations();

	realLobe->SetPosition( Point3( 0, 0, -4 ) );     // spans z in [-6,-2]: overlaps A's near wall (same as the P2-e test's B)
	realLobe->FinalizeTransformations();

	// A decoy lobe just OUTSIDE the real exit face (z=-2), separated by a
	// 0.3-unit gap.
	decoyLobe->SetPosition( Point3( 0, 0, -1.5 ) );  // spans z in [-1.7,-1.3]
	decoyLobe->FinalizeTransformations();

	CSGObject* nestedB = new CSGObject( CSG_UNION );
	const bool nestedAssigned = nestedB->AssignObjects( realLobe, decoyLobe );
	Check( nestedAssigned, "Test11 (P2 item3): nestedB takes realLobe/decoyLobe operands" );
	nestedB->FinalizeTransformations();

	CSGObject* outerCsg = new CSGObject( CSG_SUBTRACTION );
	const bool outerAssigned = outerCsg->AssignObjects( oA, nestedB );
	Check( outerAssigned, "Test11 (P2 item3): outerCsg takes oA/nestedB operands" );
	outerCsg->FinalizeTransformations();

	// LONG camera range: exitRangeCsgLocal (the CSG-local distance from
	// the ray origin to the exit-designated boundary) scales with this
	// distance -- the OLD margin formula grows right along with it,
	// eventually exceeding the 0.3-unit gap to the decoy lobe.  The NEW
	// margin (operand bbox diagonal * 1e-6) does not move with camera
	// range at all.
	const Scalar D = 1000000.0;
	Ray r( Point3( 0.1, 0.05, -D ), Vector3( 0, 0, 1 ) );

	RayIntersection ri( r, nullRasterizerState );
	Hit( outerCsg, r, ri );
	Check( ri.geometric.bHit, "Test11 (P2 item3): (control) ray hits the composite at long camera range" );

	// Ground truth: the REAL exit face, probed directly (independent of
	// production code) -- exactly the P2-e test's own oracle pattern.
	Ray probeRefReal( Point3( 0.1, 0.05, -1.9 ), Vector3( 0, 0, -1 ) );
	RayIntersection refRealExit( probeRefReal, nullRasterizerState );
	Hit( realLobe, probeRefReal, refRealExit );
	Check( refRealExit.geometric.bHit, "Test11 (P2 item3): (control) direct probe hits realLobe's exit face" );

	// The WRONG answer an overshooting probe lands on: the decoy lobe's
	// near face, probed directly.
	Ray probeDecoy( Point3( 0.1, 0.05, -1.0 ), Vector3( 0, 0, -1 ) );
	RayIntersection refDecoy( probeDecoy, nullRasterizerState );
	Hit( decoyLobe, probeDecoy, refDecoy );
	Check( refDecoy.geometric.bHit, "Test11 (P2 item3): (control) direct probe hits decoyLobe's near face" );

	// Sanity: the two candidate faces produce distinct UV / object-space
	// points, so this test is discriminating.
	Check( !Point2Close( refRealExit.geometric.ptCoord, refDecoy.geometric.ptCoord ), "Test11 (P2 item3): (sanity) real-exit/decoy ptCoord are distinct" );
	Check( !PointClose( refRealExit.geometric.ptObjIntersec, refDecoy.geometric.ptObjIntersec ), "Test11 (P2 item3): (sanity) real-exit/decoy ptObjIntersec are distinct" );

	// The composite's recovered payload must be the REAL exit face, never
	// the decoy.
	Check( Point2Close( ri.geometric.ptCoord, refRealExit.geometric.ptCoord ), "Test11 (P2 item3): MONEY ASSERTION -- composite ptCoord matches the REAL exit face" );
	Check( PointClose( ri.geometric.ptObjIntersec, refRealExit.geometric.ptObjIntersec ), "Test11 (P2 item3): MONEY ASSERTION -- composite ptObjIntersec matches the REAL exit face" );
	Check( !Point2Close( ri.geometric.ptCoord, refDecoy.geometric.ptCoord ), "Test11 (P2 item3): composite ptCoord did NOT overshoot to the decoy lobe" );
	Check( !PointClose( ri.geometric.ptObjIntersec, refDecoy.geometric.ptObjIntersec ), "Test11 (P2 item3): composite ptObjIntersec did NOT overshoot to the decoy lobe" );

	safe_release( outerCsg );
	safe_release( oA );
	safe_release( nestedB );
	safe_release( realLobe );
	safe_release( decoyLobe );
}

namespace
{
	// Shared scenario for Tests 12-13 (external review round 3, item 1 --
	// CSG passes the CALLER-FRAME dHowFar straight through to its
	// operand IntersectRay calls instead of first converting it into
	// THIS CSG's own local frame).  A CSG_UNION uniformly compresses a
	// Z-axis cylinder operand by 10x (world span z in [-1,1]), unioned
	// with a sphere operand far enough away to never be hit (so the
	// switch statement's algebra reduces to the simplest single-hit
	// passthrough and the whole test isolates the dHowFar plumbing).
	// CylinderGeometry is one of the geometries with DoPreHitTest()==true
	// (see Object::IntersectRay), so its bbox-vs-dHowFar2 early-return is
	// LIVE and observes exactly whatever `dHowFar` value the CSG hands
	// it.  The cylinder carries no extra transform of its own (identity
	// relative to CSG-local), so ITS OWN direction-true factor is 1 and
	// its pretest compares directly against whatever it was handed --
	// any conversion error made one level up (by the CSG) is therefore
	// fully exposed, not masked by a second factor at the operand.
	//
	//   World ray: origin (0,0,-100), dir +Z.
	//   World hit (ground truth, via the always-dHowFar-independent main
	//   IntersectRay called through `Hit()` with RISE_INFINITY): entry at
	//   world z=-1, i.e. world range exactly 99.
	//   Test dHowFar (world units): 99.5 -- strictly farther than the
	//   true hit, so a CORRECT implementation must find/report it.
	//
	//   CSG-local ray: origin (0,0,-1000), dir +Z (unit) -- the CSG's own
	//   direction-true factor is 10 (1 / 0.1 compress).  Cylinder bbox
	//   spans local z in [-10,10], so its CSG-local bbox-entry range is
	//   990.  The CORRECT CSG-local limit is factor(10) * dHowFar(99.5)
	//   = 995: 990 < 995, so the (correct) pretest does NOT early-return
	//   and the real intersection is found.  The BUGGY code instead hands
	//   the operand the RAW world dHowFar (99.5) unconverted: 990 > 99.5,
	//   so the pretest early-returns and the hit is silently dropped --
	//   an in-range occluder missed (shadow leak) / a genuinely nearer
	//   closest-hit culled, exactly the two symptoms item 1 describes.
	struct CompressedCsgScenario
	{
		Object*    oCylinder;
		Object*    oFarSphere;
		CSGObject* csg;
		const Ray  worldRay;
		const Scalar worldDHowFar;

		CompressedCsgScenario() :
			worldRay( Point3( 0, 0, -100 ), Vector3( 0, 0, 1 ) ),
			worldDHowFar( 99.5 )
		{
			CylinderGeometry* gCyl = new CylinderGeometry( 'z', 1.0, 20.0, true );   // half-height 10, capped
			oCylinder = new Object( gCyl );
			safe_release( gCyl );
			oCylinder->FinalizeTransformations();   // identity -- no extra transform beyond the CSG's own

			SphereGeometry* gFar = new SphereGeometry( 1.0 );
			oFarSphere = new Object( gFar );
			safe_release( gFar );
			oFarSphere->SetPosition( Point3( 1000000, 1000000, 1000000 ) );
			oFarSphere->FinalizeTransformations();

			csg = new CSGObject( CSG_UNION );
			const bool assigned = csg->AssignObjects( oCylinder, oFarSphere );
			Check( assigned, "CompressedCsgScenario: composite takes oCylinder(A)/oFarSphere(B) operands" );
			csg->SetScale( 0.1 );   // uniform 10x compress -- world span z in [-1,1]
			csg->FinalizeTransformations();
		}

		~CompressedCsgScenario()
		{
			safe_release( csg );
			safe_release( oCylinder );
			safe_release( oFarSphere );
		}
	};
}

//
// Test 12 (external review round 3, item 1a -- shadow-ray light leak):
// CSGObject::IntersectRay_IntersectionOnly must convert the caller's
// (world-frame) dHowFar into ITS OWN local frame (dHowFar2) BEFORE
// handing it to the operand IntersectRay calls -- children expect the
// limit expressed in THEIR caller's frame.  See CompressedCsgScenario
// above for the full numeric derivation.  Pre-fix this reports NOT
// occluded (false negative / light leak) because the operand's own
// pretest silently drops the in-range hit.
//
void TestIntersectionOnly_CompressedCsg_NoShadowLeakThroughOperandPretest()
{
	std::cout << "IntersectRay_IntersectionOnly: compressed CSG doesn't leak shadow through operand pre-test (review r3, item 1a)..." << std::endl;

	CompressedCsgScenario s;

	// Ground truth: the always dHowFar-independent full IntersectRay
	// (Hit() always passes RISE_INFINITY) confirms the occluder really is
	// within the test light's range.
	RayIntersection ri( s.worldRay, nullRasterizerState );
	Hit( s.csg, s.worldRay, ri );
	Check( ri.geometric.bHit, "Test12 (review r3, item1a): (control) full IntersectRay hits the occluder" );
	Check( Close( ri.geometric.range, 99.0, 1e-3 ), "Test12 (review r3, item1a): (control) hit range is ~99" );
	Check( ri.geometric.range < s.worldDHowFar, "Test12 (review r3, item1a): (control) occluder is within worldDHowFar" );

	const bool occluded = s.csg->IntersectRay_IntersectionOnly( s.worldRay, s.worldDHowFar, true, true );
	Check( occluded, "Test12 (review r3, item1a): MONEY ASSERTION -- occluded, no shadow-ray light leak through operand pretest" );
}

//
// Test 13 (external review round 3, item 1b -- closest-hit culled):
// CSGObject::IntersectRay (the closest-hit path) must apply the SAME
// caller-frame -> CSG-local conversion before calling its operands.
// This is live in production: ObjectManager::RayElementIntersection
// passes the RUNNING closest-hit distance (`ri.geometric.range`,
// world-frame) as `dHowFar` on every candidate object it tests,
// including CSG objects -- so a nearer object found earlier in a BVH
// leaf directly becomes the `dHowFar` a later CSG candidate receives.
// Exercised directly against CSGObject::IntersectRay (bypassing
// ObjectManager/BVH -- a direct construction is simpler and exercises
// exactly the code this item fixes) with a dHowFar standing in for "the
// current best distance so far": a genuinely nearer hit must not be
// culled by the operand's own pretest.
//
void TestIntersectRay_CompressedCsg_ClosestHitNotCulledByOperandPretest()
{
	std::cout << "IntersectRay: compressed CSG closest hit not culled through operand pre-test (review r3, item 1b)..." << std::endl;

	CompressedCsgScenario s;

	RayIntersection ri( s.worldRay, nullRasterizerState );
	ri.geometric.bHit = false;
	ri.geometric.range = RISE_INFINITY;
	ri.geometric.range2 = RISE_INFINITY;
	ri.geometric.ray = s.worldRay;
	// dHowFar stands in for a running "current closest hit so far" that
	// is farther than the CSG's true hit but closer than a WRONG
	// (unconverted) local-frame comparison would tolerate.
	s.csg->IntersectRay( ri, s.worldDHowFar, true, true, true );

	Check( ri.geometric.bHit, "Test13 (review r3, item1b): closest hit found (not culled by operand pretest)" );
	Check( Close( ri.geometric.range, 99.0, 1e-3 ), "Test13 (review r3, item1b): MONEY ASSERTION -- closest hit range is ~99, not culled" );
}

//
// Test 14 (external review round 4, item 2 -- P2, same disease as the
// RayCaster P1): the exit-face reverse probe's margin must be
// DIRECTION-WEIGHTED, not max-abs-COMPONENT -- a large TRANSVERSE
// coordinate (one the probe's travel direction barely moves along) must
// NOT inflate the margin.  Mirrors Test 11's decoy-lobe setup exactly
// (same A/realLobe/decoyLobe shapes and z-placement), but the whole
// assembly sits at world X = 1e12 and the ray travels along +Z (dir.x ==
// dir.y == 0), so the OLD margin (kUlpFactor * max-abs-component of
// ptExitLocal, which reads the huge X coordinate regardless of travel
// direction) balloons to ~1.4e-2 -- wide enough to reach a decoy lobe
// placed just 0.01 units beyond the real exit face -- while the NEW
// margin (kUlpFactor * direction-weighted-abs, which only sees the small
// Z-axis component the probe actually travels along) stays at the
// 1e-12 floor and finds the real face.
//
// Numeric derivation (kUlpFactor = 64 * DBL_EPSILON ~= 1.4210854715e-14):
//   OLD margin ~= kUlpFactor * 1e12                 ~= 0.01421
//   OLD maxAcceptRange = margin * 2.1                ~= 0.02984
//   NEW margin ~= max(1e-12, kUlpFactor * |z_exit|)  == 1e-12 (floor; z_exit == -2)
//   NEW maxAcceptRange = margin * 2.1                == 2.1e-12
//   decoy gap (real exit face at z=-2, decoy near face at z=-1.99): 0.01
//     -- inside the OLD accept window (0.01 < 0.02984), outside the NEW
//     one (0.01 >> 2.1e-12).
//   2026-09-05 (debt-25 review, 655f352a): the margin's max() gained a
//   third term, the operand primitive's own self-hit band mapped
//   through its stretch and exit angle (selfHitFloor in
//   AdoptCsgExitFacePayloadViaProbe), which dominates here:
//     margin ~= 2 * (4*NEARZERO + kUlpFactor*|z_exit|) / (1 * 1) ~= 8.06e-12
//     maxAcceptRange ~= 1.69e-11
//   -- still 6e8x under the 0.01 gap.  That term reads ONLY the exit
//   point's component along the face normal, which is what keeps this
//   test's transverse 1e12 out of it (an L1-of-ptExit floor turned the
//   margin into 8 world units here and adopted the decoy; that is the
//   regression this test caught during that review).
//
void TestSubtraction_ExitProbe_TransverseCoordinateDoesNotInflateMargin()
{
	std::cout << "CSG_SUBTRACTION: exit-probe margin is direction-weighted, not inflated by a large transverse coordinate (review r4, item 2)..." << std::endl;

	const Scalar BIG_X = 1e12;

	BoxGeometry* gA = new BoxGeometry( 6.0, 6.0, 6.0 );           // half-extent 3, spans z in [-3,3]
	BoxGeometry* gRealLobe = new BoxGeometry( 4.0, 4.0, 4.0 );    // half-extent (2,2,2)
	BoxGeometry* gDecoyLobe = new BoxGeometry( 3.6, 3.6, 0.005 ); // half-extent (1.8,1.8,0.0025) -- narrower in X/Y than realLobe so their ptCoord mappings differ too

	Object* oA = new Object( gA );
	Object* realLobe = new Object( gRealLobe );
	Object* decoyLobe = new Object( gDecoyLobe );
	safe_release( gA );
	safe_release( gRealLobe );
	safe_release( gDecoyLobe );

	oA->SetPosition( Point3( BIG_X, 0, 0 ) );
	oA->FinalizeTransformations();

	realLobe->SetPosition( Point3( BIG_X, 0, -4 ) );     // spans z in [-6,-2]: overlaps A's near wall (same as Test 4/11's B)
	realLobe->FinalizeTransformations();

	// Decoy lobe just OUTSIDE the real exit face (z=-2), separated by a
	// 0.01-unit gap -- inside the OLD margin's inflated accept window
	// (probe origin at OLD_margin=0.0142 past the exit face, reaching
	// down to the decoy's near face at z=-1.990 first), outside the NEW
	// one (see the derivation above).  Near face (largest z, hit FIRST by
	// a probe travelling in -Z from just past the exit face) at z=-1.990;
	// far face at z=-1.995 -- entirely on the near side of the real exit
	// face (z=-2), never overlapping realLobe or A's own carve boundary.
	decoyLobe->SetPosition( Point3( BIG_X, 0, -1.9925 ) );  // spans z in [-1.995,-1.990]
	decoyLobe->FinalizeTransformations();

	CSGObject* nestedB = new CSGObject( CSG_UNION );
	const bool nestedAssigned = nestedB->AssignObjects( realLobe, decoyLobe );
	Check( nestedAssigned, "Test14 (review r4, item2): nestedB takes realLobe/decoyLobe operands" );
	nestedB->FinalizeTransformations();

	CSGObject* outerCsg = new CSGObject( CSG_SUBTRACTION );
	const bool outerAssigned = outerCsg->AssignObjects( oA, nestedB );
	Check( outerAssigned, "Test14 (review r4, item2): outerCsg takes oA/nestedB operands" );
	outerCsg->FinalizeTransformations();

	// Ray travels along +Z (dir.x == dir.y == 0) -- the probe's own
	// travel direction (see AdoptCsgExitFacePayloadViaProbe) barely moves
	// along X at all, so the huge X coordinate at BIG_X must NOT inflate
	// the margin.  Off-centre in Y only (matches Test 4/11's off-centre
	// idiom) to avoid the degenerate box-UV centre point; X stays exactly
	// BIG_X so dir.x is exactly 0 for this ray.
	Ray r( Point3( BIG_X, 0.05, -10 ), Vector3( 0, 0, 1 ) );

	RayIntersection ri( r, nullRasterizerState );
	Hit( outerCsg, r, ri );
	Check( ri.geometric.bHit, "Test14 (review r4, item2): (control) ray hits the composite at BIG_X" );

	// Ground truth: the REAL exit face, probed directly (independent of
	// production code) -- same oracle pattern as Test 4/11.
	Ray probeRefReal( Point3( BIG_X, 0.05, -1.9 ), Vector3( 0, 0, -1 ) );
	RayIntersection refRealExit( probeRefReal, nullRasterizerState );
	Hit( realLobe, probeRefReal, refRealExit );
	Check( refRealExit.geometric.bHit, "Test14 (review r4, item2): (control) direct probe hits realLobe's exit face" );

	// The WRONG answer an overshooting (OLD-margin) probe lands on: the
	// decoy lobe's near face, probed directly.
	Ray probeDecoy( Point3( BIG_X, 0.05, -1.98 ), Vector3( 0, 0, -1 ) );
	RayIntersection refDecoy( probeDecoy, nullRasterizerState );
	Hit( decoyLobe, probeDecoy, refDecoy );
	Check( refDecoy.geometric.bHit, "Test14 (review r4, item2): (control) direct probe hits decoyLobe's near face" );

	// Sanity: the two candidate faces produce distinct UV / object-space
	// points, so this test is discriminating.
	Check( !Point2Close( refRealExit.geometric.ptCoord, refDecoy.geometric.ptCoord ), "Test14 (review r4, item2): (sanity) real-exit/decoy ptCoord are distinct" );
	Check( !PointClose( refRealExit.geometric.ptObjIntersec, refDecoy.geometric.ptObjIntersec ), "Test14 (review r4, item2): (sanity) real-exit/decoy ptObjIntersec are distinct" );

	// The composite's recovered payload must be the REAL exit face, never
	// the decoy -- this is the money assertion: it FAILS under the OLD
	// max-abs-component margin (verified manually during development by
	// temporarily reintroducing that formula), because the OLD margin at
	// BIG_X=1e12 (~0.0142) comfortably overshoots the 0.01-unit gap to
	// the decoy, while the NEW direction-weighted margin (~1e-12, since
	// dir.x==dir.y==0 here) does not.
	Check( Point2Close( ri.geometric.ptCoord, refRealExit.geometric.ptCoord ), "Test14 (review r4, item2): MONEY ASSERTION -- composite ptCoord matches the REAL exit face" );
	Check( PointClose( ri.geometric.ptObjIntersec, refRealExit.geometric.ptObjIntersec ), "Test14 (review r4, item2): MONEY ASSERTION -- composite ptObjIntersec matches the REAL exit face" );
	Check( !Point2Close( ri.geometric.ptCoord, refDecoy.geometric.ptCoord ), "Test14 (review r4, item2): composite ptCoord did NOT overshoot to decoy (transverse coord)" );
	Check( !PointClose( ri.geometric.ptObjIntersec, refDecoy.geometric.ptObjIntersec ), "Test14 (review r4, item2): composite ptObjIntersec did NOT overshoot to decoy (transverse coord)" );

	safe_release( outerCsg );
	safe_release( oA );
	safe_release( nestedB );
	safe_release( realLobe );
	safe_release( decoyLobe );
}

//
// Test 15 (external review round 6, item 2 -- P2, REVERTS the r5 margin
// floor bump): AdoptCsgExitFacePayloadViaProbe's margin floor must stay
// at 1e-12 (SURFACE_INTERSEC_ERROR's own scale), NOT the 1e-9 RayCaster
// borrowed "identically" in round 5.  Round 5's reasoning does not apply
// here: RayCaster's consumer-side nudge starts from a point Object::
// IntersectRay publishes SHORT of the true surface (entry bias,
// `range - SURFACE_INTERSEC_ERROR`), so a local->world stretch amplifies
// a real gap the nudge still has to cross.  This probe's `ptExitLocal`
// instead comes from the operand's own `range2`, which Object::
// IntersectRay publishes PAST the true surface (exit bias, `range2 +
// SURFACE_INTERSEC_ERROR`, then `range2` itself is recomputed from that
// already-advanced point) -- the producer-side bias already clears the
// standoff before this function runs, so stacking the bigger (1e-9)
// margin on top doesn't clear anything further; it only inflates the
// same-face acceptance radius (maxAcceptRange, ~2.1e-9 at the r5 floor)
// far past what's needed, wide enough to reach a SECOND, decoy face of
// the operand separated from the true exit by less than ~2e-9 -- exactly
// the tiny-gap adoption bug this test reproduces.
//
// Same nested-CSG-union decoy-lobe idiom as Tests 11/14 (realLobe is
// byte-identical to those tests' realLobe -- box, half-extent 2, spans
// z in [-6,-2], exit face at z=-2), but the decoy gap is shrunk to the
// exact scale that separates the two margin floors: ~5e-10, i.e.
// smaller than the r5 floor's own acceptance window (~2.1e-9) but
// larger than the reverted floor's (~2.1e-12).  decoyLobe is a thin
// (4e-10 deep) slab whose NEAR face (largest z, the one a backward-
// travelling probe reaches first) sits exactly `gap` past realLobe's
// exit, narrower in X/Y (3.6 vs realLobe's 4.0) so its UV mapping is
// provably distinct from realLobe's own (see the discriminating sanity
// check below, same idiom as Tests 11/14).
//
// Numeric derivation (kUlpFactor * dirWeightedAbs ~= 2.8e-14 at this
// ray's |z|~2 magnitude -- negligible next to either floor, so the
// floor alone determines the margin):
//   r5 (buggy) margin    = 1e-9    -> maxAcceptRange ~= 2.1e-9
//     probeOrigin = exitZ + 1e-9 = -1.999999999, which is PAST
//     decoyNearZ (-1.9999999995) by 5e-10 -- backward search finds the
//     DECOY first, well inside the 2.1e-9 acceptance window.  ADOPTS
//     THE DECOY (wrong).
//   reverted (fixed) margin = 1e-12 -> maxAcceptRange ~= 2.1e-12
//     probeOrigin = exitZ + 1e-12 = -1.999999999999, which is BEFORE
//     decoyNearZ by ~5e-10 -- backward search finds the REAL exit face
//     first (~1e-12 away, inside the 2.1e-12 window), never reaching
//     the decoy.  RECOVERS THE REAL FACE (correct).
//   2026-09-05 (debt-25 review, 655f352a): the margin's max() gained the
//   operand primitive's self-hit band term (selfHitFloor), which now
//   dominates: margin ~= 8.06e-12, maxAcceptRange ~= 1.69e-11 -- still
//   ~30x BEFORE the decoy's near face (gap 5e-10), so the same
//   conclusion holds; this test is what bounds that term from above
//   (a floor >= ~2.4e-10 at normal incidence would overshoot here).
//
// Discrimination proven by temporarily restoring the 1e-9 floor in
// CSGObject.cpp, rebuilding, and re-running this test: the money
// assertion (composite payload == real exit face, != decoy) fails --
// see the review round 6 handoff notes for the exact observed values.
//
void TestSubtraction_ExitProbe_TinyGapDecoyDoesNotOverwhelmMarginFloor()
{
	std::cout << "CSG_SUBTRACTION: exit-probe margin floor (1e-12, plus the primitive self-hit band term ~8e-12 since 2026-09-05) doesn't overshoot a tiny-gap decoy face (review r6, item 2)..." << std::endl;

	BoxGeometry* gA = new BoxGeometry( 6.0, 6.0, 6.0 );          // half-extent 3, spans z in [-3,3]
	BoxGeometry* gRealLobe = new BoxGeometry( 4.0, 4.0, 4.0 );   // half-extent (2,2,2) -- identical to Tests 11/14
	// Decoy: thin (4e-10 deep) slab, narrower in X/Y than realLobe so
	// their ptCoord mappings differ too (same discriminating idiom as
	// Tests 11/14).
	const Scalar gap = 5e-10;          // real-exit-to-decoy-near-face separation
	const Scalar decoyDepth = 4e-10;
	BoxGeometry* gDecoyLobe = new BoxGeometry( 3.6, 3.6, decoyDepth );

	Object* oA = new Object( gA );
	Object* realLobe = new Object( gRealLobe );
	Object* decoyLobe = new Object( gDecoyLobe );
	safe_release( gA );
	safe_release( gRealLobe );
	safe_release( gDecoyLobe );

	oA->SetPosition( Point3( 0, 0, 0 ) );
	oA->FinalizeTransformations();

	realLobe->SetPosition( Point3( 0, 0, -4 ) );     // spans z in [-6,-2]: overlaps A's near wall (same as Tests 4/11/14)
	realLobe->FinalizeTransformations();

	// Decoy lobe just OUTSIDE the real exit face (z=-2), separated by the
	// tiny `gap` -- inside the r5-floor's inflated accept window, outside
	// the reverted floor's.  Near face (largest z, hit FIRST by a probe
	// travelling in -Z from just past the exit face) at z = -2 + gap; far
	// face at z = -2 + gap - decoyDepth -- entirely on the near side of
	// the real exit face (z=-2), never overlapping realLobe.
	const Scalar realExitZ = -2.0;
	const Scalar decoyNearZ = realExitZ + gap;
	const Scalar decoyCenterZ = decoyNearZ - decoyDepth * 0.5;
	decoyLobe->SetPosition( Point3( 0, 0, decoyCenterZ ) );
	decoyLobe->FinalizeTransformations();

	CSGObject* nestedB = new CSGObject( CSG_UNION );
	const bool nestedAssigned = nestedB->AssignObjects( realLobe, decoyLobe );
	Check( nestedAssigned, "Test15 (review r6, item2): nestedB takes realLobe/decoyLobe operands" );
	nestedB->FinalizeTransformations();

	CSGObject* outerCsg = new CSGObject( CSG_SUBTRACTION );
	const bool outerAssigned = outerCsg->AssignObjects( oA, nestedB );
	Check( outerAssigned, "Test15 (review r6, item2): outerCsg takes oA/nestedB operands" );
	outerCsg->FinalizeTransformations();

	Ray r( Point3( 0.1, 0.05, -10 ), Vector3( 0, 0, 1 ) );

	RayIntersection ri( r, nullRasterizerState );
	Hit( outerCsg, r, ri );
	Check( ri.geometric.bHit, "Test15 (review r6, item2): (control) ray hits the composite" );

	// Ground truth: the REAL exit face, probed directly (independent of
	// production code) -- same oracle pattern as Tests 4/11/14.
	Ray probeRefReal( Point3( 0.1, 0.05, -1.9 ), Vector3( 0, 0, -1 ) );
	RayIntersection refRealExit( probeRefReal, nullRasterizerState );
	Hit( realLobe, probeRefReal, refRealExit );
	Check( refRealExit.geometric.bHit, "Test15 (review r6, item2): (control) direct probe hits realLobe's exit face" );

	// The WRONG answer an overshooting (r5-floor) probe lands on: the
	// decoy lobe's near face, probed directly.  Probe starts well before
	// (more positive z than) the decoy -- an ordinary, precision-
	// insensitive direct intersection, unrelated to the tiny production
	// margin under test.
	Ray probeDecoy( Point3( 0.1, 0.05, -1.0 ), Vector3( 0, 0, -1 ) );
	RayIntersection refDecoy( probeDecoy, nullRasterizerState );
	Hit( decoyLobe, probeDecoy, refDecoy );
	Check( refDecoy.geometric.bHit, "Test15 (review r6, item2): (control) direct probe hits decoyLobe's near face" );

	// Sanity: the two candidate faces produce distinct UV / object-space
	// points, so this test is discriminating.
	Check( !Point2Close( refRealExit.geometric.ptCoord, refDecoy.geometric.ptCoord ), "Test15 (review r6, item2): (sanity) real-exit/decoy ptCoord are distinct" );
	Check( !PointClose( refRealExit.geometric.ptObjIntersec, refDecoy.geometric.ptObjIntersec ), "Test15 (review r6, item2): (sanity) real-exit/decoy ptObjIntersec are distinct" );

	// The composite's recovered payload must be the REAL exit face, never
	// the decoy -- this is the money assertion.  It FAILS with the r5
	// floor (1e-9) restored (verified manually during development -- see
	// the derivation above), because that margin's accept window
	// (~2.1e-9) comfortably overshoots the 5e-10 gap to the decoy, while
	// the reverted 1e-12 floor's window (~2.1e-12) does not.
	Check( Point2Close( ri.geometric.ptCoord, refRealExit.geometric.ptCoord ), "Test15 (review r6, item2): MONEY ASSERTION -- composite ptCoord matches the REAL exit face" );
	Check( PointClose( ri.geometric.ptObjIntersec, refRealExit.geometric.ptObjIntersec ), "Test15 (review r6, item2): MONEY ASSERTION -- composite ptObjIntersec matches the REAL exit face" );
	Check( !Point2Close( ri.geometric.ptCoord, refDecoy.geometric.ptCoord ), "Test15 (review r6, item2): composite ptCoord did NOT overshoot to the tiny-gap decoy" );
	Check( !PointClose( ri.geometric.ptObjIntersec, refDecoy.geometric.ptObjIntersec ), "Test15 (review r6, item2): composite ptObjIntersec did NOT overshoot to the tiny-gap decoy" );

	safe_release( outerCsg );
	safe_release( oA );
	safe_release( nestedB );
	safe_release( realLobe );
	safe_release( decoyLobe );
}

namespace
{
	// Duplicated from GeometryUVRoundtripTest.cpp's identical helpers
	// (P1-2) -- tests are standalone executables with no shared support
	// library.  Builds a smooth-shaded (per-vertex analytic normal)
	// triangle-mesh approximation of a sphere by reusing
	// SphereGeometry::TessellateToMesh, so vertex positions/normals are
	// exactly what the already-tested tessellator produces, not
	// hand-derived spherical trig.  useFaceNormals=false so per-vertex
	// normal differences are non-degenerate and dndu/dndv come out
	// non-zero (a flat-shaded mesh would test nothing here).
	TriangleMeshGeometryIndexed* BuildCsgTessellatedSphereMesh( Scalar radius, unsigned int detail )
	{
		SphereGeometry* g = new SphereGeometry( radius );
		IndexTriangleListType tris;
		VerticesListType verts;
		NormalsListType norms;
		TexCoordsListType coords;
		const bool ok = g->TessellateToMesh( tris, verts, norms, coords, detail );
		g->release();
		if( !ok ) {
			return 0;
		}

		TriangleMeshGeometryIndexed* pMesh = new TriangleMeshGeometryIndexed( true, false );
		pMesh->BeginIndexedTriangles();
		pMesh->AddVertices( verts );
		pMesh->AddNormals( norms );
		pMesh->AddTexCoords( coords );
		pMesh->AddIndexedTriangles( tris );
		pMesh->DoneIndexedTriangles();
		return pMesh;
	}

	// Non-indexed twin of the helper above (P1-4b): builds a
	// TriangleMeshGeometry (triangle-SOUP, no shared-vertex indices) with
	// the SAME tessellation and per-vertex normals, by de-indexing the
	// already-tested SphereGeometry::TessellateToMesh output rather than
	// hand-deriving spherical trig.  TriangleMeshGeometry's Triangle element
	// (Polygon_Template<3>, see Polygon.h) carries `vertices[3]`,
	// `normals[3]`, `coords[3]` directly PER TRIANGLE -- there is no
	// indexed/shared-vertex layer to be missing, so smooth per-vertex
	// shading (non-degenerate dndu/dndv) is fully expressible with the
	// non-indexed API; nothing here needed to fall back to flat shading.
	TriangleMeshGeometry* BuildCsgTessellatedSphereMeshSoup( Scalar radius, unsigned int detail )
	{
		SphereGeometry* g = new SphereGeometry( radius );
		IndexTriangleListType tris;
		VerticesListType verts;
		NormalsListType norms;
		TexCoordsListType coords;
		const bool ok = g->TessellateToMesh( tris, verts, norms, coords, detail );
		g->release();
		if( !ok ) {
			return 0;
		}

		TriangleMeshGeometry* pMesh = new TriangleMeshGeometry( true );   // double sided
		pMesh->BeginTriangles();
		for( size_t i = 0; i < tris.size(); ++i ) {
			Triangle tri;
			for( int k = 0; k < 3; ++k ) {
				tri.vertices[k] = verts[ tris[i].iVertices[k] ];
				tri.normals[k]  = norms[ tris[i].iNormals[k] ];
				tri.coords[k]   = coords[ tris[i].iCoords[k] ];
			}
			pMesh->AddTriangle( tri );
		}
		pMesh->DoneTriangles();
		return pMesh;
	}

	// Duplicated from GeometryUVRoundtripTest.cpp's identical helper (P1-2).
	Scalar CsgRecordMeanCurvatureH(
		const Vector3& dpdu, const Vector3& dpdv,
		const Vector3& dndu, const Vector3& dndv )
	{
		const Scalar E = Vector3Ops::Dot( dpdu, dpdu );
		const Scalar F = Vector3Ops::Dot( dpdu, dpdv );
		const Scalar G = Vector3Ops::Dot( dpdv, dpdv );
		const Scalar e = Vector3Ops::Dot( dndu, dpdu );
		const Scalar f = 0.5 * ( Vector3Ops::Dot( dndu, dpdv ) + Vector3Ops::Dot( dndv, dpdu ) );
		const Scalar g = Vector3Ops::Dot( dndv, dpdv );
		return ( e * G - 2.0 * f * F + g * E ) / ( 2.0 * ( E * G - F * F ) );
	}

	// Fires a LOCAL-frame ray through `csg`'s own final transform (mirrors
	// GeometryUVRoundtripTest.cpp's HitMeshDerivatives / this file's own
	// TestUnion_TransformedCsgDerivativesMatchStandaloneRotated technique)
	// so the SAME local hit point is reached regardless of what transform
	// is on the CSG object itself.
	bool HitCsgDerivatives(
		CSGObject* csg,
		const Point3& localOrigin, const Vector3& localDir,
		Vector3& dpdu, Vector3& dpdv, Vector3& dndu, Vector3& dndv )
	{
		const Matrix4 mxFinal = csg->GetFinalTransformMatrix();
		const Point3 worldOrigin = Point3Ops::Transform( mxFinal, localOrigin );
		const Vector3 worldDir = Vector3Ops::Transform( mxFinal, localDir );
		const Ray r( worldOrigin, worldDir );

		RayIntersection ri( r, nullRasterizerState );
		Hit( csg, r, ri );
		if( !ri.geometric.bHit || !ri.geometric.derivatives.valid ) {
			return false;
		}
		dpdu = ri.geometric.derivatives.dpdu;
		dpdv = ri.geometric.derivatives.dpdv;
		dndu = ri.geometric.derivatives.dndu;
		dndv = ri.geometric.derivatives.dndv;
		return true;
	}
}

//
// Test 16 (P1-2): CSGObject::IntersectRay's quotient-rule derivatives
// promotion block (P2-d) had ZERO non-rigid coverage -- the only existing
// exercise, TestUnion_TransformedCsgDerivativesMatchStandaloneRotated
// (Test 8), is rotation-only, a transform under which the quotient-rule
// fix is a PROVABLE NO-OP (||M^-T n|| == 1 identically).  This test adds a
// SCALED CSG_UNION: a tessellated triangle-mesh sphere operand (analytic
// primitives like BoxGeometry/SphereGeometry never populate
// derivatives.valid in IntersectRay -- mesh only) unioned with a
// far-away never-hit second operand (this file's own established
// "never hit" pattern from Test 8), with the SCALE applied to the CSG
// OBJECT ITSELF via csg->SetScale(2.0) -- exercising CSGObject.cpp's own
// m_mxInvTranspose quotient-rule block, not just Object.cpp's.
//
// Ground truth: H_identity/H_scaled must equal the scale factor s (world
// mean curvature H_world = H_obj/s under the fixed quotient-rule
// transform); the pre-fix plain inverse-transpose would have given
// H_obj/s^2, i.e. ratio == s^2 -- s=2 separates these by 2x, far outside
// the tight 1e-6*s tolerance either way.
//
void TestUnion_TransformedCsgDerivativesMatchStandaloneScaled()
{
	std::cout << "CSG_UNION: scaled-CSG dndu/dndv quotient-rule matches H_obj/s, not H_obj/s^2 (P1-2)..." << std::endl;

	const Scalar r = 1.5;
	const unsigned int detail = 40;
	const Point3 localOrigin( 0.35, 0.22, 10.0 );
	const Vector3 localDir( 0, 0, -1 );

	Scalar H_identity = 0.0;
	{
		TriangleMeshGeometryIndexed* meshA = BuildCsgTessellatedSphereMesh( r, detail );
		Check( meshA != 0, "Test16 (P1-2): identity mesh built" );
		Object* a = new Object( meshA );
		safe_release( meshA );
		a->FinalizeTransformations();   // A itself carries no extra transform

		SphereGeometry* gB = new SphereGeometry( 1.0 );
		Object* b = new Object( gB );
		safe_release( gB );
		b->SetPosition( Point3( 10000, 10000, 10000 ) );   // never hit
		b->FinalizeTransformations();

		CSGObject* csg = new CSGObject( CSG_UNION );
		const bool assigned = csg->AssignObjects( a, b );
		Check( assigned, "Test16 (P1-2): identity composite takes mesh(A)/far-sphere(B) operands" );
		csg->FinalizeTransformations();   // identity: no scale on the CSG itself

		Vector3 dpdu, dpdv, dndu, dndv;
		const bool hit = HitCsgDerivatives( csg, localOrigin, localDir, dpdu, dpdv, dndu, dndv );
		Check( hit, "Test16 (P1-2): identity CSG hit with valid derivatives" );
		if( hit ) {
			H_identity = fabs( CsgRecordMeanCurvatureH( dpdu, dpdv, dndu, dndv ) );
			Check( H_identity > 0.0, "Test16 (P1-2): identity |H| non-degenerate" );
		}
		safe_release( csg );
		safe_release( a );
		safe_release( b );
	}

	Scalar H_scaled = 0.0;
	{
		const Scalar s = 2.0;
		TriangleMeshGeometryIndexed* meshA = BuildCsgTessellatedSphereMesh( r, detail );
		Object* a = new Object( meshA );
		safe_release( meshA );
		a->FinalizeTransformations();   // A itself STILL carries no extra transform -- the
		                                 // object-space (localOrigin, localDir) ray hits the
		                                 // SAME local point on the SAME byte-identical mesh;
		                                 // only the CSG's OWN transform (below) differs.

		SphereGeometry* gB = new SphereGeometry( 1.0 );
		Object* b = new Object( gB );
		safe_release( gB );
		b->SetPosition( Point3( 10000, 10000, 10000 ) );   // never hit, even after the CSG's own scale
		b->FinalizeTransformations();

		CSGObject* csg = new CSGObject( CSG_UNION );
		const bool assigned = csg->AssignObjects( a, b );
		Check( assigned, "Test16 (P1-2): scaled composite takes mesh(A)/far-sphere(B) operands" );
		csg->SetScale( s );             // scale applied to the CSG OBJECT ITSELF
		csg->FinalizeTransformations();

		Vector3 dpdu, dpdv, dndu, dndv;
		const bool hit = HitCsgDerivatives( csg, localOrigin, localDir, dpdu, dpdv, dndu, dndv );
		Check( hit, "Test16 (P1-2): scaled CSG hit with valid derivatives" );
		if( hit ) {
			H_scaled = fabs( CsgRecordMeanCurvatureH( dpdu, dpdv, dndu, dndv ) );
			Check( H_scaled > 0.0, "Test16 (P1-2): scaled |H| non-degenerate" );

			if( H_identity > 0.0 && H_scaled > 0.0 ) {
				const Scalar ratio = H_identity / H_scaled;
				Check( Close( ratio, s, 1e-6 * s ),
					"Test16 (P1-2): MONEY ASSERTION -- H_identity/H_scaled == s (CSG quotient-rule fix)" );
				// Explicitly exclude the old buggy s^2 ratio (4.0 here) --
				// s vs s^2 differ by 2x, far outside 1e-6*s^2 tolerance
				// either way, so this fails loudly if the bug returns.
				Check( !Close( ratio, s * s, 1e-6 * s * s ),
					"Test16 (P1-2): ratio does NOT match the old buggy CSG s^2 behaviour" );
			}
		}
		safe_release( csg );
		safe_release( a );
		safe_release( b );
	}
}

//
// Test 17 (P1-3): CSG_SUBTRACTION's entry-flip sign bug, regression guard.
// Before P1-1, `ri.geometric.vNormal = -riObjB.geometric.vNormal` (the
// reported shading normal, correctly negated) was paired with
// `ri.geometric.derivatives.{dndu,dndv}` still carrying B's UN-negated
// derivatives (from the whole-record `ri = riObjB` copy) -- a hard sign
// error in reported world curvature on carved cavity walls. This test
// exercises the "inside A, not inside B" branch (CSGObject.cpp, easiest of
// the three fixed branches to set up: ray origin strictly inside operand A,
// outside mesh-sphere B) and asserts SIGN-SENSITIVELY via two independent
// checks: (a) finite-difference consistency between the reported dndu/dndv
// and the empirical Delta(vNormal)/Delta(u) from two nearby CSG hits, and
// (b) the signed mean curvature H flips sign vs. the SAME mesh sphere hit
// as a stand-alone convex object (into-the-cavity concave vs. outward
// convex), with matching |H|.
//
// Discrimination was verified by hand during development (per the task's
// instructions): temporarily commenting out the P1-1 negation at the
// "inside A, not inside B" branch (CSGObject.cpp) reproduces a ~2x FD
// mismatch and a same-sign (convex, not concave) H -- both assertions
// below fail loudly pre-fix and pass post-fix. Do NOT re-disable the fix
// to "verify" this locally; trust the recorded result and git history.
//
void TestSubtraction_CavityWall_DndvSignMatchesFDAndCurvature()
{
	std::cout << "CSG_SUBTRACTION: cavity-wall dndu/dndv sign matches FD + flips signed curvature (P1-3)..." << std::endl;

	const Scalar sphereA = 4.0;             // A: big sphere, radius 4
	const Scalar sphereR = 1.5;             // B: carving mesh sphere, radius 1.5
	const unsigned int detail = 40;

	// A is an analytic SPHERE: CSGObject.cpp's "inside" signal is
	// `range2 == 0` EXACTLY, and RaySphereIntersection.cpp hard-codes
	// `hit.dRange2 = 0.0` for an inside-origin hit (the one-positive-root
	// case; the torus and quadric/ellipsoid intersectors and SDFGeometry
	// do likewise).  A BOX did NOT when this test was written --
	// RayBoxIntersection.cpp reported the (negative) tmin verbatim, which
	// Object::IntersectRay's exit-info promotion converted to a POSITIVE
	// magnitude via `range2 = Magnitude(ptExit - origin)`, never exactly
	// 0, misrouting a box operand into a different switch branch -- but
	// since 2026-09-05 (docs/CLOTH_FABRIC_DESIGN.md debt 25, review
	// round 3) BoxGeometry publishes range2 = 0 for an interior origin
	// too, so the sphere is now a choice, not a requirement.
	SphereGeometry* gA = new SphereGeometry( sphereA );
	Object* oA = new Object( gA );
	safe_release( gA );
	oA->FinalizeTransformations();

	TriangleMeshGeometryIndexed* meshB = BuildCsgTessellatedSphereMesh( sphereR, detail );
	Check( meshB != 0, "Test17 (P1-3): mesh-sphere B built" );
	Object* oB = new Object( meshB );
	safe_release( meshB );
	oB->SetPosition( Point3( 0, 0, 0 ) );   // centered inside A -- carves a spherical cavity
	oB->FinalizeTransformations();

	CSGObject* csg = new CSGObject( CSG_SUBTRACTION );
	const bool assigned = csg->AssignObjects( oA, oB );
	Check( assigned, "Test17 (P1-3): composite takes A(sphere)/B(mesh sphere) operands" );
	csg->FinalizeTransformations();

	// Ray origin strictly INSIDE sphere A (dist < sphereA) and OUTSIDE
	// mesh-sphere B (dist > sphereR): this trips CSGObject.cpp's
	// `riObjA.geometric.range2 == 0` "inside A, not inside B" branch --
	// B's own range2 (a mesh; TriangleMeshGeometryIndexed ignores
	// bComputeExitInfo entirely, so Object::IntersectRay's promotion
	// (`range2 = Magnitude(ptExit - origin)`, with ptExit derived from
	// RISE_INFINITY) converts B's default RISE_INFINITY range2 into EITHER
	// a huge finite magnitude OR a literal +inf, depending on whether the
	// transform's FP arithmetic overflows -- never 0 either way) never
	// satisfies the OTHER branch conditions, so this routing is
	// unambiguous. Fire straight at B's near (cavity) wall from just
	// outside B, well inside A.
	const Point3 origin( 0, 0, -3.0 );      // inside A (dist 3 < 4), outside B (dist 3 > 1.5)
	const Vector3 dir( 0, 0, 1 );
	Ray r( origin, dir );

	RayIntersection ri( r, nullRasterizerState );
	Hit( csg, r, ri );
	Check( ri.geometric.bHit, "Test17 (P1-3): (control) ray hits the composite cavity wall" );
	Check( ri.geometric.derivatives.valid, "Test17 (P1-3): (control) composite derivatives.valid (mesh B)" );

	// Sanity: confirm we exercised the intended branch -- the reported
	// range should match B's own entry range (B's near wall, dist ~1.5),
	// strictly less than A's far wall (dist 4), and range2 should read 0
	// (the "inside A" convention CSGObject.cpp uses throughout).
	RayIntersection refBEntry( r, nullRasterizerState );
	Hit( oB, r, refBEntry );
	Check( refBEntry.geometric.bHit, "Test17 (P1-3): (control) ray hits standalone B" );
	Check( Close( ri.geometric.range, refBEntry.geometric.range, 1e-3 ), "Test17 (P1-3): (sanity) composite range == B's entry range (confirms the exercised branch)" );
	Check( Close( ri.geometric.range2, 0.0, 1e-9 ), "Test17 (P1-3): (sanity) composite range2 == 0 (\"inside A\" branch convention)" );

	if( !ri.geometric.bHit || !ri.geometric.derivatives.valid ) {
		safe_release( csg );
		safe_release( oA );
		safe_release( oB );
		return;
	}

	// ---- Check (a): finite-difference consistency ----
	// Two nearby rays, offset in world X at the SAME z (still inside A,
	// still outside B, still hitting the same cavity-wall region), give an
	// empirical Delta(vNormal)/Delta(x) that the reported dndu (transformed
	// to a per-world-unit derivative alongside dpdu) must agree with in
	// DIRECTION -- the old bug was off by sign (~2x the vector distance),
	// far outside any FD-consistency tolerance.
	const Scalar dx = 1e-4;
	Ray rPlus( Point3( dx, 0, -3.0 ), dir );
	Ray rMinus( Point3( -dx, 0, -3.0 ), dir );
	RayIntersection riPlus( rPlus, nullRasterizerState );
	Hit( csg, rPlus, riPlus );
	RayIntersection riMinus( rMinus, nullRasterizerState );
	Hit( csg, rMinus, riMinus );
	Check( riPlus.geometric.bHit && riMinus.geometric.bHit, "Test17 (P1-3): (control) FD-offset rays hit the composite" );

	if( riPlus.geometric.bHit && riMinus.geometric.bHit ) {
		const Vector3 dN_FD(
			( riPlus.geometric.vNormal.x - riMinus.geometric.vNormal.x ) / ( 2.0 * dx ),
			( riPlus.geometric.vNormal.y - riMinus.geometric.vNormal.y ) / ( 2.0 * dx ),
			( riPlus.geometric.vNormal.z - riMinus.geometric.vNormal.z ) / ( 2.0 * dx ) );

		// Express the reported analytical dN/dx via the chain rule through
		// (dndu, dndv) and the LOCAL (u,v)-per-world-x rate implied by
		// (dpdu, dpdv): solve the 2x2 least-squares system
		// dpdu*du/dx + dpdv*dv/dx ~= (1,0,0) (world X unit step) for
		// (du/dx, dv/dx) via normal equations, then
		// dN/dx ~= dndu*du/dx + dndv*dv/dx. This avoids assuming any
		// particular parameterization scale/orthogonality.
		const Vector3& dpdu = ri.geometric.derivatives.dpdu;
		const Vector3& dpdv = ri.geometric.derivatives.dpdv;
		const Vector3& dndu = ri.geometric.derivatives.dndu;
		const Vector3& dndv = ri.geometric.derivatives.dndv;
		const Vector3 target( 1, 0, 0 );
		const Scalar Muu = Vector3Ops::Dot( dpdu, dpdu );
		const Scalar Muv = Vector3Ops::Dot( dpdu, dpdv );
		const Scalar Mvv = Vector3Ops::Dot( dpdv, dpdv );
		const Scalar bu = Vector3Ops::Dot( dpdu, target );
		const Scalar bv = Vector3Ops::Dot( dpdv, target );
		const Scalar det = Muu * Mvv - Muv * Muv;
		Check( fabs( det ) > 1e-12, "Test17 (P1-3): (control) dpdu/dpdv normal-equations system non-degenerate" );
		if( fabs( det ) > 1e-12 ) {
			const Scalar duDx = ( bu * Mvv - bv * Muv ) / det;
			const Scalar dvDx = ( bv * Muu - bu * Muv ) / det;
			const Vector3 dN_analytic(
				dndu.x * duDx + dndv.x * dvDx,
				dndu.y * duDx + dndv.y * dvDx,
				dndu.z * duDx + dndv.z * dvDx );

			// Sign-sensitive: dot product of FD and analytical dN/dx must be
			// POSITIVE (same general direction) -- the pre-fix bug flips
			// dndu/dndv's sign, which flips this dot product negative.
			const Scalar dot = Vector3Ops::Dot( dN_FD, dN_analytic );
			Check( dot > 0.0, "Test17 (P1-3): MONEY ASSERTION -- FD dN/dx and reported dndu/dndv agree in sign" );
		}
	}

	// ---- Check (b): signed curvature flips vs. the same mesh sphere as a
	// stand-alone convex object ----
	// Reference: fire the mirror-image ray at the SAME sphere B, standing
	// alone (no CSG, no box, no subtraction) -- from OUTSIDE, straight in,
	// landing on the SAME local mesh point (sphere is centered at the
	// origin in both cases, so the local hit geometry at world (0,0,-1.5)
	// approached along +Z is identical either way).
	Ray refRay( Point3( 0, 0, -10.0 ), dir );
	RayIntersection refRi( refRay, nullRasterizerState );
	Hit( oB, refRay, refRi );
	Check( refRi.geometric.bHit, "Test17 (P1-3): (control) standalone-B reference ray hits" );
	Check( refRi.geometric.derivatives.valid, "Test17 (P1-3): (control) standalone-B reference derivatives.valid" );

	if( refRi.geometric.bHit && refRi.geometric.derivatives.valid ) {
		Check( PointClose( ri.geometric.ptObjIntersec, refRi.geometric.ptObjIntersec, 1e-3 ),
			"Test17 (P1-3): (sanity) composite and standalone-B reference land on the SAME local mesh point" );

		const Scalar H_composite = CsgRecordMeanCurvatureH(
			ri.geometric.derivatives.dpdu, ri.geometric.derivatives.dpdv,
			ri.geometric.derivatives.dndu, ri.geometric.derivatives.dndv );
		const Scalar H_standalone = CsgRecordMeanCurvatureH(
			refRi.geometric.derivatives.dpdu, refRi.geometric.derivatives.dpdv,
			refRi.geometric.derivatives.dndu, refRi.geometric.derivatives.dndv );

		Check( fabs( H_composite ) > 1e-6 && fabs( H_standalone ) > 1e-6,
			"Test17 (P1-3): (control) both signed curvatures non-degenerate" );
		// Opposite sign (concave cavity wall vs. convex stand-alone sphere).
		Check( ( H_composite > 0.0 ) != ( H_standalone > 0.0 ),
			"Test17 (P1-3): MONEY ASSERTION -- composite signed H has OPPOSITE sign from standalone convex sphere" );
		// Same magnitude, up to mesh discretization tolerance -- same mesh,
		// same local point, only the reported normal orientation differs.
		Check( Close( fabs( H_composite ), fabs( H_standalone ), 0.02 * fabs( H_standalone ) ),
			"Test17 (P1-3): composite |H| matches standalone |H| (same mesh, same point, up to mesh tolerance)" );
	}

	safe_release( csg );
	safe_release( oA );
	safe_release( oB );
}

//
// Test 18 (P1-A item 1): CSG_SUBTRACTION's "inside both A and B" entry-flip
// branch (CSGObject.cpp ~line 876) -- the third of the three P1-1
// dndu/dndv-negation branches, and the one with NO existing coverage at
// all (Test 17 covers "inside A, not B"; the sibling below covers
// "inside B, not A").
//
// REACHABILITY: this branch requires BOTH riObjA.range2==0 AND
// riObjB.range2==0.  Only ANALYTIC geometries report range2=0 for an
// inside-origin hit (the sphere, torus, and quadric/ellipsoid
// intersectors hard-code it; SDFGeometry sets it directly; BoxGeometry
// since 2026-09-05);
// TriangleMeshGeometry(Indexed)::IntersectRay ignores bComputeExitInfo
// entirely, so a mesh operand's range2 never reads as exactly 0.
//
// UPDATED 2026-08-29 (geometry-shading-signals Phase 1): this note used to
// continue "and no analytic geometry populates derivatives.valid, so the
// dndu/dndv negation inside this branch is UNREACHABLE code".  The analytic
// primitives now publish their closed-form Weingarten map at intersection
// time (design doc 5.4), so the negation is REACHABLE -- and since the
// geometries that reach this branch are exactly the ones that gained the
// population, this test's two NESTED analytic spheres now cover BOTH halves:
// the vNormal/vGeomNormal negation AND the dndu/dndv pairing, with a signed
// curvature check on top (the carved cavity wall must read concave).
//
void TestSubtraction_InsideBothAAndB_NormalNegationRoutesThroughBranch()
{
	std::cout << "CSG_SUBTRACTION: \"inside both A and B\" branch negates vNormal (P1-A item 1)..." << std::endl;

	// A: big sphere, radius 4. B: smaller NESTED sphere, radius 2, same
	// center -- so any point inside B is automatically inside A too.
	SphereGeometry* gA = new SphereGeometry( 4.0 );
	Object* oA = new Object( gA );
	safe_release( gA );
	oA->FinalizeTransformations();

	SphereGeometry* gB = new SphereGeometry( 2.0 );
	Object* oB = new Object( gB );
	safe_release( gB );
	oB->FinalizeTransformations();

	CSGObject* csg = new CSGObject( CSG_SUBTRACTION );
	const bool assigned = csg->AssignObjects( oA, oB );
	Check( assigned, "Test18 (P1-A item1): composite takes A(sphere r4)/B(nested sphere r2) operands" );
	csg->FinalizeTransformations();

	// Origin strictly inside BOTH spheres (dist 1 < 2 < 4).
	const Point3 origin( 0, 0, -1.0 );
	const Vector3 dir( 0, 0, 1 );
	Ray r( origin, dir );

	RayIntersection ri( r, nullRasterizerState );
	Hit( csg, r, ri );
	Check( ri.geometric.bHit, "Test18 (P1-A item1): (control) ray hits the composite" );

	// Reference: standalone hits on A and B alone confirm which branch fired.
	RayIntersection refA( r, nullRasterizerState );
	Hit( oA, r, refA );
	RayIntersection refB( r, nullRasterizerState );
	Hit( oB, r, refB );
	Check( refA.geometric.bHit && Close( refA.geometric.range2, 0.0, 1e-9 ),
		"Test18 (P1-A item1): (control) standalone A reports \"inside\" (range2==0)" );
	Check( refB.geometric.bHit && Close( refB.geometric.range2, 0.0, 1e-9 ),
		"Test18 (P1-A item1): (control) standalone B reports \"inside\" (range2==0)" );
	Check( refB.geometric.range < refA.geometric.range,
		"Test18 (P1-A item1): (control) B's exit is nearer than A's exit (branch's inner `if` condition)" );

	// This routing means B is analytic -- derivatives are NEVER valid here.
	// This is the empirical half of the reachability conclusion above.
	// UPDATED 2026-08-29 (geometry-shading-signals Phase 1): this assertion
	// used to read "derivatives.valid is FALSE -- sphere B never populates
	// derivatives, so the dndu/dndv negation in this branch is unreachable".
	// The analytic primitives now publish their closed-form Weingarten map at
	// intersection time (design doc 5.4), so the branch IS reachable with
	// valid derivatives -- which turns this from a documented gap into real
	// coverage of the P1-1 negation.
	Check( refB.geometric.derivatives.valid,
		"Test18 (P1-A item1): (control) standalone analytic B publishes derivatives" );
	Check( ri.geometric.derivatives.valid,
		"Test18 (P1-A item1): composite carries B's derivatives through the entry-flip branch" );
	if( ri.geometric.derivatives.valid && refB.geometric.derivatives.valid ) {
		// The reported normal is -B.vNormal, so the derivatives OF that normal
		// field must be negated with it.  dpdu/dpdv deliberately are NOT (that
		// would reparameterize the surface -- docs/GEOMETRY_DERIVATIVES.md's
		// CSG-subtraction exception).
		Check( VecClose( ri.geometric.derivatives.dndu, -refB.geometric.derivatives.dndu, 1e-6 ),
			"Test18 (P1-A item1): MONEY ASSERTION -- dndu negated in step with vNormal (P1-1)" );
		Check( VecClose( ri.geometric.derivatives.dndv, -refB.geometric.derivatives.dndv, 1e-6 ),
			"Test18 (P1-A item1): MONEY ASSERTION -- dndv negated in step with vNormal (P1-1)" );
		Check( VecClose( ri.geometric.derivatives.dpdu, refB.geometric.derivatives.dpdu, 1e-6 ),
			"Test18 (P1-A item1): dpdu UNCHANGED (no reparameterization -- the documented CSG exception)" );
		Check( VecClose( ri.geometric.derivatives.dpdv, refB.geometric.derivatives.dpdv, 1e-6 ),
			"Test18 (P1-A item1): dpdv UNCHANGED (no reparameterization)" );

		// The user-visible payoff of that pairing: signed mean curvature reads
		// CONCAVE on the carved cavity wall and CONVEX on the same sphere
		// standing alone.  Without the negation both would read convex.
		Scalar Hcavity = 0, Hstandalone = 0;
		const bool okC = SurfaceCurvature::MeanCurvatureFromDerivatives(
			ri.geometric.derivatives.dpdu, ri.geometric.derivatives.dpdv,
			ri.geometric.derivatives.dndu, ri.geometric.derivatives.dndv, Hcavity );
		const bool okS = SurfaceCurvature::MeanCurvatureFromDerivatives(
			refB.geometric.derivatives.dpdu, refB.geometric.derivatives.dpdv,
			refB.geometric.derivatives.dndu, refB.geometric.derivatives.dndv, Hstandalone );
		Check( okC && okS, "Test18 (P1-A item1): curvature well-defined on both records" );
		Check( okC && Hcavity < 0, "Test18 (P1-A item1): carved cavity wall reads CONCAVE (H < 0)" );
		Check( okS && Hstandalone > 0, "Test18 (P1-A item1): the same sphere standing alone reads CONVEX (H > 0)" );
	}

	if( ri.geometric.bHit ) {
		// MONEY ASSERTIONS: range == B's exit range, range2 == A's own
		// "range" field (A's exit distance, since A also reports "inside"),
		// and vNormal/vGeomNormal are B's exit-point normal, NEGATED.
		Check( Close( ri.geometric.range, refB.geometric.range, 1e-3 ),
			"Test18 (P1-A item1): (sanity) composite range == B's exit range" );
		Check( Close( ri.geometric.range2, refA.geometric.range, 1e-3 ),
			"Test18 (P1-A item1): (sanity) composite range2 == A's own \"range\" field (A's exit distance)" );
		Check( VecClose( ri.geometric.vNormal, -refB.geometric.vNormal, 1e-6 ),
			"Test18 (P1-A item1): MONEY ASSERTION -- composite vNormal == -(B's exit normal)" );
		Check( VecClose( ri.geometric.vGeomNormal, -refB.geometric.vGeomNormal, 1e-6 ),
			"Test18 (P1-A item1): MONEY ASSERTION -- composite vGeomNormal == -(B's exit geometric normal)" );
	}

	safe_release( csg );
	safe_release( oA );
	safe_release( oB );
}

//
// Test 19 (P1-A item 2): CSG_SUBTRACTION's "inside B, not A" entry-flip
// branch (CSGObject.cpp ~line 920) -- the second of the three P1-1
// dndu/dndv-negation branches without existing coverage.
//
// REACHABILITY: same reasoning as Test 18 -- this branch requires
// riObjB.range2==0 (origin inside B), which only an ANALYTIC B
// (sphere/torus/quadric/SDF) can report.  UPDATED 2026-08-29: those
// geometries now DO publish derivatives at intersection time, so the
// dndu/dndv negation here is REACHABLE and this test now asserts it
// directly.  The historical note read: UNREACHABLE today under the
// same analysis.  This test routes through the branch with an analytic
// sphere B (nested inside a farther, off-center sphere A) to exercise the
// reachable vNormal/vGeomNormal-negation half.
//
void TestSubtraction_InsideBNotA_NormalNegationRoutesThroughBranch()
{
	std::cout << "CSG_SUBTRACTION: \"inside B, not A\" branch negates vNormal (P1-A item 2)..." << std::endl;

	// B: sphere radius 2, centered at the origin -- origin (0,0,-1) starts
	// inside B.
	SphereGeometry* gB = new SphereGeometry( 2.0 );
	Object* oB = new Object( gB );
	safe_release( gB );
	oB->FinalizeTransformations();

	// A: small sphere radius 1.5, centered further along +Z (3,0,0 in Z)
	// so the origin (0,0,-1) starts OUTSIDE A, A is entered WHILE still
	// inside B (A's entry z=1.5 < B's exit z=2), and A's exit (z=4.5) is
	// AFTER B's exit (z=2) -- so at B's exit the ray is still inside A,
	// giving a genuine A-minus-B solid boundary there (not an A wholly
	// swallowed inside B, which would make A-minus-B empty).
	SphereGeometry* gA = new SphereGeometry( 1.5 );
	Object* oA = new Object( gA );
	safe_release( gA );
	oA->SetPosition( Point3( 0, 0, 3.0 ) );
	oA->FinalizeTransformations();

	CSGObject* csg = new CSGObject( CSG_SUBTRACTION );
	const bool assigned = csg->AssignObjects( oA, oB );
	Check( assigned, "Test19 (P1-A item2): composite takes A(offset sphere r1.5)/B(sphere r2) operands" );
	csg->FinalizeTransformations();

	const Point3 origin( 0, 0, -1.0 );
	const Vector3 dir( 0, 0, 1 );
	Ray r( origin, dir );

	RayIntersection ri( r, nullRasterizerState );
	Hit( csg, r, ri );
	Check( ri.geometric.bHit, "Test19 (P1-A item2): (control) ray hits the composite" );

	RayIntersection refA( r, nullRasterizerState );
	Hit( oA, r, refA );
	RayIntersection refB( r, nullRasterizerState );
	Hit( oB, r, refB );
	Check( refA.geometric.bHit && !Close( refA.geometric.range2, 0.0, 1e-9 ),
		"Test19 (P1-A item2): (control) standalone A is a normal outside hit (range2 != 0)" );
	Check( refB.geometric.bHit && Close( refB.geometric.range2, 0.0, 1e-9 ),
		"Test19 (P1-A item2): (control) standalone B reports \"inside\" (range2==0)" );
	Check( refA.geometric.range < refB.geometric.range,
		"Test19 (P1-A item2): (control) A's entry precedes B's exit (branch's inner `if` condition)" );
	Check( refA.geometric.range2 > refB.geometric.range,
		"Test19 (P1-A item2): (control) A's exit is AFTER B's exit -- A is not wholly swallowed inside B" );

	// UPDATED 2026-08-29 (geometry-shading-signals Phase 1): this assertion
	// used to read "derivatives.valid is FALSE -- sphere B never populates
	// derivatives, so the dndu/dndv negation in this branch is unreachable".
	// The analytic primitives now publish their closed-form Weingarten map at
	// intersection time (design doc 5.4), so the branch IS reachable with
	// valid derivatives -- which turns this from a documented gap into real
	// coverage of the P1-1 negation.
	Check( refB.geometric.derivatives.valid,
		"Test19 (P1-A item2): (control) standalone analytic B publishes derivatives" );
	Check( ri.geometric.derivatives.valid,
		"Test19 (P1-A item2): composite carries B's derivatives through the entry-flip branch" );
	if( ri.geometric.derivatives.valid && refB.geometric.derivatives.valid ) {
		// The reported normal is -B.vNormal, so the derivatives OF that normal
		// field must be negated with it.  dpdu/dpdv deliberately are NOT (that
		// would reparameterize the surface -- docs/GEOMETRY_DERIVATIVES.md's
		// CSG-subtraction exception).
		Check( VecClose( ri.geometric.derivatives.dndu, -refB.geometric.derivatives.dndu, 1e-6 ),
			"Test19 (P1-A item2): MONEY ASSERTION -- dndu negated in step with vNormal (P1-1)" );
		Check( VecClose( ri.geometric.derivatives.dndv, -refB.geometric.derivatives.dndv, 1e-6 ),
			"Test19 (P1-A item2): MONEY ASSERTION -- dndv negated in step with vNormal (P1-1)" );
		Check( VecClose( ri.geometric.derivatives.dpdu, refB.geometric.derivatives.dpdu, 1e-6 ),
			"Test19 (P1-A item2): dpdu UNCHANGED (no reparameterization -- the documented CSG exception)" );
		Check( VecClose( ri.geometric.derivatives.dpdv, refB.geometric.derivatives.dpdv, 1e-6 ),
			"Test19 (P1-A item2): dpdv UNCHANGED (no reparameterization)" );

		// The user-visible payoff of that pairing: signed mean curvature reads
		// CONCAVE on the carved cavity wall and CONVEX on the same sphere
		// standing alone.  Without the negation both would read convex.
		Scalar Hcavity = 0, Hstandalone = 0;
		const bool okC = SurfaceCurvature::MeanCurvatureFromDerivatives(
			ri.geometric.derivatives.dpdu, ri.geometric.derivatives.dpdv,
			ri.geometric.derivatives.dndu, ri.geometric.derivatives.dndv, Hcavity );
		const bool okS = SurfaceCurvature::MeanCurvatureFromDerivatives(
			refB.geometric.derivatives.dpdu, refB.geometric.derivatives.dpdv,
			refB.geometric.derivatives.dndu, refB.geometric.derivatives.dndv, Hstandalone );
		Check( okC && okS, "Test19 (P1-A item2): curvature well-defined on both records" );
		Check( okC && Hcavity < 0, "Test19 (P1-A item2): carved cavity wall reads CONCAVE (H < 0)" );
		Check( okS && Hstandalone > 0, "Test19 (P1-A item2): the same sphere standing alone reads CONVEX (H > 0)" );
	}

	if( ri.geometric.bHit ) {
		// MONEY ASSERTIONS: range == B's exit range (unchanged from the
		// `ri = riObjB` copy), range2 == A's own exit range2, and
		// vNormal/vGeomNormal are B's exit-point normal, NEGATED.
		Check( Close( ri.geometric.range, refB.geometric.range, 1e-3 ),
			"Test19 (P1-A item2): (sanity) composite range == B's exit range" );
		Check( Close( ri.geometric.range2, refA.geometric.range2, 1e-3 ),
			"Test19 (P1-A item2): (sanity) composite range2 == A's exit range2" );
		Check( VecClose( ri.geometric.vNormal, -refB.geometric.vNormal, 1e-6 ),
			"Test19 (P1-A item2): MONEY ASSERTION -- composite vNormal == -(B's exit normal)" );
		Check( VecClose( ri.geometric.vGeomNormal, -refB.geometric.vGeomNormal, 1e-6 ),
			"Test19 (P1-A item2): MONEY ASSERTION -- composite vGeomNormal == -(B's exit geometric normal)" );
	}

	safe_release( csg );
	safe_release( oA );
	safe_release( oB );
}

//
// Test 20 (P1-B item 1): TriangleMeshGeometryIndexed's double-sided
// back-face flip (TriangleMeshGeometryIndexed.cpp ~line 173-188) negates
// vNormal AND (if valid) dndu/dndv together when a ray hits the mesh from
// INSIDE (Dot(vNormal, dir) > 0 before the flip).  No existing test in
// this repo fires an interior ray at a mesh -- every prior mesh test hits
// from outside.  Fire from just off-center inside a tessellated mesh
// sphere, straight out to the far wall: the interior hit trips the flip.
//
void TestMeshIndexed_BackFaceHit_DndvSignMatchesFDAndCurvature()
{
	std::cout << "TriangleMeshGeometryIndexed: back-face (interior-ray) hit flips vNormal+dndu/dndv together (P1-B item1)..." << std::endl;

	const Scalar rad = 1.5;
	const unsigned int detail = 40;

	TriangleMeshGeometryIndexed* mesh = BuildCsgTessellatedSphereMesh( rad, detail );
	Check( mesh != 0, "Test20 (P1-B item1): mesh sphere built" );
	Object* o = new Object( mesh );
	safe_release( mesh );
	o->FinalizeTransformations();

	// Interior origin, off-center (avoids the pole singularity); direction
	// +Z hits the far wall FROM INSIDE.
	const Point3 origin( 0.3, 0.2, 0.0 );
	const Vector3 dir( 0, 0, 1 );
	Ray r0( origin, dir );

	RayIntersection ri( r0, nullRasterizerState );
	Hit( o, r0, ri );
	Check( ri.geometric.bHit, "Test20 (P1-B item1): (control) interior ray hits the far wall" );
	Check( ri.geometric.derivatives.valid, "Test20 (P1-B item1): (control) derivatives.valid" );
	// Confirms this really is a back-face hit that tripped the flip (see
	// TriangleMeshGeometryIndexed.cpp:197-201 -- bGeomNormalOrientedToRay
	// is set true exactly when the geometric-normal flip fired).
	Check( ri.geometric.bGeomNormalOrientedToRay,
		"Test20 (P1-B item1): (control) bGeomNormalOrientedToRay confirms a back-face hit" );

	if( !ri.geometric.bHit || !ri.geometric.derivatives.valid ) {
		safe_release( o );
		return;
	}

	// ---- Check (a): finite-difference consistency (Test17's technique) ----
	const Scalar dx = 1e-4;
	Ray rPlus( Point3( 0.3 + dx, 0.2, 0.0 ), dir );
	Ray rMinus( Point3( 0.3 - dx, 0.2, 0.0 ), dir );
	RayIntersection riPlus( rPlus, nullRasterizerState );
	Hit( o, rPlus, riPlus );
	RayIntersection riMinus( rMinus, nullRasterizerState );
	Hit( o, rMinus, riMinus );
	Check( riPlus.geometric.bHit && riMinus.geometric.bHit,
		"Test20 (P1-B item1): (control) FD-offset interior rays hit the far wall" );

	if( riPlus.geometric.bHit && riMinus.geometric.bHit ) {
		const Vector3 dN_FD(
			( riPlus.geometric.vNormal.x - riMinus.geometric.vNormal.x ) / ( 2.0 * dx ),
			( riPlus.geometric.vNormal.y - riMinus.geometric.vNormal.y ) / ( 2.0 * dx ),
			( riPlus.geometric.vNormal.z - riMinus.geometric.vNormal.z ) / ( 2.0 * dx ) );

		const Vector3& dpdu = ri.geometric.derivatives.dpdu;
		const Vector3& dpdv = ri.geometric.derivatives.dpdv;
		const Vector3& dndu = ri.geometric.derivatives.dndu;
		const Vector3& dndv = ri.geometric.derivatives.dndv;
		const Vector3 target( 1, 0, 0 );
		const Scalar Muu = Vector3Ops::Dot( dpdu, dpdu );
		const Scalar Muv = Vector3Ops::Dot( dpdu, dpdv );
		const Scalar Mvv = Vector3Ops::Dot( dpdv, dpdv );
		const Scalar bu = Vector3Ops::Dot( dpdu, target );
		const Scalar bv = Vector3Ops::Dot( dpdv, target );
		const Scalar det = Muu * Mvv - Muv * Muv;
		Check( fabs( det ) > 1e-12, "Test20 (P1-B item1): (control) dpdu/dpdv normal-equations system non-degenerate" );
		if( fabs( det ) > 1e-12 ) {
			const Scalar duDx = ( bu * Mvv - bv * Muv ) / det;
			const Scalar dvDx = ( bv * Muu - bu * Muv ) / det;
			const Vector3 dN_analytic(
				dndu.x * duDx + dndv.x * dvDx,
				dndu.y * duDx + dndv.y * dvDx,
				dndu.z * duDx + dndv.z * dvDx );
			const Scalar dot = Vector3Ops::Dot( dN_FD, dN_analytic );
			Check( dot > 0.0, "Test20 (P1-B item1): MONEY ASSERTION -- FD dN/dx and reported (flipped) dndu/dndv agree in sign" );
		}
	}

	// ---- Check (b): signed curvature flips vs. a FRONT-face hit of the
	// SAME mesh, fired from outside (no flip fires there) ----
	Ray refRay( Point3( 0, 0, -10.0 ), dir );
	RayIntersection refRi( refRay, nullRasterizerState );
	Hit( o, refRay, refRi );
	Check( refRi.geometric.bHit, "Test20 (P1-B item1): (control) front-face reference ray hits" );
	Check( refRi.geometric.derivatives.valid, "Test20 (P1-B item1): (control) front-face reference derivatives.valid" );
	Check( !refRi.geometric.bGeomNormalOrientedToRay,
		"Test20 (P1-B item1): (control) front-face reference did NOT flip (confirms it's the un-flipped twin)" );

	if( refRi.geometric.bHit && refRi.geometric.derivatives.valid ) {
		const Scalar H_back = CsgRecordMeanCurvatureH(
			ri.geometric.derivatives.dpdu, ri.geometric.derivatives.dpdv,
			ri.geometric.derivatives.dndu, ri.geometric.derivatives.dndv );
		const Scalar H_front = CsgRecordMeanCurvatureH(
			refRi.geometric.derivatives.dpdu, refRi.geometric.derivatives.dpdv,
			refRi.geometric.derivatives.dndu, refRi.geometric.derivatives.dndv );

		Check( fabs( H_back ) > 1e-6 && fabs( H_front ) > 1e-6,
			"Test20 (P1-B item1): (control) both signed curvatures non-degenerate" );
		Check( ( H_back > 0.0 ) != ( H_front > 0.0 ),
			"Test20 (P1-B item1): MONEY ASSERTION -- back-face signed H has OPPOSITE sign from front-face" );
		Check( Close( fabs( H_back ), fabs( H_front ), 0.05 * fabs( H_front ) ),
			"Test20 (P1-B item1): back-face |H| matches front-face |H| (same mesh, up to discretization tolerance)" );
	}

	safe_release( o );
}

//
// Test 21 (P1-B item 2): non-indexed TriangleMeshGeometry twin of Test 20.
// Also gives TriangleMeshGeometry its first-ever end-to-end coverage of
// the quotient-rule derivatives path in this repo (front-face sanity), on
// top of the back-face sign check.
//
void TestMeshNonIndexed_FrontAndBackFace_DndvSign()
{
	std::cout << "TriangleMeshGeometry (non-indexed): front-face sanity + back-face sign flip (P1-B item2)..." << std::endl;

	const Scalar rad = 1.5;
	const unsigned int detail = 40;

	TriangleMeshGeometry* mesh = BuildCsgTessellatedSphereMeshSoup( rad, detail );
	Check( mesh != 0, "Test21 (P1-B item2): non-indexed mesh sphere built" );
	Object* o = new Object( mesh );
	safe_release( mesh );
	o->FinalizeTransformations();

	// ---- Front-face sanity: standard outside hit, |H| close to analytic
	// 1/r, first-ever exercise of this class's quotient-rule path. ----
	const Vector3 dir( 0, 0, 1 );
	Ray frontRay( Point3( 0, 0, -10.0 ), dir );
	RayIntersection frontRi( frontRay, nullRasterizerState );
	Hit( o, frontRay, frontRi );
	Check( frontRi.geometric.bHit, "Test21 (P1-B item2): (control) front-face ray hits" );
	Check( frontRi.geometric.derivatives.valid, "Test21 (P1-B item2): (control) non-indexed mesh sets derivatives.valid" );
	Check( !frontRi.geometric.bGeomNormalOrientedToRay,
		"Test21 (P1-B item2): (control) front-face hit did NOT flip" );

	if( frontRi.geometric.bHit && frontRi.geometric.derivatives.valid ) {
		const Scalar H_front = fabs( CsgRecordMeanCurvatureH(
			frontRi.geometric.derivatives.dpdu, frontRi.geometric.derivatives.dpdv,
			frontRi.geometric.derivatives.dndu, frontRi.geometric.derivatives.dndv ) );
		Check( Close( H_front, 1.0 / rad, 0.05 * (1.0/rad) ),
			"Test21 (P1-B item2): non-indexed front-face |H| close to analytic sphere 1/r" );
		const Vector3& n = frontRi.geometric.vNormal;
		Check( fabs( Vector3Ops::Dot( frontRi.geometric.derivatives.dndu, n ) ) < 1e-6,
			"Test21 (P1-B item2): non-indexed dndu . n ~= 0" );
		Check( fabs( Vector3Ops::Dot( frontRi.geometric.derivatives.dndv, n ) ) < 1e-6,
			"Test21 (P1-B item2): non-indexed dndv . n ~= 0" );
	}

	// ---- Back-face sign test: same interior-ray technique as Test 20. ----
	const Point3 origin( 0.3, 0.2, 0.0 );
	Ray r0( origin, dir );
	RayIntersection ri( r0, nullRasterizerState );
	Hit( o, r0, ri );
	Check( ri.geometric.bHit, "Test21 (P1-B item2): (control) interior ray hits the far wall" );
	Check( ri.geometric.derivatives.valid, "Test21 (P1-B item2): (control) back-face derivatives.valid" );
	Check( ri.geometric.bGeomNormalOrientedToRay,
		"Test21 (P1-B item2): (control) bGeomNormalOrientedToRay confirms a back-face hit" );

	if( ri.geometric.bHit && ri.geometric.derivatives.valid && frontRi.geometric.bHit && frontRi.geometric.derivatives.valid ) {
		const Scalar H_back = CsgRecordMeanCurvatureH(
			ri.geometric.derivatives.dpdu, ri.geometric.derivatives.dpdv,
			ri.geometric.derivatives.dndu, ri.geometric.derivatives.dndv );
		const Scalar H_front_signed = CsgRecordMeanCurvatureH(
			frontRi.geometric.derivatives.dpdu, frontRi.geometric.derivatives.dpdv,
			frontRi.geometric.derivatives.dndu, frontRi.geometric.derivatives.dndv );

		Check( fabs( H_back ) > 1e-6 && fabs( H_front_signed ) > 1e-6,
			"Test21 (P1-B item2): (control) both signed curvatures non-degenerate" );
		Check( ( H_back > 0.0 ) != ( H_front_signed > 0.0 ),
			"Test21 (P1-B item2): MONEY ASSERTION -- back-face signed H has OPPOSITE sign from front-face (non-indexed twin)" );
		Check( Close( fabs( H_back ), fabs( H_front_signed ), 0.05 * fabs( H_front_signed ) ),
			"Test21 (P1-B item2): back-face |H| matches front-face |H| (same mesh, up to discretization tolerance)" );
	}

	safe_release( o );
}

//
// Test 22 (P2-C): CSGObject::IntersectRay's dndu/dndv quotient-rule
// projection block (CSGObject.cpp ~1220-1232) previously had UNIFORM-scale
// coverage only (Test 16, csg->SetScale).  Uniform scale cannot
// discriminate whether the projection-onto-tangent-plane step actually
// ran: under uniform scale s, M^-T is a scalar multiple of the identity,
// and a scalar multiple trivially preserves orthogonality --
// dndu_obj . n_obj == 0 (dndu is a derivative of a UNIT normal field)
// implies (s * dndu_obj) . (s * n_obj)-direction is STILL proportional to
// dndu_obj . n_obj == 0, with or without the explicit projection.  A
// NON-uniform stretch has no such shortcut: skipping the projection would
// generally leave dndu_lin . n_w nonzero, because (M^-T u) . (M^-T v) != u.v
// in general (only the PAIRED transform (Mu).(M^-Tv) == u.v holds).  This
// test exercises the discriminating non-uniform case on a mesh operand hit
// and asserts the full orthogonality + finiteness invariant set
// (docs/GEOMETRY_DERIVATIVES.md).  Also covers the CSG singular-guard
// contract (derivatives.valid == false under a near-degenerate transform)
// as a bonus, cheap addition.
//
void TestUnion_CsgQuotientRule_NonUniformStretch_OrthogonalityInvariants()
{
	std::cout << "CSG_UNION: non-uniform-stretch dndu/dndv/dpdu/dpdv orthogonality invariants (P2-C)..." << std::endl;

	const Scalar rad = 1.5;
	const unsigned int detail = 40;
	const Point3 localOrigin( 0.35, 0.22, 10.0 );
	const Vector3 localDir( 0, 0, -1 );

	TriangleMeshGeometryIndexed* meshA = BuildCsgTessellatedSphereMesh( rad, detail );
	Check( meshA != 0, "Test22 (P2-C): mesh A built" );
	Object* a = new Object( meshA );
	safe_release( meshA );
	a->FinalizeTransformations();

	SphereGeometry* gB = new SphereGeometry( 1.0 );
	Object* b = new Object( gB );
	safe_release( gB );
	b->SetPosition( Point3( 10000, 10000, 10000 ) );   // never hit
	b->FinalizeTransformations();

	CSGObject* csg = new CSGObject( CSG_UNION );
	const bool assigned = csg->AssignObjects( a, b );
	Check( assigned, "Test22 (P2-C): composite takes mesh(A)/far-sphere(B) operands" );
	csg->SetStretch( Vector3( 2.0, 0.5, 1.25 ) );   // NON-uniform -- the discriminating case
	csg->FinalizeTransformations();

	const Matrix4 mxFinal = csg->GetFinalTransformMatrix();
	const Point3 worldOrigin = Point3Ops::Transform( mxFinal, localOrigin );
	const Vector3 worldDir = Vector3Ops::Transform( mxFinal, localDir );
	const Ray r0( worldOrigin, worldDir );

	RayIntersection ri( r0, nullRasterizerState );
	Hit( csg, r0, ri );
	Check( ri.geometric.bHit, "Test22 (P2-C): (control) non-uniform-stretched CSG hit" );
	Check( ri.geometric.derivatives.valid, "Test22 (P2-C): (control) derivatives.valid" );

	if( ri.geometric.bHit && ri.geometric.derivatives.valid ) {
		const Vector3& n = ri.geometric.vNormal;
		const Vector3& dpdu = ri.geometric.derivatives.dpdu;
		const Vector3& dpdv = ri.geometric.derivatives.dpdv;
		const Vector3& dndu = ri.geometric.derivatives.dndu;
		const Vector3& dndv = ri.geometric.derivatives.dndv;

		Check( Vector3Ops::SquaredModulus( dpdu ) > 1e-8 && Vector3Ops::SquaredModulus( dndu ) > 1e-8,
			"Test22 (P2-C): (sanity) dpdu/dndu are non-degenerate (a zero vector would trivially pass orthogonality)" );

		Check( std::isfinite( n.x ) && std::isfinite( n.y ) && std::isfinite( n.z ), "Test22 (P2-C): n finite" );
		Check( std::isfinite( dpdu.x ) && std::isfinite( dpdu.y ) && std::isfinite( dpdu.z ), "Test22 (P2-C): dpdu finite" );
		Check( std::isfinite( dpdv.x ) && std::isfinite( dpdv.y ) && std::isfinite( dpdv.z ), "Test22 (P2-C): dpdv finite" );
		Check( std::isfinite( dndu.x ) && std::isfinite( dndu.y ) && std::isfinite( dndu.z ), "Test22 (P2-C): dndu finite" );
		Check( std::isfinite( dndv.x ) && std::isfinite( dndv.y ) && std::isfinite( dndv.z ), "Test22 (P2-C): dndv finite" );

		Check( fabs( Vector3Ops::Dot( dpdu, n ) ) < 1e-6, "Test22 (P2-C): dpdu_w . n_w ~= 0 under non-uniform stretch" );
		Check( fabs( Vector3Ops::Dot( dpdv, n ) ) < 1e-6, "Test22 (P2-C): dpdv_w . n_w ~= 0 under non-uniform stretch" );
		Check( fabs( Vector3Ops::Dot( dndu, n ) ) < 1e-6, "Test22 (P2-C): MONEY ASSERTION -- dndu_w . n_w ~= 0 under non-uniform stretch (projection step ran)" );
		Check( fabs( Vector3Ops::Dot( dndv, n ) ) < 1e-6, "Test22 (P2-C): MONEY ASSERTION -- dndv_w . n_w ~= 0 under non-uniform stretch (projection step ran)" );
	}

	safe_release( csg );
	safe_release( a );
	safe_release( b );
}

//
// Test 23 (P2-C, optional singular-guard bonus): a HUGE non-uniform
// stretch along an axis the hit's local normal is EXACTLY aligned with
// must collapse the promoted world-space normal magnitude below NEARZERO,
// tripping CSGObject.cpp's singular-guard (`derivatives.valid = false`,
// ~line 1233-1244) rather than dividing by ~0.  Fires straight down at the
// mesh sphere's north pole (local normal (0,0,1) there) under a 1e13
// stretch on Z.
//
void TestUnion_CsgSingularGuard_HugeAxisAlignedStretch()
{
	std::cout << "CSG_UNION: singular-guard trips under huge axis-aligned stretch (P2-C bonus)..." << std::endl;

	const Scalar rad = 1.5;
	const unsigned int detail = 40;
	const Point3 localOrigin( 0, 0, 10.0 );
	const Vector3 localDir( 0, 0, -1 );   // straight down onto the north pole

	TriangleMeshGeometryIndexed* meshA = BuildCsgTessellatedSphereMesh( rad, detail );
	Check( meshA != 0, "Test23 (P2-C bonus): mesh A built" );
	Object* a = new Object( meshA );
	safe_release( meshA );
	a->FinalizeTransformations();

	SphereGeometry* gB = new SphereGeometry( 1.0 );
	Object* b = new Object( gB );
	safe_release( gB );
	b->SetPosition( Point3( 10000, 10000, 10000 ) );   // never hit
	b->FinalizeTransformations();

	CSGObject* csg = new CSGObject( CSG_UNION );
	const bool assigned = csg->AssignObjects( a, b );
	Check( assigned, "Test23 (P2-C bonus): composite takes mesh(A)/far-sphere(B) operands" );
	csg->SetStretch( Vector3( 1.0, 1.0, 1e13 ) );   // huge stretch on Z, matching the hit's normal axis
	csg->FinalizeTransformations();

	const Matrix4 mxFinal = csg->GetFinalTransformMatrix();
	const Point3 worldOrigin = Point3Ops::Transform( mxFinal, localOrigin );
	const Vector3 worldDir = Vector3Ops::Transform( mxFinal, localDir );
	const Ray r0( worldOrigin, worldDir );

	RayIntersection ri( r0, nullRasterizerState );
	Hit( csg, r0, ri );
	Check( ri.geometric.bHit, "Test23 (P2-C bonus): (control) hit lands (north pole, local normal (0,0,1))" );
	if( ri.geometric.bHit ) {
		Check( !ri.geometric.derivatives.valid,
			"Test23 (P2-C bonus): MONEY ASSERTION -- singular guard trips (derivatives.valid == false) instead of dividing by ~0" );
	}

	safe_release( csg );
	safe_release( a );
	safe_release( b );
}

//
// Test 24 (debt-25 review round 3, mutation M7 -- the `/ stretch`
// factor in AdoptCsgExitFacePayloadViaProbe's `selfHitFloor`): mirrors
// Test 4's geometry exactly, but the SUBTRACTED operand B (and the
// containing operand A, so the exit face stays inside A) is uniformly
// SCALED UP 1000x via `SetScale` -- baked into the OBJECT TRANSFORM,
// not the box's own local dimensions, so the operand's local->world
// matrix genuinely carries a non-1 `stretch = |M^-1 dir|` for the
// probe's margin to divide by.  No existing decoy-lobe test (4, 11,
// 14, 15) uses a scaled operand, so none of them can catch a dropped
// `/ stretch`.
//
// Numeric derivation (kUlpFactor = 64*DBL_EPSILON ~= 1.4211e-14,
// NEARZERO = 1e-12):
//   B's local geometry is UNCHANGED from Test 4 (half-extent 2); the
//   world exit point (100, 50, -2000) maps back through B's inverse
//   transform (translate (0,0,-4000), scale 1000) to local
//   (0.1, 0.05, 2.0) -- the SAME local +Z-face point Test 4 uses.
//   stretch = |M^-1 dir| = 1/1000 = 1e-3 (dir=(0,0,1), uniform scale
//     1000 -- a pure scale+translate doesn't rotate the ray direction,
//     it only shrinks it by 1/scale).
//   cosExit = |dir . exit normal| = 1 (axis-aligned, straight-in ray;
//     uniform scale doesn't rotate the face normal either).
//   alongNormalLocal = |exitLocal.z| = 2.0 (identical to Test 4/14/15's
//     own figure, since B's local shape is identical to theirs).
//   bandLocal = 4*NEARZERO + kUlpFactor*2.0 ~= 4.03e-12.
//   CORRECT selfHitFloor = 2*bandLocal / rate, rate = |M^-1 dir . n_local|
//     (== stretch*cosExit for THIS geometry -- uniform scale, so the
//     factorisation the production code replaced in round 3 coincides
//     with the exact rate here; it does NOT in general, see CSGObject.cpp's
//     "Rate, exactly" note)
//     ~= 8.06e-12 / 1e-3 = 8.06e-9 world units -- this dominates the
//     margin's max() (the pre-existing dirWeightedAbs term is only
//     kUlpFactor*2000 ~= 2.84e-11, an order of magnitude smaller).
//   WITHOUT the `/ stretch` division (mutation M7), selfHitFloor'
//     ~= 8.06e-12 world -- now SMALLER than the 2.84e-11
//     dirWeightedAbs term, so the margin's max() picks 2.84e-11
//     instead.  Mapped back into B's LOCAL frame (multiply by
//     stretch=1e-3, since a world displacement along dir becomes a
//     `stretch`-times-smaller local displacement under this scale),
//     that is only 2.84e-14 local units of clearance past the exit
//     plane -- far under B's own self-hit band (~4.03e-12 local).
//     BoxGeometry::DropSelfHitRoot re-treats the probe origin as
//     sitting ON the exit face it is trying to re-hit and drops that
//     root; the probe's surviving hit is then B's FAR (-Z, entry)
//     face, thousands of world units away -- far outside
//     maxAcceptRange -- so AdoptCsgExitFacePayloadViaProbe falls back
//     to B's entry-face payload (the wrong face) instead of the real
//     exit face.
//
void TestSubtraction_ExitProbe_ScaledOperandMarginUsesStretch()
{
	std::cout << "CSG_SUBTRACTION: exit-probe margin divides by the operand's own local->world stretch (debt-25 review r3, M7)..." << std::endl;

	BoxGeometry* gA = new BoxGeometry( 6.0, 6.0, 6.0 );   // half-extent 3 (local)
	BoxGeometry* gB = new BoxGeometry( 4.0, 4.0, 4.0 );   // half-extent 2 (local)
	Object* oA = new Object( gA );
	Object* oB = new Object( gB );
	safe_release( gA );
	safe_release( gB );

	// The 1000x scale is on the OBJECT TRANSFORM (SetScale), not the
	// box's own dimensions -- that is what makes
	// GetFinalInverseTransformMatrix() report a non-trivial stretch for
	// the probe's margin to divide by.
	oA->SetPosition( Point3( 0, 0, 0 ) );
	oA->SetScale( 1000.0 );                    // world span [-3000,3000]
	oB->SetPosition( Point3( 0, 0, -4000 ) );
	oB->SetScale( 1000.0 );                    // world span [-6000,-2000]
	oA->FinalizeTransformations();
	oB->FinalizeTransformations();

	Ray r( Point3( 100, 50, -10000 ), Vector3( 0, 0, 1 ) );

	CSGObject* csg = new CSGObject( CSG_SUBTRACTION );
	const bool assigned = csg->AssignObjects( oA, oB );
	Check( assigned, "Test24 (debt-25 r3, M7): composite takes A/B operands" );
	csg->FinalizeTransformations();

	RayIntersection ri( r, nullRasterizerState );
	Hit( csg, r, ri );
	Check( ri.geometric.bHit, "Test24 (debt-25 r3, M7): (control) ray hits the composite at all" );

	RayIntersection refA( r, nullRasterizerState );
	Hit( oA, r, refA );
	Check( refA.geometric.bHit, "Test24 (debt-25 r3, M7): (control) ray hits standalone A" );

	// B's ENTRY-face payload (the pre-P2-e "honestly wrong face" answer)
	// -- used below only as a NEGATIVE reference.
	RayIntersection refBEntry( r, nullRasterizerState );
	Hit( oB, r, refBEntry );
	Check( refBEntry.geometric.bHit, "Test24 (debt-25 r3, M7): (control) ray hits standalone B" );

	// Sanity: confirm we hit the exit-designated branch, i.e. the
	// composite range lands on B's EXIT (range2), not B's entry nor A's
	// entry (same shape as Test 4, just scaled 1000x throughout).
	Check( Close( ri.geometric.range, refBEntry.geometric.range2, 1e-3 ), "Test24 (debt-25 r3, M7): (sanity) composite range == B's exit range (range2)" );
	Check( !Close( ri.geometric.range, refBEntry.geometric.range, 1e-3 ), "Test24 (debt-25 r3, M7): (sanity) composite range != B's entry range" );
	Check( !Close( ri.geometric.range, refA.geometric.range, 1e-3 ), "Test24 (debt-25 r3, M7): (sanity) composite range != A's entry range" );

	// P2-e reference: probe B's EXIT face DIRECTLY from just past its
	// world +Z face (z=-2000), same technique as Test 4/14 -- a
	// hand-built test oracle, not a call into the production probe.
	Ray probeRef( Point3( 100, 50, -1900 ), Vector3( 0, 0, -1 ) );
	RayIntersection refBExit( probeRef, nullRasterizerState );
	Hit( oB, probeRef, refBExit );
	Check( refBExit.geometric.bHit, "Test24 (debt-25 r3, M7): (control) direct probe hits B's exit face" );

	// Sanity: the probe reference lands on a DIFFERENT UV than B's
	// entry face -- otherwise this test isn't discriminating.
	Check( !Point2Close( refBExit.geometric.ptCoord, refBEntry.geometric.ptCoord ), "Test24 (debt-25 r3, M7): (sanity) exit-face ptCoord differs from entry-face ptCoord" );
	Check( !PointClose( refBExit.geometric.ptObjIntersec, refBEntry.geometric.ptObjIntersec ), "Test24 (debt-25 r3, M7): (sanity) exit-face ptObjIntersec differs from entry-face ptObjIntersec" );

	// The composite's payload must be the REAL exit-face data, not B's
	// entry-face data -- this is the assertion mutation M7 (dropping
	// the `/ stretch` division) breaks (see derivation above).
	Check( Point2Close( ri.geometric.ptCoord, refBExit.geometric.ptCoord ), "Test24 (debt-25 r3, M7): MONEY ASSERTION -- composite ptCoord matches the REAL exit-face probe" );
	Check( PointClose( ri.geometric.ptObjIntersec, refBExit.geometric.ptObjIntersec ), "Test24 (debt-25 r3, M7): MONEY ASSERTION -- composite ptObjIntersec matches the REAL exit-face probe" );
	Check( !Point2Close( ri.geometric.ptCoord, refBEntry.geometric.ptCoord ), "Test24 (debt-25 r3, M7): composite ptCoord is NOT B's entry-face data" );
	Check( !PointClose( ri.geometric.ptObjIntersec, refBEntry.geometric.ptObjIntersec ), "Test24 (debt-25 r3, M7): composite ptObjIntersec is NOT B's entry-face data" );

	// Never A's payload either.
	Check( !Point2Close( ri.geometric.ptCoord, refA.geometric.ptCoord ), "Test24 (debt-25 r3, M7): composite ptCoord is NOT A's" );
	Check( !PointClose( ri.geometric.ptObjIntersec, refA.geometric.ptObjIntersec ), "Test24 (debt-25 r3, M7): composite ptObjIntersec is NOT A's" );

	safe_release( csg );
	safe_release( oA );
	safe_release( oB );
}

//
// Test 25 (debt-25 review round 3, mutation M8 -- the `/ cosExit`
// factor in AdoptCsgExitFacePayloadViaProbe's `selfHitFloor`): mirrors
// Test 4's UNSCALED geometry (A half-extent 3 at the origin, B
// half-extent 2 at world (0,0,-4)), but fires an OBLIQUE ray that
// exits B's +Z face at |cos(angle-to-normal)| = 0.4 instead of
// straight-in, so the probe's margin genuinely needs the `/ cosExit`
// division.  No existing decoy-lobe test uses an oblique ray (4, 11,
// 14, 15 all fire straight down the box's own axis), so none of them
// can catch a dropped `/ cosExit`.
//
// Ray direction d = (0, sqrt(0.84), 0.4) -- already unit length, since
// 0.84 + 0.16 = 1 exactly; cosExit = |d . (0,0,1)| = 0.4.  Tracing the
// box slab per-axis entry/exit parametric distances by hand from the
// chosen origin (0.1, -16.8303, -10.0), the ray:
//   * enters B (y in [-2,2], z in [-6,-2]) through its y=-2 SIDE face
//     at world length s ~= 16.18 -- z there is ~= -3.53, i.e. BELOW A
//     (A hasn't been entered yet, since A's own z=-3 face isn't
//     reached until s = 17.5);
//   * enters A (x,y,z in [-3,3]) through its z=-3 face at s = 17.5 --
//     y there is ~= -0.79, still inside B (B's own y=-2/z=-6 exit
//     bounds aren't reached yet: B's y=2 exit is at s ~= 20.55, its
//     z=-2 exit is at s = 20.0);
//   * exits B through its z=-2 face at s = 20.0 -- y there = 1.5,
//     still well inside A (A's own z=3 exit isn't reached until
//     s = 32.5).  THIS is the exit-designated boundary the composite
//     reports, same face Test 4 exercises, just approached obliquely.
// (These figures describe the ray's path for readers checking the
// geometry by hand; the test itself reads the true exit point off B's
// own `range2` along ray `r`, so it is not sensitive to rounding in
// this by-hand trace.)
//
// Numeric derivation of the margin (kUlpFactor = 64*DBL_EPSILON ~=
// 1.4211e-14, NEARZERO = 1e-12; B is unscaled here, so stretch = 1):
//   exitLocal (B's own local frame) ~= (0.1, 1.5, 2.0) -- the same
//   half-extent-2 +Z face Test 4 uses, just a different (x,y) landing
//   point on it.  alongNormalLocal = |exitLocal.z| = 2.0 (identical to
//   Test 4/14/15/24's own figure, since B's shape is identical).
//   bandLocal = 4*NEARZERO + kUlpFactor*2.0 ~= 4.03e-12.
//   CORRECT selfHitFloor = 2*bandLocal / rate, rate = |M^-1 dir . n_local|
//     (== stretch*cosExit for THIS geometry -- uniform scale, so the
//     factorisation the production code replaced in round 3 coincides
//     with the exact rate here; it does NOT in general, see CSGObject.cpp's
//     "Rate, exactly" note)
//     = 8.06e-12 / 0.4 ~= 2.01e-11 world units, and the ACTUAL
//     clearance this buys along the face normal is
//     margin*cosExit = 2*bandLocal/stretch = 8.06e-12 -- exactly the
//     "2x headroom over the band" the selfHitFloor derivation
//     promises, independent of the oblique angle.
//   WITHOUT the `/ cosExit` division (mutation M8), margin'
//     = 8.06e-12 (missing the /0.4), and the clearance it buys along
//     the normal is only margin'*cosExit = 8.06e-12*0.4 ~= 3.22e-12 --
//     LESS than B's own band (~4.03e-12), so the perturbed probe
//     origin is STILL inside the band and BoxGeometry::
//     DropSelfHitRoot drops the same root a second time.  The probe's
//     surviving hit is then B's OWN entry face (y=-2), ~3.8 world
//     units back along the probe -- far outside maxAcceptRange -- so
//     AdoptCsgExitFacePayloadViaProbe falls back to B's entry-face
//     payload instead of the real exit face.  (The pre-existing
//     dirWeightedAbs margin term stays negligible throughout, using
//     the CSG-local exit point (0.1, 1.5, -2): ~kUlpFactor*2.17 ~=
//     3.1e-14, so it never masks this.)
//
void TestSubtraction_ExitProbe_ObliqueRayMarginUsesCosExit()
{
	std::cout << "CSG_SUBTRACTION: exit-probe margin divides by the oblique exit angle's cosine (debt-25 review r3, M8)..." << std::endl;

	BoxGeometry* gA = new BoxGeometry( 6.0, 6.0, 6.0 );   // half-extent 3
	BoxGeometry* gB = new BoxGeometry( 4.0, 4.0, 4.0 );   // half-extent 2
	Object* oA = new Object( gA );
	Object* oB = new Object( gB );
	safe_release( gA );
	safe_release( gB );

	oA->SetPosition( Point3( 0, 0, 0 ) );      // spans z in [-3, 3]
	oB->SetPosition( Point3( 0, 0, -4 ) );     // spans z in [-6, -2]
	oA->FinalizeTransformations();
	oB->FinalizeTransformations();

	// Unit direction with cosExit = 0.4 against B's +Z face normal
	// (0,0,1): dz = 0.4, dy = sqrt(1 - 0.4^2) = sqrt(0.84).
	const Vector3 dir = Vector3Ops::Normalize( Vector3( 0.0, std::sqrt( 0.84 ), 0.4 ) );
	// Origin chosen (by tracing backward from the exit point, see the
	// derivation above) so the ray starts well outside both operands
	// and takes the enter-B / enter-A / exit-B path.
	Ray r( Point3( 0.1, -16.8303, -10.0 ), dir );

	CSGObject* csg = new CSGObject( CSG_SUBTRACTION );
	const bool assigned = csg->AssignObjects( oA, oB );
	Check( assigned, "Test25 (debt-25 r3, M8): composite takes A/B operands" );
	csg->FinalizeTransformations();

	RayIntersection ri( r, nullRasterizerState );
	Hit( csg, r, ri );
	Check( ri.geometric.bHit, "Test25 (debt-25 r3, M8): (control) ray hits the composite at all" );

	RayIntersection refA( r, nullRasterizerState );
	Hit( oA, r, refA );
	Check( refA.geometric.bHit, "Test25 (debt-25 r3, M8): (control) ray hits standalone A" );

	// B's ENTRY-face payload -- used below only as a NEGATIVE reference.
	RayIntersection refBEntry( r, nullRasterizerState );
	Hit( oB, r, refBEntry );
	Check( refBEntry.geometric.bHit, "Test25 (debt-25 r3, M8): (control) ray hits standalone B" );

	// Sanity: confirm the exit-designated branch fired -- composite
	// range == B's exit range (range2), not B's entry nor A's entry.
	Check( Close( ri.geometric.range, refBEntry.geometric.range2, 1e-3 ), "Test25 (debt-25 r3, M8): (sanity) composite range == B's exit range (range2)" );
	Check( !Close( ri.geometric.range, refBEntry.geometric.range, 1e-3 ), "Test25 (debt-25 r3, M8): (sanity) composite range != B's entry range" );
	Check( !Close( ri.geometric.range, refA.geometric.range, 1e-3 ), "Test25 (debt-25 r3, M8): (sanity) composite range != A's entry range" );

	// P2-e reference: probe B's EXIT face DIRECTLY, using the exact
	// technique AdoptCsgExitFacePayloadViaProbe itself uses -- a point
	// just past the exit point along the ORIGINAL ray direction,
	// travelling back in the REVERSED direction -- so this hand-built
	// oracle lands on the SAME (x,y) point on the z=-2 plane the
	// production probe targets.  The exit point is read from B's own
	// range2 along the SAME ray `r` (exact, not the by-hand backward-
	// trace numbers in the derivation above), so this oracle is
	// insensitive to rounding in that hand trace.
	const Point3 ptExit = r.PointAtLength( refBEntry.geometric.range2 );
	const Point3 probeOrigin( ptExit.x + dir.x * 0.1, ptExit.y + dir.y * 0.1, ptExit.z + dir.z * 0.1 );
	Ray probeRef( probeOrigin, Vector3( -dir.x, -dir.y, -dir.z ) );
	RayIntersection refBExit( probeRef, nullRasterizerState );
	Hit( oB, probeRef, refBExit );
	Check( refBExit.geometric.bHit, "Test25 (debt-25 r3, M8): (control) direct probe hits B's exit face" );

	// Sanity: entry- and exit-face payloads are distinct (otherwise
	// this test isn't discriminating).
	Check( !Point2Close( refBExit.geometric.ptCoord, refBEntry.geometric.ptCoord ), "Test25 (debt-25 r3, M8): (sanity) exit-face ptCoord differs from entry-face ptCoord" );
	Check( !PointClose( refBExit.geometric.ptObjIntersec, refBEntry.geometric.ptObjIntersec ), "Test25 (debt-25 r3, M8): (sanity) exit-face ptObjIntersec differs from entry-face ptObjIntersec" );

	// The composite's payload must be the REAL exit-face data, not B's
	// entry-face data -- this is the assertion mutation M8 (dropping
	// the `/ cosExit` division) breaks (see derivation above).
	Check( Point2Close( ri.geometric.ptCoord, refBExit.geometric.ptCoord ), "Test25 (debt-25 r3, M8): MONEY ASSERTION -- composite ptCoord matches the REAL exit-face probe" );
	Check( PointClose( ri.geometric.ptObjIntersec, refBExit.geometric.ptObjIntersec ), "Test25 (debt-25 r3, M8): MONEY ASSERTION -- composite ptObjIntersec matches the REAL exit-face probe" );
	Check( !Point2Close( ri.geometric.ptCoord, refBEntry.geometric.ptCoord ), "Test25 (debt-25 r3, M8): composite ptCoord is NOT B's entry-face data" );
	Check( !PointClose( ri.geometric.ptObjIntersec, refBEntry.geometric.ptObjIntersec ), "Test25 (debt-25 r3, M8): composite ptObjIntersec is NOT B's entry-face data" );

	// Never A's payload either.
	Check( !Point2Close( ri.geometric.ptCoord, refA.geometric.ptCoord ), "Test25 (debt-25 r3, M8): composite ptCoord is NOT A's" );
	Check( !PointClose( ri.geometric.ptObjIntersec, refA.geometric.ptObjIntersec ), "Test25 (debt-25 r3, M8): composite ptObjIntersec is NOT A's" );

	safe_release( csg );
	safe_release( oA );
	safe_release( oB );
}

int main()
{
	TestIntersection_AEntersFirst_EntryIsWhollyB();
	TestIntersection_BEntersFirst_EntryIsWhollyA();
	TestSubtraction_VisibleBoundaryIsSubtractedOperand();
	TestSubtraction_ExitDesignatedBoundary_ProbedPayloadMatchesRealFace();
	TestIntersection_CrossOperandMaterialBindingFollowsOwner();
	TestSubtraction_DisjointBothSides_CompositeIsAAlone();
	TestIntersectionOnly_ScaledOperandNoShadowLeak();
	TestUnion_TransformedCsgDerivativesMatchStandaloneRotated();
	TestIntersectionOnly_NonUniformScale_DirectionTrueFactor();
	TestUnion_CsgHonoursShadingTangentFromGeometry();
	TestSubtraction_ExitProbe_DoesNotOvershootToADifferentLobe();
	TestIntersectionOnly_CompressedCsg_NoShadowLeakThroughOperandPretest();
	TestIntersectRay_CompressedCsg_ClosestHitNotCulledByOperandPretest();
	TestSubtraction_ExitProbe_TransverseCoordinateDoesNotInflateMargin();
	TestSubtraction_ExitProbe_TinyGapDecoyDoesNotOverwhelmMarginFloor();
	TestUnion_TransformedCsgDerivativesMatchStandaloneScaled();
	TestSubtraction_CavityWall_DndvSignMatchesFDAndCurvature();
	TestSubtraction_InsideBothAAndB_NormalNegationRoutesThroughBranch();
	TestSubtraction_InsideBNotA_NormalNegationRoutesThroughBranch();
	TestMeshIndexed_BackFaceHit_DndvSignMatchesFDAndCurvature();
	TestMeshNonIndexed_FrontAndBackFace_DndvSign();
	TestUnion_CsgQuotientRule_NonUniformStretch_OrthogonalityInvariants();
	TestUnion_CsgSingularGuard_HugeAxisAlignedStretch();
	TestSubtraction_ExitProbe_ScaledOperandMarginUsesStretch();
	TestSubtraction_ExitProbe_ObliqueRayMarginUsesCosExit();
	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
