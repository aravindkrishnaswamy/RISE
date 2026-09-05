//////////////////////////////////////////////////////////////////////
//
//  CsgFloorOwnershipTest.cpp
//
//  Regression coverage for the two OWNERSHIP defects the adversarial
//  review of 7923bf2f found in the self-hit-floor machinery.  Both are
//  the same mistake at two different layers: a filter that is supposed
//  to charge the floor of whatever actually owns the queried point
//  instead charges (or drops) the wrong thing, and the resulting number
//  is off by one to eight orders of magnitude.
//
//    P2-1 (CSGObject).  `CSGObject::SelfHitRootFloor` decides which
//      operand owns a face by firing a short ray along the face normal
//      through the point, over a window `delta = 1e-6 * (1 + |o|_1)`.
//      Two ways that mis-fires:
//
//        (a) OWNERSHIP WINDOW vs the child's OWN floor.  A child whose
//            own floor EXCEEDS 2*delta can never be hit inside the
//            window -- so the operand whose gate matters most is
//            precisely the one the filter drops.  Test 1: a union of an
//            SDF sphere R=4 (own floor 2.77e-4; delta is 5e-6, deep
//            inside March's 2*m_eps step-off band, so the ownership ray
//            is marched straight past it and reports a miss) with a box
//            whose -Z face is COINCIDENT with the sphere's -Z pole.  The
//            composite reported the box's 4.06e-12 -- 68 MILLION times
//            under the SDF's real requirement.  The exit probe then
//            stands off less than the SDF needs, the box wins the re-hit
//            inside the window, and the union adopts the wrong operand's
//            payload on a face the SDF may own.
//
//        (b) TANGENCY.  On a face two operands share exactly, the
//            ownership ray runs in the plane of one of them and its slab
//            test misses.  Test 2: two boxes meeting along an edge.  At
//            exactly y = 1 the long box misses and the unit box hits; a
//            hair below (y = 1 - 1e-15) the verdict flips.  No choice of
//            ray direction or length cures it -- the reverse orientation
//            and a millionfold-longer ray were both measured and both
//            still miss -- so a child the ray misses gets a second,
//            orientation-free chance against its bounding-box SHELL.
//
//    P2-2 (SDFGeometry).  `SDFGeometry::SelfHitRootFloor` divided
//      March's 2*m_eps step-off band by the smallest Lipschitz shrink
//      ratio ANY part applies -- a GLOBAL minimum, which charges the
//      whole field the worst squash any lobe applies anywhere.  Test 3:
//      `union(sphere R=3 uniform, sphere R=3 scaled (0.02,1,1) at x=8)`,
//      queried on the exit face of the UNIFORM lobe, claimed 1.39e-2
//      against a bisected gate of 2.79e-4 -- a 50x over-statement, 6x
//      past the contract's bound (4x now, 8x when first measured), and a ~2.9e-2 WORLD-UNIT same-face
//      acceptance window once the probe's 2x margin and 10 % slack are
//      applied.  That is the decoy-face trade CsgSurfacePayloadTest
//      Test 15 bounds, re-opened at a scale where a whole second lobe
//      fits inside it.
//
//  Test map:
//    1. Coincident -Z face, CSG_UNION(SDF sphere R=4, box 2x2x8): the
//       composite floor is the SDF's own, not the box's (P2-1a).
//    2. Two boxes sharing an edge: BOTH own it, so the composite charges
//       the grazed operand's much wider gate (P2-1b).
//    3. SDF two-lobe union: the claim on the uniform lobe's exit face
//       brackets the BISECTED gate the way BoxGeometryTest's
//       `CheckFloorBrackets` requires -- never under, never past 4x
//       (P2-2).  `MeasureGate` below is that helper's bisection, copied.
//    4. The widened window did NOT re-admit unrelated siblings: a mesh
//       lobe parked 1e6 units away has an own floor of ~3e-6, WIDER than
//       the base `delta`, and is still excluded.  This is the case that
//       rejects the alternative fix ("treat floorChild >= delta as an
//       automatic owner"), which would adopt it.
//
//  Style follows tests/CsgProbeFloorTest.cpp (`Check` probes, plain
//  counters, no framework).  Separate file from that one because these
//  are ownership-attribution mechanics rather than the finite-floor and
//  sibling-distance mechanics it covers.
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
#include <vector>

