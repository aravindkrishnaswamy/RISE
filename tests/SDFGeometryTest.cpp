//////////////////////////////////////////////////////////////////////
//
//  SDFGeometryTest.cpp - Unit tests for the sphere-traced signed-
//  distance-field geometry.  Validates that ray intersection + ray
//  marching are correct independent of any scene: hit distance and
//  normal match the analytic primitive across many ray directions,
//  smooth-min bulges the seam outward, the bounding box contains the
//  surface, shadow (intersection-only) queries respect dHowFar, misses
//  miss, inside-start rays exit, and transforms / non-uniform scale
//  land where expected.
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <vector>
#include "../src/Library/Geometry/SDFGeometry.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Interfaces/IFunction2D.h"
#include "../src/Library/Utilities/Reference.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool cond, const char* name )
{
	if( cond ) { ++passCount; }
	else { ++failCount; std::cout << "  FAIL: " << name << std::endl; }
}

static bool IsClose( Scalar a, Scalar b, Scalar eps = 2e-3 ) { return std::fabs(a-b) <= eps; }
static Scalar Len( const Vector3& v ) { return std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z); }
static bool VClose( const Vector3& a, const Vector3& b, Scalar eps = 6e-3 )
{
	return IsClose(a.x,b.x,eps) && IsClose(a.y,b.y,eps) && IsClose(a.z,b.z,eps);
}
static RayIntersectionGeometric MkRI( const Point3& o, const Vector3& d )
{
	return RayIntersectionGeometric( Ray(o,d), nullRasterizerState );
}
static Vector3 norm3( Scalar x, Scalar y, Scalar z )
{
	const Scalar l = std::sqrt(x*x+y*y+z*z);
	return Vector3( x/l, y/l, z/l );
}

// build an SDFGeometry from a single part / a list (tight eps for accuracy)
static SDFGeometry* MakeGeom( const std::vector<SDFGeometry::Part>& parts )
{
	return new SDFGeometry( parts, 512, Scalar(1e-5) );
}

//////////////////////////////////////////////////////////////////////

static void TestSphereMatchesAnalytic()
{
	std::cout << "Test 1: SDF sphere == analytic sphere (range + normal, many dirs)" << std::endl;
	const Scalar R = 5.0;
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), R, 0, 0, 0 ) );
	SDFGeometry* g = MakeGeom( parts );

	// cast from |P|=20 toward the origin: the near hit is at distance 20-R, normal = P/|P|
	const Point3 origins[6] = {
		Point3(0,0,20), Point3(20,0,0), Point3(0,20,0),
		Point3(12,12,8), Point3(-9,6,16), Point3(-11,-11,-9)
	};
	for( int i = 0; i < 6; ++i )
	{
		const Point3 P = origins[i];
		const Scalar Rmag = std::sqrt(P.x*P.x + P.y*P.y + P.z*P.z);
		const Vector3 dir( -P.x/Rmag, -P.y/Rmag, -P.z/Rmag );  // toward origin
		RayIntersectionGeometric ri = MkRI( P, dir );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit, "sphere ray hits" );
		Check( IsClose( ri.range, Rmag - R, 3e-3 ), "sphere hit distance == analytic" );
		Check( VClose( ri.vNormal, norm3(P.x,P.y,P.z) ), "sphere normal == radial" );
		Check( IsClose( Len(ri.vNormal), 1.0, 1e-4 ), "sphere normal is unit" );
	}
	safe_release( g );
}

static void TestBoxMatchesAnalytic()
{
	std::cout << "Test 2: SDF box == analytic box (face hits + normals)" << std::endl;
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpUnion, 0,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), 3.0, 2.0, 4.0, 0 ) );   // half-extents
	SDFGeometry* g = MakeGeom( parts );

	{	RayIntersectionGeometric ri = MkRI( Point3(0,0,20), Vector3(0,0,-1) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose(ri.range, 16.0), "box +Z face hit distance" );
		Check( VClose(ri.vNormal, Vector3(0,0,1)), "box +Z face normal" ); }
	{	RayIntersectionGeometric ri = MkRI( Point3(20,0,0), Vector3(-1,0,0) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose(ri.range, 17.0), "box +X face hit distance" );
		Check( VClose(ri.vNormal, Vector3(1,0,0)), "box +X face normal" ); }
	{	RayIntersectionGeometric ri = MkRI( Point3(0,-20,0), Vector3(0,1,0) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose(ri.range, 18.0), "box -Y face hit distance" );
		Check( VClose(ri.vNormal, Vector3(0,-1,0)), "box -Y face normal" ); }
	safe_release( g );
}

static void TestSmoothMin()
{
	std::cout << "Test 3: smooth-min bulges the seam outward vs hard union" << std::endl;
	// two spheres r=4 at y=+/-3; a ray along +X through the neck (y=0,z=0)
	const Vector3 dir(1,0,0);
	const Point3  o(-20,0,0);

	std::vector<SDFGeometry::Part> uni;
	uni.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0, Point3(0,-3,0),0,0,0,Vector3(1,1,1),4,0,0,0 ) );
	uni.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0, Point3(0, 3,0),0,0,0,Vector3(1,1,1),4,0,0,0 ) );
	SDFGeometry* gu = MakeGeom( uni );

	std::vector<SDFGeometry::Part> smn;
	smn.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,   Point3(0,-3,0),0,0,0,Vector3(1,1,1),4,0,0,0 ) );
	smn.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpSmin,  3.0, Point3(0, 3,0),0,0,0,Vector3(1,1,1),4,0,0,0 ) );
	SDFGeometry* gs = MakeGeom( smn );

	RayIntersectionGeometric ru = MkRI(o,dir);  gu->IntersectRay( ru, true, true, false );
	RayIntersectionGeometric rs = MkRI(o,dir);  gs->IntersectRay( rs, true, true, false );
	Check( ru.bHit && rs.bHit, "both union + smin hit the neck" );
	// the smin fillet bulges OUTWARD at the neck, so the front surface is closer to the ray origin -> smaller range
	Check( rs.range < ru.range - 1e-3, "smin front is outside the hard-union crease (smaller range)" );
	safe_release( gu ); safe_release( gs );
}

static void TestBoundingBoxContainsSurface()
{
	std::cout << "Test 4: bounding box contains the surface" << std::endl;
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3(2,0,0), 0,0,0, Vector3(1,1,1), 5.0, 0,0,0 ) );
	SDFGeometry* g = MakeGeom( parts );
	BoundingBox bb = g->GenerateBoundingBox();
	// sphere r5 centred at (2,0,0) -> surface spans x in [-3,7], y,z in [-5,5]
	Check( bb.ll.x <= -3.0 && bb.ur.x >= 7.0, "bbox spans sphere in X" );
	Check( bb.ll.y <= -5.0 && bb.ur.y >= 5.0, "bbox spans sphere in Y" );
	Check( bb.ll.z <= -5.0 && bb.ur.z >= 5.0, "bbox spans sphere in Z" );
	// a hit should land inside the bbox
	RayIntersectionGeometric ri = MkRI( Point3(2,0,20), Vector3(0,0,-1) );
	g->IntersectRay( ri, true, true, false );
	Check( ri.bHit && ri.ptIntersection.z <= bb.ur.z + 1e-3 && ri.ptIntersection.z >= bb.ll.z - 1e-3,
	       "hit point inside bbox" );
	safe_release( g );
}

