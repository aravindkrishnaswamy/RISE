//////////////////////////////////////////////////////////////////////
//
//  CSGObjectIdentityTest.cpp - CSGObject's two identity contracts:
//    (1) an intersection reports the COMPOSITE, never an operand;
//    (2) being an operand is a CONSUMPTION COUNT, not a flag (87 step 3b),
//        so N composites can share one operand and tearing one down does
//        not resurrect it under the others.
//
//  WHY THIS FILE USES A FAILURE TALLY AND NOT `assert` (round 3, 2026-08-17).
//  It was written assert-only, which on the project's own DOCUMENTED WINDOWS
//  PATH verifies NOTHING: build/cmake/rise-tests/CMakeLists.txt does not
//  override CMAKE_CXX_FLAGS_RELEASE, MSVC's default for that config carries
//  `/DNDEBUG`, and run_all_tests.ps1 defaults to `-Config Release`.  Under
//  NDEBUG every `assert` compiles to nothing, so an INVERTED assertion still
//  prints "Passed!" and exits 0.
//
//  Worse than a silently-absent CHECK: the SETUP disappeared too.  The
//  consumption count is established by `AssignObjects`, and that call used to
//  sit INSIDE an `assert(...)` -- so under NDEBUG no composite was ever
//  assigned and the case tested an unrelated scene.  Every side-effecting
//  call is therefore made on its own line here, with only its RESULT checked.
//
//  The count's balance across N composites is the property both 3b review
//  rounds lean on, and this file is its only regression guard in the tree, so
//  it has to hold in every configuration.  Style follows
//  CstSourceInstanceTest: a counted Check(), a printed tally, and a non-zero
//  exit on any failure -- which is what run_all_tests.{sh,ps1} judge.
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>

#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Intersection/RayIntersection.h"
#include "../src/Library/Objects/CSGObject.h"
#include "../src/Library/Objects/Object.h"

using namespace RISE;
using namespace RISE::Implementation;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const char* w ) { if( c ) ++g_pass; else { ++g_fail; std::printf( "  FAIL: %s\n", w ); } }

void TestCSGIntersectionReportsCompositeObject()
{
	std::printf( "=== CSGObjectIdentityTest: a CSG intersection reports the COMPOSITE's identity ===\n" );

	SphereGeometry* pSphereA = new SphereGeometry( 0.55 );
	SphereGeometry* pSphereB = new SphereGeometry( 0.55 );

	Object* pObjectA = new Object( pSphereA );
	Object* pObjectB = new Object( pSphereB );

	safe_release( pSphereA );
	safe_release( pSphereB );

	pObjectA->SetPosition( Point3( 0, 0.25, 0 ) );
	pObjectB->SetPosition( Point3( 0, -0.25, 0 ) );
	pObjectA->FinalizeTransformations();
	pObjectB->FinalizeTransformations();

	CSGObject* pCSG = new CSGObject( CSG_INTERSECTION );
	// SIDE EFFECT ON ITS OWN LINE -- this is the call that builds the scene the
	// assertions below read.  Inside an `assert` it vanished under NDEBUG.
	const bool assigned = pCSG->AssignObjects( pObjectA, pObjectB );
	Check( assigned, "the composite takes both operands" );
	pCSG->FinalizeTransformations();

	RayIntersection ri(
		Ray( Point3( 0, 0, 2 ), Vector3( 0, 0, -1 ) ),
		nullRasterizerState );
	pCSG->IntersectRay( ri, RISE_INFINITY, true, true, true );

	Check( ri.geometric.bHit, "(control) the ray hits the composite at all" );
	Check( ri.pObject == pCSG, "the intersection reports the COMPOSITE as the object hit" );
	Check( ri.pObject != pObjectA, "... not operand A, whose surface the ray actually met" );
	Check( ri.pObject != pObjectB, "... and not operand B" );

	safe_release( pCSG );
	safe_release( pObjectA );
	safe_release( pObjectB );
}

