//////////////////////////////////////////////////////////////////////
//
//  CappedCylinderTest.cpp
//
//    Standalone regression test for CylinderGeometry's closed-solid
//    (capped) mode, added when cylinder_geometry gained end caps.
//
//    Why this test exists:
//      cylinder_geometry used to be an UNCAPPED tube: IntersectRay only
//      tested the radial side wall, never the end-cap disks.  A camera
//      ray travelling along the cylinder axis passed straight through
//      the open ends without crossing any surface, so an interior_medium
//      bound to a cylinder object was SILENTLY never entered (zero
//      absorption, no warning).  The fix makes cylinder_geometry a
//      closed solid by default (capped=true) while keeping the legacy
//      open tube available (capped=false).
//
//    What it verifies:
//      1. Foot-gun regression: an axial ray MISSES the open tube but
//         HITS the capped solid (entering and exiting through the caps).
//      2. Cap normals point outward (±axis) for x/y/z axes.
//      3. Entry/exit ranges for an axial ray match the cap planes.
//      4. The side wall still intersects correctly in capped mode.
//      5. IntersectRay_IntersectionOnly: an axial shadow ray is occluded
//         by the capped solid, not by the open tube.
//      6. A ray originating inside the solid exits through a cap.
//      7. GetArea() = side + two caps when capped; side only when open.
//      8. UniformRandomPoint stays on the surface, emits unit outward
//         normals, and splits side/cap in proportion to area.
//      9. TessellateToMesh closes the solid: cap triangles exist with
//         outward-facing winding; the open tube emits none.
//     10. ComputeSurfaceDerivatives on a cap point is valid, tangent to
//         the cap plane, and right-handed.
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../src/Library/Geometry/CylinderGeometry.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Utilities/Math3D/Constants.h"
#include "../src/Library/Utilities/Reference.h"

using namespace RISE;
using namespace RISE::Implementation;

static unsigned int g_failures = 0;

#define REQUIRE( cond, label ) do { \
	if( !(cond) ) { \
		std::cout << "  FAIL [" << label << "] at " << __FILE__ << ":" << __LINE__ << std::endl; \
		g_failures++; \
	} \
} while(0)

static bool IsClose( Scalar a, Scalar b, Scalar eps = 1e-6 )
{
	return std::fabs( a - b ) < eps;
}

// Axial / radial-squared decomposition matching CylinderGeometry's own
// per-axis convention.
static Scalar AxialOf( const Point3& p, char axis )
{
	switch( axis ) { case 'x': return p.x; case 'y': return p.y; default: return p.z; }
}
static Scalar Radial2Of( const Point3& p, char axis )
{
	switch( axis ) {
		case 'x': return p.y*p.y + p.z*p.z;
		case 'y': return p.x*p.x + p.z*p.z;
		default:  return p.x*p.x + p.y*p.y;
	}
}

// Inside predicate for a capped cylinder centred at the origin.
static bool InsideCapped( const Point3& p, char axis, Scalar r, Scalar h, Scalar tol )
{
	const Scalar rad2 = Radial2Of( p, axis );
	const Scalar ax   = AxialOf( p, axis );
	return ( rad2 <= (r+tol)*(r+tol) ) && ( std::fabs(ax) <= h*0.5 + tol );
}

// A hit query returning the full geometric record.
struct Hit {
	bool    bHit;
	Scalar  range, range2;
	Point3  pos, exit;
	Vector3 normal, normal2;
	Point2  uv;
};
static Hit Cast( const IGeometry& g, const Point3& o, const Vector3& d, bool exitInfo = true )
{
	RayIntersectionGeometric ri( Ray( o, d ), nullRasterizerState );
	g.IntersectRay( ri, true, true, exitInfo );
	Hit h;
	h.bHit = ri.bHit; h.range = ri.range; h.range2 = ri.range2;
	h.pos = ri.ptIntersection; h.exit = ri.ptExit;
	h.normal = ri.vNormal; h.normal2 = ri.vNormal2;
	h.uv = ri.ptCoord;
	return h;
}