// The bbox must be OP-AWARE: an `intersect` part CLIPS the field to within its
// own box, and a `subtract` part only carves -- neither should inflate the AABB
// to a giant additive part's full extent.  This mirrors the domed-crystal
// construction (a sphere of radius ~68 kept only inside a thin cap slab, with
// marker cavities subtracted): unioning every part's box regardless of op would
// blow the box to +-68 even though the visible surface is a ~43-wide, ~3.5-tall
// cap, fattening the TLAS leaf and starving the marching-tet grid.
static void TestBoundingBoxOpAware()
{
	std::cout << "Test 4b: bbox respects intersect (clip) and subtract (no-grow)" << std::endl;

	// (1) giant sphere r=68 at z=-63 (apex ~+5), INTERSECT a keep-slab box
	// half(22,22,2.35) at z=3.65 -> the surviving solid is the cap in z in
	// [1.3, 6.0], xy within +-22.  The bbox must clip to the slab, NOT span the
	// whole sphere (which alone would give z in [-131, +5], xy in [-68, 68]).
	{
		std::vector<SDFGeometry::Part> parts;
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
			Point3(0,0,-63.0), 0,0,0, Vector3(1,1,1), 68.0, 0,0,0 ) );
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpIntersect, 0,
			Point3(0,0,3.65), 0,0,0, Vector3(1,1,1), 22.0, 22.0, 2.35, 0 ) );
		SDFGeometry* g = MakeGeom( parts );
		const BoundingBox bb = g->GenerateBoundingBox();
		// clipped to the slab (allow the small pad/margin on each side)
		Check( bb.ll.x >= -22.0 - 0.5 && bb.ur.x <= 22.0 + 0.5, "intersect clips X to the slab (not +-68)" );
		Check( bb.ll.y >= -22.0 - 0.5 && bb.ur.y <= 22.0 + 0.5, "intersect clips Y to the slab (not +-68)" );
		Check( bb.ll.z >= 1.3 - 0.5 && bb.ur.z <= 6.0 + 0.5,    "intersect clips Z to the slab (not z=-131)" );
		// but the box must still CONTAIN the surviving cap: a ray angled down at
		// the axis hits the cap's outer dome inside the slab (z in [1.3, 5]).
		// (A pure-axis ray would enter the clipped bbox EXACTLY at the sphere's
		// tangent apex z~=5.001 -- inside the marcher's surface band -- and step
		// off it, a known grazing-entry edge case unrelated to the clip; come in
		// at a slight angle so the entry is a clean transverse crossing.)
		RayIntersectionGeometric ri = MkRI( Point3(2,0,20), Vector3Ops::Normalize( Vector3(-0.05,0,-1) ) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && ri.ptIntersection.z >= 1.3 - 5e-3 && ri.ptIntersection.z <= 5.0 + 5e-3,
		       "clipped cap is hittable, hit lands on the cap (z in [1.3,5])" );
		Check( ri.ptIntersection.z <= bb.ur.z + 1e-3 && ri.ptIntersection.z >= bb.ll.z - 1e-3, "cap hit inside clipped bbox" );
		safe_release( g );
	}

	// (2) a SUBTRACT part reaching far outside the solid must not grow the box:
	// box half(3,3,3) at origin, subtract a huge box half(50,50,50) offset so it
	// pokes far in +X -- the carve shrinks the solid but the AABB stays ~[-3,3].
	{
		std::vector<SDFGeometry::Part> base;
		base.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpUnion, 0,
			Point3(0,0,0), 0,0,0, Vector3(1,1,1), 3,3,3, 0 ) );
		SDFGeometry* gBase = MakeGeom( base );
		const BoundingBox bbBase = gBase->GenerateBoundingBox();
		safe_release( gBase );

		std::vector<SDFGeometry::Part> parts;
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpUnion, 0,
			Point3(0,0,0), 0,0,0, Vector3(1,1,1), 3,3,3, 0 ) );
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpSubtract, 0,
			Point3(40,0,0), 0,0,0, Vector3(1,1,1), 50,50,50, 0 ) );
		SDFGeometry* g = MakeGeom( parts );
		const BoundingBox bb = g->GenerateBoundingBox();
		Check( IsClose( bb.ur.x, bbBase.ur.x, 1e-6 ) && IsClose( bb.ll.x, bbBase.ll.x, 1e-6 ), "subtract does not grow X" );
		Check( IsClose( bb.ur.y, bbBase.ur.y, 1e-6 ) && IsClose( bb.ll.y, bbBase.ll.y, 1e-6 ), "subtract does not grow Y" );
		Check( IsClose( bb.ur.z, bbBase.ur.z, 1e-6 ) && IsClose( bb.ll.z, bbBase.ll.z, 1e-6 ), "subtract does not grow Z" );
		safe_release( g );
	}

	// (3) ORDER-AWARENESS: the bbox must fold like Map() (a strict left fold),
	// NOT split by op-class.  Parts: union box half(1,1,1)@origin, INTERSECT box
	// half(1,1,1)@origin, then union box half(1,1,1)@(10,0,0).  The true solid is
	// ( box[-1,1] INTERSECT box[-1,1] ) UNION box[9,11] -> x in [-1,1] U [9,11],
	// so the AABB x-extent is [-1, 11].  An order-BLIND class split (union the two
	// additive boxes -> x[-1,11], intersect the clip box -> x[-1,1], combine ->
	// x[-1,1]) would clip the second lobe entirely and bbox-gate every ray to it.
	{
		std::vector<SDFGeometry::Part> parts;
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpUnion, 0,
			Point3(0,0,0), 0,0,0, Vector3(1,1,1), 1,1,1, 0 ) );
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpIntersect, 0,
			Point3(0,0,0), 0,0,0, Vector3(1,1,1), 1,1,1, 0 ) );
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpUnion, 0,
			Point3(10,0,0), 0,0,0, Vector3(1,1,1), 1,1,1, 0 ) );
		SDFGeometry* g = MakeGeom( parts );
		const BoundingBox bb = g->GenerateBoundingBox();
		// the bound must reach the second lobe (>= 11 minus the tiny safety pad)
		// AND still contain the first lobe (<= -1 plus the pad).
		Check( bb.ur.x >= 10.9, "order-aware bbox reaches the post-intersect union lobe (x>=~11)" );
		Check( bb.ll.x <= -0.9, "order-aware bbox still contains the first lobe (x<=~-1)" );
		// the regression an order-blind bbox would cause: a ray aimed at the second
		// lobe gets gated out by the (wrongly clipped) box and never hits.  Fire one
		// straight down onto the top face of the second box (top at z=+1).
		RayIntersectionGeometric ri = MkRI( Point3(10,0,5), Vector3(0,0,-1) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose( ri.ptIntersection.z, 1.0, 5e-3 ), "second lobe is hittable (top face z=+1)" );
		safe_release( g );
	}
}

// A WIDE-but-THIN field has a HUGE bbox diagonal, so m_eps (= diagonal*epsFrac)
// and the marcher's surfBand (= 2*m_eps) blow up well past a fixed 1e-3 bbox pad.
// The old fixed pad then let a camera ray entering through the thin TOP face land
// INSIDE the surface band: the step-off logic (meant only for continuation rays
// spawned ON a surface) marched it DOWN into the solid, read side=-1, concluded
// the ray began inside, and reported the EXIT (bottom) face -- the top face was
// silently skipped (emissive boxes whose sides glow but tops render dark/holes).
// The fix sizes the bbox pad to clear the band (pad = max(1e-3, 3*eps)).  Build
// the field at the SCENE's epsFrac (5e-5) so the band (~5.4e-3) is unambiguously
// larger than the old 1e-3 pad -- this case fails on the pre-fix engine.
static void TestWideThinTopFaceEntry()
{
	std::cout << "Test 4c: wide-thin SDF -- top-face entry not mis-sided into the exit" << std::endl;
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpUnion, 0,
		Point3(0,0,1.5), 0,0,0, Vector3(1,1,1), 19.0, 19.0, 0.2, 0 ) );  // half-extents: top z=1.7, bottom z=1.3
	SDFGeometry* g = new SDFGeometry( parts, 512, Scalar(5e-5) );          // scene epsFrac -> band ~5.4e-3 >> old 1e-3 pad

	// Camera ray straight down through the TOP face: must HIT the TOP (z~=1.7),
	// NOT tunnel to the bottom (z~=1.3 is what the old mis-siding reported), and
	// the front-face normal must point +Z.
	{	RayIntersectionGeometric ri = MkRI( Point3(5,5,50), Vector3(0,0,-1) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit, "wide-thin: top-entry ray hits" );
		Check( ri.bHit && IsClose( ri.ptIntersection.z, 1.7, 1e-2 ), "wide-thin: hit lands on the TOP face z~=1.7 (not the exit z=1.3)" );
		Check( ri.bHit && ri.ptIntersection.z > 1.5, "wide-thin: hit is the top half, never the bottom exit" );
		Check( VClose( ri.vNormal, Vector3(0,0,1) ), "wide-thin: top-face normal points +Z (front side)" ); }

	// Entry through a SIDE face still works (the thin axis is z; +X face at x=19).
	{	RayIntersectionGeometric ri = MkRI( Point3(50,5,1.5), Vector3(-1,0,0) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose( ri.ptIntersection.x, 19.0, 1e-2 ), "wide-thin: side-face entry hits +X face x~=19" );
		Check( VClose( ri.vNormal, Vector3(1,0,0) ), "wide-thin: side-face normal points +X" ); }
	safe_release( g );
}

static void TestShadowQuery()
{
	std::cout << "Test 5: IntersectRay_IntersectionOnly (shadow) hit/miss + dHowFar" << std::endl;
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), 5.0, 0,0,0 ) );
	SDFGeometry* g = MakeGeom( parts );

	// toward the sphere from z=20: surface at z=5 -> distance 15
	Check(  g->IntersectRay_IntersectionOnly( Ray(Point3(0,0,20), Vector3(0,0,-1)), 100.0, true, true ), "shadow ray toward sphere hits" );
	Check( !g->IntersectRay_IntersectionOnly( Ray(Point3(0,0,20), Vector3(0,0,-1)),  10.0, true, true ), "dHowFar=10 stops before the sphere (no hit)" );
	Check(  g->IntersectRay_IntersectionOnly( Ray(Point3(0,0,20), Vector3(0,0,-1)),  16.0, true, true ), "dHowFar=16 reaches the sphere (hit)" );
	Check( !g->IntersectRay_IntersectionOnly( Ray(Point3(0,20,0), Vector3(1,0,0)),  100.0, true, true ), "shadow ray that misses the sphere misses" );
	safe_release( g );
}