// 87 step 3b: ONE OPERAND, N COMPOSITES.  A `source` instance of a subtree containing
// a `csg_object` re-Finalizes that chunk with `obja` / `objb` UNCHANGED, so the clone
// SHARES its operands by pointer -- correct, because an operand's matrix is CSG-local
// and is read relative to whichever composite is asking.
//
// What that sharing broke: being an operand was recorded as `SetWorldVisible(false)`,
// a plain BOOL, and both `~CSGObject` and `AssignObjects`'s outgoing branch set it
// unconditionally back to TRUE.  Tearing down ONE composite therefore resurrected an
// operand that the others were still consuming: world visibility is the TLAS /
// enumeration admission filter, so it rendered as a standalone shape beside the
// composites that own it, and -- being visible -- also stopped answering to
// `ObjectManager::SetObjectParent`'s "hidden and has geometry" test for an operand,
// so it became parentable.  Consumption is a COUNT now, and this is its guard.
void TestOperandStaysConsumedWhileAnotherCompositeHoldsIt()
{
	std::printf( "=== CSGObjectIdentityTest: CSG operand consumption is COUNTED, not a flag ===\n" );

	SphereGeometry* pSphereA = new SphereGeometry( 0.55 );
	SphereGeometry* pSphereB = new SphereGeometry( 0.55 );
	Object* pObjectA = new Object( pSphereA );
	Object* pObjectB = new Object( pSphereB );
	safe_release( pSphereA );
	safe_release( pSphereB );

	// (control) a standalone object is world-visible before anything consumes it.
	Check( pObjectA->IsWorldVisible() && pObjectB->IsWorldVisible(),
	       "(control) both operands start as ordinary world-visible objects" );

	CSGObject* pFirst = new CSGObject( CSG_UNION );
	const bool firstAssigned = pFirst->AssignObjects( pObjectA, pObjectB );
	Check( firstAssigned, "the first composite takes both operands" );
	Check( !pObjectA->IsWorldVisible() && !pObjectB->IsWorldVisible(),
	       "... and consuming them makes both world-invisible" );

	// THE 3b SHAPE: a second composite over the SAME two operands, exactly what a
	// subtree instance of a csg_object produces.
	CSGObject* pSecond = new CSGObject( CSG_UNION );
	const bool secondAssigned = pSecond->AssignObjects( pObjectA, pObjectB );
	Check( secondAssigned, "a SECOND composite takes the same two operands -- the shape a subtree instance produces" );
	Check( !pObjectA->IsWorldVisible() && !pObjectB->IsWorldVisible(),
	       "... and both are still world-invisible, now consumed twice" );

	// TEARING ONE DOWN MUST NOT RESURRECT THEM.  This is the assertion the bool
	// could not make: the first composite is still consuming both operands.
	safe_release( pSecond );
	Check( !pObjectA->IsWorldVisible() && !pObjectB->IsWorldVisible(),
	       "MONEY ASSERTION: tearing ONE composite down leaves both operands consumed -- the other still holds them" );

	// ...and the count is BALANCED, not merely monotone: with the last composite
	// gone the operands are ordinary objects again.  (An accounting that only ever
	// incremented would pass the assertion above and fail this one.)
	safe_release( pFirst );
	Check( pObjectA->IsWorldVisible() && pObjectB->IsWorldVisible(),
	       "MONEY ASSERTION: and the count is BALANCED -- with the last composite gone they are ordinary objects "
	       "again, which an only-ever-incrementing accounting would fail" );

	safe_release( pObjectA );
	safe_release( pObjectB );
}

// The OTHER unconditional re-show: `AssignObjects` called a second time on a live
// composite drops the outgoing pair.  Same rule -- it gives up only its OWN claim.
void TestReassignReleasesOnlyItsOwnClaim()
{
	std::printf( "=== CSGObjectIdentityTest: a CSG re-assign releases only its OWN claim ===\n" );

	SphereGeometry* pGeom = new SphereGeometry( 0.55 );
	Object* pShared    = new Object( pGeom );
	Object* pOther     = new Object( pGeom );
	Object* pIncomingA = new Object( pGeom );
	Object* pIncomingB = new Object( pGeom );
	safe_release( pGeom );

	CSGObject* pKeeper = new CSGObject( CSG_UNION );
	CSGObject* pMover  = new CSGObject( CSG_UNION );
	const bool keeperAssigned = pKeeper->AssignObjects( pShared, pOther );
	const bool moverAssigned  = pMover->AssignObjects( pShared, pOther );
	Check( keeperAssigned && moverAssigned, "two composites each take the same shared pair" );

	// pMover moves off the shared pair.  It releases ITS claim; pKeeper's stands.
	const bool reassigned = pMover->AssignObjects( pIncomingA, pIncomingB );
	Check( reassigned, "one of them RE-assigns onto a different pair" );
	Check( !pShared->IsWorldVisible() && !pOther->IsWorldVisible(),
	       "MONEY ASSERTION: the outgoing pair stays consumed -- the re-assign released only the mover's own claim" );
	Check( !pIncomingA->IsWorldVisible() && !pIncomingB->IsWorldVisible(),
	       "... and the incoming pair is now consumed" );

	safe_release( pKeeper );
	Check( pShared->IsWorldVisible() && pOther->IsWorldVisible(),
	       "with the keeper gone the outgoing pair is ordinary again" );

	safe_release( pMover );
	Check( pIncomingA->IsWorldVisible() && pIncomingB->IsWorldVisible(),
	       "and with the mover gone, so is the incoming pair" );

	safe_release( pShared );
	safe_release( pOther );
	safe_release( pIncomingA );
	safe_release( pIncomingB );
}

int main()
{
	TestCSGIntersectionReportsCompositeObject();
	TestOperandStaysConsumedWhileAnotherCompositeHoldsIt();
	TestReassignReleasesOnlyItsOwnClaim();
	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