// Is a normal (nearly) parallel to the given axis?
static bool IsAxial( const Vector3& n, char axis )
{
	Vector3 u = Vector3Ops::Normalize( n );
	const Scalar c = ( axis == 'x' ) ? u.x : ( axis == 'y' ) ? u.y : u.z;
	return std::fabs( std::fabs(c) - 1.0 ) < 1e-6;
}

// ============================================================
// 1 + 2 + 3: axial ray through caps (the foot-gun) per axis
// ============================================================
static void TestAxialThroughCaps( char axis )
{
	const Scalar r = 1.0, h = 2.0;

	// Axis unit vector and a start point 5 units "below" the min cap looking
	// toward +axis, straight along the axis (radial offset 0).
	Vector3 dir;
	Point3  start;
	switch( axis ) {
		case 'x': dir = Vector3(1,0,0); start = Point3(-5,0,0); break;
		case 'y': dir = Vector3(0,1,0); start = Point3(0,-5,0); break;
		default:  dir = Vector3(0,0,1); start = Point3(0,0,-5); break;
	}

	CylinderGeometry* solid = new CylinderGeometry( axis, r, h, true );
	CylinderGeometry* tube  = new CylinderGeometry( axis, r, h, false );

	const std::string tag = std::string("axial-") + axis;

	// Open tube: axial ray crosses no surface -> the silent foot-gun.
	Hit ht = Cast( *tube, start, dir );
	REQUIRE( !ht.bHit, (tag + " open tube MISSES (documents the bug)").c_str() );

	// Capped solid: enters at min cap (axial = -h/2), exits at max cap (+h/2).
	Hit hs = Cast( *solid, start, dir );
	REQUIRE( hs.bHit, (tag + " capped solid HITS").c_str() );
	// Entry at the min cap: distance from -5 to -h/2 = 5 - h/2 = 4.
	REQUIRE( IsClose( hs.range, 5.0 - h*0.5, 1e-6 ), (tag + " entry range = to min cap").c_str() );
	REQUIRE( IsClose( hs.range2, 5.0 + h*0.5, 1e-6 ), (tag + " exit range = to max cap").c_str() );
	// Entry point on the min cap plane, exit on the max cap plane.
	REQUIRE( IsClose( AxialOf(hs.pos, axis), -h*0.5, 1e-6 ), (tag + " entry on min cap plane").c_str() );
	REQUIRE( IsClose( AxialOf(hs.exit, axis),  h*0.5, 1e-6 ), (tag + " exit on max cap plane").c_str() );

	// Cap normals point OUTWARD: entry normal = -axis, exit normal = +axis.
	Vector3 nEntry = Vector3Ops::Normalize( hs.normal );
	Vector3 nExit  = Vector3Ops::Normalize( hs.normal2 );
	REQUIRE( IsClose( Vector3Ops::Dot( nEntry, dir ), -1.0, 1e-6 ), (tag + " entry cap normal outward (-axis)").c_str() );
	REQUIRE( IsClose( Vector3Ops::Dot( nExit,  dir ),  1.0, 1e-6 ), (tag + " exit cap normal outward (+axis)").c_str() );

	// 5: occlusion — the capped solid blocks a shadow ray along the axis; the
	// open tube does not.
	REQUIRE(  solid->IntersectRay_IntersectionOnly( Ray(start,dir), 100.0, true, true ), (tag + " capped occludes shadow ray").c_str() );
	REQUIRE( !tube ->IntersectRay_IntersectionOnly( Ray(start,dir), 100.0, true, true ), (tag + " open tube does not occlude").c_str() );

	solid->release();
	tube->release();
}