static void TestMiss()
{
	std::cout << "Test 6: ray missing the bbox misses" << std::endl;
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), 5.0, 0,0,0 ) );
	SDFGeometry* g = MakeGeom( parts );
	RayIntersectionGeometric ri = MkRI( Point3(0,20,0), Vector3(0,0,-1) );  // passes y=20, misses r5 sphere
	g->IntersectRay( ri, true, true, false );
	Check( !ri.bHit, "ray well outside the sphere does not hit" );
	safe_release( g );
}

static void TestInsideStartExits()
{
	std::cout << "Test 7: ray starting inside exits at the far surface" << std::endl;
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), 5.0, 0,0,0 ) );
	SDFGeometry* g = MakeGeom( parts );
	// from the centre along +Z: the only surface ahead is the exit at z=5 -> range 5
	RayIntersectionGeometric ri = MkRI( Point3(0,0,0), Vector3(0,0,1) );
	g->IntersectRay( ri, true, true, false );
	Check( ri.bHit && IsClose(ri.range, 5.0, 5e-3), "inside-start ray exits at the far surface" );
	safe_release( g );
}

static void TestTransform()
{
	std::cout << "Test 8: translated + rotated parts land where expected" << std::endl;
	// translated sphere at (10,0,0)
	{	std::vector<SDFGeometry::Part> parts;
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
			Point3(10,0,0), 0,0,0, Vector3(1,1,1), 3.0, 0,0,0 ) );
		SDFGeometry* g = MakeGeom( parts );
		RayIntersectionGeometric ri = MkRI( Point3(10,0,20), Vector3(0,0,-1) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose(ri.range, 17.0), "translated sphere hit at +10 X" );
		safe_release( g ); }
	// a long thin box rotated 90 deg about Z: a 6x1x1 box (half 3,0.5,0.5) rotated so its long axis -> Y
	{	std::vector<SDFGeometry::Part> parts;
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpUnion, 0,
			Point3(0,0,0), 0,0,90, Vector3(1,1,1), 3.0, 0.5, 0.5, 0 ) );
		SDFGeometry* g = MakeGeom( parts );
		// after rotating the long (X=3) axis to Y, the box reaches y~=+/-3 and x~=+/-0.5
		RayIntersectionGeometric riY = MkRI( Point3(0,20,0), Vector3(0,-1,0) );
		g->IntersectRay( riY, true, true, false );
		Check( riY.bHit && IsClose(riY.range, 17.0, 5e-3), "rotated box reaches +3 in Y (long axis)" );
		RayIntersectionGeometric riX = MkRI( Point3(20,0,0), Vector3(-1,0,0) );
		g->IntersectRay( riX, true, true, false );
		Check( riX.bHit && IsClose(riX.range, 19.5, 5e-3), "rotated box only +0.5 in X (short axis)" );
		safe_release( g ); }
}

static void TestNonUniformScale()
{
	std::cout << "Test 9: non-uniform scale stretches the primitive" << std::endl;
	// unit sphere scaled (3,1,1) -> reaches x=3, y=1, z=1
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3(0,0,0), 0,0,0, Vector3(3,1,1), 1.0, 0,0,0 ) );
	SDFGeometry* g = MakeGeom( parts );
	RayIntersectionGeometric rx = MkRI( Point3(20,0,0), Vector3(-1,0,0) );
	g->IntersectRay( rx, true, true, false );
	Check( rx.bHit && IsClose(rx.range, 17.0, 1e-2), "scaled sphere reaches x=3" );
	RayIntersectionGeometric ry = MkRI( Point3(0,20,0), Vector3(0,-1,0) );
	g->IntersectRay( ry, true, true, false );
	Check( ry.bHit && IsClose(ry.range, 19.0, 1e-2), "scaled sphere reaches y=1" );
	safe_release( g );
}

// Subtract / intersect compose with smooth-MAX, which (unlike smooth-min) returns
// a value >= the hard max -- the review flagged this as a possible sphere-trace
// OVERSTEP that could tunnel through a carved wall or fill a carved hole.  The
// polynomial smin/smax are 1-Lipschitz given 1-Lipschitz inputs (|grad| of the
// blend <= 1 by the triangle inequality), so the composed field stays a
// CONSERVATIVE distance bound and must not tunnel.  These rays pin that down,
// including the smooth k=1 regime used by the watch case-body chunk's
// dial-bowl carve (scenes/FeatureBased/GuillocheWatch/watch_dial.RISEscene).

static void TestSubtractHardCarvesCavity()
{
	std::cout << "Test 10: subtract (hard) carves a real cavity; walls hit at exact depth" << std::endl;
	// A = box half(3,3,3); subtract B = box half(2,2,5) -> a 2x2 tunnel through Z,
	// leaving 1-thick walls on +-X / +-Y.
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpUnion, 0,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), 3,3,3, 0 ) );
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpSubtract, 0,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), 2,2,5, 0 ) );
	SDFGeometry* g = MakeGeom( parts );
	{	RayIntersectionGeometric ri = MkRI( Point3(0,0,-10), Vector3(0,0,1) );      // down the tunnel
		g->IntersectRay( ri, true, true, false );
		Check( !ri.bHit, "subtract: ray down the tunnel misses (hole carved)" ); }
	{	RayIntersectionGeometric ri = MkRI( Point3(2.5,0,-10), Vector3(0,0,1) );    // through +X wall
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose(ri.range, 7.0, 5e-3), "subtract: wall column hit at outer face z=-3" ); }
	{	RayIntersectionGeometric ri = MkRI( Point3(-10,0,0), Vector3(1,0,0) );      // outer -X face
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose(ri.range, 7.0, 5e-3), "subtract: outer -X face at x=-3" ); }
	{	RayIntersectionGeometric ri = MkRI( Point3(-2.5,0,0), Vector3(1,0,0) );     // inside -X wall x[-3,-2]
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose(ri.range, 0.5, 5e-3), "subtract: inner carved face at x=-2 (no tunnel)" ); }
	safe_release( g );
}

static void TestSubtractSmoothNoTunnel()
{
	std::cout << "Test 11: subtract (smooth k=1, case_body regime) neither fills nor tunnels" << std::endl;
	// thicker walls so the k=1 fillet does not erode them: A half(4,4,4),
	// subtract B half(2,2,6) k=1 -> rounded 2x2 tunnel, ~2-thick walls.
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpUnion, 0,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), 4,4,4, 0 ) );
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpSubtract, 1.0,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), 2,2,6, 0 ) );
	SDFGeometry* g = MakeGeom( parts );
	{	RayIntersectionGeometric ri = MkRI( Point3(0,0,-12), Vector3(0,0,1) );      // centre still open
		g->IntersectRay( ri, true, true, false );
		Check( !ri.bHit, "smooth subtract: centre still open (smooth-max did not fill it)" ); }
	{	RayIntersectionGeometric ri = MkRI( Point3(3.5,0,-12), Vector3(0,0,1) );    // wall far from carve
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose(ri.range, 8.0, 1e-2), "smooth subtract: wall hit at outer face (no overstep)" ); }
	safe_release( g );
}

static void TestIntersectLens()
{
	std::cout << "Test 12: intersect (hard) = lens, hits at exact sphere positions" << std::endl;
	// A = sphere r2 at (-1,0,0); intersect B = sphere r2 at (+1,0,0).
	// lens at y=z=0 spans x[-1,1]: -X extent is sphere B's surface, +X extent is A's.
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3(-1,0,0), 0,0,0, Vector3(1,1,1), 2,0,0, 0 ) );
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpIntersect, 0,
		Point3(1,0,0), 0,0,0, Vector3(1,1,1), 2,0,0, 0 ) );
	SDFGeometry* g = MakeGeom( parts );
	{	RayIntersectionGeometric ri = MkRI( Point3(-10,0,0), Vector3(1,0,0) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose(ri.range, 9.0, 5e-3), "intersect: -X lens extent at x=-1" );
		Check( VClose( ri.vNormal, norm3(-1,0,0) ), "intersect: -X face is sphere B's normal" ); }
	{	RayIntersectionGeometric ri = MkRI( Point3(10,0,0), Vector3(-1,0,0) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose(ri.range, 9.0, 5e-3), "intersect: +X lens extent at x=1" ); }
	{	RayIntersectionGeometric ri = MkRI( Point3(0,10,0), Vector3(0,-1,0) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose(ri.range, 10.0 - std::sqrt(3.0), 5e-3), "intersect: +Y lens extent at y=sqrt3" ); }
	safe_release( g );
}