#include "../src/Library/Geometry/BoxGeometry.h"
#include "../src/Library/Geometry/SDFGeometry.h"
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
	RayIntersectionGeometric MakeIntersection( const Point3& origin, const Vector3& dir )
	{
		return RayIntersectionGeometric( Ray( origin, dir ), nullRasterizerState );
	}

	SDFGeometry* MakeSdfSphere( const Scalar radius )
	{
		std::vector<SDFGeometry::Part> parts;
		parts.push_back( SDFGeometry::MakePart(
			SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
			Point3( 0, 0, 0 ), 0, 0, 0, Vector3( 1, 1, 1 ), radius, 0, 0, 0 ) );
		return new SDFGeometry( parts, 512, Scalar(1e-5) );
	}

	// A single big triangle whose vertices all sit ~1e6 units out, i.e. a
	// bbox whose corner L1 is ~3e6 and whose RayTriangleIntersection gate is
	// therefore ~3e-6 -- deliberately WIDER than the base ownership window
	// `delta` at a unit-scale query point (~2e-6).  That is what makes Test 4
	// discriminating: the rejected "floorChild >= delta means automatic
	// owner" rule would adopt this lobe.
	TriangleMeshGeometryIndexed* MakeVeryFarTriangle()
	{
		TriangleMeshGeometryIndexed* g = new TriangleMeshGeometryIndexed( true, true );
		g->BeginIndexedTriangles();
		g->AddVertex( Point3( 1.0e6, 1.0e6, 1.0e6 ) );
		g->AddVertex( Point3( 1.0e6 + 40.0, 1.0e6, 1.0e6 ) );
		g->AddVertex( Point3( 1.0e6, 1.0e6 + 40.0, 1.0e6 ) );
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

	// The production caller's probe: stand off `standoff` past the face along
	// the original ray direction, fire back, and require a hit at range
	// <= 2.1 * standoff -- i.e. THIS face, not the far side.  Copied from
	// BoxGeometryTest's `AcceptsStandoff`, which is what the SelfHitRootFloor
	// contract is measured against there.
	bool AcceptsStandoff( const IGeometry* pGeom, const Point3& ptFace, const Vector3& exitDir, double standoff )
	{
		const Vector3 probeDir( -exitDir.x, -exitDir.y, -exitDir.z );
		const Point3 o( ptFace.x + exitDir.x * standoff,
		                ptFace.y + exitDir.y * standoff,
		                ptFace.z + exitDir.z * standoff );
		RayIntersectionGeometric ri = MakeIntersection( o, probeDir );
		pGeom->IntersectRay( ri, true, true, false );
		return ri.bHit && ri.range <= 2.1 * standoff;
	}

	// Bisect for the smallest standoff the intersection routine actually
	// accepts -- BoxGeometryTest's `CheckFloorBrackets` bisection, copied so
	// this file can compare a CLAIM to a MEASUREMENT rather than to another
	// hand-computed constant.  Returns 0 on an unbracketed interval, which the
	// caller asserts against.
	double MeasureGate( const IGeometry* pGeom, const Point3& ptFace, const Vector3& exitDir,
	                    double loSeed, double hiSeed )
	{
		if( AcceptsStandoff( pGeom, ptFace, exitDir, loSeed ) ) { return 0.0; }
		if( !AcceptsStandoff( pGeom, ptFace, exitDir, hiSeed ) ) { return 0.0; }
		double lo = loSeed, hi = hiSeed;
		for( int i = 0; i < 200; i++ ) {
			const double mid = std::sqrt( lo * hi );
			if( AcceptsStandoff( pGeom, ptFace, exitDir, mid ) ) { hi = mid; } else { lo = mid; }
			if( hi <= lo * 1.000001 ) { break; }
		}
		return hi;
	}
}