// ============================================================
// 3b: cap texture coordinates (planar disk UV) and the documented
//     agreement between IntersectRay's ptCoord and the UV that
//     ComputeSurfaceDerivatives reports for the same cap point.
// ============================================================
static void TestCapUV()
{
	const Scalar r = 1.0, h = 2.0;
	CylinderGeometry* g = new CylinderGeometry( 'z', r, h, true );

	// Axial ray offset in the radial plane hits the +Z (max) cap off-centre.
	const Point3 start( 0.3, -0.4, 5.0 );
	Hit hit = Cast( *g, start, Vector3(0,0,-1) );
	REQUIRE( hit.bHit, "cap-UV ray hits" );
	REQUIRE( IsClose( AxialOf(hit.pos,'z'), h*0.5, 1e-6 ), "cap-UV hit on max cap" );

	// Planar disk UV: u = (ra/r+1)/2, v = (rb/r+1)/2 with (ra,rb)=(x,y) for z-axis.
	const Scalar expU = 0.5 * ( hit.pos.x / r + 1.0 );
	const Scalar expV = 0.5 * ( hit.pos.y / r + 1.0 );
	REQUIRE( IsClose( hit.uv.x, expU, 1e-6 ), "cap-UV u = (x/r+1)/2" );
	REQUIRE( IsClose( hit.uv.y, expV, 1e-6 ), "cap-UV v = (y/r+1)/2" );
	// Concrete value sanity: (0.3,-0.4) -> (0.65, 0.30).
	REQUIRE( IsClose( hit.uv.x, 0.65, 1e-6 ), "cap-UV u concrete" );
	REQUIRE( IsClose( hit.uv.y, 0.30, 1e-6 ), "cap-UV v concrete" );

	// The cap UV from ComputeSurfaceDerivatives must agree with IntersectRay.
	SurfaceDerivatives sd = g->ComputeSurfaceDerivatives( hit.pos, Vector3(0,0,1) );
	REQUIRE( sd.valid, "cap-UV derivatives valid" );
	REQUIRE( IsClose( sd.uv.x, hit.uv.x, 1e-6 ), "cap-UV: derivatives u agrees with IntersectRay" );
	REQUIRE( IsClose( sd.uv.y, hit.uv.y, 1e-6 ), "cap-UV: derivatives v agrees with IntersectRay" );

	g->release();
}

// ============================================================
// 3c: mixed-surface intersection selection (cap-entry -> side-exit and
//     side-entry -> cap-exit) + rim membership boundary (radial r±eps).
// ============================================================
static void TestObliqueAndRim()
{
	const Scalar r = 1.0, h = 2.0;
	CylinderGeometry* g = new CylinderGeometry( 'z', r, h, true );

	// (a) Cap-entry -> side-exit: enter the +Z cap at the centre (0,0,1) and drift
	// out so the exit is on the side wall.  dir=(0.9,0,-1) from (-1.8,0,3): first
	// crossing is the top cap at (0,0,1); radial then reaches r at x=1,z=-0.11
	// (|z|<r), so the exit is the side wall, not the bottom cap.
	{
		Hit hit = Cast( *g, Point3(-1.8, 0.0, 3.0), Vector3Ops::Normalize( Vector3(0.9, 0.0, -1.0) ) );
		REQUIRE( hit.bHit, "oblique cap->side hits" );
		REQUIRE(  IsAxial( hit.normal,  'z' ), "oblique entry is a cap (axial normal)" );
		REQUIRE( !IsAxial( hit.normal2, 'z' ), "oblique exit is the side wall (radial normal)" );
		REQUIRE( IsClose( Vector3Ops::Normalize(hit.normal2).z, 0.0, 1e-6 ), "oblique side-exit normal has no axial component" );
	}

	// (b) Side-entry -> cap-exit: enter the side wall low (at (1,0,0.1)) and rise
	// so the exit is on the +Z cap (at (0,0,1)).  dir=(-1,0,0.9): from (2,0,-0.8)
	// the first crossing is the side at x=1,z=0.1; z then reaches 1 at x=0
	// (radial 0 < r), so the exit is the top cap, not the far side wall.
	{
		Hit hit = Cast( *g, Point3(2.0, 0.0, -0.8), Vector3Ops::Normalize( Vector3(-1.0, 0.0, 0.9) ) );
		REQUIRE( hit.bHit, "oblique side->cap hits" );
		REQUIRE( !IsAxial( hit.normal,  'z' ), "oblique entry is the side wall (radial normal)" );
		REQUIRE(  IsAxial( hit.normal2, 'z' ), "oblique exit is a cap (axial normal)" );
	}

	// (c) Rim membership: an axial ray just inside the radius hits a cap; just
	// outside misses everything (parallel to the axis, beyond the disk).
	{
		Hit in  = Cast( *g, Point3( r - 1e-3, 0.0, 5.0 ), Vector3(0,0,-1) );
		Hit out = Cast( *g, Point3( r + 1e-3, 0.0, 5.0 ), Vector3(0,0,-1) );
		REQUIRE(  in.bHit,  "axial ray at radial r-eps hits the cap" );
		REQUIRE( !out.bHit, "axial ray at radial r+eps misses (outside the disk)" );
	}

	g->release();
}