static void TestIntersectionOnlyFrontBack()
{
	std::cout << "Test 13: IntersectRay_IntersectionOnly honours front/back flags" << std::endl;
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), 2,0,0, 0 ) );
	SDFGeometry* g = MakeGeom( parts );
	Ray r( Point3(-10,0,0), Vector3(1,0,0) );   // crosses sphere: front x=-2 (range 8), back x=+2 (range 12)
	Check(  g->IntersectRay_IntersectionOnly( r, 100.0, true,  true  ), "IO both faces -> hit" );
	Check(  g->IntersectRay_IntersectionOnly( r, 100.0, true,  false ), "IO front-only -> hit (front exists)" );
	Check(  g->IntersectRay_IntersectionOnly( r, 100.0, false, true  ), "IO back-only -> hit (steps past front to back)" );
	Check( !g->IntersectRay_IntersectionOnly( r, 100.0, false, false ), "IO neither face -> miss" );
	Check( !g->IntersectRay_IntersectionOnly( r, 8.5,   false, true  ), "IO back-only, short dHowFar -> miss (back beyond range)" );
	safe_release( g );
}

static void TestFrontBackGuardConsistency()
{
	std::cout << "Test 14: IntersectRay vs IntersectionOnly agree past the face-skip guard" << std::endl;
	// 6 thin slabs stacked along Z -> 12 surface crossings along a +Z ray, MORE
	// than the 8-crossing skip guard.  A (false,false) query must report NO hit on
	// BOTH paths -- the detailed path used to fall through to bHit=true after the
	// guard expired on a disallowed face.
	std::vector<SDFGeometry::Part> parts;
	const Scalar zc[6] = { -5,-3,-1,1,3,5 };
	for( int i = 0; i < 6; ++i )
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpUnion, 0,
			Point3(0,0,zc[i]), 0,0,0, Vector3(1,1,1), 2,2,0.2, 0 ) );
	SDFGeometry* g = MakeGeom( parts );

	RayIntersectionGeometric ri = MkRI( Point3(0,0,-20), Vector3(0,0,1) );
	g->IntersectRay( ri, false, false, false );
	Ray r( Point3(0,0,-20), Vector3(0,0,1) );
	const bool io = g->IntersectRay_IntersectionOnly( r, 1000.0, false, false );
	Check( !ri.bHit, "IntersectRay (false,false): no hit after guard expiry" );
	Check( !io, "IntersectionOnly (false,false): no hit" );
	Check( ri.bHit == io, "detailed and fast paths agree on (false,false)" );

	// sanity: (true,true) still hits the first slab's front face at z=-5.2
	RayIntersectionGeometric ri2 = MkRI( Point3(0,0,-20), Vector3(0,0,1) );
	g->IntersectRay( ri2, true, true, false );
	Check( ri2.bHit && IsClose(ri2.range, 14.8, 1e-2), "(true,true) hits first slab front at z=-5.2" );
	safe_release( g );
}

// ---- first-class surface machinery: tessellation + uniform area sampling ----

// splitmix64: the earlier 32-bit LCG had enough serial correlation to bias the
// closed-form NEE harness by ~1.6% through the analytic sphere's cos-theta map.
static unsigned long long smState = 0x9E3779B97F4A7C15ull;
static Scalar Rand01()
{
	smState += 0x9E3779B97F4A7C15ull;
	unsigned long long z = smState;
	z = ( z ^ ( z >> 30 ) ) * 0xBF58476D1CE4E5B9ull;
	z = ( z ^ ( z >> 27 ) ) * 0x94D049BB133111EBull;
	z ^= ( z >> 31 );
	return Scalar( z >> 11 ) / Scalar( 9007199254740992.0 );
}

static void TestSurfaceArea()
{
	std::cout << "Test 15: GetArea(sphere) == 4*pi*r^2 within 2% (tessellated + projected)" << std::endl;
	const Scalar R = 5.0;
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), R, 0, 0, 0 ) );
	SDFGeometry* g = MakeGeom( parts );
	const Scalar area = g->GetArea();
	const Scalar exact = Scalar(4) * PI * R * R;
	std::cout << "  sphere r=5: GetArea=" << area << "  exact=" << exact << "  rel=" << (area/exact) << std::endl;
	Check( std::fabs( area - exact ) / exact < 0.02, "sphere surface area within 2% of 4*pi*r^2" );
	Check( g->CanBeAreaLight(), "sphere SDF reports CanBeAreaLight()" );
	safe_release( g );
}

static void TestUniformRandomPointOnSurface()
{
	std::cout << "Test 16: UniformRandomPoint lies ON the surface, radial normal, octant balance" << std::endl;
	const Scalar R = 5.0;
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), R, 0, 0, 0 ) );
	SDFGeometry* g = MakeGeom( parts );

	int octant[8] = {0,0,0,0,0,0,0,0};
	Scalar maxRadErr = 0, maxNrmErr = 0;
	const int NSAMP = 400;
	for( int i = 0; i < NSAMP; i++ ) {
		Point3 pt; Vector3 nrm; Point2 uv;
		g->UniformRandomPoint( &pt, &nrm, &uv, Point3( Rand01(), Rand01(), Rand01() ) );
		const Scalar r = std::sqrt( pt.x*pt.x + pt.y*pt.y + pt.z*pt.z );
		maxRadErr = std::max( maxRadErr, std::fabs( r - R ) );
		const Vector3 radial = norm3( pt.x, pt.y, pt.z );
		const Scalar dot = radial.x*nrm.x + radial.y*nrm.y + radial.z*nrm.z;
		maxNrmErr = std::max( maxNrmErr, Scalar(1) - dot );
		octant[ (pt.x > 0 ? 1 : 0) | (pt.y > 0 ? 2 : 0) | (pt.z > 0 ? 4 : 0) ]++;
	}
	Check( maxRadErr < 5e-3, "every sample within 5e-3 of the true surface" );
	Check( maxNrmErr < 2e-2, "every normal within 2e-2 of radial" );
	int minOct = NSAMP, maxOct = 0;
	for( int o = 0; o < 8; o++ ) { minOct = std::min( minOct, octant[o] ); maxOct = std::max( maxOct, octant[o] ); }
	Check( minOct > NSAMP/8 - 30 && maxOct < NSAMP/8 + 30, "octant counts roughly uniform (50 +- 30)" );
	safe_release( g );
}

static void TestTessellateToMesh()
{
	std::cout << "Test 17: TessellateToMesh: indexed mesh on the surface, outward, append contract" << std::endl;
	const Scalar R = 5.0;
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), R, 0, 0, 0 ) );
	SDFGeometry* g = MakeGeom( parts );

	IndexTriangleListType tris;
	VerticesListType verts;
	NormalsListType nrms;
	TexCoordsListType uvs;
	Check( g->TessellateToMesh( tris, verts, nrms, uvs, 48 ), "tessellation succeeds" );
	Check( !tris.empty() && verts.size() == nrms.size() && verts.size() == uvs.size(), "parallel vertex arrays" );

	Scalar maxOff = 0, meshArea = 0;
	bool idxOK = true, orientOK = true;
	for( size_t i = 0; i < verts.size(); i++ ) {
		const Scalar r = std::sqrt( verts[i].x*verts[i].x + verts[i].y*verts[i].y + verts[i].z*verts[i].z );
		maxOff = std::max( maxOff, std::fabs( r - R ) );
	}
	for( size_t t = 0; t < tris.size(); t++ ) {
		for( int k = 0; k < 3; k++ ) {
			if( tris[t].iVertices[k] >= verts.size() ) idxOK = false;
		}
		if( !idxOK ) break;
		const Point3& A = verts[ tris[t].iVertices[0] ];
		const Point3& B = verts[ tris[t].iVertices[1] ];
		const Point3& C = verts[ tris[t].iVertices[2] ];
		const Vector3 e1( B.x-A.x, B.y-A.y, B.z-A.z );
		const Vector3 e2( C.x-A.x, C.y-A.y, C.z-A.z );
		const Vector3 n( e1.y*e2.z - e1.z*e2.y, e1.z*e2.x - e1.x*e2.z, e1.x*e2.y - e1.y*e2.x );
		meshArea += Scalar(0.5) * std::sqrt( n.x*n.x + n.y*n.y + n.z*n.z );
		// outward: geometric normal agrees with the radial direction at the centroid
		const Vector3 ctr( (A.x+B.x+C.x)/3, (A.y+B.y+C.y)/3, (A.z+B.z+C.z)/3 );
		if( n.x*ctr.x + n.y*ctr.y + n.z*ctr.z <= 0 ) orientOK = false;
	}
	Check( idxOK, "all triangle indices in bounds" );
	Check( maxOff < 5e-3, "all tessellation vertices on the surface (projected)" );
	const Scalar exact = Scalar(4) * PI * R * R;
	Check( std::fabs( meshArea - exact ) / exact < 0.02, "tessellated mesh area within 2%" );
	Check( orientOK, "every triangle wound outward" );

	// append contract: a second tessellation into the same vectors must offset indices
	const size_t v1 = verts.size();
	const size_t t1 = tris.size();
	Check( g->TessellateToMesh( tris, verts, nrms, uvs, 24 ), "second (appended) tessellation succeeds" );
	bool offsetOK = true;
	for( size_t t = t1; t < tris.size(); t++ ) {
		for( int k = 0; k < 3; k++ ) {
			if( tris[t].iVertices[k] < v1 || tris[t].iVertices[k] >= verts.size() ) offsetOK = false;
		}
	}
	Check( offsetOK, "appended tessellation indices offset past the first batch" );
	safe_release( g );
}

