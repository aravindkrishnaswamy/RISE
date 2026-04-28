//////////////////////////////////////////////////////////////////////
//
//  BDPTVertexRIGRebuildTest.cpp - Verifies that
//    PathVertexEval::PopulateRIGFromVertex copies every surface-state
//    field from a BDPTVertex into a freshly-built
//    RayIntersectionGeometric.
//
//    Why this test exists:  BDPT and VCM contain ~four sites that
//    manually reconstruct a RayIntersectionGeometric from a stored
//    BDPTVertex and then hand that `ri` to a BSDF / painter.  When a
//    new field was added to RayIntersectionGeometric (e.g. vertex
//    color) and to the BDPTVertex mirror, two of the four sites were
//    silently left out, biasing one BDPT strategy's BSDF evaluation
//    relative to the others and producing fireflies in MIS-weighted
//    output.  We've since centralised every reconstruction through
//    PopulateRIGFromVertex.  This test is the canary that catches
//    regression of that helper: if a future refactor drops a field
//    from the helper, the corresponding sentinel assertion below
//    fails.  When adding a new mirrored field, extend this test with
//    one more sentinel-value assertion.
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>

#include "../src/Library/Shaders/BDPTVertex.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Utilities/PathVertexEval.h"

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

static bool IsClose( Scalar a, Scalar b, Scalar eps = 1e-9 )
{
	return std::fabs( a - b ) < eps;
}

//////////////////////////////////////////////////////////////////////
// FillSurfaceStateSentinels
//
// Populates a BDPTVertex with non-default, distinguishable values for
// every field that PopulateRIGFromVertex copies.  Each scalar is
// unique so a swapped-source field would produce a detectable mismatch.
//////////////////////////////////////////////////////////////////////
static BDPTVertex MakeSentinelSurfaceVertex()
{
	BDPTVertex v;
	v.type = BDPTVertex::SURFACE;
	v.position      = Point3( 7.0,  -3.0,  11.0 );
	v.normal        = Vector3( 0.0,  1.0,   0.0 );
	v.onb.CreateFromW( v.normal );
	v.ptCoord       = Point2( 0.375, 0.625 );
	v.ptObjIntersec = Point3( 0.125, -0.875, 0.5 );
	v.vColor          = RISEPel( 0.42, 0.71, 0.13 );
	v.bHasVertexColor = true;
	return v;
}

//////////////////////////////////////////////////////////////////////
// TestPopulateRIG_AllFields
//
// The canonical sentinel-value test.  Every field in the helper's copy
// list must be propagated.  If a future refactor drops a field, the
// matching Check fails and points at the omitted line in
// PathVertexEval::PopulateRIGFromVertex.
//////////////////////////////////////////////////////////////////////
void TestPopulateRIG_AllFields()
{
	std::cout << "Testing PopulateRIGFromVertex copies every mirrored field..." << std::endl;

	const BDPTVertex v = MakeSentinelSurfaceVertex();

	const Ray dummyRay( Point3( 0, 0, 0 ), Vector3( 1, 0, 0 ) );
	RayIntersectionGeometric ri( dummyRay, nullRasterizerState );

	PathVertexEval::PopulateRIGFromVertex( v, ri );

	// bHit
	Check( ri.bHit == true,
		"bHit should be set to true" );

	// position -> ptIntersection
	Check( IsClose( ri.ptIntersection.x, 7.0 )  &&
		   IsClose( ri.ptIntersection.y, -3.0 ) &&
		   IsClose( ri.ptIntersection.z, 11.0 ),
		"ptIntersection should mirror vertex.position" );

	// normal -> vNormal
	Check( IsClose( ri.vNormal.x, 0.0 ) &&
		   IsClose( ri.vNormal.y, 1.0 ) &&
		   IsClose( ri.vNormal.z, 0.0 ),
		"vNormal should mirror vertex.normal" );

	// onb (compare W axis — the onb was built from the normal)
	Check( IsClose( ri.onb.w().x, 0.0 ) &&
		   IsClose( ri.onb.w().y, 1.0 ) &&
		   IsClose( ri.onb.w().z, 0.0 ),
		"onb.w() should mirror the vertex onb (built from normal)" );

	// ptCoord
	Check( IsClose( ri.ptCoord.x, 0.375 ) &&
		   IsClose( ri.ptCoord.y, 0.625 ),
		"ptCoord should mirror vertex.ptCoord" );

	// ptObjIntersec
	Check( IsClose( ri.ptObjIntersec.x, 0.125 )  &&
		   IsClose( ri.ptObjIntersec.y, -0.875 ) &&
		   IsClose( ri.ptObjIntersec.z, 0.5 ),
		"ptObjIntersec should mirror vertex.ptObjIntersec" );

	// vColor — the firefly-prone field that prompted the helper.
	Check( IsClose( ri.vColor.r, 0.42 ) &&
		   IsClose( ri.vColor.g, 0.71 ) &&
		   IsClose( ri.vColor.b, 0.13 ),
		"vColor should mirror vertex.vColor" );

	// bHasVertexColor — gates whether painters consume vColor.
	Check( ri.bHasVertexColor == true,
		"bHasVertexColor should mirror vertex.bHasVertexColor" );
}

//////////////////////////////////////////////////////////////////////
// TestPopulateRIG_DefaultsAlsoCopy
//
// Confirms the helper isn't selectively copying "interesting" values:
// a vertex with bHasVertexColor=false and a zero vColor must produce
// the same false flag and zero color in `ri`.  Important because the
// painter's behaviour gates on bHasVertexColor; a copy that always set
// it to true (or always set vColor to a sentinel) would silently break
// non-vertex-color materials.
//////////////////////////////////////////////////////////////////////
void TestPopulateRIG_DefaultsAlsoCopy()
{
	std::cout << "Testing PopulateRIGFromVertex preserves default/false values..." << std::endl;

	BDPTVertex v;
	v.type = BDPTVertex::SURFACE;
	v.position        = Point3( 0, 0, 0 );
	v.normal          = Vector3( 0, 0, 1 );
	v.onb.CreateFromW( v.normal );
	// Leave ptCoord, ptObjIntersec, vColor as default-constructed (zero).
	// bHasVertexColor is already false from the BDPTVertex default ctor.

	const Ray dummyRay( Point3( 0, 0, 0 ), Vector3( 1, 0, 0 ) );
	RayIntersectionGeometric ri( dummyRay, nullRasterizerState );

	// Pre-set the destination ri's vertex-color fields to sentinel
	// values so we can detect whether the helper actually overwrites
	// them — a no-op helper would let the sentinel through.
	ri.vColor          = RISEPel( 9.99, 9.99, 9.99 );
	ri.bHasVertexColor = true;

	PathVertexEval::PopulateRIGFromVertex( v, ri );

	Check( IsClose( ri.vColor.r, 0.0 ) &&
		   IsClose( ri.vColor.g, 0.0 ) &&
		   IsClose( ri.vColor.b, 0.0 ),
		"vColor should be overwritten with vertex.vColor (zero) — not the pre-existing sentinel" );

	Check( ri.bHasVertexColor == false,
		"bHasVertexColor should be overwritten with vertex.bHasVertexColor (false)" );
}

//////////////////////////////////////////////////////////////////////
// main
//////////////////////////////////////////////////////////////////////
int main()
{
	std::cout << "=== BDPTVertexRIGRebuildTest ===" << std::endl;

	TestPopulateRIG_AllFields();
	TestPopulateRIG_DefaultsAlsoCopy();

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;

	return failCount == 0 ? 0 : 1;
}