// ============================================================
// 3d: ray originating INSIDE the solid uses the started-inside convention
//     (single exit crossing; range2 left at 0), even when the exit lands on
//     the rim where side wall and cap coincide (regression for the phantom
//     entry+exit segment the count-distinct discriminator produced).
// ============================================================
static void TestInsideRimExit()
{
	const Scalar r = 1.0, h = 2.0;
	CylinderGeometry* g = new CylinderGeometry( 'z', r, h, true );

	// The reviewer's specific rim-exit case plus a couple of nearby interior
	// origins; all must report bHit with range2 == 0 (no phantom segment).
	struct Case { Point3 o; Vector3 d; const char* tag; };
	const Case cases[] = {
		{ Point3(0.0, 0.0, 0.9), Vector3Ops::Normalize(Vector3(1.0, 0.0, 0.1)), "inside rim-exit" },
		{ Point3(0.1, 0.2, 0.0), Vector3Ops::Normalize(Vector3(0.7, 0.3, 0.4)), "inside oblique" },
		{ Point3(0.0, 0.0, 0.0), Vector3Ops::Normalize(Vector3(0.0, 0.0, 1.0)), "inside axial" },
	};
	for( const Case& c : cases ) {
		Hit hit = Cast( *g, c.o, c.d, true );
		REQUIRE( hit.bHit, (std::string(c.tag) + " hits").c_str() );
		REQUIRE( IsClose( hit.range2, 0.0, 1e-12 ), (std::string(c.tag) + " range2==0 (started inside)").c_str() );
		// The single crossing must be a real boundary point (on side or cap).
		const Scalar ax   = AxialOf( hit.pos, 'z' );
		const Scalar rad2 = Radial2Of( hit.pos, 'z' );
		const bool onCap  = IsClose( std::fabs(ax), h*0.5, 1e-6 ) && ( rad2 <= r*r + 1e-6 );
		const bool onSide = IsClose( std::sqrt(rad2), r, 1e-6 ) && ( std::fabs(ax) <= h*0.5 + 1e-6 );
		REQUIRE( onCap || onSide, (std::string(c.tag) + " exit on the boundary").c_str() );
	}

	g->release();
}

// ============================================================
// 4: side wall still works in capped mode
// ============================================================
static void TestSideStillHits()
{
	const Scalar r = 1.0, h = 2.0;
	CylinderGeometry* g = new CylinderGeometry( 'y', r, h, true );

	// Radial ray toward the axis at mid-height hits the +X side wall at x=r.
	Hit hit = Cast( *g, Point3(5, 0, 0), Vector3(-1,0,0) );
	REQUIRE( hit.bHit, "capped side hit" );
	REQUIRE( IsClose( hit.range, 4.0, 1e-6 ), "capped side range" );
	REQUIRE( IsClose( hit.pos.x, 1.0, 1e-6 ), "capped side point at x=r" );
	Vector3 n = Vector3Ops::Normalize( hit.normal );
	REQUIRE( IsClose( Vector3Ops::Dot( n, Vector3(1,0,0) ), 1.0, 1e-6 ), "capped side normal outward radial" );
	// Side normal has no axial component.
	REQUIRE( IsClose( n.y, 0.0, 1e-6 ), "capped side normal has no axial component" );

	g->release();
}

// ============================================================
// 6: ray starting inside exits through a cap
// ============================================================
static void TestInteriorExit()
{
	const Scalar r = 1.0, h = 2.0;
	CylinderGeometry* g = new CylinderGeometry( 'z', r, h, true );

	// Start at the centre, travel +Z: the nearest boundary is the max cap at
	// z = +h/2, distance h/2.
	Hit hit = Cast( *g, Point3(0,0,0), Vector3(0,0,1), false );
	REQUIRE( hit.bHit, "interior-origin ray hits" );
	REQUIRE( IsClose( hit.range, h*0.5, 1e-6 ), "interior-origin exit range = to cap" );
	REQUIRE( IsClose( AxialOf(hit.pos,'z'), h*0.5, 1e-6 ), "interior-origin exit on max cap" );
	Vector3 n = Vector3Ops::Normalize( hit.normal );
	REQUIRE( IsClose( Vector3Ops::Dot( n, Vector3(0,0,1) ), 1.0, 1e-6 ), "interior-origin cap normal outward" );

	g->release();
}