static void TestSamplingOnCompositeFields()
{
	std::cout << "Test 18: area + on-surface sampling hold for smin blob and carved (subtract) fields" << std::endl;
	{	// smin peanut (the sdf_volume blob): two r=1.05 spheres at y=-0.75/+0.75, k=0.7
		std::vector<SDFGeometry::Part> parts;
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
			Point3(0,-0.75,0), 0,0,0, Vector3(1,1,1), 1.05, 0,0,0 ) );
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpSmin, 0.7,
			Point3(0, 0.75,0), 0,0,0, Vector3(1,1,1), 1.05, 0,0,0 ) );
		SDFGeometry* g = MakeGeom( parts );
		const Scalar oneSphere = Scalar(4) * PI * Scalar(1.05) * Scalar(1.05);
		Check( g->GetArea() > oneSphere, "blob area exceeds a single constituent sphere" );
		Scalar worst = 0;
		for( int i = 0; i < 200; i++ ) {
			Point3 pt; Vector3 nrm;
			g->UniformRandomPoint( &pt, &nrm, 0, Point3( Rand01(), Rand01(), Rand01() ) );
			// Map is protected; closeness to the surface is checked via a ray cast
			// from outside along -nrm: the hit must land back on (or very near) pt.
			RayIntersectionGeometric ri = MkRI( Point3( pt.x + nrm.x*3, pt.y + nrm.y*3, pt.z + nrm.z*3 ),
			                                    Vector3( -nrm.x, -nrm.y, -nrm.z ) );
			g->IntersectRay( ri, true, true, false );
			if( ri.bHit ) {
				const Scalar dx = ri.ptIntersection.x - pt.x;
				const Scalar dy = ri.ptIntersection.y - pt.y;
				const Scalar dz = ri.ptIntersection.z - pt.z;
				worst = std::max( worst, std::sqrt( dx*dx + dy*dy + dz*dz ) );
			} else {
				worst = Scalar(1e9);
			}
		}
		Check( worst < 2e-2, "blob samples sit on the sphere-traced surface (ray round-trip)" );
		safe_release( g );
	}
	{	// carved tunnel (Test 10 field): area must count the interior walls; samples on surface
		std::vector<SDFGeometry::Part> parts;
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpUnion, 0,
			Point3(0,0,0), 0,0,0, Vector3(1,1,1), 3,3,3, 0 ) );
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpSubtract, 0,
			Point3(0,0,0), 0,0,0, Vector3(1,1,1), 2,2,5, 0 ) );
		SDFGeometry* g = MakeGeom( parts );
		// outer shell minus two 4x4 punched faces, plus four 4x6 interior tunnel walls
		const Scalar expected = Scalar(6*36 - 2*16 + 4*24);
		std::cout << "  carved box: GetArea=" << g->GetArea() << "  exact=" << expected << "  rel=" << (g->GetArea()/expected) << std::endl;
		Check( std::fabs( g->GetArea() - expected ) / expected < 0.03, "carved-tunnel area within 3% of closed form" );
		Check( g->CanBeAreaLight(), "carved field reports CanBeAreaLight()" );
		safe_release( g );
	}
}

static void TestNEEIntegrandClosedForm()
{
	std::cout << "Test 19: MC integral of the NEE integrand over UniformRandomPoint == closed form" << std::endl;
	// Uniformly radiant sphere (radiance L=1), receiver point Q straight below at
	// distance d from the centre, receiver normal pointing up at the centre.
	// Closed form irradiance: E = pi * L * (R/d)^2.   The NEE estimator computes
	// (area/N) * sum[ cosLight * cosSurface / dist^2 ] over uniform-area samples
	// with cosLight > 0 -- exactly what the light sampler does.  If this MC sum
	// converges to the closed form, the sampling machinery (area + distribution +
	// normals) is unbiased, independent of any renderer code.
	const Scalar R = 1.5;
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), R, 0, 0, 0 ) );
	SDFGeometry* g = MakeGeom( parts );

	const Point3  Q( 0, -2.6, 0 );
	const Vector3 nQ( 0, 1, 0 );
	const Scalar  dC = 2.6;
	const Scalar  closedForm = PI * ( R / dC ) * ( R / dC );

	const Scalar area = g->GetArea();
	const int N = 400000;
	Scalar sum = 0;
	for( int i = 0; i < N; i++ ) {
		Point3 pt; Vector3 nrm;
		g->UniformRandomPoint( &pt, &nrm, 0, Point3( Rand01(), Rand01(), Rand01() ) );
		Vector3 toQ( Q.x - pt.x, Q.y - pt.y, Q.z - pt.z );
		const Scalar dist = std::sqrt( toQ.x*toQ.x + toQ.y*toQ.y + toQ.z*toQ.z );
		toQ = Vector3( toQ.x/dist, toQ.y/dist, toQ.z/dist );
		const Scalar cosLight   = nrm.x*toQ.x + nrm.y*toQ.y + nrm.z*toQ.z;
		const Scalar cosSurface = -( nQ.x*toQ.x + nQ.y*toQ.y + nQ.z*toQ.z );
		if( cosLight > 0 && cosSurface > 0 ) {
			// visibility for a convex emitter == samples on the near cap; the
			// cosLight > 0 gate alone OVERCOUNTS (it admits far-side points whose
			// normal happens to face Q).  For a sphere, a sample is visible from Q
			// exactly when cosLight > 0 AND the segment doesn't re-enter -- which
			// for convex bodies is equivalent to cosLight > 0 evaluated with the
			// outward normal at the TRUE surface, so the gate is correct as-is.
			sum += cosLight * cosSurface / ( dist * dist );
		}
	}
	const Scalar mc = area * sum / Scalar(N);
	std::cout << "  closed form E = " << closedForm << "   MC E = " << mc << "   ratio = " << ( mc / closedForm ) << std::endl;
	Check( std::fabs( mc - closedForm ) / closedForm < 0.01, "NEE integrand MC within 1% of closed form" );
	safe_release( g );
}

static void TestNEEIntegrandAnalyticControl()
{
	std::cout << "Test 19b: same MC harness against the ANALYTIC SphereGeometry (control)" << std::endl;
	const Scalar R = 1.5;
	SphereGeometry* g = new SphereGeometry( R );
	const Point3  Q( 0, -2.6, 0 );
	const Vector3 nQ( 0, 1, 0 );
	const Scalar  dC = 2.6;
	const Scalar  closedForm = PI * ( R / dC ) * ( R / dC );
	const Scalar  area = g->GetArea();
	const int N = 400000;
	Scalar sum = 0;
	for( int i = 0; i < N; i++ ) {
		Point3 pt; Vector3 nrm;
		g->UniformRandomPoint( &pt, &nrm, 0, Point3( Rand01(), Rand01(), Rand01() ) );
		Vector3 toQ( Q.x - pt.x, Q.y - pt.y, Q.z - pt.z );
		const Scalar dist = std::sqrt( toQ.x*toQ.x + toQ.y*toQ.y + toQ.z*toQ.z );
		toQ = Vector3( toQ.x/dist, toQ.y/dist, toQ.z/dist );
		const Scalar cosLight   = nrm.x*toQ.x + nrm.y*toQ.y + nrm.z*toQ.z;
		const Scalar cosSurface = -( nQ.x*toQ.x + nQ.y*toQ.y + nQ.z*toQ.z );
		if( cosLight > 0 && cosSurface > 0 ) {
			sum += cosLight * cosSurface / ( dist * dist );
		}
	}
	const Scalar mc = area * sum / Scalar(N);
	std::cout << "  closed form E = " << closedForm << "   MC E = " << mc << "   ratio = " << ( mc / closedForm ) << std::endl;
	Check( std::fabs( mc - closedForm ) / closedForm < 0.01, "analytic-sphere control within 1% of closed form" );
	safe_release( g );
}