//
// Test 1 (P2-1a): the ownership window must never be narrower than the
// child's OWN self-hit floor.
//
// CSG_UNION of an SDF sphere R=4 (spanning z in [-4,4]) with a box
// 2 x 2 x 8 (spanning z in [-4,4] too), queried at (0, 0, -4) -- the
// sphere's -Z pole, which is also a point of the box's -Z face, so BOTH
// operands genuinely own it.  The SDF's own floor there is 2*m_eps =
// 2.77e-4; the box's is 4.06e-12.  The base window is
// delta = 1e-6 * (1 + 4) = 5e-6, which is INSIDE March's 2*m_eps step-off
// band, so the ownership ray is marched straight past the sphere and
// reports a miss -- the sphere is dropped and the composite claims the
// box's figure, 68 million times under what the SDF needs.
//
void TestOwnershipWindowCoversChildFloor()
{
	std::cout << "Ownership window is never narrower than the child's own floor (P2-1a)..." << std::endl;

	SDFGeometry* gSdf = MakeSdfSphere( 4.0 );
	BoxGeometry* gBox = new BoxGeometry( 2.0, 2.0, 8.0 );   // [-1,1] x [-1,1] x [-4,4]

	Object* oSdf = new Object( gSdf );
	Object* oBox = new Object( gBox );
	safe_release( gSdf );
	safe_release( gBox );

	oSdf->SetPosition( Point3( 0, 0, 0 ) );
	oSdf->FinalizeTransformations();
	oBox->SetPosition( Point3( 0, 0, 0 ) );
	oBox->FinalizeTransformations();

	CSGObject* csg = new CSGObject( CSG_UNION );
	Check( csg->AssignObjects( oSdf, oBox ), "Test1: composite takes sdf(A)/box(B) operands" );
	csg->FinalizeTransformations();

	// The coincident face, and the probe geometry the exit-face probe would
	// hand the composite there.
	const Point3  ptFace( 0.0, 0.0, -4.0 );
	const Vector3 dirIn( 0.0, 0.0, -1.0 );
	const Vector3 nOut( 0.0, 0.0, -1.0 );

	const Scalar floorSdf = oSdf->SelfHitRootFloor( ptFace, dirIn, nOut );
	const Scalar floorBox = oBox->SelfHitRootFloor( ptFace, dirIn, nOut );
	const Scalar floorCsg = csg->SelfHitRootFloor( ptFace, dirIn, nOut );
	std::printf( "  sdf %.6g   box %.6g   composite %.6g\n", floorSdf, floorBox, floorCsg );

	// Sanity: the two gates really are orders apart, and the SDF's exceeds the
	// base ownership window -- which is the whole mechanism of the defect.
	const Scalar deltaBase = 1e-6 * ( 1.0 + 4.0 );
	Check( floorSdf > 1e-5, "Test1: (sanity) the SDF's own gate is sphere-tracer scale, ~2.8e-4" );
	Check( floorBox < 1e-10, "Test1: (sanity) the box's own gate is ulp-scale" );
	Check( floorSdf > 2.0 * deltaBase,
		"Test1: (sanity) the SDF's gate EXCEEDS the base ownership window -- the defect's precondition" );

	// MONEY: the composite must cover the SDF's requirement, not the box's.
	Check( floorCsg >= floorSdf,
		"Test1: MONEY ASSERTION -- composite floor covers the SDF operand's own floor" );
	Check( floorCsg > 1e-5,
		"Test1: MONEY ASSERTION -- composite floor is NOT the box's ulp-scale 4e-12" );

	// And it is not INFLATED past what the owning operand asks for either.
	Check( floorCsg <= floorSdf * 1.000001,
		"Test1: composite floor is exactly the widest OWNER's, not more" );

	safe_release( csg );
	safe_release( oSdf );
	safe_release( oBox );
}

