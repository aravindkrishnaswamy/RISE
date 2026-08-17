#include <cassert>
#include <iostream>

#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Intersection/RayIntersection.h"
#include "../src/Library/Objects/CSGObject.h"
#include "../src/Library/Objects/Object.h"

using namespace RISE;
using namespace RISE::Implementation;

void TestCSGIntersectionReportsCompositeObject()
{
	std::cout << "Testing CSG intersection reports composite object identity..." << std::endl;

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
	assert( pCSG->AssignObjects( pObjectA, pObjectB ) );
	pCSG->FinalizeTransformations();

	RayIntersection ri(
		Ray( Point3( 0, 0, 2 ), Vector3( 0, 0, -1 ) ),
		nullRasterizerState );
	pCSG->IntersectRay( ri, RISE_INFINITY, true, true, true );

	assert( ri.geometric.bHit );
	assert( ri.pObject == pCSG );
	assert( ri.pObject != pObjectA );
	assert( ri.pObject != pObjectB );

	safe_release( pCSG );
	safe_release( pObjectA );
	safe_release( pObjectB );

	std::cout << "CSG intersection reports composite object identity Passed!" << std::endl;
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
	std::cout << "Testing CSG operand consumption is counted, not a flag..." << std::endl;

	SphereGeometry* pSphereA = new SphereGeometry( 0.55 );
	SphereGeometry* pSphereB = new SphereGeometry( 0.55 );
	Object* pObjectA = new Object( pSphereA );
	Object* pObjectB = new Object( pSphereB );
	safe_release( pSphereA );
	safe_release( pSphereB );

	// (control) a standalone object is world-visible before anything consumes it.
	assert( pObjectA->IsWorldVisible() );
	assert( pObjectB->IsWorldVisible() );

	CSGObject* pFirst = new CSGObject( CSG_UNION );
	assert( pFirst->AssignObjects( pObjectA, pObjectB ) );
	assert( !pObjectA->IsWorldVisible() );
	assert( !pObjectB->IsWorldVisible() );

	// THE 3b SHAPE: a second composite over the SAME two operands, exactly what a
	// subtree instance of a csg_object produces.
	CSGObject* pSecond = new CSGObject( CSG_UNION );
	assert( pSecond->AssignObjects( pObjectA, pObjectB ) );
	assert( !pObjectA->IsWorldVisible() );
	assert( !pObjectB->IsWorldVisible() );

	// TEARING ONE DOWN MUST NOT RESURRECT THEM.  This is the assertion the bool
	// could not make: the first composite is still consuming both operands.
	safe_release( pSecond );
	assert( !pObjectA->IsWorldVisible() );
	assert( !pObjectB->IsWorldVisible() );

	// ...and the count is BALANCED, not merely monotone: with the last composite
	// gone the operands are ordinary objects again.  (An accounting that only ever
	// incremented would pass the assertion above and fail this one.)
	safe_release( pFirst );
	assert( pObjectA->IsWorldVisible() );
	assert( pObjectB->IsWorldVisible() );

	safe_release( pObjectA );
	safe_release( pObjectB );

	std::cout << "CSG operand consumption is counted Passed!" << std::endl;
}

// The OTHER unconditional re-show: `AssignObjects` called a second time on a live
// composite drops the outgoing pair.  Same rule -- it gives up only its OWN claim.
void TestReassignReleasesOnlyItsOwnClaim()
{
	std::cout << "Testing CSG re-assign releases only its own claim..." << std::endl;

	SphereGeometry* pGeom = new SphereGeometry( 0.55 );
	Object* pShared    = new Object( pGeom );
	Object* pOther     = new Object( pGeom );
	Object* pIncomingA = new Object( pGeom );
	Object* pIncomingB = new Object( pGeom );
	safe_release( pGeom );

	CSGObject* pKeeper = new CSGObject( CSG_UNION );
	CSGObject* pMover  = new CSGObject( CSG_UNION );
	assert( pKeeper->AssignObjects( pShared, pOther ) );
	assert( pMover->AssignObjects( pShared, pOther ) );

	// pMover moves off the shared pair.  It releases ITS claim; pKeeper's stands.
	assert( pMover->AssignObjects( pIncomingA, pIncomingB ) );
	assert( !pShared->IsWorldVisible() );
	assert( !pOther->IsWorldVisible() );
	assert( !pIncomingA->IsWorldVisible() );
	assert( !pIncomingB->IsWorldVisible() );

	safe_release( pKeeper );
	assert( pShared->IsWorldVisible() );
	assert( pOther->IsWorldVisible() );

	safe_release( pMover );
	assert( pIncomingA->IsWorldVisible() );
	assert( pIncomingB->IsWorldVisible() );

	safe_release( pShared );
	safe_release( pOther );
	safe_release( pIncomingA );
	safe_release( pIncomingB );

	std::cout << "CSG re-assign releases only its own claim Passed!" << std::endl;
}

int main()
{
	TestCSGIntersectionReportsCompositeObject();
	TestOperandStaysConsumedWhileAnotherCompositeHoldsIt();
	TestReassignReleasesOnlyItsOwnClaim();
	return 0;
}