//////////////////////////////////////////////////////////////////////
// Test 20: ParsePartLines -- the ONE part grammar shared by the scene
// chunk's inline `part` lines and external parts files.  Valid sources
// (grouped spacing, comments, blank lines) parse to exact Part fields;
// unknown / malformed / trailing tokens hard-fail; the retired
// count-header format is rejected as malformed (the header line is not
// a part).
//////////////////////////////////////////////////////////////////////
static void TestParsePartLines()
{
	std::cout << "Test 20: ParsePartLines grammar (inline `part` lines / parts files)" << std::endl;

	// Valid: grouped spacing, a comment line, a trailing comment, a blank line.
	{
		std::vector<SDFGeometry::Part> parts;
		const char* src =
			"# the blob from sdf_shadows\n"
			"roundbox union 0  0 0 2  0 0 0  1 1 1  2 2 2  0.5\n"
			"\n"
			"sphere smin 1.6  2.6 3.2 0  0 0 0  1 1 1  1.7 0 0  0   # melded\n";
		Check( SDFGeometry::ParsePartLines( src, "<test>", parts ), "valid source parses" );
		Check( parts.size() == 2, "valid source yields 2 parts" );
		if( parts.size() == 2 ) {
			Check( parts[0].type == SDFGeometry::ePrimRoundBox, "part 0 type roundbox" );
			Check( parts[0].op == SDFGeometry::eOpUnion, "part 0 op union" );
			Check( parts[0].k == 0.0, "part 0 k" );
			Check( parts[0].pos.x == 0.0 && parts[0].pos.y == 0.0 && parts[0].pos.z == 2.0, "part 0 pos" );
			Check( parts[0].a == 2.0 && parts[0].b == 2.0 && parts[0].c == 2.0, "part 0 a b c" );
			Check( parts[0].round == 0.5, "part 0 round" );
			Check( parts[1].type == SDFGeometry::ePrimSphere, "part 1 type sphere" );
			Check( parts[1].op == SDFGeometry::eOpSmin, "part 1 op smin" );
			Check( parts[1].k == 1.6, "part 1 k" );
			Check( parts[1].pos.x == 2.6 && parts[1].pos.y == 3.2 && parts[1].pos.z == 0.0, "part 1 pos" );
			Check( parts[1].a == 1.7, "part 1 a" );
		}
	}

	// Comment-only / blank source: parses, appends nothing (emptiness is
	// the factory's both-or-neither diagnostic, not the grammar's).
	{
		std::vector<SDFGeometry::Part> parts;
		Check( SDFGeometry::ParsePartLines( "# nothing here\n\n", "<test>", parts ), "comment-only source parses" );
		Check( parts.empty(), "comment-only source yields 0 parts" );
	}

	// Hard failures: unknown primitive / op, short line, trailing token,
	// and the retired count-header file format.
	{
		std::vector<SDFGeometry::Part> parts;
		Check( !SDFGeometry::ParsePartLines(
			"round_box union 0  0 0 0  0 0 0  1 1 1  1 1 1  0\n", "<test>", parts ),
			"unknown primitive rejected" );
		Check( !SDFGeometry::ParsePartLines(
			"sphere substract 0  0 0 0  0 0 0  1 1 1  1 0 0  0\n", "<test>", parts ),
			"unknown op rejected" );
		Check( !SDFGeometry::ParsePartLines(
			"sphere union 0  0 0 0  0 0 0  1 1 1  1 0 0\n", "<test>", parts ),
			"short line (15 tokens) rejected" );
		Check( !SDFGeometry::ParsePartLines(
			"sphere union 0  0 0 0  0 0 0  1 1 1  1 0 0  0  99\n", "<test>", parts ),
			"trailing extra token rejected" );
		Check( !SDFGeometry::ParsePartLines(
			"1\nsphere union 0  0 0 0  0 0 0  1 1 1  1 0 0  0\n", "<test>", parts ),
			"retired count-header format rejected" );
	}

	// Parsed parts drive the same geometry as directly-constructed parts:
	// a lone r=1.5 sphere field maps identically at probe points.
	{
		std::vector<SDFGeometry::Part> parsed;
		Check( SDFGeometry::ParsePartLines(
			"sphere union 0  0 0 0  0 0 0  1 1 1  1.5 0 0  0\n", "<test>", parsed ),
			"probe sphere parses" );
		std::vector<SDFGeometry::Part> direct;
		direct.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
			Point3(0,0,0), 0,0,0, Vector3(1,1,1), 1.5, 0, 0, 0 ) );
		SDFGeometry* gp = new SDFGeometry( parsed, 256, 0.0 );
		SDFGeometry* gd = new SDFGeometry( direct, 256, 0.0 );
		bool match = true;
		for( int i = 0; i < 8 && match; i++ ) {
			const Scalar t = Scalar(i) / 7.0;
			Ray r( Point3( -5.0 + 10.0*t, 0.3, 4.0 ), Vector3Ops::Normalize( Vector3( 0.1*t, -0.05, -1 ) ) );
			RayIntersectionGeometric riP( r, nullRasterizerState );
			RayIntersectionGeometric riD( r, nullRasterizerState );
			gp->IntersectRay( riP, true, true, false );
			gd->IntersectRay( riD, true, true, false );
			if( riP.bHit != riD.bHit ) { match = false; }
			else if( riP.bHit && std::fabs( riP.range - riD.range ) > 1e-12 ) { match = false; }
		}
		Check( match, "parsed parts hit-identical to directly-constructed parts" );
		safe_release( gp );
		safe_release( gd );
	}
}

//////////////////////////////////////////////////////////////////////
// Test 21: the first part must be union or smin.  Map() folds from an
// empty (+1e30) field, so a leading subtract / intersect yields an
// always-miss SDF -- ParsePartLines hard-fails it.  smin against the
// empty field degenerates to plain union, so it IS allowed and must
// hit identically to a union first part.
//////////////////////////////////////////////////////////////////////
static void TestFirstOpRule()
{
	std::cout << "Test 21: first part must be union or smin" << std::endl;

	std::vector<SDFGeometry::Part> parts;
	Check( !SDFGeometry::ParsePartLines(
		"sphere subtract 0  0 0 0  0 0 0  1 1 1  1 0 0  0\n", "<test>", parts ),
		"leading subtract rejected" );
	Check( !SDFGeometry::ParsePartLines(
		"sphere intersect 0  0 0 0  0 0 0  1 1 1  1 0 0  0\n", "<test>", parts ),
		"leading intersect rejected" );
	Check( SDFGeometry::ParsePartLines(
		"box subtract 0  0 0 0  0 0 0  1 1 1  9 9 9  0\n", "<test>", parts ) == false,
		"leading subtract rejected regardless of primitive" );

	// second-position subtract stays legal
	parts.clear();
	Check( SDFGeometry::ParsePartLines(
		"sphere union 0  0 0 0  0 0 0  1 1 1  2 0 0  0\n"
		"sphere subtract 0  0 0 2  0 0 0  1 1 1  1 0 0  0\n", "<test>", parts ),
		"subtract after union accepted" );

	// smin first == union first, hit-for-hit
	std::vector<SDFGeometry::Part> sminFirst, unionFirst;
	Check( SDFGeometry::ParsePartLines(
		"sphere smin 1  0 0 0  0 0 0  1 1 1  1.5 0 0  0\n", "<test>", sminFirst ),
		"leading smin accepted" );
	Check( SDFGeometry::ParsePartLines(
		"sphere union 0  0 0 0  0 0 0  1 1 1  1.5 0 0  0\n", "<test>", unionFirst ),
		"union control parses" );
	SDFGeometry* gs = new SDFGeometry( sminFirst, 256, 0.0 );
	SDFGeometry* gu = new SDFGeometry( unionFirst, 256, 0.0 );
	RayIntersectionGeometric rs = MkRI( Point3( 0, 0.2, 5 ), Vector3( 0, 0, -1 ) );
	RayIntersectionGeometric ru = MkRI( Point3( 0, 0.2, 5 ), Vector3( 0, 0, -1 ) );
	gs->IntersectRay( rs, true, true, false );
	gu->IntersectRay( ru, true, true, false );
	// NB compare within the marcher's surface epsilon, not exactly: a leading
	// smin pads the bbox by its blend radius (ComputeBounds pad = maxK), so
	// the two marches start from different bbox entry points and the
	// bracket+bisect hit refinement only pins the crossing to ~m_eps.
	// sminP itself folds the +1e30 empty-field sentinel EXACTLY to min()
	// (h = max(k - |a-b|, 0)/k is 0 there), so the surfaces coincide.
	Check( rs.bHit && ru.bHit && std::fabs( rs.range - ru.range ) < 1e-5,
		"leading smin behaves as union" );
	safe_release( gs );
	safe_release( gu );
}