// ============================================================
// 7: area
// ============================================================
static void TestArea()
{
	const Scalar r = 1.3, h = 2.7;
	CylinderGeometry* solid = new CylinderGeometry( 'y', r, h, true );
	CylinderGeometry* tube  = new CylinderGeometry( 'y', r, h, false );

	const Scalar side = 2.0 * PI * r * h;
	const Scalar caps = 2.0 * PI * r * r;
	REQUIRE( IsClose( solid->GetArea(), side + caps, 1e-9 ), "capped area = side + 2 caps" );
	REQUIRE( IsClose( tube ->GetArea(), side,        1e-9 ), "open tube area = side" );

	solid->release();
	tube->release();
}

// ============================================================
// 8: UniformRandomPoint — on-surface, outward unit normal, area split
// ============================================================
static void TestUniformRandomPoint()
{
	// Radius/height chosen so the side/cap area boundary does NOT land on a
	// stratum edge — the cap-fraction tolerance below then genuinely does work
	// (with r=1,h=2 the boundary sits exactly on 1/3 and the check is vacuous).
	const Scalar r = 1.0, h = 2.3;
	const char axis = 'z';
	CylinderGeometry* g = new CylinderGeometry( axis, r, h, true );

	const Scalar sideA = 2.0 * PI * r * h;
	const Scalar capA  = PI * r * r;
	const Scalar total = sideA + 2.0 * capA;
	const Scalar expectedCapFrac = 2.0 * capA / total;

	const int N = 40;
	int nSamples = 0, nCap = 0, nOffSurface = 0, nBadNormal = 0;

	for( int i = 0; i < N; i++ ) {
		for( int j = 0; j < N; j++ ) {
			for( int k = 0; k < N; k++ ) {
				const Point3 prand( (i+0.5)/N, (j+0.5)/N, (k+0.5)/N );
				Point3  pt;
				Vector3 nrm;
				Point2  uv;
				g->UniformRandomPoint( &pt, &nrm, &uv, prand );
				nSamples++;

				// On surface: either on a cap plane (within the disk) or on the
				// side wall (radial == r).
				const Scalar ax   = AxialOf( pt, axis );
				const Scalar rad2 = Radial2Of( pt, axis );
				const bool onCap  = IsClose( std::fabs(ax), h*0.5, 1e-6 ) && ( rad2 <= r*r + 1e-6 );
				const bool onSide = IsClose( std::sqrt(rad2), r, 1e-6 ) && ( std::fabs(ax) <= h*0.5 + 1e-6 );
				if( !onCap && !onSide ) { nOffSurface++; continue; }
				if( onCap ) nCap++;

				// Normal is unit and points outward (P + eps*n leaves the solid,
				// P - eps*n stays inside).
				const Scalar nlen = Vector3Ops::Magnitude( nrm );
				if( !IsClose( nlen, 1.0, 1e-6 ) ) { nBadNormal++; continue; }
				const Scalar eps = 1e-3;
				const Point3 outP( pt.x + eps*nrm.x, pt.y + eps*nrm.y, pt.z + eps*nrm.z );
				const Point3 inP ( pt.x - eps*nrm.x, pt.y - eps*nrm.y, pt.z - eps*nrm.z );
				if( InsideCapped( outP, axis, r, h, 1e-9 ) || !InsideCapped( inP, axis, r, h, 1e-2 ) ) {
					nBadNormal++;
				}
			}
		}
	}

	REQUIRE( nOffSurface == 0, "all UniformRandomPoint samples lie on the surface" );
	REQUIRE( nBadNormal == 0, "all UniformRandomPoint normals are unit + outward" );

	const Scalar capFrac = Scalar(nCap) / Scalar(nSamples);
	// prand.z is stratified across [0,1], so the cap fraction tracks the area
	// fraction to within the 1/N stratification error (~0.025 here); a broken
	// area split (e.g. dropping the factor-of-2 for two caps) is off by >0.1.
	REQUIRE( std::fabs( capFrac - expectedCapFrac ) < 0.02,
		"UniformRandomPoint cap fraction matches area fraction" );

	g->release();
}

