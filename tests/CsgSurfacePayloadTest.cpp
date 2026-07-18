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

#include "../src/Library/Functions/ConstantFunctions.h"
#include "../src/Library/Geometry/BoxGeometry.h"
#include "../src/Library/Geometry/CylinderGeometry.h"
#include "../src/Library/Geometry/SDFGeometry.h"
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
	assert( csg->AssignObjects( oA, oB ) );
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
	assert( ri.geometric.bHit );
	assert( ri.geometric.range > 95.0 && ri.geometric.range < 105.0 );

	// A "light" at world distance 90 is CLOSER than the occluder's entry
	// (~100) -- the occluder must NOT report as blocking it.
	const Scalar dHowFarTest = 90.0;
	const bool occluded = csg->IntersectRay_IntersectionOnly( r, dHowFarTest, true, true );
	assert( !occluded );

	safe_release( csg );
	safe_release( oA );
	safe_release( oB );
	std::cout << "  Passed." << std::endl;
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
	assert( csg->AssignObjects( aOperand, farB ) );
	csg->FinalizeTransformations();

	Ray r( Point3( 0.3, 0.2, 5.0 ), Vector3( 0, 0, -1 ) );

	RayIntersection ri( r, nullRasterizerState );
	Hit( csg, r, ri );
	assert( ri.geometric.bHit );
	assert( ri.geometric.bShadingTangentFromGeometry );

	RayIntersection refRi( r, nullRasterizerState );
	Hit( standalone, r, refRi );
	assert( refRi.geometric.bHit );
	assert( refRi.geometric.bShadingTangentFromGeometry );

	// Sanity: the flag really does route to the WORLD-X-PROJECTED branch
	// (CreateFromWU), not the default CreateFromW axis -- for an exact +Z
	// normal, CreateFromW picks U=(-1,0,0) (cross(W,canonicalV)), while
	// the geometry-aware branch picks U=(+1,0,0) (world-X projected into
	// the normal plane, which for n=(0,0,1) is just world-X itself).  If
	// these coincided the test wouldn't discriminate.
	assert( VecClose( refRi.geometric.onb.u(), Vector3( 1, 0, 0 ) ) );
	assert( !VecClose( refRi.geometric.onb.u(), Vector3( -1, 0, 0 ) ) );

	assert( VecClose( ri.geometric.onb.u(), refRi.geometric.onb.u() ) );
	assert( VecClose( ri.geometric.onb.v(), refRi.geometric.onb.v() ) );
	assert( VecClose( ri.geometric.onb.w(), refRi.geometric.onb.w() ) );

	safe_release( csg );
	safe_release( aOperand );
	safe_release( farB );
	safe_release( standalone );
	std::cout << "  Passed." << std::endl;
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
	assert( nestedB->AssignObjects( realLobe, decoyLobe ) );
	nestedB->FinalizeTransformations();

	CSGObject* outerCsg = new CSGObject( CSG_SUBTRACTION );
	assert( outerCsg->AssignObjects( oA, nestedB ) );
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
	assert( ri.geometric.bHit );

	// Ground truth: the REAL exit face, probed directly (independent of
	// production code) -- exactly the P2-e test's own oracle pattern.
	Ray probeRefReal( Point3( 0.1, 0.05, -1.9 ), Vector3( 0, 0, -1 ) );
	RayIntersection refRealExit( probeRefReal, nullRasterizerState );
	Hit( realLobe, probeRefReal, refRealExit );
	assert( refRealExit.geometric.bHit );

	// The WRONG answer an overshooting probe lands on: the decoy lobe's
	// near face, probed directly.
	Ray probeDecoy( Point3( 0.1, 0.05, -1.0 ), Vector3( 0, 0, -1 ) );
	RayIntersection refDecoy( probeDecoy, nullRasterizerState );
	Hit( decoyLobe, probeDecoy, refDecoy );
	assert( refDecoy.geometric.bHit );

	// Sanity: the two candidate faces produce distinct UV / object-space
	// points, so this test is discriminating.
	assert( !Point2Close( refRealExit.geometric.ptCoord, refDecoy.geometric.ptCoord ) );
	assert( !PointClose( refRealExit.geometric.ptObjIntersec, refDecoy.geometric.ptObjIntersec ) );

	// The composite's recovered payload must be the REAL exit face, never
	// the decoy.
	assert( Point2Close( ri.geometric.ptCoord, refRealExit.geometric.ptCoord ) );
	assert( PointClose( ri.geometric.ptObjIntersec, refRealExit.geometric.ptObjIntersec ) );
	assert( !Point2Close( ri.geometric.ptCoord, refDecoy.geometric.ptCoord ) );
	assert( !PointClose( ri.geometric.ptObjIntersec, refDecoy.geometric.ptObjIntersec ) );

	safe_release( outerCsg );
	safe_release( oA );
	safe_release( nestedB );
	safe_release( realLobe );
	safe_release( decoyLobe );
	std::cout << "  Passed." << std::endl;
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
			assert( csg->AssignObjects( oCylinder, oFarSphere ) );
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
	assert( ri.geometric.bHit );
	assert( Close( ri.geometric.range, 99.0, 1e-3 ) );
	assert( ri.geometric.range < s.worldDHowFar );

	const bool occluded = s.csg->IntersectRay_IntersectionOnly( s.worldRay, s.worldDHowFar, true, true );
	assert( occluded );

	std::cout << "  Passed." << std::endl;
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

	assert( ri.geometric.bHit );
	assert( Close( ri.geometric.range, 99.0, 1e-3 ) );

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
	TestIntersectionOnly_NonUniformScale_DirectionTrueFactor();
	TestUnion_CsgHonoursShadingTangentFromGeometry();
	TestSubtraction_ExitProbe_DoesNotOvershootToADifferentLobe();
	TestIntersectionOnly_CompressedCsg_NoShadowLeakThroughOperandPretest();
	TestIntersectRay_CompressedCsg_ClosestHitNotCulledByOperandPretest();
	std::cout << "\nAll CsgSurfacePayloadTest cases passed." << std::endl;
	return 0;
}