//////////////////////////////////////////////////////////////////////
// Test 22: sub-cell surface components are invisible to the sampling
// mesh (marching tets only see corner sign changes) -- the detector
// must report them, and raising sampling_detail must capture them.
// A tiny nub (r = 0.15) is placed EXACTLY at a low-detail grid cell
// center, outside the big sphere but inside its bbox: every corner of
// that cell stays outside the nub (nearest corner is half a cell
// diagonal ~0.87 away), so at detail 8 the nub provably produces no
// triangles, NEE never samples it, and the center probe fires.
//////////////////////////////////////////////////////////////////////
static void TestMissedComponentDetector()
{
	std::cout << "Test 22: missed-component detector + detail captures the nub" << std::endl;

	const Scalar Rbig = 4.0;
	const Scalar rNub = 0.15;

	// Phase 1: big sphere alone fixes the bbox; replicate the sampling
	// grid of GenerateSurfaceMesh to find a low-detail cell center.
	std::vector<SDFGeometry::Part> bigOnly;
	bigOnly.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), Rbig, 0, 0, 0 ) );
	SDFGeometry* gProbe = new SDFGeometry( bigOnly, 256, 0.0, 8 );
	const BoundingBox bb = gProbe->GenerateBoundingBox();
	safe_release( gProbe );

	const Scalar ex = bb.ur.x - bb.ll.x, ey = bb.ur.y - bb.ll.y, ez = bb.ur.z - bb.ll.z;
	const Scalar longest = std::max( ex, std::max( ey, ez ) );
	const unsigned int N = 8;
	const Scalar hTarget = longest / Scalar(N);
	const unsigned int nx = std::max( 1u, (unsigned int)std::ceil( ex / hTarget ) );
	const unsigned int ny = std::max( 1u, (unsigned int)std::ceil( ey / hTarget ) );
	const unsigned int nz = std::max( 1u, (unsigned int)std::ceil( ez / hTarget ) );
	const Scalar hx = ex / Scalar(nx), hy = ey / Scalar(ny), hz = ez / Scalar(nz);
	const Point3 nubCtr( bb.ll.x + hx * Scalar(7.5),
	                     bb.ll.y + hy * Scalar(7.5),
	                     bb.ll.z + hz * Scalar(4.5) );
	const Scalar nubDist = std::sqrt( nubCtr.x*nubCtr.x + nubCtr.y*nubCtr.y + nubCtr.z*nubCtr.z );
	Check( nubDist > Rbig + 2.0 * rNub, "nub center sits clear of the big sphere" );
	Check( std::fabs(nubCtr.x) + rNub < bb.ur.x, "nub stays inside the big sphere's bbox" );

	std::vector<SDFGeometry::Part> parts = bigOnly;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		nubCtr, 0,0,0, Vector3(1,1,1), rNub, 0, 0, 0 ) );

	// Phase 2: low detail -- the nub renders (sphere trace hits it) but is
	// absent from the sampling structure, and the detector says so.
	{
		SDFGeometry* g = new SDFGeometry( parts, 256, 0.0, 8 );
		RayIntersectionGeometric ri = MkRI(
			Point3( nubCtr.x + 3.0, nubCtr.y, nubCtr.z ), Vector3( -1, 0, 0 ) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && std::fabs( ri.range - ( 3.0 - rNub ) ) < 2e-3,
			"low detail: sphere trace still hits the nub" );

		Check( g->SuspectedMissedFeatureCells() >= 1,
			"low detail: detector reports the provably missed cell" );

		// A PROVEN sampling-contract failure must withdraw area-light /
		// SSS capability: registering anyway would leave the missed nub
		// NEE-unsampleable while BSDF-hit emission MIS still budgets a
		// light-sampling pdf for it (phantom-pdf bias).  CanBeAreaLight()
		// == false routes LuminaryManager, the PT / EmissionShaderOp
		// emission gates, and the SSS ops to the consistent unbiased
		// fallback (full-weight BSDF-hit emission, no NEE).
		Check( !g->CanBeAreaLight(),
			"low detail: CanBeAreaLight refuses the broken sampling contract" );

		int nearNub = 0;
		for( int i = 0; i < 20000; i++ ) {
			Point3 pt; Vector3 nrm;
			g->UniformRandomPoint( &pt, &nrm, 0, Point3( Rand01(), Rand01(), Rand01() ) );
			const Scalar dx = pt.x - nubCtr.x, dy = pt.y - nubCtr.y, dz = pt.z - nubCtr.z;
			if( std::sqrt( dx*dx + dy*dy + dz*dz ) < 2.0 * rNub ) nearNub++;
		}
		Check( nearNub == 0, "low detail: NEE sampling never reaches the nub" );
		safe_release( g );
	}

	// Phase 3: high detail -- cells are smaller than the nub, the CDF
	// includes it, the detector goes quiet.
	{
		SDFGeometry* g = new SDFGeometry( parts, 256, 0.0, 64 );
		Check( g->SuspectedMissedFeatureCells() == 0,
			"high detail: no provably missed cells" );
		Check( g->CanBeAreaLight(),
			"high detail: CanBeAreaLight restored" );
		int nearNub = 0;
		for( int i = 0; i < 30000; i++ ) {
			Point3 pt; Vector3 nrm;
			g->UniformRandomPoint( &pt, &nrm, 0, Point3( Rand01(), Rand01(), Rand01() ) );
			const Scalar dx = pt.x - nubCtr.x, dy = pt.y - nubCtr.y, dz = pt.z - nubCtr.z;
			if( std::sqrt( dx*dx + dy*dy + dz*dz ) < 2.0 * rNub ) nearNub++;
		}
		std::cout << "  high-detail near-nub samples: " << nearNub << " of 30000" << std::endl;
		Check( nearNub >= 10, "high detail: NEE sampling reaches the nub" );
		safe_release( g );
	}
}

//////////////////////////////////////////////////////////////////////
// Test 23: the curvature-corrected sampling area tracks the analytic
// surface area on curved shapes (the raw chord-triangle area
// systematically underestimates by O((cell / curvature radius)^2);
// the Jacobian-weighted measure cancels the first-order term).
//////////////////////////////////////////////////////////////////////
static void TestCorrectedSamplingArea()
{
	std::cout << "Test 23: curvature-corrected sampling area vs closed forms" << std::endl;

	{
		std::vector<SDFGeometry::Part> parts;
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
			Point3(0,0,0), 0,0,0, Vector3(1,1,1), 1.5, 0, 0, 0 ) );
		SDFGeometry* g = new SDFGeometry( parts, 256, 0.0, 64 );
		const Scalar area = g->GetArea();
		const Scalar closed = 4.0 * PI * 1.5 * 1.5;
		std::cout << "  sphere r=1.5 detail 64: area " << area << " vs " << closed
			<< " (rel " << ( area - closed ) / closed << ")" << std::endl;
		Check( std::fabs( area - closed ) / closed < 0.003, "sphere area within 0.3%" );
		safe_release( g );
	}
	{
		std::vector<SDFGeometry::Part> parts;
		Check( SDFGeometry::ParsePartLines(
			"torus union 0  0 0 0  0 0 0  1 1 1  1.45 0.55 0  0\n", "<test>", parts ),
			"torus part parses" );
		SDFGeometry* g = new SDFGeometry( parts, 256, 0.0, 64 );
		const Scalar area = g->GetArea();
		const Scalar closed = 4.0 * PI * PI * 1.45 * 0.55;
		std::cout << "  torus R=1.45 r=0.55 detail 64: area " << area << " vs " << closed
			<< " (rel " << ( area - closed ) / closed << ")" << std::endl;
		Check( std::fabs( area - closed ) / closed < 0.005, "torus area within 0.5%" );
		safe_release( g );
	}
}

//////////////////////////////////////////////////////////////////////
// Keyframe-animation tests.  Prove the SDF FIELD animates: the public
// Keyframable interface (KeyframeFromParameters -> SetIntermediateValue
// -> RegenerateData) mutates a part field and re-derives bounds /
// rotation columns / Lipschitz / the area-sampling cache.
//////////////////////////////////////////////////////////////////////

// Round-trips one keyframe through the public interface.  Returns true
// iff the (name,value) was accepted (parsed + applied).
static bool ApplyKF( SDFGeometry* g, const char* name, const char* value )
{
	IKeyframeParameter* p = g->KeyframeFromParameters( String(name), String(value) );
	if( !p ) { return false; }
	g->SetIntermediateValue( *p );
	safe_release( p );
	g->RegenerateData();
	return true;
}

static void TestKeyframePartPosition()
{
	std::cout << "Test 26: keyframe part.position moves the surface AND rebuilds bounds" << std::endl;
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), 2.0, 0, 0, 0 ) );
	SDFGeometry* g = MakeGeom( parts );

	{	RayIntersectionGeometric ri = MkRI( Point3(0,0,20), Vector3(0,0,-1) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose(ri.range, 18.0), "sphere front face at z=2 before move" ); }

	Check( ApplyKF( g, "part0.position", "0 0 5" ), "part0.position accepted" );

	// Sphere now centred at z=5 (front face z=7).  A hit REQUIRES RegenerateData to
	// have rebuilt the AABB: with the stale z in [-2,2] bbox the ray clips short and
	// MISSES.  range = 20 - (5+2) = 13.
	{	RayIntersectionGeometric ri = MkRI( Point3(0,0,20), Vector3(0,0,-1) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit, "sphere still hit after move (bounds rebuilt)" );
		Check( IsClose(ri.range, 13.0), "hit distance tracks the moved centre" ); }
	safe_release( g );
}