// ============================================================
// 9: tessellation closes the solid with outward-wound cap faces
// ============================================================
static void TestTessellationClosure( char axis )
{
	const Scalar r = 1.0, h = 2.0;
	const unsigned int detail = 16;

	CylinderGeometry* solid = new CylinderGeometry( axis, r, h, true );
	CylinderGeometry* tube  = new CylinderGeometry( axis, r, h, false );

	IndexTriangleListType triS, triT;
	VerticesListType      verS, verT;
	NormalsListType       nrmS, nrmT;
	TexCoordsListType     uvS,  uvT;

	REQUIRE( solid->TessellateToMesh( triS, verS, nrmS, uvS, detail ), "capped tessellation ok" );
	REQUIRE( tube ->TessellateToMesh( triT, verT, nrmT, uvT, detail ), "open tessellation ok" );

	const std::string tag = std::string("tess-") + axis;

	// The capped mesh must have strictly more triangles (two cap fans).
	REQUIRE( triS.size() > triT.size(), (tag + " capped mesh has extra cap triangles").c_str() );
	REQUIRE( triS.size() == triT.size() + 2*detail, (tag + " capped adds exactly 2*detail cap tris").c_str() );

	// Every triangle whose 3 vertices sit on a cap plane must be wound so its
	// geometric normal points outward along the cap's axis.
	int capTris = 0, badWind = 0;
	for( size_t t = 0; t < triS.size(); t++ ) {
		const IndexedTriangle& it = triS[t];
		const Point3& v0 = verS[ it.iVertices[0] ];
		const Point3& v1 = verS[ it.iVertices[1] ];
		const Point3& v2 = verS[ it.iVertices[2] ];

		const Scalar a0 = AxialOf( v0, axis );
		const Scalar a1 = AxialOf( v1, axis );
		const Scalar a2 = AxialOf( v2, axis );

		// A cap triangle: all three axial coords equal +h/2 or all equal -h/2.
		const bool maxCap = IsClose(a0, h*0.5,1e-6) && IsClose(a1,h*0.5,1e-6) && IsClose(a2,h*0.5,1e-6);
		const bool minCap = IsClose(a0,-h*0.5,1e-6) && IsClose(a1,-h*0.5,1e-6) && IsClose(a2,-h*0.5,1e-6);
		if( !maxCap && !minCap ) continue;
		capTris++;

		const Vector3 e1 = Vector3Ops::mkVector3( v1, v0 );
		const Vector3 e2 = Vector3Ops::mkVector3( v2, v0 );
		const Vector3 fn = Vector3Ops::Normalize( Vector3Ops::Cross( e1, e2 ) );
		const Scalar  fnAxial = AxialOf( Point3(fn.x,fn.y,fn.z), axis );
		const Scalar  wantSign = maxCap ? 1.0 : -1.0;
		if( !( fnAxial * wantSign > 0.9 ) ) badWind++;
	}
	REQUIRE( capTris == int(2*detail), (tag + " found both cap fans").c_str() );
	REQUIRE( badWind == 0, (tag + " all cap faces wound outward").c_str() );

	solid->release();
	tube->release();
}

