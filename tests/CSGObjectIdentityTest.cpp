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

int main()
{
	TestCSGIntersectionReportsCompositeObject();
	return 0;
}