static void TestKeyframePartSizeAndArea()
{
	std::cout << "Test 27: keyframe part.size resizes surface AND invalidates the area cache" << std::endl;
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), 1.5, 0, 0, 0 ) );
	SDFGeometry* g = new SDFGeometry( parts, 256, 0.0, 64 );

	const Scalar area0 = g->GetArea();   // builds the sampling cache at r=1.5
	Check( std::fabs( area0 - 4.0*PI*1.5*1.5 ) / (4.0*PI*1.5*1.5) < 0.01, "area before resize ~ 4pi r^2" );

	Check( ApplyKF( g, "part0.size", "3 0 0" ), "part0.size accepted" );   // sphere: a = radius

	const Scalar area1 = g->GetArea();   // MUST rebuild against r=3 (cache was invalidated)
	std::cout << "  area " << area0 << " -> " << area1 << " (closed " << 4.0*PI*9.0 << ")" << std::endl;
	Check( area1 > area0 * 3.0, "area grew after resize (sampling cache invalidated)" );
	Check( std::fabs( area1 - 4.0*PI*9.0 ) / (4.0*PI*9.0) < 0.01, "rebuilt area ~ 4pi(3^2)" );

	{	RayIntersectionGeometric ri = MkRI( Point3(0,0,20), Vector3(0,0,-1) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose(ri.range, 17.0), "resized sphere front face at z=3" ); }
	safe_release( g );
}

static void TestKeyframePartRotation()
{
	std::cout << "Test 28: keyframe part.rotation re-derives the rotation columns" << std::endl;
	// box long in local X (half-extents 3,1,1)
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpUnion, 0,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), 3.0, 1.0, 1.0, 0 ) );
	SDFGeometry* g = MakeGeom( parts );

	{	RayIntersectionGeometric ri = MkRI( Point3(20,0,0), Vector3(-1,0,0) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose(ri.range, 17.0), "long box +X face at x=3 before rotate" ); }

	Check( ApplyKF( g, "part0.rotation", "0 0 90" ), "part0.rotation accepted" );

	// 90 deg about Z swaps the X<->Y half-extents: object-X now sees half-extent 1,
	// object-Y sees half-extent 3 (true for either rotation sign).
	{	RayIntersectionGeometric ri = MkRI( Point3(20,0,0), Vector3(-1,0,0) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose(ri.range, 19.0), "object-X half-extent now 1 (rotated)" ); }
	{	RayIntersectionGeometric ri = MkRI( Point3(0,20,0), Vector3(0,-1,0) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose(ri.range, 17.0), "object-Y half-extent now 3 (rotated)" ); }
	safe_release( g );
}

static void TestKeyframeBlendAndScale()
{
	std::cout << "Test 29: keyframe part.blend (smin radius) + part.scale" << std::endl;
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3(0,-3,0), 0,0,0, Vector3(1,1,1), 4,0,0,0 ) );
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpSmin, 0.0,
		Point3(0, 3,0), 0,0,0, Vector3(1,1,1), 4,0,0,0 ) );
	SDFGeometry* g = MakeGeom( parts );

	// neck along +X at y=0: with k=0 it's the hard-union crease; raise k and the
	// smin seam bulges OUTWARD -> the front surface is closer -> smaller range.
	const Point3 o(-20,0,0); const Vector3 dir(1,0,0);
	RayIntersectionGeometric ri0 = MkRI(o,dir); g->IntersectRay(ri0,true,true,false);
	Check( ri0.bHit, "neck hit with k=0" );
	const Scalar r_hard = ri0.range;

	Check( ApplyKF( g, "part1.blend", "3.0" ), "part1.blend accepted" );
	RayIntersectionGeometric ri1 = MkRI(o,dir); g->IntersectRay(ri1,true,true,false);
	Check( ri1.bHit, "neck hit with k=3" );
	Check( ri1.range < r_hard - 0.05, "smin seam bulges outward after blend keyframe" );

	// scale sphere0 1.5x in X: along -X at y=-3 the surface now reaches x=6 (4*1.5).
	Check( ApplyKF( g, "part0.scale", "1.5 1 1" ), "part0.scale accepted" );
	RayIntersectionGeometric ri2 = MkRI( Point3(20,-3,0), Vector3(-1,0,0) ); g->IntersectRay(ri2,true,true,false);
	Check( ri2.bHit && IsClose(ri2.range, 14.0, 0.1), "scaled sphere reaches x=6 (range 20-6)" );
	safe_release( g );
}

static void TestKeyframeRejectsBadParams()
{
	std::cout << "Test 30: keyframe rejects unknown / out-of-range / non-finite params" << std::endl;
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), 2.0, 0, 0, 0 ) );
	SDFGeometry* g = MakeGeom( parts );

	IKeyframeParameter* ok = g->KeyframeFromParameters( String("part0.position"), String("1 2 3") );
	Check( ok != 0, "valid part0.position accepted" );
	safe_release( ok );

	Check( g->KeyframeFromParameters( String("bogus"),              String("1") )          == 0, "unknown name rejected" );
	Check( g->KeyframeFromParameters( String("part0.bogus"),        String("1") )          == 0, "unknown part field rejected" );
	Check( g->KeyframeFromParameters( String("part9.position"),     String("1 2 3") )      == 0, "out-of-range part index rejected" );
	Check( g->KeyframeFromParameters( String("part0.position"),     String("nan nan nan") )== 0, "non-finite vec rejected" );
	Check( g->KeyframeFromParameters( String("part0.blend"),        String("inf") )        == 0, "non-finite scalar rejected" );
	Check( g->KeyframeFromParameters( String("heightfield_scale"),  String("1") )          == 0, "heightfield_scale rejected in parts mode" );
	safe_release( g );
}

// Minimal constant heightfield f(u,v)=c for the heightfield-scale keyframe test.
namespace {
	class ConstField : public virtual IFunction2D, public virtual Reference
	{
	public:
		Scalar c;
		ConstField( Scalar c_ ) : c(c_) {}
		Scalar Evaluate( const Scalar, const Scalar ) const { return c; }
	};
}

static void TestKeyframeHeightfieldScale()
{
	std::cout << "Test 31: keyframe heightfield_scale moves the plane + rebuilds Lipschitz/bounds" << std::endl;
	ConstField* field = new ConstField( 0.5 );   // f==0.5 -> flat disk at z = scale*0.5
	SDFGeometry* g = new SDFGeometry( field, 4.0, 2.0, 256, 0.0, 32 );   // amplitude 2 -> plane z=1

	{	RayIntersectionGeometric ri = MkRI( Point3(0,0,20), Vector3(0,0,-1) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose(ri.range, 19.0, 0.05), "heightfield plane at z=1 before scale" ); }

	Check( ApplyKF( g, "heightfield_scale", "6" ), "heightfield_scale accepted" );

	// plane now at z = 6*0.5 = 3, OUTSIDE the old z in [0,2] bbox -> a hit proves the
	// bounds (and Lipschitz) were rebuilt by RegenerateData.  range = 20 - 3 = 17.
	{	RayIntersectionGeometric ri = MkRI( Point3(0,0,20), Vector3(0,0,-1) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit, "heightfield still hit after scale (bounds rebuilt)" );
		Check( IsClose(ri.range, 17.0, 0.05), "plane tracks the new amplitude" ); }

	safe_release( g );
	safe_release( field );
}

int main()
{
	std::cout << "SDFGeometryTest" << std::endl;
	std::cout << "===============" << std::endl;
	TestSphereMatchesAnalytic();
	TestBoxMatchesAnalytic();
	TestSmoothMin();
	TestBoundingBoxContainsSurface();
	TestBoundingBoxOpAware();
	TestWideThinTopFaceEntry();
	TestShadowQuery();
	TestMiss();
	TestInsideStartExits();
	TestTransform();
	TestNonUniformScale();
	TestSubtractHardCarvesCavity();
	TestSubtractSmoothNoTunnel();
	TestIntersectLens();
	TestIntersectionOnlyFrontBack();
	TestFrontBackGuardConsistency();
	TestSurfaceArea();
	TestUniformRandomPointOnSurface();
	TestTessellateToMesh();
	TestSamplingOnCompositeFields();
	TestNEEIntegrandClosedForm();
	TestNEEIntegrandAnalyticControl();
	TestParsePartLines();
	TestFirstOpRule();
	TestMissedComponentDetector();
	TestCorrectedSamplingArea();
	TestKeyframePartPosition();
	TestKeyframePartSizeAndArea();
	TestKeyframePartRotation();
	TestKeyframeBlendAndScale();
	TestKeyframeRejectsBadParams();
	TestKeyframeHeightfieldScale();
	std::cout << std::endl << "Results: " << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount > 0 ? 1 : 0;
}