// ============================================================
// 10: ComputeSurfaceDerivatives on a cap point
// ============================================================
static void TestCapDerivatives()
{
	const Scalar r = 1.0, h = 2.0;
	CylinderGeometry* g = new CylinderGeometry( 'z', r, h, true );

	// A point on the +Z (max) cap, off-centre so the frame is exercised.
	const Point3  p( 0.3, -0.4, h*0.5 );
	const Vector3 n( 0, 0, 1 );
	SurfaceDerivatives sd = g->ComputeSurfaceDerivatives( p, n );

	REQUIRE( sd.valid, "cap derivatives valid" );
	// Tangency: dpdu, dpdv lie in the cap plane (perpendicular to the normal).
	REQUIRE( IsClose( Vector3Ops::Dot( sd.dpdu, n ), 0.0, 1e-6 ), "cap dpdu tangent" );
	REQUIRE( IsClose( Vector3Ops::Dot( sd.dpdv, n ), 0.0, 1e-6 ), "cap dpdv tangent" );
	// Right-handed: (dpdu x dpdv) . n > 0.
	const Scalar hand = Vector3Ops::Dot( Vector3Ops::Cross( sd.dpdu, sd.dpdv ), n );
	REQUIRE( hand > 0.0, "cap frame right-handed" );

	// Same check on the -Z (min) cap, where the outward normal flips.
	const Point3  pm( -0.2, 0.5, -h*0.5 );
	const Vector3 nm( 0, 0, -1 );
	SurfaceDerivatives sm = g->ComputeSurfaceDerivatives( pm, nm );
	REQUIRE( sm.valid, "min-cap derivatives valid" );
	REQUIRE( IsClose( Vector3Ops::Dot( sm.dpdu, nm ), 0.0, 1e-9 ), "min-cap dpdu tangent" );
	REQUIRE( IsClose( Vector3Ops::Dot( sm.dpdv, nm ), 0.0, 1e-9 ), "min-cap dpdv tangent" );
	REQUIRE( Vector3Ops::Dot( Vector3Ops::Cross( sm.dpdu, sm.dpdv ), nm ) > 0.0, "min-cap frame right-handed" );

	g->release();
}