//
// Test 2 (P2-1b): a face two operands share EXACTLY must charge both.
//
// A long box spanning x in [-4e6+1, 1], y in [-1,1] and a unit box
// spanning x in [1,3], y in [1,3] meet along the edge x = 1, y = 1.
// Queried at (1, 1, 0) with the long box's +X face normal, the ownership
// ray runs exactly in the long box's y = +1 face plane, and its slab test
// misses (measured: at exactly y = 1 the long box misses and the small box
// hits; at y = 1 - 1e-15 the verdict flips).  The long box's own gate is
// ~2.8e-8 -- it reads |o.x| = 2e6 in its own frame -- against the small
// box's 4e-12, so dropping it under-states the composite by ~7000x on a
// face it genuinely owns.
//
void TestSharedEdgeChargesBothOwners()
{
	std::cout << "A shared edge charges BOTH owners, tangency notwithstanding (P2-1b)..." << std::endl;

	BoxGeometry* gLong  = new BoxGeometry( 4.0e6, 2.0, 2.0 );   // half-extent 2e6 in x
	BoxGeometry* gSmall = new BoxGeometry( 2.0, 2.0, 2.0 );     // half-extent 1

	Object* oLong  = new Object( gLong );
	Object* oSmall = new Object( gSmall );
	safe_release( gLong );
	safe_release( gSmall );

	oLong->SetPosition( Point3( -2.0e6 + 1.0, 0.0, 0.0 ) );     // x in [-4e6+1, 1], y in [-1,1]
	oLong->FinalizeTransformations();
	oSmall->SetPosition( Point3( 2.0, 2.0, 0.0 ) );             // x in [1,3],      y in [1,3]
	oSmall->FinalizeTransformations();

	CSGObject* csg = new CSGObject( CSG_UNION );
	Check( csg->AssignObjects( oLong, oSmall ), "Test2: composite takes long(A)/small(B) operands" );
	csg->FinalizeTransformations();

	const Point3  ptEdge( 1.0, 1.0, 0.0 );      // the shared edge
	const Vector3 dirIn( 1.0, 0.0, 0.0 );
	const Vector3 nOut( 1.0, 0.0, 0.0 );        // the LONG box's +X face normal

	// The long box's real requirement, asked in its OWN frame the way the
	// composite asks it (|o.x| = 2e6 there, not 1).
	const Scalar floorLongOwn = oLong->SelfHitRootFloor( Point3( 2.0e6, 1.0, 0.0 ), dirIn, nOut );
	const Scalar floorSmallOwn = oSmall->SelfHitRootFloor( Point3( -1.0, -1.0, 0.0 ), dirIn, nOut );
	const Scalar floorCsg = csg->SelfHitRootFloor( ptEdge, dirIn, nOut );
	std::printf( "  long %.6g   small %.6g   composite %.6g\n", floorLongOwn, floorSmallOwn, floorCsg );

	// Sanity: the tangency really is there -- the ownership ray, fired at the
	// base window, misses the long box and hits the small one.
	const Scalar deltaBase = 1e-6 * ( 1.0 + 1.0 + 1.0 );
	const Ray ownRay( Point3( ptEdge.x - deltaBase, ptEdge.y, ptEdge.z ), Vector3( 1, 0, 0 ) );
	Check( !oLong->IntersectRay_IntersectionOnly( ownRay, 2.0 * deltaBase, true, true ),
		"Test2: (sanity) the ownership ray MISSES the long box -- it runs in that box's face plane" );
	Check( oSmall->IntersectRay_IntersectionOnly( ownRay, 2.0 * deltaBase, true, true ),
		"Test2: (sanity) the same ray hits the small box" );
	Check( floorLongOwn > floorSmallOwn * 1000.0,
		"Test2: (sanity) the grazed operand's gate is >1000x the one the ray finds" );

	// MONEY: the composite covers the grazed co-owner's gate anyway.
	Check( floorCsg >= floorLongOwn * 0.999999,
		"Test2: MONEY ASSERTION -- composite floor covers the GRAZED co-owner's own floor" );
	Check( floorCsg > 1e-9,
		"Test2: MONEY ASSERTION -- composite floor is NOT the small box's ulp-scale 4e-12" );
	Check( floorCsg <= floorLongOwn * 1.000001,
		"Test2: composite floor is exactly the widest owner's, not more" );

	safe_release( csg );
	safe_release( oLong );
	safe_release( oSmall );
}

