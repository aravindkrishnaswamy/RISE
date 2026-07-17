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
//////////////////////////////////////////////////////////////////////

#include <cassert>
#include <cmath>
#include <iostream>

#include "../src/Library/Geometry/BoxGeometry.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"
#include "../src/Library/Intersection/RayIntersection.h"
#include "../src/Library/Materials/LambertianMaterial.h"
#include "../src/Library/Objects/CSGObject.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Painters/UniformColorPainter.h"

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
	assert( csg->AssignObjects( oA, oB ) );
	csg->FinalizeTransformations();

	RayIntersection ri( r, nullRasterizerState );
	Hit( csg, r, ri );
	assert( ri.geometric.bHit );

	RayIntersection refA( r, nullRasterizerState );
	Hit( oA, r, refA );
	assert( refA.geometric.bHit );

	// B's ENTRY-face payload (the pre-P2-e "honestly wrong face" answer)
	// -- used below only as a NEGATIVE reference.
	RayIntersection refBEntry( r, nullRasterizerState );
	Hit( oB, r, refBEntry );
	assert( refBEntry.geometric.bHit );

	// Sanity: confirm we actually hit the exit-designated branch, i.e.
	// the composite range lands on B's EXIT (range2), not B's entry
	// (range) nor A's entry.
	assert( Close( ri.geometric.range, refBEntry.geometric.range2, 1e-3 ) );
	assert( !Close( ri.geometric.range, refBEntry.geometric.range, 1e-3 ) );
	assert( !Close( ri.geometric.range, refA.geometric.range, 1e-3 ) );

	// Normal is B's EXIT normal, flipped -- untouched by the P2-e probe
	// (the branch's own normal fields are authoritative; the probe only
	// ever supplies the AUXILIARY payload, never range/vNormal/vGeomNormal).
	assert( VecClose( ri.geometric.vNormal, Vector3( -refBEntry.geometric.vNormal2.x, -refBEntry.geometric.vNormal2.y, -refBEntry.geometric.vNormal2.z ) ) );
	assert( VecClose( ri.geometric.vGeomNormal, Vector3( -refBEntry.geometric.vGeomNormal2.x, -refBEntry.geometric.vGeomNormal2.y, -refBEntry.geometric.vGeomNormal2.z ) ) );

	// P2-e reference: probe B's EXIT face DIRECTLY from the outside --
	// an INDEPENDENT ray starting just past world z=-2 (B's +Z face)
	// heading back in -Z, exactly what a direct render ray hitting that
	// face from the empty region beyond it would see.  This is a
	// hand-built test oracle, not a call into the production probe.
	Ray probeRef( Point3( 0.1, 0.05, -1.9 ), Vector3( 0, 0, -1 ) );
	RayIntersection refBExit( probeRef, nullRasterizerState );
	Hit( oB, probeRef, refBExit );
	assert( refBExit.geometric.bHit );

	// Sanity: the probe reference actually lands on a DIFFERENT UV than
	// B's entry face (box UV mapping is per-face) -- otherwise this
	// test isn't discriminating between "entry payload" and "real exit
	// payload".
	assert( !Point2Close( refBExit.geometric.ptCoord, refBEntry.geometric.ptCoord ) );

	// The composite's payload must be the REAL exit-face data (P2-e),
	// not B's entry-face data (the pre-fix "honestly wrong face").
	assert( Point2Close( ri.geometric.ptCoord, refBExit.geometric.ptCoord ) );
	assert( PointClose( ri.geometric.ptObjIntersec, refBExit.geometric.ptObjIntersec ) );
	assert( !Point2Close( ri.geometric.ptCoord, refBEntry.geometric.ptCoord ) );
	assert( !PointClose( ri.geometric.ptObjIntersec, refBEntry.geometric.ptObjIntersec ) );

	// Never A's payload either.
	assert( !Point2Close( ri.geometric.ptCoord, refA.geometric.ptCoord ) );
	assert( !PointClose( ri.geometric.ptObjIntersec, refA.geometric.ptObjIntersec ) );

	safe_release( csg );
	safe_release( oA );
	safe_release( oB );
	std::cout << "  Passed." << std::endl;
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
		assert( csg->AssignObjects( outer, inner ) );   // A = outer/matA, B = inner/matB

		csg->FinalizeTransformations();

		RayIntersection ri( r, nullRasterizerState );
		Hit( csg, r, ri );
		assert( ri.geometric.bHit );

		// Composite entry is wholly B's (inner's) surface -- material
		// must be B's (matB), NOT A's (matA, the operand `ri` started
		// life as a whole-record copy of -- the exact P1-a bug).
		assert( ri.pMaterial == matB );
		assert( ri.pMaterial != matA );

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
		assert( csg->AssignObjects( inner, outer ) );

		csg->FinalizeTransformations();

		RayIntersection ri( r, nullRasterizerState );
		Hit( csg, r, ri );
		assert( ri.geometric.bHit );

		// Composite entry is still wholly the inner box's surface --
		// material must be matB regardless of which CSG slot it's in.
		assert( ri.pMaterial == matB );
		assert( ri.pMaterial != matA );

		safe_release( csg );
		safe_release( outer );
		safe_release( inner );
	}

	safe_release( matA );
	safe_release( matB );
	safe_release( painterA );
	safe_release( painterB );
	std::cout << "  Passed." << std::endl;
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
	assert( refA.geometric.bHit );

	// -- (i) B wholly BEFORE A: half-extent 0.5 box spanning z in
	//    [-5.5,-4.5], well before A's entry at z=-2. --
	{
		BoxGeometry* gB = new BoxGeometry( 1.0, 1.0, 1.0 );
		Object* oB = new Object( gB );
		safe_release( gB );
		oB->SetPosition( Point3( 0, 0, -5.0 ) );
		oB->FinalizeTransformations();

		CSGObject* csg = new CSGObject( CSG_SUBTRACTION );
		assert( csg->AssignObjects( oA, oB ) );
		csg->FinalizeTransformations();

		RayIntersection ri( r, nullRasterizerState );
		Hit( csg, r, ri );
		assert( ri.geometric.bHit );

		assert( Close( ri.geometric.range, refA.geometric.range ) );
		assert( Close( ri.geometric.range2, refA.geometric.range2 ) );
		assert( VecClose( ri.geometric.vNormal, refA.geometric.vNormal ) );
		assert( VecClose( ri.geometric.vGeomNormal, refA.geometric.vGeomNormal ) );
		assert( Point2Close( ri.geometric.ptCoord, refA.geometric.ptCoord ) );

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
		assert( csg->AssignObjects( oA, oB ) );
		csg->FinalizeTransformations();

		RayIntersection ri( r, nullRasterizerState );
		Hit( csg, r, ri );
		assert( ri.geometric.bHit );

		assert( Close( ri.geometric.range, refA.geometric.range ) );
		assert( Close( ri.geometric.range2, refA.geometric.range2 ) );
		assert( VecClose( ri.geometric.vNormal, refA.geometric.vNormal ) );
		assert( VecClose( ri.geometric.vGeomNormal, refA.geometric.vGeomNormal ) );
		assert( Point2Close( ri.geometric.ptCoord, refA.geometric.ptCoord ) );

		safe_release( csg );
		safe_release( oB );
	}

	safe_release( oA );
	std::cout << "  Passed." << std::endl;
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
	assert( csg->AssignObjects( oA, oB ) );
	csg->FinalizeTransformations();

	Ray r( Point3( 0, 0, -100 ), Vector3( 0, 0, 1 ) );
	const Scalar dHowFar = 200.0;

	// Ground truth: the full IntersectRay (always exit-info-correct)
	// says the composite IS hit, well within dHowFar (entry ~98).
	RayIntersection ri( r, nullRasterizerState );
	Hit( csg, r, ri );
	assert( ri.geometric.bHit );
	assert( ri.geometric.range < dHowFar );
	assert( ri.geometric.range > 90.0 && ri.geometric.range < 100.0 );

	// IntersectRay_IntersectionOnly must agree.
	const bool occluded = csg->IntersectRay_IntersectionOnly( r, dHowFar, true, true );
	assert( occluded );

	safe_release( csg );
	safe_release( oA );
	safe_release( oB );
	std::cout << "  Passed." << std::endl;
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
	assert( csg->AssignObjects( a, b ) );
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
	assert( ri.geometric.bHit );
	assert( ri.geometric.derivatives.valid );

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
	assert( refRi.geometric.bHit );
	assert( refRi.geometric.derivatives.valid );

	// Sanity: dpdu is actually non-degenerate, so this comparison is
	// discriminating (a zero vector would trivially "match" whether or
	// not promotion happened).
	assert( Vector3Ops::SquaredModulus( refRi.geometric.derivatives.dpdu ) > 1e-6 );

	assert( VecClose( ri.geometric.derivatives.dpdu, refRi.geometric.derivatives.dpdu, 1e-4 ) );
	assert( VecClose( ri.geometric.derivatives.dpdv, refRi.geometric.derivatives.dpdv, 1e-4 ) );
	assert( VecClose( ri.geometric.derivatives.dndu, refRi.geometric.derivatives.dndu, 1e-4 ) );
	assert( VecClose( ri.geometric.derivatives.dndv, refRi.geometric.derivatives.dndv, 1e-4 ) );

	safe_release( csg );
	safe_release( a );
	safe_release( b );
	safe_release( ref );
	std::cout << "  Passed." << std::endl;
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
	std::cout << "\nAll CsgSurfacePayloadTest cases passed." << std::endl;
	return 0;
}