// ============================================================
// Front/back-face filtering on the capped solid (IGeometry contract,
// consistent with Sphere/Box/SDF).  Regression for: capped IntersectRay /
// IntersectRay_IntersectionOnly discarded bHitFrontFaces/bHitBackFaces, so a
// front-only ray started INSIDE reported the back-face exit and (false,false)
// still hit.  Revert-proof: these fail on the pre-fix capped path.
// ============================================================
static void TestFrontBackFilter()
{
	std::cout << "-- front/back-face filter (capped)" << std::endl;
	const Scalar r = 1.0, h = 2.0;                       // z-axis caps at z = +-1
	CylinderGeometry* g = new CylinderGeometry( 'z', r, h, true );

	auto cast = [&]( const Point3& o, const Vector3& d, bool front, bool back ) -> Hit {
		RayIntersectionGeometric ri( Ray( o, d ), nullRasterizerState );
		g->IntersectRay( ri, front, back, true );
		Hit hh; hh.bHit = ri.bHit; hh.range = ri.range; hh.pos = ri.ptIntersection; hh.normal = ri.vNormal; return hh;
	};

	// From OUTSIDE, straight down the axis: entry = +z top cap (FRONT), exit = -z (BACK).
	{	Hit hf = cast( Point3(0,0,5), Vector3(0,0,-1), true,  false );
		REQUIRE( hf.bHit && IsClose( hf.pos.z, 1.0, 1e-4 ), "front-only outside hits the top ENTRY cap z=+1" ); }
	{	Hit hb = cast( Point3(0,0,5), Vector3(0,0,-1), false, true );
		REQUIRE( hb.bHit && IsClose( hb.pos.z, -1.0, 1e-4 ), "back-only outside steps PAST the entry to the exit cap z=-1" ); }
	{	Hit hn = cast( Point3(0,0,5), Vector3(0,0,-1), false, false );
		REQUIRE( !hn.bHit, "(false,false) does not hit the capped solid" ); }

	// From INSIDE (origin), going down: only the exit (-z, BACK) lies ahead.
	{	Hit hi = cast( Point3(0,0,0), Vector3(0,0,-1), true, false );
		REQUIRE( !hi.bHit, "front-only from INSIDE does not report the back-face exit (P1)" ); }
	{	Hit hib = cast( Point3(0,0,0), Vector3(0,0,-1), false, true );
		REQUIRE( hib.bHit && IsClose( hib.pos.z, -1.0, 1e-4 ), "back-only from inside hits the exit cap z=-1" ); }

	// Shadow / visibility path honours the same filter.
	REQUIRE( !g->IntersectRay_IntersectionOnly( Ray( Point3(0,0,0), Vector3(0,0,-1) ), 100.0, true,  false ),
	         "shadow: front-only from inside is NOT occluded by the back exit (P1)" );
	REQUIRE(  g->IntersectRay_IntersectionOnly( Ray( Point3(0,0,0), Vector3(0,0,-1) ), 100.0, false, true ),
	         "shadow: back-only from inside IS occluded by the exit" );
	REQUIRE( !g->IntersectRay_IntersectionOnly( Ray( Point3(0,0,5), Vector3(0,0,-1) ), 100.0, false, false ),
	         "shadow: (false,false) never occludes" );

	// Sanity: with BOTH flags (the render default) the nearest crossing still wins
	// -- byte-identical to the pre-filter behaviour.
	{	Hit hd = cast( Point3(0,0,5), Vector3(0,0,-1), true, true );
		REQUIRE( hd.bHit && IsClose( hd.pos.z, 1.0, 1e-4 ), "both-flags reports the nearest crossing (top cap)" ); }

	// --- OPEN tube (capped=false): the same filter on the side wall, via a RADIAL
	// ray (an axial ray misses the wall entirely -- that is the foot-gun).  Near
	// wall = FRONT, far wall = BACK; from inside only the exit wall (BACK) is ahead.
	CylinderGeometry* tube = new CylinderGeometry( 'z', r, h, false );
	auto castT = [&]( const Point3& o, const Vector3& d, bool front, bool back ) -> Hit {
		RayIntersectionGeometric ri( Ray( o, d ), nullRasterizerState );
		tube->IntersectRay( ri, front, back, true );
		Hit hh; hh.bHit = ri.bHit; hh.pos = ri.ptIntersection; return hh;
	};
	{	Hit hf = castT( Point3(5,0,0), Vector3(-1,0,0), true,  false );
		REQUIRE( hf.bHit && IsClose( hf.pos.x, 1.0, 1e-4 ), "tube: front-only outside hits the NEAR wall x=+1" ); }
	{	Hit hb = castT( Point3(5,0,0), Vector3(-1,0,0), false, true );
		REQUIRE( hb.bHit && IsClose( hb.pos.x, -1.0, 1e-4 ), "tube: back-only outside steps to the FAR wall x=-1" ); }
	{	Hit hn = castT( Point3(5,0,0), Vector3(-1,0,0), false, false );
		REQUIRE( !hn.bHit, "tube: (false,false) does not hit the side wall" ); }
	{	Hit hi = castT( Point3(0,0,0), Vector3(-1,0,0), true, false );
		REQUIRE( !hi.bHit, "tube: front-only from INSIDE does not report the back exit wall" ); }
	{	Hit hib = castT( Point3(0,0,0), Vector3(-1,0,0), false, true );
		REQUIRE( hib.bHit && IsClose( hib.pos.x, -1.0, 1e-4 ), "tube: back-only from inside hits the exit wall x=-1" ); }
	REQUIRE( !tube->IntersectRay_IntersectionOnly( Ray( Point3(0,0,0), Vector3(-1,0,0) ), 100.0, true,  false ),
	         "tube shadow: front-only from inside is NOT occluded by the exit wall" );
	REQUIRE(  tube->IntersectRay_IntersectionOnly( Ray( Point3(0,0,0), Vector3(-1,0,0) ), 100.0, false, true ),
	         "tube shadow: back-only from inside IS occluded by the exit wall" );

	safe_release( tube );
	safe_release( g );
}

int main()
{
	std::cout << "========================================" << std::endl;
	std::cout << "  CappedCylinderTest" << std::endl;
	std::cout << "========================================" << std::endl;

	const char axes[3] = { 'x', 'y', 'z' };
	for( int i = 0; i < 3; ++i ) {
		TestAxialThroughCaps( axes[i] );
		TestTessellationClosure( axes[i] );
	}
	TestCapUV();
	TestObliqueAndRim();
	TestInsideRimExit();
	TestSideStillHits();
	TestInteriorExit();
	TestArea();
	TestUniformRandomPoint();
	TestCapDerivatives();
	TestFrontBackFilter();

	std::cout << "----------------------------------------" << std::endl;
	if( g_failures == 0 ) {
		std::cout << "CappedCylinderTest: ALL PASSED" << std::endl;
		return 0;
	}
	std::cout << "CappedCylinderTest: " << g_failures << " FAILURE(S)" << std::endl;
	return 1;
}