//
// Test 3 (P2-2): the SDF's Lipschitz divisor comes from the part that OWNS
// the point, not from the worst part anywhere in the field.
//
// The review's configuration: a uniform sphere R=3 at the origin unioned
// with a sphere R=3 squashed to (0.02, 1, 1) and parked at x = 8.  On the
// UNIFORM lobe's exit face the field is 1-Lipschitz -- nothing there is
// squashed -- but the global minimum charged the thin lobe's 0.02, and the
// claim came out 50x the gate the sphere-tracer actually enforces.
//
// Measured by bisection, exactly as BoxGeometryTest's `CheckFloorBrackets`
// does, and held to that contract's own two-sided bound.
//
void TestSdfShrinkComesFromOwningPart()
{
	std::cout << "SDF Lipschitz shrink comes from the OWNING part (P2-2)..." << std::endl;

	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart(              // uniform lobe at the origin
		SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3( 0, 0, 0 ), 0, 0, 0, Vector3( 1, 1, 1 ), 3.0, 0, 0, 0 ) );
	parts.push_back( SDFGeometry::MakePart(              // thin lobe, 8 units away
		SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3( 8, 0, 0 ), 0, 0, 0, Vector3( 0.02, 1, 1 ), 3.0, 0, 0, 0 ) );
	SDFGeometry* g = new SDFGeometry( parts, 512, Scalar(1e-5) );

	// Exit face of the uniform lobe, read from the geometry's own range2
	// rather than assumed.
	const Vector3 dir( 0.0, 0.0, -1.0 );
	const Vector3 nOut( 0.0, 0.0, -1.0 );
	const Point3  farOrigin( 0.0, 0.0, 12.0 );
	RayIntersectionGeometric riCam = MakeIntersection( farOrigin, dir );
	g->IntersectRay( riCam, true, true, true );
	Check( riCam.bHit, "Test3: (setup) camera ray hits the uniform lobe" );
	Check( riCam.range2 > riCam.range, "Test3: (setup) the lobe has an exit face" );
	const Point3 ptExit = Ray( farOrigin, dir ).PointAtLength( riCam.range2 );

	const Scalar claim = g->SelfHitRootFloor( ptExit, Vector3( 0, 0, 1 ), nOut );
	// hiFrac 1e-1 for the same reason BoxGeometryTest's SDF row raises it: a
	// sphere-tracer's gate is m_epsFrac of the field's bbox diagonal, six
	// orders coarser than an analytic primitive's ulp-scale gate.
	const double gate = MeasureGate( g, ptExit, dir, 1e-18, 1e-1 );
	std::printf( "  claim %.6g   measured gate %.6g   ratio %.6g\n", claim, gate, gate > 0 ? claim / gate : 0.0 );

	Check( gate > 0.0, "Test3: (setup) the bisection is bracketed" );

	// The SelfHitRootFloor contract, both sides (BoxGeometryTest's own bounds).
	Check( claim >= gate * 0.999,
		"Test3: claim never UNDER-states the sphere-tracer's real gate" );
	Check( claim <= gate * 4.0,
		"Test3: MONEY ASSERTION -- claim never OVER-states past 4x (was 50x, the thin lobe's 0.02 charged here)" );
	Check( AcceptsStandoff( g, ptExit, dir, 2.0 * claim ),
		"Test3: standing off 2x the claim re-hits the SAME face" );

	// And the thin lobe's own face still gets the thin lobe's divisor -- the
	// fix must not have simply deleted the shrink.
	const Point3 farOriginThin( 8.0, 0.0, 12.0 );
	RayIntersectionGeometric riThin = MakeIntersection( farOriginThin, dir );
	g->IntersectRay( riThin, true, true, true );
	Check( riThin.bHit, "Test3: (setup) camera ray hits the thin lobe" );
	const Point3 ptExitThin = Ray( farOriginThin, dir ).PointAtLength( riThin.range2 );
	const Scalar claimThin = g->SelfHitRootFloor( ptExitThin, Vector3( 0, 0, 1 ), nOut );
	std::printf( "  thin-lobe claim %.6g (uniform-lobe claim %.6g)\n", claimThin, claim );
	Check( claimThin > claim * 10.0,
		"Test3: the THIN lobe's own face still carries the thin lobe's much wider divisor" );

	safe_release( g );
}

//
// Test 4 (P2-1, no regression): widening the window must not re-admit
// unrelated siblings.
//
// This is the case that rejects the alternative fix the review offered --
// "treat floorChild >= delta as an automatic owner".  A triangle-mesh lobe
// parked 1e6 units away claims ~3e-6, which is WIDER than the base window
// delta (~2.2e-6) at the unit-scale query point, so the automatic-owner
// rule would charge it on a face it is a million units from.  The window is
// widened instead, which keeps the test geometric: the sibling's surface
// still has to pass through it, and it does not.
//
void TestWideSiblingStillExcluded()
{
	std::cout << "A wide-gated but distant sibling is still excluded (P2-1, no regression)..." << std::endl;

	BoxGeometry* gBox = new BoxGeometry( 2.0, 2.0, 2.0 );       // [-1,1]^3
	TriangleMeshGeometryIndexed* gFar = MakeVeryFarTriangle();  // ~1e6 units out

	Object* box = new Object( gBox );
	Object* far = new Object( gFar );
	safe_release( gBox );
	safe_release( gFar );

	box->SetPosition( Point3( 0, 0, 0 ) );
	box->FinalizeTransformations();
	far->SetPosition( Point3( 0, 0, 0 ) );
	far->FinalizeTransformations();

	CSGObject* csg = new CSGObject( CSG_UNION );
	Check( csg->AssignObjects( box, far ), "Test4: composite takes box(A)/far-mesh(B) operands" );
	csg->FinalizeTransformations();

	const Point3  ptFace( 0.1, 0.05, -1.0 );    // on the box's -Z face
	const Vector3 dirIn( 0.0, 0.0, 1.0 );
	const Vector3 nOut( 0.0, 0.0, -1.0 );

	const Scalar floorBox = box->SelfHitRootFloor( ptFace, dirIn, nOut );
	const Scalar floorFar = far->SelfHitRootFloor( ptFace, dirIn, nOut );
	const Scalar floorCsg = csg->SelfHitRootFloor( ptFace, dirIn, nOut );
	const Scalar deltaBase = 1e-6 * ( 1.0 + 0.1 + 0.05 + 1.0 );
	std::printf( "  box %.6g   far %.6g (delta %.6g)   composite %.6g\n",
		floorBox, floorFar, deltaBase, floorCsg );

	// Sanity: this sibling is exactly the shape that defeats the rejected
	// alternative -- its own gate is wider than the base window.
	Check( floorFar > deltaBase,
		"Test4: (sanity) the distant sibling's own gate EXCEEDS the base window" );
	Check( floorFar > floorBox * 1000.0, "Test4: (sanity) the sibling gate is >1000x the box's" );

	// MONEY: the composite still reports the OWNING box's floor.
	Check( floorCsg <= floorBox * 1.000001,
		"Test4: MONEY ASSERTION -- composite floor is still the OWNING box's" );
	Check( floorCsg < 1e-10,
		"Test4: MONEY ASSERTION -- composite floor is NOT the distant sibling's ~3e-6" );

	safe_release( csg );
	safe_release( box );
	safe_release( far );
}

int main()
{
	std::cout << "CSG / SDF self-hit-floor OWNERSHIP tests" << std::endl;

	TestOwnershipWindowCoversChildFloor();
	TestSharedEdgeChargesBothOwners();
	TestSdfShrinkComesFromOwningPart();
	TestWideSiblingStillExcluded();

	std::cout << "\n" << g_pass << " checks passed, " << g_fail << " failed." << std::endl;
	return g_fail == 0 ? 0 : 1;
}
