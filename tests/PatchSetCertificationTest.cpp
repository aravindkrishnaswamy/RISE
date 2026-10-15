//////////////////////////////////////////////////////////////////////
//
//  PatchSetCertificationTest.cpp - DL-616 / DL-617 review round 1.
//
//  The patch-set certifier (`GeometricUtilities::CertifyBezierPatchSet`,
//  `CertifyBilinearPatchSet`) and the open-sheet CSG composites built on
//  it.  The dangerous direction is a FALSE "provably open" or
//  "watertight" certificate, so most rows are refusals of sets whose
//  topology looks fine but whose geometry is not an embedding:
//
//   A. a closed Bezier solid (cube-sphere) certifies watertight; its
//      inward-wound twin is refused; one patch removed certifies open;
//      a genus-1 bilinear frame certifies watertight;
//   B. a helical strip whose second turn passes through the first is
//      refused -- an immersion is not an embedding (P1-1);
//   C. a closed cube whose top face is split (two T-junctions) is
//      refused, not certified open (P2-4);
//   D. a vertex fan whose last wedge crosses the first through the
//      shared vertex is refused (P2-3);
//   E. sheet INTERSECT / MINUS a certified-closed patch solid: a convex
//      Bezier solid, and a frame the ray crosses twice so the sheet hit
//      lies in the SECOND interval (P2-5 / P2-6); shadow rays agree;
//      a transformed composite keeps range2 == RISE_INFINITY;
//   F. CSG union of disjoint certified sheets is NOT certified (P2-8);
//      nested CSG keeps the certificate and the trimming;
//   G. timing of a large closed set, and the shipped Bezier models'
//      verdicts with the reason each is refused (P1-2).
//
//  Built against the pre-review baseline with
//  -DDL616_BASELINE_SIGNATURE (the certifiers had no reason output).
//
//////////////////////////////////////////////////////////////////////

#define main DomainFixtureMain
#include "SMSDomainReplayTest.cpp"
#undef main

#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "../src/Library/Polygon.h"
#include "../src/Library/Utilities/GeometricUtilities.h"
#include "../src/Library/Objects/CSGObject.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Geometry/BezierPatchGeometry.h"
#include "../src/Library/Geometry/BoxGeometry.h"
#include "../src/Library/Geometry/ClippedPlaneGeometry.h"
#include "../src/Library/Geometry/DisplacedGeometry.h"
#include "../src/Library/Geometry/TriangleMeshGeometry.h"
#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"

using namespace RISE;
using namespace RISE::Implementation;

struct Verdict
{
	bool watertight = false;
	bool open = false;
	bool negative = false;
	const char* reason = "";
	unsigned int grid = 0;
};

static Verdict CertBezier( const std::vector<BezierPatch>& p, std::size_t budget = 0 )
{
	Verdict v;
#ifdef DL616_BASELINE_SIGNATURE
	(void)budget;
	GeometricUtilities::CertifyBezierPatchSet( p, v.watertight, v.open, v.negative );
#else
	GeometricUtilities::PatchSetCertDiagnostics d;
	d.workBudget = budget;
	GeometricUtilities::CertifyBezierPatchSet( p, v.watertight, v.open, v.negative, &d );
	v.reason = d.reason;
	v.grid = d.grid;
#endif
	return v;
}

static Verdict CertBilinear( const std::vector<BilinearPatch>& p, std::size_t budget = 0 )
{
	Verdict v;
#ifdef DL616_BASELINE_SIGNATURE
	(void)budget;
	GeometricUtilities::CertifyBilinearPatchSet( p, v.watertight, v.open, v.negative );
#else
	GeometricUtilities::PatchSetCertDiagnostics d;
	d.workBudget = budget;
	GeometricUtilities::CertifyBilinearPatchSet( p, v.watertight, v.open, v.negative, &d );
	v.reason = d.reason;
	v.grid = d.grid;
#endif
	return v;
}

static int MeshOpenness( const std::vector<Point3>& corners )
{
#ifdef DL621_BASELINE_SIGNATURE
	(void)corners;
	return -2;
#else
	return GeometricUtilities::TriangleMeshOpenness( corners );
#endif
}

template<typename GeomT>
static int TopOpenness( const GeomT* g )
{
#ifdef DL621_BASELINE_SIGNATURE
	if( !g ) {
		return -2;
	}
	if( g->IsProvablyOpenSheet() ) {
		return 1;
	}
	if( g->IsCertifiedWatertight() ) {
		return 0;
	}
	return -1;
#else
	return g ? g->TopologicalOpenness() : -2;
#endif
}

static int TopOpenness( const TriangleMeshGeometry* g )
{
#ifdef DL621_BASELINE_SIGNATURE
	if( !g ) {
		return -2;
	}
	return g->IsProvablyOpenSheet() ? 1 : -1;
#else
	return g ? g->TopologicalOpenness() : -2;
#endif
}

static std::string Describe( const Verdict& v )
{
	return std::string( v.watertight ? "watertight" : ( v.open ? ( v.negative ? "open(neg)" : "open" ) : "refused" ) ) +
		( v.reason && *v.reason ? std::string( " [" ) + v.reason + "]" : std::string() ) +
		( v.grid ? " grid " + std::to_string( v.grid ) : std::string() );
}

//////////////////////////////////////////////////////////////////////
// Fixtures
//////////////////////////////////////////////////////////////////////

//! Cube-sphere: each cube face split into k x k bicubic patches whose
//! control points are integer lattice points of the cube surface pushed
//! onto the sphere.  Shared edges are the same lattice points, so glued
//! sides are bitwise reversed.  `outward` controls the winding.
static std::vector<BezierPatch> MakeCubeSphere( int k, bool outward, Scalar radius = 1 )
{
	const int m = 3 * k;
	struct Face { int axis; int sign; int uAxis; int vAxis; };
	// U x V = outward normal for each face.
	const Face faces[6] = {
		{ 2, +1, 0, 1 },	// +Z: x, y
		{ 2, -1, 1, 0 },	// -Z: y, x
		{ 0, +1, 1, 2 },	// +X: y, z
		{ 0, -1, 2, 1 },	// -X: z, y
		{ 1, +1, 2, 0 },	// +Y: z, x
		{ 1, -1, 0, 2 }		// -Y: x, z
	};
	auto lattice = [&]( const Face& f, int I, int J ) {
		int c[3];
		c[f.axis] = f.sign * m;
		c[f.uAxis] = 2 * I - m;
		c[f.vAxis] = 2 * J - m;
		const Scalar x = Scalar( c[0] ), y = Scalar( c[1] ), z = Scalar( c[2] );
		const Scalar len = std::sqrt( x * x + y * y + z * z );
		return Point3( radius * x / len, radius * y / len, radius * z / len );
	};
	std::vector<BezierPatch> out;
	for( const Face& f : faces ) {
		for( int a = 0; a < k; ++a ) {
			for( int b = 0; b < k; ++b ) {
				BezierPatch p;
				for( int i = 0; i < 4; ++i ) {
					for( int j = 0; j < 4; ++j ) {
						const Point3 q = lattice( f, 3 * a + i, 3 * b + j );
						if( outward ) {
							p.c[i].pts[j] = q;
						} else {
							p.c[j].pts[i] = q;	// transpose flips Pu x Pv
						}
					}
				}
				out.push_back( p );
			}
		}
	}
	return out;
}

//! Bilinear quad from a polygon P0..P3 counter-clockwise seen from the
//! side its normal points to (normal = (P1 - P0) x (P3 - P0)).
static BilinearPatch MakeQuad( const Point3& P0, const Point3& P1, const Point3& P2, const Point3& P3 )
{
	BilinearPatch b;
	b.pts[0] = P0;	// (u0, v0)
	b.pts[2] = P1;	// (u1, v0)
	b.pts[3] = P2;	// (u1, v1)
	b.pts[1] = P3;	// (u0, v1)
	return b;
}

//! Outward-wound unit cube [0,1]^3 as six quads.
static std::vector<BilinearPatch> MakeBilinearCube()
{
	const Point3 v[8] = {
		Point3(0,0,0), Point3(1,0,0), Point3(1,1,0), Point3(0,1,0),
		Point3(0,0,1), Point3(1,0,1), Point3(1,1,1), Point3(0,1,1) };
	std::vector<BilinearPatch> c;
	c.push_back( MakeQuad( v[0], v[3], v[2], v[1] ) );	// -Z
	c.push_back( MakeQuad( v[4], v[5], v[6], v[7] ) );	// +Z
	c.push_back( MakeQuad( v[0], v[1], v[5], v[4] ) );	// -Y
	c.push_back( MakeQuad( v[2], v[3], v[7], v[6] ) );	// +Y
	c.push_back( MakeQuad( v[0], v[4], v[7], v[3] ) );	// -X
	c.push_back( MakeQuad( v[1], v[2], v[6], v[5] ) );	// +X
	return c;
}

//! Square "picture frame" torus: outer square half-width 2, hole
//! half-width 1, thickness z in [-0.5, 0.5]; 16 outward quads, genus 1.
static std::vector<BilinearPatch> MakeBilinearFrame()
{
	const Scalar ox[4] = { 2, 2, -2, -2 }, oy[4] = { -2, 2, 2, -2 };
	const Scalar ix[4] = { 1, 1, -1, -1 }, iy[4] = { -1, 1, 1, -1 };
	auto O = [&]( int i, Scalar z ) { return Point3( ox[i & 3], oy[i & 3], z ); };
	auto I = [&]( int i, Scalar z ) { return Point3( ix[i & 3], iy[i & 3], z ); };
	const Scalar t = 0.5, b = -0.5;
	std::vector<BilinearPatch> f;
	for( int i = 0; i < 4; ++i ) {
		f.push_back( MakeQuad( O( i, t ), O( i + 1, t ), I( i + 1, t ), I( i, t ) ) );	// top
		f.push_back( MakeQuad( O( i, b ), I( i, b ), I( i + 1, b ), O( i + 1, b ) ) );	// bottom
		f.push_back( MakeQuad( O( i, b ), O( i + 1, b ), O( i + 1, t ), O( i, t ) ) );	// outer wall
		f.push_back( MakeQuad( I( i + 1, b ), I( i, b ), I( i, t ), I( i + 1, t ) ) );	// inner wall
	}
	return f;
}

//! A bicubic annulus-sector patch: u radial (r0 -> r1), v angular
//! (lattice angles a0 -> a0 + 3 in units of pi/6), heights z(r, a).
//! Angles and radii come from integer lattice indices so neighbouring
//! patches share bitwise-identical side control points.
template<class ZFn>
static BezierPatch AnnulusPatch( Scalar r0, Scalar r1, int a0, ZFn z )
{
	BezierPatch p;
	const Scalar pi = 3.14159265358979323846;
	for( int i = 0; i < 4; ++i ) {
		const Scalar r = r0 + ( r1 - r0 ) * Scalar( i ) / 3;
		for( int j = 0; j < 4; ++j ) {
			const int a = a0 + j;
			const Scalar th = Scalar( a ) * pi / 6;
			p.c[i].pts[j] = Point3( r * std::cos( th ), r * std::sin( th ), z( r, a ) );
		}
	}
	return p;
}

//////////////////////////////////////////////////////////////////////
// A. closed solid / inward / open
//////////////////////////////////////////////////////////////////////

static void TestClosedSolids()
{
	std::cout << "A. closed patch solids" << std::endl;
	for( int k = 1; k <= 2; ++k ) {
		const Verdict v = CertBezier( MakeCubeSphere( k, true ) );
		std::cout << "    cube-sphere k=" << k << " outward: " << Describe( v ) << std::endl;
		Check( v.watertight && !v.open, "A: closed cube-sphere (k=" + std::to_string( k ) + ") certifies watertight" );

		const Verdict w = CertBezier( MakeCubeSphere( k, false ) );
		std::cout << "    cube-sphere k=" << k << " inward: " << Describe( w ) << std::endl;
		Check( !w.watertight && !w.open, "A: inward-wound closed cube-sphere is refused" );

		std::vector<BezierPatch> holed = MakeCubeSphere( k, true );
		holed.pop_back();
		const Verdict h = CertBezier( holed );
		std::cout << "    cube-sphere k=" << k << " one patch removed: " << Describe( h ) << std::endl;
		Check( h.open && !h.watertight && !h.negative, "A: cube-sphere minus one patch certifies open, outward winding" );

		std::vector<BezierPatch> holedIn = MakeCubeSphere( k, false );
		holedIn.pop_back();
		const Verdict hi = CertBezier( holedIn );
		Check( hi.open && hi.negative, "A: inward cube-sphere minus one patch certifies open with the negative-volume flag" );
	}

	const Verdict c = CertBilinear( MakeBilinearCube() );
	std::cout << "    bilinear cube: " << Describe( c ) << std::endl;
	Check( c.watertight, "A: closed bilinear cube certifies watertight" );

	const Verdict fr = CertBilinear( MakeBilinearFrame() );
	std::cout << "    bilinear frame (genus 1): " << Describe( fr ) << std::endl;
	Check( fr.watertight, "A: closed genus-1 bilinear frame certifies watertight" );
}

//////////////////////////////////////////////////////////////////////
// B. crossing helical strip (P1-1)
//////////////////////////////////////////////////////////////////////

//! Five annulus sectors of 90 degrees (lattice 3 each), a helicoid
//! rising h per radian.  `cross` makes the fifth (second-turn) sector
//! descend steeply from its glued side so it passes down through the
//! first.  Every Bernstein normal of the strip has a positive z
//! component either way, so it is an immersion over the xy plane; only
//! the crossing variant fails to be an embedding.
static std::vector<BezierPatch> MakeHelicalStrip( bool cross )
{
	const Scalar pi = 3.14159265358979323846;
	const Scalar h = 0.1;
	std::vector<BezierPatch> s;
	for( int k = 0; k < 5; ++k ) {
		const bool last = ( k == 4 );
		s.push_back( AnnulusPatch( 1.0, 2.0, 3 * k, [&]( Scalar, int a ) {
			const Scalar th = Scalar( a ) * pi / 6;
			if( last && cross ) {
				return h * 2 * pi - 1.0 * ( th - 2 * pi );
			}
			return h * th;
		} ) );
	}
	return s;
}

//! The second-turn sector equals the first plus a gap
//! d(v) = s (eps - K (v - a)(v - b)(v - c)) that is POSITIVE at every
//! quarter node in v (so any 5x5 sampling of the two sectors stays
//! apart) but, at K = 16, negative inside (a, b): the true surfaces
//! cross between the samples.  At K = 10 the gap stays positive (a
//! near-miss control).  All z scaled by s keeps every normal near +z.
static std::vector<BezierPatch> MakeSaggingStrip( Scalar K )
{
	const Scalar pi = 3.14159265358979323846;
	const Scalar h = 0.1, s = 0.05, a = 0.1, b = 0.2, c = 2;
	const Scalar eps = 2 * pi * h - K * a * b * c;
	const Scalar p0 = eps + K * a * b * c;
	const Scalar p1 = -K * ( a * b + a * c + b * c );
	const Scalar p2 = K * ( a + b + c );
	const Scalar p3 = -K;
	// Bernstein coefficients of the cubic gap.
	const Scalar bf[4] = { p0, p0 + p1 / 3, p0 + 2 * p1 / 3 + p2 / 3, p0 + p1 + p2 + p3 };
	std::vector<BezierPatch> strip;
	for( int k = 0; k < 5; ++k ) {
		const bool last = ( k == 4 );
		strip.push_back( AnnulusPatch( 1.0, 2.0, 3 * k, [&]( Scalar, int aa ) {
			const Scalar th = Scalar( aa ) * pi / 6;
			if( last && aa != 12 ) {
				return s * ( h * ( th - 2 * pi ) + bf[aa - 12] );
			}
			return s * h * th;
		} ) );
	}
	return strip;
}

static void TestCrossingStrip()
{
	std::cout << "B. helical strip" << std::endl;
	const Verdict ok = CertBezier( MakeHelicalStrip( false ) );
	std::cout << "    embedded helicoid: " << Describe( ok ) << std::endl;
	Check( ok.open && !ok.watertight, "B: embedded 1.25-turn helicoid strip certifies open" );

	const Verdict bad = CertBezier( MakeHelicalStrip( true ) );
	std::cout << "    crossing strip: " << Describe( bad ) << std::endl;
	Check( !bad.open && !bad.watertight, "B: strip whose second turn passes through the first is refused" );

	const Verdict nearMiss = CertBezier( MakeSaggingStrip( 10 ) );
	std::cout << "    strip passing close above itself (control): " << Describe( nearMiss ) << std::endl;
	Check( nearMiss.open, "B: strip whose second turn passes close above the first still certifies open" );

	const Verdict sag = CertBezier( MakeSaggingStrip( 16 ) );
	std::cout << "    strip sagging through between samples: " << Describe( sag ) << std::endl;
	Check( !sag.open && !sag.watertight,
		"B: strip whose second turn sags through the first only between grid samples is refused" );
}

//////////////////////////////////////////////////////////////////////
// C. T-junction closed body (P2-4)
//////////////////////////////////////////////////////////////////////

//! The unit cube with its top face split at x = 0.5: the -Y and +Y
//! faces' top sides each meet two half-sides -> T-junctions at
//! (0.5, 0, 1) and (0.5, 1, 1).  The body still encloses the cube.
static std::vector<BilinearPatch> MakeTJunctionCube()
{
	std::vector<BilinearPatch> c = MakeBilinearCube();
	c[1] = MakeQuad( Point3(0,0,1), Point3(0.5,0,1), Point3(0.5,1,1), Point3(0,1,1) );
	c.push_back( MakeQuad( Point3(0.5,0,1), Point3(1,0,1), Point3(1,1,1), Point3(0.5,1,1) ) );
	return c;
}

static void TestTJunction()
{
	std::cout << "C. T-junction" << std::endl;
	const Verdict v = CertBilinear( MakeTJunctionCube() );
	std::cout << "    cube with split top: " << Describe( v ) << std::endl;
	Check( !v.open && !v.watertight, "C: closed body with T-junctions is refused (not certified open)" );
}

//////////////////////////////////////////////////////////////////////
// D. fan crossing at its shared vertex (P2-3)
//////////////////////////////////////////////////////////////////////

//! Five quarter-turn bilinear wedges around O, chained by glued radial
//! sides, rising along a helicoid.  `cross` sends the fifth wedge's far
//! corners below the first wedge, so the two cross along a curve that
//! ends AT the shared vertex O.
static std::vector<BilinearPatch> MakeFan( bool cross )
{
	const Scalar pi = 3.14159265358979323846;
	const Scalar R = 1, h = 0.1;
	const Point3 O( 0, 0, 0 );
	auto rim = [&]( int q, Scalar rad, Scalar z ) {	// q in eighth turns
		const Scalar th = Scalar( q ) * pi / 4;
		return Point3( rad * std::cos( th ), rad * std::sin( th ), z );
	};
	std::vector<BilinearPatch> f;
	for( int k = 0; k < 5; ++k ) {
		const int q0 = 2 * k, q2 = 2 * k + 2;
		Scalar z0 = h * Scalar( q0 ) * pi / 4 * R;
		Scalar z1 = h * Scalar( q0 + 1 ) * pi / 4 * R;
		Scalar z2 = h * Scalar( q2 ) * pi / 4 * R;
		if( k == 4 && cross ) {
			z1 = -0.5;
			z2 = -0.8;
		}
		f.push_back( MakeQuad( O, rim( q0, R, z0 ), rim( q0 + 1, R * 1.4142, z1 ), rim( q2, R, z2 ) ) );
	}
	return f;
}

static void TestFanCrossingAtVertex()
{
	std::cout << "D. vertex fan" << std::endl;
	const Verdict ok = CertBilinear( MakeFan( false ) );
	std::cout << "    helicoid fan: " << Describe( ok ) << std::endl;
	Check( ok.open, "D: embedded 1.25-turn fan around one vertex certifies open" );

	const Verdict bad = CertBilinear( MakeFan( true ) );
	std::cout << "    fan crossing through its vertex: " << Describe( bad ) << std::endl;
	Check( !bad.open && !bad.watertight, "D: fan whose last wedge crosses the first through the shared vertex is refused" );
}

//////////////////////////////////////////////////////////////////////
// E/F. open-sheet CSG against patch solids
//////////////////////////////////////////////////////////////////////

static std::string WriteBezier( const char* name, const std::vector<BezierPatch>& patches )
{
	const std::string path = TestTempPath( name );
	std::ofstream out( path );
	out.precision( 17 );
	out << patches.size() << "\n";
	for( const BezierPatch& p : patches ) {
		for( int j = 0; j < 4; ++j ) {
			for( int k = 0; k < 4; ++k ) {
				out << p.c[j].pts[k].x << ' ' << p.c[j].pts[k].y << ' ' << p.c[j].pts[k].z << "\n";
			}
		}
	}
	return path;
}

static std::string WriteBilinear( const char* name, const std::vector<BilinearPatch>& patches )
{
	const std::string path = TestTempPath( name );
	std::ofstream out( path );
	out.precision( 17 );
	out << patches.size() << "\n";
	for( const BilinearPatch& p : patches ) {
		for( int k = 0; k < 4; ++k ) {
			out << p.pts[k].x << ' ' << p.pts[k].y << ' ' << p.pts[k].z << "\n";
		}
	}
	return path;
}

static RayIntersection Cast( const IObject& o, const Point3& origin, const Vector3& dir )
{
	RayIntersection hit( Ray( origin, dir ), nullRasterizerState );
	o.IntersectRay( hit, RISE_INFINITY, true, true, false );
	return hit;
}

static bool Shadow( const IObject& o, const Point3& origin, const Vector3& dir, Scalar far )
{
	return o.IntersectRay_IntersectionOnly( Ray( origin, dir ), far, true, true );
}

//! A record whose normals agree with each other and carry no stale
//! "flipped toward the ray" flag.
static bool Coherent( const RayIntersection& h )
{
	return !h.geometric.bGeomNormalOrientedToRay &&
		Vector3Ops::Dot( h.geometric.vGeomNormal, h.geometric.vNormal ) > 0;
}

//! The fixture library's closed double-sided mesh cube under a new
//! geometry name, optionally inward-wound and/or smooth-shaded.
static std::string NamedMeshCube( const std::string& name, bool reverse, bool faceNormals )
{
	std::string m = Mesh( true, reverse );
	const std::string nameTag = " name shape\n";
	m.replace( m.find( nameTag ), nameTag.size(), " name " + name + "\n" );
	if( !faceNormals ) {
		const std::string tag = " face_normals TRUE\n";
		m.replace( m.find( tag ), tag.size(), " face_normals FALSE\n" );
	}
	return m;
}

//! Normal derivatives paired with the reported shading normal: on a
//! convex (smooth-shaded) body the OUTWARD normal field diverges, so
//! dn/du . dp/du + dn/dv . dp/dv > 0 exactly when the derivatives belong
//! to the outward field.
static Scalar NormalFieldDivergence( const RayIntersection& h )
{
	const SurfaceDerivativesInfo& d = h.geometric.derivatives;
	return Vector3Ops::Dot( d.dndu, d.dpdu ) + Vector3Ops::Dot( d.dndv, d.dpdv );
}

//! Derivatives either withheld or describing a right-handed
//! (dpdu, dpdv, n) frame with the reported shading normal.
static bool FrameHonest( const RayIntersection& h )
{
	const SurfaceDerivativesInfo& d = h.geometric.derivatives;
	return !d.valid || Vector3Ops::Dot( Vector3Ops::Cross( d.dpdu, d.dpdv ), h.geometric.vNormal ) > 0;
}

static const CSGObject* AsCSG( const IObject* o )
{
	return dynamic_cast<const CSGObject*>( o );
}

static void TestOpenSheetCSG()
{
	std::cout << "E/F. open-sheet CSG against patch solids" << std::endl;
	const std::string spherePath = WriteBezier( "dl616_cubesphere.bezier", MakeCubeSphere( 1, true ) );
	const std::string framePath = WriteBilinear( "dl616_frame.bilinear", MakeBilinearFrame() );
	const std::string tcubePath = WriteBilinear( "dl616_tcube.bilinear", MakeTJunctionCube() );
	const std::string ballInPath = WriteBezier( "dl616_ballin.bezier", MakeCubeSphere( 1, false ) );
	const std::string bcubePath = WriteBilinear( "dl616_bcube.bilinear", MakeBilinearCube() );
	const std::string sheetZ =
		"clippedplane_geometry\n{\n name sheet_z\n pta -3 -3 0\n ptb 3 -3 0\n ptc 3 3 0\n ptd -3 3 0\n}\n";
	const std::string sheetX =
		"clippedplane_geometry\n{\n name sheet_x\n pta -1.5 -3 -3\n ptb -1.5 3 -3\n ptc -1.5 3 3\n ptd -1.5 -3 3\n}\n";
	auto sheetAt = [&]( const std::string& name, Scalar z ) {
		const std::string zs = std::to_string( z );
		return "clippedplane_geometry\n{\n name " + name + "\n pta -3 -3 " + zs + "\n ptb 3 -3 " + zs +
			"\n ptc 3 3 " + zs + "\n ptd -3 3 " + zs + "\n}\n";
	};
	std::string scene = Materials( false ) + sheetZ + sheetX +
		sheetAt( "sheet_m12", -1.2 ) + sheetAt( "sheet_m17", -1.7 ) +
		"bezierpatch_geometry\n{\n name ball\n file " + spherePath + "\n}\n"
		"bilinearpatch_geometry\n{\n name frame\n file " + framePath + "\n}\n"
		"bezierpatch_geometry\n{\n name ball_edit\n file " + spherePath + "\n}\n"
		"bilinearpatch_geometry\n{\n name tcube\n file " + tcubePath + "\n}\n"
		"bezierpatch_geometry\n{\n name ballin\n file " + ballInPath + "\n}\n"
		"clippedplane_geometry\n{\n name sheet_half\n pta -1 -1 0.5\n ptb 2 -1 0.5\n ptc 2 2 0.5\n ptd -1 2 0.5\n}\n"
		"sphere_geometry\n{\n name sph\n radius 0.5\n}\n";
	auto obj = [&]( const std::string& name, const std::string& geom, const std::string& extra = "" ) {
		scene += "standard_object\n{\n name " + name + "\n geometry " + geom + "\n material glass\n" + extra + "}\n";
	};
	auto csg = [&]( const std::string& name, const std::string& a, const std::string& b, const std::string& op, const std::string& extra = "" ) {
		scene += "csg_object\n{\n name " + name + "\n obja " + a + "\n objb " + b + "\n operation " + op + "\n material glass\n" + extra + "}\n";
	};
	obj( "s1", "sheet_z" ); obj( "b1", "ball" ); csg( "inter_ball", "s1", "b1", "intersection" );
	obj( "s2", "sheet_z" ); obj( "b2", "ball" ); csg( "minus_ball", "s2", "b2", "subtraction" );
	obj( "b3", "ball" ); obj( "s3", "sheet_z" ); csg( "ball_inter", "b3", "s3", "intersection" );
	obj( "s4", "sheet_x" ); obj( "f4", "frame" ); csg( "inter_frame", "s4", "f4", "intersection" );
	obj( "s5", "sheet_x" ); obj( "f5", "frame" ); csg( "minus_frame", "s5", "f5", "subtraction" );
	obj( "s6", "sheet_z" ); obj( "b6", "ball" ); csg( "moved", "s6", "b6", "intersection", " position 0 0 1\n orientation 20 0 0\n" );
	// F: union of two disjoint certified sheets.
	obj( "u1", "sheet_z" ); obj( "u2", "sheet_z", " position 0 0 5\n" ); csg( "sheet_union", "u1", "u2", "union" );
	// F: nested -- (sheet minus small sphere) intersect ball.
	obj( "n1", "sheet_z" ); obj( "n2", "sph" ); csg( "holed", "n1", "n2", "subtraction" );
	obj( "n3", "ball" ); csg( "nested", "holed", "n3", "intersection" );
	// F: nested closed operand -- sheet minus (ball union small sphere).
	obj( "c1", "ball" ); obj( "c2", "sph", " position 2 0 0\n" ); csg( "solids", "c1", "c2", "union" );
	obj( "c3", "sheet_z" ); csg( "minus_solids", "c3", "solids", "subtraction" );
	// P3-e: a composite whose patch operand is edited after preparation.
	obj( "es", "sheet_z" ); obj( "eb", "ball_edit" ); csg( "epoch_probe", "es", "eb", "subtraction" );
	// Round 3 P2-1: secondary rays leaving a certified-closed patch
	// operand.  A 2-unit box MINUS the Bezier ball centred on its top face
	// leaves a hemispherical dimple.
	scene += "box_geometry\n{\n name cube2\n width 2\n height 2\n depth 2\n}\n";
	obj( "da", "cube2" ); obj( "db", "ball", " position 0 0 1\n" ); csg( "dimple", "da", "db", "subtraction" );
	// Final review P1: a union entered from inside the analytic sphere
	// whose exit is the completed exit of an overlapping Bezier ball.
	obj( "ua", "sph" ); obj( "ub", "ball", " position 1.2 0 0\n" ); csg( "union_exit", "ua", "ub", "union" );
	// The ray origin INSIDE the Bezier ball (the contained-origin path).
	obj( "ui", "ball" ); obj( "uj", "sph", " position 5 0 0\n" ); csg( "union_in", "ui", "uj", "union" );
	// ... and a bilinear (unflipped-normal) operand: box minus a unit
	// bilinear cube poking out of its top face leaves a square pit.
	scene += "bilinearpatch_geometry\n{\n name bcube\n file " + bcubePath + "\n}\n";
	obj( "pa", "cube2" ); obj( "pb", "bcube", " position -0.5 -0.5 0.5\n" ); csg( "pit", "pa", "pb", "subtraction" );
	// Review P2: a double-sided watertight indexed MESH cube poking out
	// of the box's top face (the DL-411 signed-distance path).
	scene += Mesh( true, false );
	obj( "ma", "cube2" ); obj( "mb", "shape", " position 0 0 1\n scale 0.5 0.5 0.5\n" ); csg( "mesh_pit", "ma", "mb", "subtraction" );
	// Review round 7 P2-2: an INWARD-wound watertight double-sided mesh
	// (certified: manifold edges, not winding) poking out of the top face.
	scene += NamedMeshCube( "shape_in", true, true );
	obj( "ia", "cube2" ); obj( "ib", "shape_in", " position 0 0 1\n scale 0.5 0.5 0.5\n" ); csg( "mesh_pit_in", "ia", "ib", "subtraction" );
	// Review round 7 P2-1: a smooth-shaded (interpolated-normal) mesh cube
	// exited from inside, intersected with a huge sphere.
	scene += NamedMeshCube( "shape_smooth", false, false );
	scene += "sphere_geometry\n{\n name huge2\n radius 100000\n}\n";
	obj( "sa", "shape_smooth" ); obj( "sz", "huge2" ); csg( "smooth_lens", "sa", "sz", "intersection" );
	// Review of 8309af4f4 (P3-2): the inward-wound mesh exited from inside.
	obj( "wa", "shape_in" ); obj( "wz", "huge2" ); csg( "inward_lens", "wa", "wz", "intersection" );
	// Review of 4f3183984 (P2): the same two meshes under a MIRRORING
	// operand transform (negative determinant).
	obj( "ma2", "shape_smooth", " scale -1 1 1\n" ); obj( "mz2", "huge2" ); csg( "mirror_lens", "ma2", "mz2", "intersection" );
	obj( "mi2", "shape_in", " scale -1 1 1\n" ); obj( "mj2", "huge2" ); csg( "mirror_in_lens", "mi2", "mj2", "intersection" );
	// Review P3-2: a genuine grazing exit through the bilinear cube's top.
	obj( "ga", "cube2" ); obj( "gb", "bcube" ); csg( "graze_lens", "ga", "gb", "intersection" );
	// Review P3-2: a Bezier operand scaled 1000x (back-off in its frame).
	scene += "sphere_geometry\n{\n name huge\n radius 100000\n}\n";
	obj( "sb", "ball", " scale 1000 1000 1000\n" ); obj( "sh", "huge" ); csg( "scaled_lens", "sb", "sh", "intersection" );
	// Review P3-3: box MINUS sphere entered from inside both, nested in a
	// union with a slightly larger sphere: the outer exit adopts the inner
	// record's exit normal (the box's).
	scene += "sphere_geometry\n{\n name sph07\n radius 0.7\n}\n";
	obj( "na", "cube2" ); obj( "nb", "sph" ); csg( "box_minus_sph", "na", "nb", "subtraction" );
	obj( "nc", "sph07" ); csg( "nested_exit", "box_minus_sph", "nc", "union" );
	// P3-b: the same trimming geometry when the solid is closed but NOT
	// certified (the T-junction cube).
	// The uncertified closed trimmer is the INWARD-wound cube-sphere: closed
	// by topology, refused by the certifier (negative volume).
	obj( "ts1", "sheet_half" ); obj( "tc1", "ballin" ); csg( "inter_tcube", "ts1", "tc1", "intersection" );
	obj( "ts2", "sheet_half" ); obj( "tc2", "ballin" ); csg( "minus_tcube", "ts2", "tc2", "subtraction" );
	obj( "ts3", "sheet_half" ); obj( "tc3", "ball" ); csg( "inter_ballcert", "ts3", "tc3", "intersection" );
	// Round 3 P3-1: the T-junction cube has free sides by exact topology,
	// so it is OPEN and must never act as a trimmer.
	obj( "ts4", "sheet_half" ); obj( "tc4", "tcube" ); csg( "inter_open", "ts4", "tc4", "intersection" );
	// P2-1: sheet minus (Bezier ball UNION a sphere overlapping the ball's
	// exit along the ray).  A ray down x = 0.3 enters the ball at z ~ 0.95,
	// leaves it at z ~ -0.95 inside the sphere (centre z = -1, r = 0.5) and
	// leaves the union at z ~ -1.40.
	const char* ovlSheets[3] = { "sheet_z", "sheet_m12", "sheet_m17" };
	for( int k = 0; k < 3; ++k ) {
		const std::string id = std::to_string( k );
		obj( "ob" + id, "ball" ); obj( "os" + id, "sph", " position 0 0 -1\n" );
		csg( "ovl" + id, "ob" + id, "os" + id, "union" );
		obj( "oq" + id, ovlSheets[k] ); csg( "minus_ovl" + id, "oq" + id, "ovl" + id, "subtraction" );
	}

	LoadedScene loaded( scene );
	const Vector3 down( 0, 0, -1 ), west( -1, 0, 0 );

	const IObject* interBall = loaded.Object( "inter_ball" );
	const IObject* minusBall = loaded.Object( "minus_ball" );
	const IObject* ballInter = loaded.Object( "ball_inter" );
	const IObject* interFrame = loaded.Object( "inter_frame" );
	const IObject* minusFrame = loaded.Object( "minus_frame" );
	const IObject* moved = loaded.Object( "moved" );
	const IObject* sheetUnion = loaded.Object( "sheet_union" );
	const IObject* nested = loaded.Object( "nested" );
	const IObject* minusSolids = loaded.Object( "minus_solids" );
	Check( interBall && minusBall && ballInter && interFrame && minusFrame && moved && sheetUnion && nested && minusSolids,
		"E: CSG fixtures load" );
	if( !( interBall && minusBall && ballInter && interFrame && minusFrame && moved && sheetUnion && nested && minusSolids ) ) {
		return;
	}
	{	// Every later fixture is read behind an `if( ptr )` guard, so a parse
		// failure must fail here, loudly, instead of silently skipping rows.
		const char* const guarded[] = { "epoch_probe", "dimple", "union_exit", "union_in", "pit", "mesh_pit",
			"mesh_pit_in", "smooth_lens", "inward_lens", "mirror_lens", "mirror_in_lens", "graze_lens",
			"scaled_lens", "nested_exit", "inter_tcube", "minus_tcube", "inter_ballcert", "inter_open",
			"minus_ovl0", "minus_ovl1", "minus_ovl2" };
		bool all = true;
		for( const char* name : guarded ) {
			if( !loaded.Object( name ) ) {
				std::cout << "    missing fixture " << name << std::endl;
				all = false;
			}
		}
		Check( all, "E: every guarded CSG fixture loads" );
	}
	Check( AsCSG( interBall )->IsProvablyOpenSheet() && AsCSG( minusBall )->IsProvablyOpenSheet() &&
		AsCSG( ballInter )->IsProvablyOpenSheet(), "E: sheet ∩/− a certified-closed Bezier solid is a certified open sheet" );
	Check( AsCSG( interFrame )->IsProvablyOpenSheet(), "E: sheet ∩ a certified-closed genus-1 bilinear solid is a certified open sheet" );

	{	// Convex Bezier solid.
		const RayIntersection in = Cast( *interBall, Point3( 0.3, 0, 2 ), down );
		Check( in.geometric.bHit && std::fabs( in.geometric.range - 2 ) < 1e-6 && in.geometric.range2 == RISE_INFINITY,
			"E: sheet ∩ Bezier solid keeps the sheet inside the solid (zero-thickness hit)" );
		Check( !Cast( *interBall, Point3( 1.5, 0, 2 ), down ).geometric.bHit,
			"E: sheet ∩ Bezier solid drops the sheet outside the solid" );
		const RayIntersection inB = Cast( *ballInter, Point3( 0.3, 0, 2 ), down );
		Check( inB.geometric.bHit && std::fabs( inB.geometric.range - 2 ) < 1e-6,
			"E: Bezier solid ∩ sheet (operand order swapped) keeps the inside part" );
		Check( !Cast( *minusBall, Point3( 0.3, 0, 2 ), down ).geometric.bHit,
			"E: sheet − Bezier solid removes the sheet inside the solid" );
		const RayIntersection out = Cast( *minusBall, Point3( 1.5, 0, 2 ), down );
		Check( out.geometric.bHit && std::fabs( out.geometric.range - 2 ) < 1e-6 && out.geometric.range2 == RISE_INFINITY,
			"E: sheet − Bezier solid keeps the sheet outside the solid" );
		Check( Shadow( *interBall, Point3( 0.3, 0, 2 ), down, 10 ) && !Shadow( *interBall, Point3( 0.3, 0, 2 ), down, 1.5 ) &&
			!Shadow( *interBall, Point3( 1.5, 0, 2 ), down, 10 ),
			"E: shadow rays agree with sheet ∩ Bezier solid" );
		Check( Shadow( *minusBall, Point3( 1.5, 0, 2 ), down, 10 ) && !Shadow( *minusBall, Point3( 0.3, 0, 2 ), down, 10 ),
			"E: shadow rays agree with sheet − Bezier solid" );
	}
	{	// Genus-1 frame: the ray from x = 5 crosses the frame on [1, 2],
		// passes the hole, and meets the sheet x = -1.5 inside the SECOND
		// interval [-2, -1].
		const RayIntersection in = Cast( *interFrame, Point3( 5, 0, 0 ), west );
		Check( in.geometric.bHit && std::fabs( in.geometric.range - 6.5 ) < 1e-6,
			"E: sheet ∩ frame keeps a sheet hit lying in the solid's second interval" );
		Check( !Cast( *minusFrame, Point3( 5, 0, 0 ), west ).geometric.bHit,
			"E: sheet − frame removes a sheet hit lying in the solid's second interval" );
		Check( Shadow( *interFrame, Point3( 5, 0, 0 ), west, 10 ) && !Shadow( *minusFrame, Point3( 5, 0, 0 ), west, 10 ),
			"E: shadow rays agree for the second-interval sheet hit" );
		const RayIntersection hole = Cast( *minusFrame, Point3( 5, 0, 1 ), west );
		Check( hole.geometric.bHit && std::fabs( hole.geometric.range - 6.5 ) < 1e-6,
			"E: sheet − frame keeps the sheet above the frame" );
	}
	{	// Transformed composite.
		const RayIntersection t = Cast( *moved, Point3( 0.3, 0, 3 ), down );
		Check( t.geometric.bHit && t.geometric.range2 == RISE_INFINITY && std::fabs( t.geometric.ptIntersection.z - 1 ) < 1e-6,
			"E: a transformed open-sheet composite keeps range2 == RISE_INFINITY and lands on its sheet" );
		Check( std::isfinite( t.geometric.ptExit.x ) && std::isfinite( t.geometric.ptExit.z ),
			"E: a transformed open-sheet composite reports a finite ptExit" );
	}
	{	// Round 3 P2-1: the inside test from a point within the self-hit
		// floor of a certified-closed patch surface.  Points just off the
		// frame's inner wall x = 1, in the hole (truly OUTSIDE the frame),
		// probed along many directions -- some grazing, some across the
		// hole: every answer must be "outside".  Directions are nudged off
		// the frame's edges: a ray through an edge exactly is a degenerate
		// double contact that no first-hit or parity rule settles.
		const IObject* interFrameObj = loaded.Object( "inter_frame" );
		const IObjectPriv* frameLeaf = interFrameObj ? AsCSG( interFrameObj )->GetOperandB() : nullptr;
		Check( frameLeaf != nullptr, "R3: frame leaf operand available" );
		if( frameLeaf ) {
			int wrong = 0, total = 0;
			for( const Scalar off : { 1e-13, 1e-12 } ) {
				const Point3 p( 1 - off, 0.1, 0.05 );
				for( int a = -3; a <= 3; ++a ) {
					for( int b = -3; b <= 3; ++b ) {
						const Vector3 d = Vector3Ops::Normalize( Vector3( -1, 0.3 * a + 0.0137, 0.3 * b + 0.0071 ) );
						++total;
						if( CSGObject::OperandContainsPoint( frameLeaf, p, d, 5 ) != -1 ) ++wrong;
						// Grazing along the wall, leaving it.
						const Vector3 g = Vector3Ops::Normalize( Vector3( -0.05, 0.3 * a + 0.01, 0.3 * b + 0.01 ) );
						++total;
						if( CSGObject::OperandContainsPoint( frameLeaf, p, g, 5 ) != -1 ) ++wrong;
					}
				}
			}
			std::cout << "    inside test just off a patch surface: " << wrong << " of " << total << " wrong" << std::endl;
			Check( wrong == 0, "R3: a point within a patch surface's self-hit floor reads the side its ray leaves into" );
		}
	}
	{	// Round 3 P2-1: secondary rays from a surface point of a patch operand.
		const IObject* dimple = loaded.Object( "dimple" );
		Check( dimple != nullptr, "R3: dimple composite loads" );
		if( dimple ) {
			// Land on the dimple floor (the ball's surface inside the box);
			// the hit point is backed off a hair into the cavity, i.e. it
			// lies within the ball's self-hit floor of its surface.
			const RayIntersection floorHit = Cast( *dimple, Point3( 0.2, 0, 3 ), down );
			Check( floorHit.geometric.bHit, "R3: camera ray lands on the dimple floor" );
			Check( floorHit.geometric.bHit && Vector3Ops::Dot( floorHit.geometric.vNormal, down ) < 0 &&
				Vector3Ops::Dot( floorHit.geometric.UnflippedGeomNormal(), Vector3( 0, 0, 1 ) ) > 0 && Coherent( floorHit ),
				"P1: the dimple floor's shading normal faces the camera and its true outward normal points up" );
			const Point3 o = floorHit.geometric.ptIntersection;
			int phantomHits = 0, phantomShadows = 0, total = 0;
			for( int k = 0; k < 16; ++k ) {
				// Up and out through the open top at assorted azimuths and
				// grazing elevations: nothing of the composite is in the way.
				const Scalar az = Scalar( k ) * 3.14159265358979323846 / 8;
				const Scalar el = Scalar( 1.0 ) + Scalar( 0.1 ) * Scalar( k % 4 );
				const Vector3 d = Vector3Ops::Normalize( Vector3( std::cos( az ) * std::cos( el ), std::sin( az ) * std::cos( el ), std::sin( el ) ) );
				// Keep only directions that really leave through the opening.
				const Scalar tx = o.x + d.x * ( 1 - o.z ) / d.z, ty = o.y + d.y * ( 1 - o.z ) / d.z;
				if( tx * tx + ty * ty > 0.9 ) continue;
				++total;
				if( Cast( *dimple, o, d ).geometric.bHit ) ++phantomHits;
				if( Shadow( *dimple, o, d, 10 ) ) ++phantomShadows;
			}
			std::cout << "    dimple rays leaving through the opening: " << total << " cast, "
				<< phantomHits << " phantom hits, " << phantomShadows << " phantom occluders" << std::endl;
			Check( total >= 4 && phantomHits == 0 && phantomShadows == 0,
				"R3: reflection / shadow rays leaving a patch operand's surface see no phantom geometry" );
			// And a grazing ray across the dimple that DOES meet its far wall.
			const Vector3 across = Vector3Ops::Normalize( Vector3( -0.99, 0, 0.14 ) );
			const RayIntersection wall = Cast( *dimple, o, across );
			std::cout << "    grazing ray across the dimple: hit " << wall.geometric.bHit << " at ("
				<< wall.geometric.ptIntersection.x << ", " << wall.geometric.ptIntersection.y << ", "
				<< wall.geometric.ptIntersection.z << ")" << std::endl;
			Check( wall.geometric.bHit && wall.geometric.ptIntersection.x < -0.3 && wall.geometric.ptIntersection.z < 0.5,
				"R3: a grazing ray across the dimple meets the far dimple wall" );
			// That ray starts inside the box AND the ball; the far wall is
			// where it enters the composite's material, so both the shading
			// normal and the true outward normal must face back along it.
			Check( wall.geometric.bHit && Vector3Ops::Dot( wall.geometric.vNormal, across ) < 0 &&
				Vector3Ops::Dot( wall.geometric.UnflippedGeomNormal(), across ) < 0 && Coherent( wall ),
				"P1: the far bowl wall seen from inside the ball faces the ray (shading and true normal)" );
			// Camera inside the ball, above the box, looking down: the ray
			// leaves the ball at the dimple floor inside the box.
			const RayIntersection fromBall = Cast( *dimple, Point3( 0.1, 0, 1.5 ), down );
			Check( fromBall.geometric.bHit && fromBall.geometric.ptIntersection.z < 0.2 &&
				Vector3Ops::Dot( fromBall.geometric.vNormal, down ) < 0 &&
				Vector3Ops::Dot( fromBall.geometric.UnflippedGeomNormal(), down ) < 0 && Coherent( fromBall ),
				"P1: a camera inside the ball sees the dimple floor facing it (shading and true normal)" );
		}
	}
	{	// Final review P1: the completed exit's normal is OUTWARD.
		const IObject* ue = loaded.Object( "union_exit" );
		Check( ue != nullptr, "P1: union composite loads" );
		if( ue ) {
			const Vector3 east( 1, 0, 0 );
			const RayIntersection h = Cast( *ue, Point3( 0, 0.05, 0.03 ), east );
			std::cout << "    union exit from inside: hit " << h.geometric.bHit << " x " << h.geometric.ptIntersection.x
				<< " n.d " << Vector3Ops::Dot( h.geometric.vNormal, east )
				<< " trueN.d " << Vector3Ops::Dot( h.geometric.UnflippedGeomNormal(), east ) << std::endl;
			const IObject* pit = loaded.Object( "pit" );
			const RayIntersection pf = pit ? Cast( *pit, Point3( 0.1, 0.2, 3 ), Vector3( 0, 0, -1 ) ) : RayIntersection( Ray(), nullRasterizerState );
			Check( pit && pf.geometric.bHit && std::fabs( pf.geometric.ptIntersection.z - 0.5 ) < 1e-6 &&
				Vector3Ops::Dot( pf.geometric.vNormal, Vector3( 0, 0, 1 ) ) > 0 &&
				Vector3Ops::Dot( pf.geometric.UnflippedGeomNormal(), Vector3( 0, 0, 1 ) ) > 0 && Coherent( pf ),
				"P1: a box minus a bilinear cube shows its pit floor with an upward normal" );
			Check( h.geometric.bHit && h.geometric.ptIntersection.x > 2.0 &&
				Vector3Ops::Dot( h.geometric.vNormal, east ) > 0 &&
				Vector3Ops::Dot( h.geometric.UnflippedGeomNormal(), east ) > 0 && Coherent( h ),
				"P1: a union exit through a Bezier operand reports the OUTWARD normal (shading and true)" );
			const IObject* ui = loaded.Object( "union_in" );
			const RayIntersection hi = ui ? Cast( *ui, Point3( 0, 0.05, 0.03 ), east ) : RayIntersection( Ray(), nullRasterizerState );
			Check( ui && hi.geometric.bHit && hi.geometric.ptIntersection.x > 0.7 &&
				Vector3Ops::Dot( hi.geometric.vNormal, east ) > 0 &&
				Vector3Ops::Dot( hi.geometric.UnflippedGeomNormal(), east ) > 0 && Coherent( hi ),
				"P1: a union exited from INSIDE its Bezier operand reports the OUTWARD normal (shading and true)" );
		}
	}
	{	// Review round 6 rows.
		const IObject* mp = loaded.Object( "mesh_pit" );
		const IObject* mbLeaf = mp ? AsCSG( mp )->GetOperandB() : nullptr;
		const auto* meshG = mbLeaf ? dynamic_cast<const TriangleMeshGeometryIndexed*>( mbLeaf->GetGeometry() ) : nullptr;
		Check( meshG && meshG->IsCertifiedWatertight(), "P2: the double-sided mesh cube is certified watertight" );
		if( mp ) {
			const RayIntersection f = Cast( *mp, Point3( 0.1, 0.1, 0.8 ), down );
			Check( f.geometric.bHit && std::fabs( f.geometric.ptIntersection.z - 0.5 ) < 1e-6 &&
				Vector3Ops::Dot( f.geometric.vNormal, down ) < 0 &&
				Vector3Ops::Dot( f.geometric.UnflippedGeomNormal(), down ) < 0 && Coherent( f ),
				"P2: box minus a double-sided watertight mesh, seen from inside both, faces the ray" );
		}
		const IObject* mpi = loaded.Object( "mesh_pit_in" );
		const IObject* mbi = mpi ? AsCSG( mpi )->GetOperandB() : nullptr;
		const auto* meshIn = mbi ? dynamic_cast<const TriangleMeshGeometryIndexed*>( mbi->GetGeometry() ) : nullptr;
		Check( meshIn && meshIn->IsCertifiedWatertight(), "P2-2: the inward-wound double-sided mesh cube is certified watertight" );
		if( mpi ) {
			const RayIntersection f = Cast( *mpi, Point3( 0.1, 0.1, 0.8 ), down );
			Check( f.geometric.bHit && std::fabs( f.geometric.ptIntersection.z - 0.5 ) < 1e-6 &&
				Vector3Ops::Dot( f.geometric.vNormal, down ) < 0 &&
				Vector3Ops::Dot( f.geometric.UnflippedGeomNormal(), down ) < 0 && Coherent( f ),
				"P2-2: box minus an INWARD-wound watertight mesh, seen from inside both, faces the ray" );
		}
		const IObject* sml = loaded.Object( "smooth_lens" );
		if( sml ) {
			const RayIntersection e = Cast( *sml, Point3( 0.1, 0.1, 0.2 ), down );
			std::cout << "    smooth exit: derivatives " << e.geometric.derivatives.valid
				<< " divergence " << NormalFieldDivergence( e ) << std::endl;
			Check( e.geometric.bHit && std::fabs( e.geometric.ptIntersection.z + 1 ) < 1e-6 &&
				Vector3Ops::Dot( e.geometric.vGeomNormal, down ) > 0 &&
				Vector3Ops::Dot( e.geometric.vNormal, down ) > 0 && Coherent( e ),
				"P2-1: a smooth double-sided mesh exited from inside reports the OUTWARD normals" );
			Check( e.geometric.derivatives.valid && NormalFieldDivergence( e ) > 0,
				"P2-1: its normal derivatives are re-paired with the outward shading normal" );
			Check( FrameHonest( e ), "P3-2: the smooth exit's derivative frame is right-handed with its normal" );
			// Grazing exit near the +x edge of the bottom face: the outward
			// GEOMETRIC normal is along the ray (the mesh flipped it) while
			// the outward interpolated SHADING normal, tilted toward +x, is
			// against it (the mesh did not flip it).
			const Vector3 g = Vector3Ops::Normalize( Vector3( -1, 0, -0.01 ) );
			const RayIntersection ge = Cast( *sml, Point3( 0.9, 0, -0.995 ), g );
			std::cout << "    grazing smooth exit: x " << ge.geometric.ptIntersection.x
				<< " shade.dir " << Vector3Ops::Dot( ge.geometric.vNormal, g ) << std::endl;
			Check( ge.geometric.bHit && std::fabs( ge.geometric.ptIntersection.z + 1 ) < 1e-6 &&
				Vector3Ops::Dot( ge.geometric.vGeomNormal, g ) > 0 &&
				Vector3Ops::Dot( ge.geometric.vNormal, Vector3( 0, 0, -1 ) ) > 0 && Coherent( ge ),
				"P2-1: a GRAZING smooth exit keeps both normals outward, in one hemisphere" );
			Check( ge.geometric.derivatives.valid && NormalFieldDivergence( ge ) > 0,
				"P2-1: the grazing exit's normal derivatives pair with the outward shading normal" );
		}
		const IObject* iwl = loaded.Object( "inward_lens" );
		if( iwl ) {
			const RayIntersection w = Cast( *iwl, Point3( 0.1, 0.1, 0.2 ), down );
			std::cout << "    inward-wound exit: derivatives " << w.geometric.derivatives.valid << std::endl;
			Check( w.geometric.bHit && std::fabs( w.geometric.ptIntersection.z + 1 ) < 1e-6 &&
				Vector3Ops::Dot( w.geometric.vGeomNormal, down ) > 0 && Coherent( w ) && FrameHonest( w ),
				"P3-2: an inward-wound mesh exit is outward and never publishes a mirrored derivative frame" );
		}
		const IObject* mlz = loaded.Object( "mirror_lens" );
		if( mlz ) {
			const RayIntersection m = Cast( *mlz, Point3( 0.1, 0.1, 0.2 ), down );
			std::cout << "    mirrored smooth exit: derivatives " << m.geometric.derivatives.valid
				<< " divergence " << NormalFieldDivergence( m ) << std::endl;
			Check( m.geometric.bHit && std::fabs( m.geometric.ptIntersection.z + 1 ) < 1e-6 &&
				Vector3Ops::Dot( m.geometric.vGeomNormal, down ) > 0 && Coherent( m ) &&
				m.geometric.derivatives.valid && NormalFieldDivergence( m ) > 0,
				"P2: a MIRRORED outward-wound mesh exit keeps its (object-space right-handed) derivatives" );
		}
		const IObject* mil = loaded.Object( "mirror_in_lens" );
		if( mil ) {
			const RayIntersection m = Cast( *mil, Point3( 0.1, 0.1, 0.2 ), down );
			std::cout << "    mirrored inward-wound exit: derivatives " << m.geometric.derivatives.valid << std::endl;
			Check( m.geometric.bHit && std::fabs( m.geometric.ptIntersection.z + 1 ) < 1e-6 &&
				Vector3Ops::Dot( m.geometric.vGeomNormal, down ) > 0 && Coherent( m ) &&
				!m.geometric.derivatives.valid,
				"P2: a MIRRORED inward-wound mesh exit withholds its mirrored derivative frame" );
		}
		const IObject* gl = loaded.Object( "graze_lens" );
		if( gl ) {
			const Vector3 d = Vector3Ops::Normalize( Vector3( 1, 0, 5e-4 ) );
			const Point3 o( -0.5, 0.5, 1 - 5e-4 );
			RayIntersection g( Ray( o, d ), nullRasterizerState );
			gl->IntersectRay( g, RISE_INFINITY, true, true, true );
			const Scalar expected = std::sqrt( 1 + 2.5e-7 );
			std::cout << "    grazing exit: hit " << g.geometric.bHit << " range " << g.geometric.range
				<< " range2 " << g.geometric.range2 << " (exit at " << expected << ")" << std::endl;
			Check( g.geometric.bHit && std::fabs( g.geometric.range2 - expected ) < 1e-6,
				"P3-2: a genuine grazing exit (cos 5e-4) completes the bilinear operand's interval" );
		}
		const IObject* sl = loaded.Object( "scaled_lens" );
		if( sl ) {
			const IObjectPriv* ballLeaf = AsCSG( sl )->GetOperandA();
			const Point3 o( -5000, 30, 20 );
			const Vector3 east( 1, 0, 0 );
			RayIntersection g( Ray( o, east ), nullRasterizerState );
			sl->IntersectRay( g, RISE_INFINITY, true, true, true );
			// The exit face, approached from beyond it: Object::IntersectRay
			// publishes that point a hair outside the face, i.e. a hair PAST
			// the exit along `east` -- where a completed range2 must land.
			RayIntersection back( Ray( Point3( 5000, 30, 20 ), Vector3( -1, 0, 0 ) ), nullRasterizerState );
			ballLeaf->IntersectRay( back, RISE_INFINITY, true, true, false );
			const Scalar past = back.geometric.ptIntersection.x - o.x;
			std::cout.precision( 15 );
			std::cout << "    scaled operand: range2 " << g.geometric.range2 << " vs exit-from-beyond " << past
				<< " diff " << ( g.geometric.range2 - past ) << std::endl;
			std::cout.precision( 6 );
			Check( g.geometric.bHit && back.geometric.bHit && std::fabs( g.geometric.range2 - past ) < 5e-10,
				"P3-2: a 1000x scaled operand's completed range2 lands just past its exit face" );
		}
		const IObject* ne = loaded.Object( "nested_exit" );
		if( ne ) {
			const Vector3 east( 1, 0, 0 );
			const RayIntersection n = Cast( *ne, Point3( 0, 0.01, 0.02 ), east );
			std::cout << "    nested exit: hit " << n.geometric.bHit << " x " << n.geometric.ptIntersection.x
				<< " n.x " << n.geometric.vNormal.x << " g.x " << n.geometric.vGeomNormal.x << std::endl;
			Check( n.geometric.bHit && std::fabs( n.geometric.ptIntersection.x - 1 ) < 1e-6 &&
				n.geometric.vNormal.x > 0.99 && n.geometric.vGeomNormal.x > 0.99,
				"P3-3: a nested exit through box-minus-sphere adopts the box's outward exit normal" );
		}
	}
	{	// P3-e: AddPatch after preparation stales the composite's cache.
		const IObject* probe = loaded.Object( "epoch_probe" );
		Check( probe && AsCSG( probe )->IsProvablyOpenSheet(), "P3-e: probe composite starts certified open" );
		if( probe ) {
			const IObject* solid = AsCSG( probe )->GetOperandB();
			const BezierPatchGeometry* g = solid ? dynamic_cast<const BezierPatchGeometry*>( solid->GetGeometry() ) : nullptr;
			Check( g != nullptr, "P3-e: probe solid is a Bezier patch set" );
			if( g ) {
				// A stray far-away patch makes the set disconnected: no longer closed.
				BezierPatch stray = MakeCubeSphere( 1, true )[0];
				for( int i = 0; i < 4; ++i ) {
					for( int j = 0; j < 4; ++j ) {
						stray.c[i].pts[j] = Point3( stray.c[i].pts[j].x + 10, stray.c[i].pts[j].y, stray.c[i].pts[j].z );
					}
				}
				const_cast<BezierPatchGeometry*>( g )->AddPatch( stray );
				Check( !g->IsCertifiedWatertight(), "P3-e: the edited patch set is no longer certified closed" );
				Check( !AsCSG( probe )->IsProvablyOpenSheet(),
					"P3-e: the composite's cached certificate follows the operand edit without a re-preparation" );
			}
		}
	}
	{	// P3-b: geometry does not depend on the trimming solid's certificate.
		const IObject* it = loaded.Object( "inter_tcube" );
		const IObject* mt = loaded.Object( "minus_tcube" );
		Check( it && mt && !AsCSG( it )->IsProvablyOpenSheet() && !AsCSG( mt )->IsProvablyOpenSheet(),
			"P3-b: sheet trimmed by an UNCERTIFIED closed solid carries no open-sheet certificate" );
		const IObject* ic = loaded.Object( "inter_ballcert" );
		const IObject* io = loaded.Object( "inter_open" );
		Check( ic && io && AsCSG( ic )->IsProvablyOpenSheet() && AsCSG( ic )->TrimsSheet() && AsCSG( it )->TrimsSheet(),
			"P3-1: trimming roles agree for a certified and an uncertified closed trimmer" );
		Check( io && !AsCSG( io )->TrimsSheet() && !AsCSG( io )->IsProvablyOpenSheet(),
			"P3-1: a closed T-junction body reads as topologically OPEN and never trims a sheet" );
		if( it && mt && ic ) {
			for( const Scalar x : { 0.3, 0.6, 0.85, 1.2 } ) {
				const RayIntersection a = Cast( *it, Point3( x, 0.4, 3 ), down );
				const RayIntersection b = Cast( *ic, Point3( x, 0.4, 3 ), down );
				Check( a.geometric.bHit == b.geometric.bHit && ( !a.geometric.bHit || std::fabs( a.geometric.range - b.geometric.range ) < 1e-9 ),
					"P3-1: the trimmed sheet's shape does not depend on the trimmer's certificate" );
			}
			const RayIntersection in = Cast( *it, Point3( 0.3, 0.4, 3 ), down );
			Check( in.geometric.bHit && std::fabs( in.geometric.range - 2.5 ) < 1e-6 && in.geometric.range2 == RISE_INFINITY &&
				!in.geometric.bProvablyNoInterior,
				"P3-b: sheet INTERSECT uncertified solid renders the zero-thickness trimmed sheet" );
			Check( !Cast( *mt, Point3( 0.3, 0.4, 3 ), down ).geometric.bHit &&
				Cast( *mt, Point3( 1.5, 0.4, 3 ), down ).geometric.bHit,
				"P3-b: sheet MINUS uncertified solid removes only the inside part" );
		}
	}
	{	// P2-1: overlapping solids in the closed operand.
		const IObject* m0 = loaded.Object( "minus_ovl0" );
		const IObject* m1 = loaded.Object( "minus_ovl1" );
		const IObject* m2 = loaded.Object( "minus_ovl2" );
		Check( m0 && m1 && m2 && AsCSG( m0 )->IsProvablyOpenSheet(), "P2-1: sheet minus overlapping solids is certified open" );
		if( m0 && m1 && m2 ) {
			Check( !Cast( *m0, Point3( 0.3, 0, 2 ), down ).geometric.bHit,
				"P2-1: sheet inside the Bezier ball of an overlapping union is removed" );
			Check( !Cast( *m1, Point3( 0.3, 0, 2 ), down ).geometric.bHit,
				"P2-1: sheet inside only the overlapping sphere is removed" );
			const RayIntersection keep = Cast( *m2, Point3( 0.3, 0, 2 ), down );
			Check( keep.geometric.bHit && std::fabs( keep.geometric.range - 3.7 ) < 1e-6,
				"P2-1: sheet below the overlapping union is kept" );
			Check( !Shadow( *m1, Point3( 0.3, 0, 2 ), down, 10 ) && Shadow( *m2, Point3( 0.3, 0, 2 ), down, 10 ),
				"P2-1: shadow rays agree with the overlapping-union trim" );
		}
	}
	{	// F.
		const CSGObject* u = AsCSG( sheetUnion );
		Check( !u->IsProvablyOpenSheet() && !u->IsCertifiedWatertight(),
			"F: union of two disjoint certified sheets is not certified (medium convention undefined)" );
		const CSGObject* nc = AsCSG( nested );
		Check( nc->IsProvablyOpenSheet(), "F: nested (sheet − sphere) ∩ Bezier solid is a certified open sheet" );
		Check( !Cast( *nested, Point3( 0, 0, 2 ), down ).geometric.bHit,
			"F: nested composite removes the inner hole" );
		Check( Cast( *nested, Point3( 0.7, 0, 2 ), down ).geometric.bHit,
			"F: nested composite keeps the ring inside the ball" );
		Check( !Cast( *nested, Point3( 1.5, 0, 2 ), down ).geometric.bHit,
			"F: nested composite drops the sheet outside the ball" );
		const CSGObject* ms = AsCSG( minusSolids );
		Check( ms->IsProvablyOpenSheet(), "F: sheet − (closed ∪ closed) is a certified open sheet" );
		Check( !Cast( *minusSolids, Point3( 2.2, 0, 2 ), down ).geometric.bHit &&
			!Cast( *minusSolids, Point3( 0.3, 0, 2 ), down ).geometric.bHit &&
			Cast( *minusSolids, Point3( -2, 0, 2 ), down ).geometric.bHit,
			"F: sheet − (closed ∪ closed) removes both solids' footprints only" );
	}
	std::remove( spherePath.c_str() );
	std::remove( framePath.c_str() );
	std::remove( tcubePath.c_str() );
	std::remove( ballInPath.c_str() );
	std::remove( bcubePath.c_str() );
}

//////////////////////////////////////////////////////////////////////
// G. cost and shipped models
//////////////////////////////////////////////////////////////////////

static bool ReasonIs( const Verdict& v, const char* expected )
{
	return v.reason && std::strcmp( v.reason, expected ) == 0;
}

static const char* const kCollapsed = "patch with coincident corners (collapsed side or loop)";

static std::vector<BezierPatch> ReadBezierFile( const std::string& path )
{
	std::vector<BezierPatch> out;
	std::FILE* f = std::fopen( path.c_str(), "r" );
	if( !f ) {
		return out;
	}
	unsigned int n = 0;
	if( std::fscanf( f, "%u", &n ) == 1 ) {
		for( unsigned int i = 0; i < n; ++i ) {
			BezierPatch p;
			bool ok = true;
			for( int j = 0; j < 4 && ok; ++j ) {
				for( int k = 0; k < 4 && ok; ++k ) {
					double x, y, z;
					ok = std::fscanf( f, "%lf %lf %lf", &x, &y, &z ) == 3;
					p.c[j].pts[k] = Point3( x, y, z );
				}
			}
			if( !ok ) break;
			out.push_back( p );
		}
	}
	std::fclose( f );
	return out;
}

static std::vector<BilinearPatch> ReadBilinearFile( const std::string& path )
{
	std::vector<BilinearPatch> out;
	std::FILE* f = std::fopen( path.c_str(), "r" );
	if( !f ) {
		return out;
	}
	unsigned int n = 0;
	if( std::fscanf( f, "%u", &n ) == 1 ) {
		for( unsigned int i = 0; i < n; ++i ) {
			BilinearPatch p;
			bool ok = true;
			for( int k = 0; k < 4 && ok; ++k ) {
				double x, y, z;
				ok = std::fscanf( f, "%lf %lf %lf", &x, &y, &z ) == 3;
				p.pts[k] = Point3( x, y, z );
			}
			if( !ok ) break;
			out.push_back( p );
		}
	}
	std::fclose( f );
	return out;
}

//! A watertight double-sided mesh with two disjoint cube lobes, [-1,1]^3
//! and the same cube moved to y in [-5,-3]: closed, but not convex.
static std::string TwoLobeMesh( const std::string& name )
{
	std::ostringstream s;
	s << "indexedmesh_geometry\n{\n name " << name << "\n double_sided TRUE\n face_normals TRUE\n";
	const Point3 vertices[] = { Point3(-1,-1,-1), Point3(1,-1,-1), Point3(1,1,-1), Point3(-1,1,-1),
		Point3(-1,-1,1), Point3(1,-1,1), Point3(1,1,1), Point3(-1,1,1) };
	for( int lobe = 0; lobe < 2; ++lobe ) {
		for( const auto& p : vertices ) {
			s << " vertex " << p.x << ' ' << ( p.y - 4 * lobe ) << ' ' << p.z << '\n';
		}
	}
	for( int i = 0; i < 16; ++i ) {
		s << " uv " << ( i % 2 ) << ' ' << ( ( i / 2 ) % 2 ) << '\n';
	}
	const int triangles[][3] = { {0,2,1},{0,3,2},{4,5,6},{4,6,7},
		{0,1,5},{0,5,4},{3,7,6},{3,6,2},{0,4,7},{0,7,3},{1,2,6},{1,6,5} };
	for( int lobe = 0; lobe < 2; ++lobe ) {
		for( const auto& t : triangles ) {
			s << " triangle " << ( t[0] + 8 * lobe ) << ' ' << ( t[1] + 8 * lobe ) << ' ' << ( t[2] + 8 * lobe ) << '\n';
		}
	}
	return s.str() + "}\n";
}

//! Review of 8309af4f4 (P2): a secondary ray leaving a non-convex
//! watertight mesh operand.  Its origin sits a back-off off the face it
//! just left, so containment must be decided for the segment actually
//! travelled, never at that origin: a reflection leaving the upper
//! lobe's outside toward the lower lobe ENTERS the lower lobe, and its
//! entry normal must survive the contained-origin path.
static void TestNonConvexMeshUnion()
{
	std::cout << "H2. secondary rays leaving a non-convex watertight mesh operand" << std::endl;
	std::string scene = Materials( false ) + TwoLobeMesh( "lobes" ) +
		"sphere_geometry\n{\n name far_sph\n radius 0.5\n}\n" +
		"standard_object\n{\n name la\n geometry lobes\n material glass\n}\n" +
		"standard_object\n{\n name lb\n geometry far_sph\n material glass\n position 20 0 0\n}\n" +
		"csg_object\n{\n name lobe_union\n obja la\n objb lb\n operation union\n material glass\n}\n";
	LoadedScene loaded( scene );
	const IObject* u = loaded.Object( "lobe_union" );
	const IObject* la = u ? AsCSG( u )->GetOperandA() : nullptr;
	const auto* meshG = la ? dynamic_cast<const TriangleMeshGeometryIndexed*>( la->GetGeometry() ) : nullptr;
	Check( meshG && meshG->IsCertifiedWatertight(), "P2: the two-lobe mesh is certified watertight" );
	if( !u ) {
		return;
	}
	const Vector3 up( 0, 1, 0 ), downY( 0, -1, 0 );
	// Reflection: hit the upper lobe's bottom face from outside, then
	// leave from that (backed-off) point back down toward the lower lobe.
	// Several x offsets vary the triangle hit.
	int reflectOk = 0, transmitOk = 0;
	const Scalar xs[] = { 0.1, -0.3, 0.55, -0.7 };
	for( const Scalar x : xs ) {
		const RayIntersection in = Cast( *u, Point3( x, -2, 0.2 ), up );
		const RayIntersection refl = Cast( *u, in.geometric.ptIntersection, downY );
		const bool entry = in.geometric.bHit && refl.geometric.bHit &&
			std::fabs( refl.geometric.ptIntersection.y + 3 ) < 1e-6 &&
			Vector3Ops::Dot( refl.geometric.UnflippedGeomNormal(), downY ) < 0 &&
			Vector3Ops::Dot( refl.geometric.vNormal, downY ) < 0;
		if( entry ) {
			++reflectOk;
		} else {
			std::cout << "    reflection x " << x << ": y " << refl.geometric.ptIntersection.y
				<< " geom.y " << refl.geometric.UnflippedGeomNormal().y << " n.y " << refl.geometric.vNormal.y
				<< " flag " << refl.geometric.bGeomNormalOrientedToRay << std::endl;
		}
		// Transmission: leave the upper lobe through its bottom face from
		// inside and continue down from the (backed-off) exit point.  The
		// first crossing is either that face again (an exit) or the lower
		// lobe's top (an entry); the true facing must say which.
		const RayIntersection out = Cast( *u, Point3( x, 0, 0.2 ), downY );
		const RayIntersection tr = Cast( *u, out.geometric.ptIntersection, downY );
		const Scalar y = tr.geometric.ptIntersection.y;
		const Scalar facing = Vector3Ops::Dot( tr.geometric.UnflippedGeomNormal(), downY );
		const bool ok = out.geometric.bHit && tr.geometric.bHit &&
			( ( std::fabs( y + 3 ) < 1e-6 && facing < 0 ) || ( std::fabs( y + 1 ) < 1e-6 && facing > 0 ) ) &&
			Vector3Ops::Dot( tr.geometric.vNormal, tr.geometric.UnflippedGeomNormal() ) > 0;
		if( ok ) {
			++transmitOk;
		} else {
			std::cout << "    transmission x " << x << ": y " << y << " facing " << facing << std::endl;
		}
	}
	Check( reflectOk == 4, "P2: a reflection leaving one mesh lobe keeps the ENTRY normal of the lobe it enters" );
	Check( transmitOk == 4, "P2: a transmission leaving a mesh lobe reports the true facing of its first crossing" );
}

static void TestCostAndShippedModels()
{
	std::cout << "G. cost and shipped models" << std::endl;
	for( int k : { 6, 13 } ) {
		const std::vector<BezierPatch> big = MakeCubeSphere( k, true );
		const auto t0 = std::chrono::steady_clock::now();
		const Verdict v = CertBezier( big );
		const auto t1 = std::chrono::steady_clock::now();
		const double ms = std::chrono::duration<double, std::milli>( t1 - t0 ).count();
		std::cout << "    cube-sphere k=" << k << " (" << big.size() << " patches): " << Describe( v )
			<< " in " << ms << " ms" << std::endl;
		Check( v.watertight, "G: large closed cube-sphere (" + std::to_string( big.size() ) + " patches) certifies watertight" );
	}
	{	// Worst cases (P2-2): a set past the sub-patch cap is refused before
		// anything large is allocated; a dense embedded set (a tightly
		// wound helix whose turns all share grid cells) is refused when the
		// work budget runs out.  Both report their wall time.
		const std::vector<BezierPatch> huge = MakeCubeSphere( 58, true );
		auto t0 = std::chrono::steady_clock::now();
		const Verdict vh = CertBezier( huge );
		auto t1 = std::chrono::steady_clock::now();
		std::cout << "    cube-sphere k=58 (" << huge.size() << " patches): " << Describe( vh ) << " in "
			<< std::chrono::duration<double, std::milli>( t1 - t0 ).count() << " ms" << std::endl;
		Check( !vh.watertight && !vh.open && ReasonIs( vh, "patch set too large to certify at this sub-patch grid" ),
			"G: a set past the sub-patch cap is refused before allocation" );

		const Scalar pi = 3.14159265358979323846;
		std::vector<BezierPatch> coil;
		const int turns = 600;
		for( int k = 0; k < 4 * turns; ++k ) {
			coil.push_back( AnnulusPatch( 1.0, 2.0, 3 * k, [&]( Scalar r, int aa ) {
				return Scalar( 0.0002 ) * Scalar( aa ) * pi / 6 + Scalar( 0.05 ) * r;
			} ) );
		}
		t0 = std::chrono::steady_clock::now();
		const Verdict vc = CertBezier( coil );
		t1 = std::chrono::steady_clock::now();
		std::cout << "    tight coil (" << coil.size() << " patches, " << turns << " turns): " << Describe( vc ) << " in "
			<< std::chrono::duration<double, std::milli>( t1 - t0 ).count() << " ms" << std::endl;
		Check( !vc.watertight && !vc.open, "G: a dense coil beyond the work budget is refused, never certified" );
	}
	const char* models[] = { "models/raw/teapot.bezier", "models/bezier/aphrodite.bezier",
		"models/bezier/f16.bezier", "models/bezier/balanza.bezier" };
	for( const char* m : models ) {
		const std::vector<BezierPatch> p = ReadBezierFile( m );
		const auto t0 = std::chrono::steady_clock::now();
		const Verdict v = CertBezier( p );
		const auto t1 = std::chrono::steady_clock::now();
		const double ms = std::chrono::duration<double, std::milli>( t1 - t0 ).count();
		std::cout << "    " << m << " (" << p.size() << " patches): " << Describe( v ) << " in " << ms << " ms" << std::endl;
		// teapot.bezier has a non-constant loop side on its lid knob (c[0]==c[3]==(0,3,0) with c[1],c[2] offset by 0.002),
		// so it is still refused at kCollapsed; aphrodite/f16/balanza have genuine constant collapsed poles that now pass
		// FindCollapsedSide and are refused at the inconsistent-seam step.
		const char* expectedReason = ( std::strcmp( m, "models/raw/teapot.bezier" ) == 0 )
			? kCollapsed
			: "sides share endpoints but are not one reversed seam (crack, curve mismatch or inconsistent winding)";
		Check( !p.empty() && !v.watertight && !v.open && ReasonIs( v, expectedReason ),
			std::string( "G: shipped model refused with pinned reason: " ) + m );
	}
	for( const char* m : { "models/raw/teapot.bilinear", "models/raw/test.bilinear" } ) {
		const std::vector<BilinearPatch> p = ReadBilinearFile( m );
		const Verdict v = CertBilinear( p );
		std::cout << "    " << m << " (" << p.size() << " patches): " << Describe( v ) << std::endl;
		const bool single = std::strcmp( m, "models/raw/test.bilinear" ) == 0;
		// teapot.bilinear's 4 polar lid patches have a genuine constant pole at (0,3,0); the full 28-patch file advances
		// past the collapsed-side step and is refused at the 4-way handle/spout seam in z=0.
		Check( !p.empty() && ( single ? ( v.open && v.negative && ReasonIs( v, "" ) )
		                              : ( !v.watertight && !v.open && ReasonIs( v, "non-manifold seam (more than two sides share endpoints)" ) ) ),
			std::string( "G: shipped bilinear model verdict pinned: " ) + m );
	}
}

//////////////////////////////////////////////////////////////////////
// H. proof-step coverage (DL-616 review round 2)
//
// Rows whose REASON pins which proof step refused them, so a step that
// silently stopped refusing (e.g. the injectivity cone stubbed to "true")
// fails a row instead of passing it through a later, weaker step.
//////////////////////////////////////////////////////////////////////

static const char* const kConeSingle = "sub-patch not provably injective (degenerate, folded or too curved)";
static const char* const kConePair = "sub-patches sharing a side are not provably injective as a union (fold or sharp crease)";
static const char* const kBudget = "certification work budget exhausted";

//! A planar patch (z = 0) whose v-curve x(v) reverses for v in
//! (0.54, 0.58): the patch folds over itself in a thin band strictly
//! inside one sub-patch row at both grids (4 and 8).  Nothing but the
//! per-sub-patch injectivity cone sees the fold: the u-neighbours that
//! share its side meet only along that side, and every other pair is
//! separated in y or across an unfolded row boundary.
static std::vector<BezierPatch> MakeInRowFold()
{
	// x'(v) = A (v - 0.56)^2 - A d^2, d = 0.02 (negative on (0.54, 0.58));
	// Bernstein differences of the quadratic x'/3.
	const Scalar A = 1, c = 0.56, d = 0.02;
	auto q = [&]( Scalar v ) { return A * ( v - c ) * ( v - c ) - A * d * d; };
	const Scalar dq0 = -2 * A * c;
	const Scalar d0 = q( 0 ) / 3, d1 = ( q( 0 ) + dq0 / 2 ) / 3, d2 = q( 1 ) / 3;
	const Scalar x[4] = { 0, 10 * d0, 10 * ( d0 + d1 ), 10 * ( d0 + d1 + d2 ) };
	BezierPatch p;
	for( int i = 0; i < 4; ++i ) {
		for( int j = 0; j < 4; ++j ) {
			p.c[i].pts[j] = Point3( x[j], Scalar( i ) / 3, 0 );
		}
	}
	return std::vector<BezierPatch>( 1, p );
}

//! Two Bezier patches glued along x = 1: the first flat, the second
//! turning straight back over it and rising as z = c s^2 with the
//! distance s from the crease.  The surface meets itself only along the
//! crease (a cusp ridge) but is not immersed there -- the two normals at
//! the crease are opposite -- so the pooled side-pair cone is the proof
//! step that refuses it.
static std::vector<BezierPatch> MakeCuspFold( Scalar c )
{
	BezierPatch a, b;
	for( int i = 0; i < 4; ++i ) {
		for( int j = 0; j < 4; ++j ) {
			// a: u along -y (index i), v along x from 0 to 1 (index j).
			a.c[i].pts[j] = Point3( Scalar( j ) / 3, Scalar( 3 - i ) / 3, 0 );
		}
	}
	// b's v = 0 side is a's v = 1 side (x = 1) traversed the other way;
	// b runs back to x = 0.5.  Bernstein coefficients of s^2: (0,0,1/3,1).
	const Scalar zc[4] = { 0, 0, c / 3, c };
	for( int i = 0; i < 4; ++i ) {
		for( int j = 0; j < 4; ++j ) {
			b.c[i].pts[j] = Point3( 1 - Scalar( 0.5 ) * Scalar( j ) / 3, Scalar( 3 - i ) / 3, zc[j] );
		}
	}
	std::vector<BezierPatch> f;
	f.push_back( a );
	f.push_back( b );
	return f;
}

//! One bicubic helicoid strip: rulings r in [r0, r1] through the axis,
//! turning by T radians while rising h T.  Near r = 0 the normal is
//! nearly horizontal and turns with the ruling, so a sub-patch that
//! straddles r = 0 pools normals over almost a half-turn plus its share
//! of T; a finer grid splits both.
static std::vector<BezierPatch> MakeHelicoidStrip( Scalar r0, Scalar r1, Scalar T, Scalar h )
{
	const Scalar kk = Scalar( 4 ) / 3 * std::tan( T / 4 );
	const Point3 arc[4] = {
		Point3( 1, 0, 0 ), Point3( 1, kk, 0 ),
		Point3( std::cos( T ) + kk * std::sin( T ), std::sin( T ) - kk * std::cos( T ), 0 ),
		Point3( std::cos( T ), std::sin( T ), 0 ) };
	BezierPatch p;
	for( int i = 0; i < 4; ++i ) {
		const Scalar r = r0 + ( r1 - r0 ) * Scalar( i ) / 3;
		for( int j = 0; j < 4; ++j ) {
			p.c[i].pts[j] = Point3( r * arc[j].x, r * arc[j].y, h * T * Scalar( j ) / 3 );
		}
	}
	return std::vector<BezierPatch>( 1, p );
}

//! A closed bilinear "cube" whose top square is rotated half a turn
//! about the vertical axis: every seam is exact and the set is a closed
//! oriented 2-manifold, but the four twisted side faces all pass through
//! the central axis and cross one another -- an immersed, not embedded,
//! closed surface.
static std::vector<BilinearPatch> MakeInvertedCube()
{
	const Point3 v[8] = {
		Point3(0,0,0), Point3(1,0,0), Point3(1,1,0), Point3(0,1,0),
		Point3(1,1,1), Point3(0,1,1), Point3(0,0,1), Point3(1,0,1) };
	std::vector<BilinearPatch> c;
	c.push_back( MakeQuad( v[0], v[3], v[2], v[1] ) );
	c.push_back( MakeQuad( v[4], v[5], v[6], v[7] ) );
	c.push_back( MakeQuad( v[0], v[1], v[5], v[4] ) );
	c.push_back( MakeQuad( v[2], v[3], v[7], v[6] ) );
	c.push_back( MakeQuad( v[0], v[4], v[7], v[3] ) );
	c.push_back( MakeQuad( v[1], v[2], v[6], v[5] ) );
	return c;
}

static void TestProofStepCoverage()
{
	std::cout << "H. proof-step coverage" << std::endl;

	{	// Round 3 P3-3: a Step-4 budget refusal carries its own flag (no
		// geometric witness), distinct from a geometric refusal.
		const int N = 300;
		std::vector<Point3> pts;
		for( int j = 0; j <= N; ++j ) for( int i = 0; i <= N; ++i ) {
			pts.push_back( Point3( double( i ) / N, double( j ) / N, 0 ) );
		}
		const unsigned int apex = (unsigned int)pts.size();
		pts.push_back( Point3( 1e5, 0.5, 1e5 ) );
		std::vector<Point3> corners;
		std::vector<unsigned int> weld;
		const auto add = [&]( unsigned int a, unsigned int b, unsigned int c ) {
			corners.push_back( pts[a] ); corners.push_back( pts[b] ); corners.push_back( pts[c] );
			weld.push_back( a ); weld.push_back( b ); weld.push_back( c );
		};
		auto id = [&]( int i, int j ) { return (unsigned int)( j * ( N + 1 ) + i ); };
		for( int j = 0; j < N; ++j ) for( int i = 0; i < N; ++i ) {
			add( id( i, j ), id( i + 1, j ), id( i + 1, j + 1 ) );
			add( id( i, j ), id( i + 1, j + 1 ), id( i, j + 1 ) );
		}
		add( id( N, 1 ), id( N, 0 ), apex );
		const auto cert = GeometricUtilities::IsEmbeddedOpenOrientedSurface( corners, nullptr, Scalar( 0 ), &weld, Scalar( 1e-6 ) );
		Check( !cert.certified && cert.budgetRefusal && cert.refusalTriA == std::size_t( -1 ),
			"P3-3: a Step-4 pair-budget refusal is flagged as a budget refusal, with no witness" );

		// Control: two crossing triangles are a GEOMETRIC refusal.
		std::vector<Point3> x = {
			Point3( 0, 0, 0 ), Point3( 1, 0, 0 ), Point3( 0, 1, 0 ),
			Point3( 0.2, 0.2, -0.5 ), Point3( 0.2, 0.2, 0.5 ), Point3( 0.9, 0.9, 0 ) };
		const auto cx = GeometricUtilities::IsEmbeddedOpenOrientedSurface( x );
		Check( !cx.certified && !cx.budgetRefusal, "P3-3: a geometric refusal is not flagged as a budget refusal" );
	}

	const Verdict fold = CertBezier( MakeInRowFold() );
	std::cout << "    in-row fold: " << Describe( fold ) << std::endl;
	Check( !fold.open && !fold.watertight && ReasonIs( fold, kConeSingle ),
		"H: a fold inside one sub-patch row is refused by the sub-patch injectivity cone" );

	for( const Scalar c : { 0.05, 0.2, 0.5 } ) {
		std::cout << "    cusp fold c=" << c << ": " << Describe( CertBezier( MakeCuspFold( c ) ) ) << std::endl;
	}
	const Verdict flat = CertBezier( MakeCuspFold( 0.2 ) );
	std::cout << "    cusp fold at a seam: " << Describe( flat ) << std::endl;
	Check( !flat.open && !flat.watertight && ReasonIs( flat, kConePair ),
		"H: a sheet folded back across a seam (cusp ridge) is refused by the pooled side-pair cone" );

	const Verdict inv = CertBilinear( MakeInvertedCube() );
	std::cout << "    immersed closed cube: " << Describe( inv ) << std::endl;
	Check( !inv.watertight && !inv.open, "H: an immersed (self-crossing) closed surface is not certified watertight" );

	const Verdict tight = CertBezier( MakeCubeSphere( 2, true ), 200 );
	std::cout << "    cube-sphere k=2 under a 200-unit budget: " << Describe( tight ) << std::endl;
	Check( !tight.watertight && !tight.open && ReasonIs( tight, kBudget ),
		"H: a set whose proof exceeds the work budget is refused, not certified" );

	const Verdict helix = CertBezier( MakeHelicoidStrip( -1, 0.6, 1.5, 0.1 ) );
	std::cout << "    helicoid strip straddling its axis: " << Describe( helix ) << std::endl;
	Check( helix.open && helix.grid == 8,
		"H: a helicoid strip refused on the 4 x 4 grid is certified open by the 8 x 8 retry" );
	const Verdict helixSteep = CertBezier( MakeHelicoidStrip( -1, 0.6, 1.5, 0.3 ) );
	Check( helixSteep.open && helixSteep.grid == 4, "H: a steeper helicoid strip certifies on the first (4 x 4) grid" );
}

//////////////////////////////////////////////////////////////////////
// I. DL-621: CSG over mesh and displaced operands
//
//  (a) GeometricUtilities::TriangleMeshOpenness / TopologicalOpenness()
//      decides sheet-vs-trimmer role by welded-edge topology alone so
//      CSG geometry never flips when a mesh's certificate comes or goes;
//  (b) RecoverContainedOperand completes exit range2 (and orients exit
//      normals with OrientExitNormalsAlongRay) for certified, uncertified,
//      inward-wound, and non-convex mesh and displaced solids;
//  (c) sheet UNION closed solid (both operand orders) uses WalkTrimmedSheet
//      outside the solid and the solid's own finite interval inside/through
//      the solid instead of semi-infinite interval algebra.
//////////////////////////////////////////////////////////////////////

static std::string OpenMeshSheet( const std::string& name, Scalar z, bool faceNormals = true )
{
	std::ostringstream s;
	s << "indexedmesh_geometry\n{\n name " << name << "\n double_sided TRUE\n face_normals "
	  << ( faceNormals ? "TRUE" : "FALSE" ) << "\n"
	  << " vertex -3 -3 " << z << "\n vertex 3 -3 " << z << "\n vertex 3 3 " << z << "\n vertex -3 3 " << z << "\n"
	  << " normal 0 0 1\n normal 0 0 1\n normal 0 0 1\n normal 0 0 1\n"
	  << " uv 0 0\n uv 1 0\n uv 1 1\n uv 0 1\n"
	  << " triangle 0 1 2\n triangle 0 2 3\n}\n";
	return s.str();
}

//! An open 4-triangle mesh strip in [-3, 3]^2 at z whose extra fold flap
//! crosses through it at x = 2.5 (away from the test rays near x = 0..1.5),
//! so IsProvablyOpenSheet() is FALSE while TopologicalOpenness() is +1.
static std::string UncertifiedOpenMeshSheet( const std::string& name, Scalar z )
{
	std::ostringstream s;
	s << "indexedmesh_geometry\n{\n name " << name << "\n double_sided TRUE\n face_normals TRUE\n"
	  << " vertex -3 -3 " << z << "\n vertex 3 -3 " << z << "\n vertex 3 3 " << z << "\n vertex -3 3 " << z << "\n"
	  << " vertex 2.5 -1 " << ( z - 0.5 ) << "\n vertex 2.5 1 " << ( z + 0.5 ) << "\n"
	  << " uv 0 0\n uv 1 0\n uv 1 1\n uv 0 1\n uv 0 0\n uv 1 1\n"
	  << " triangle 0 1 2\n triangle 0 2 3\n"
	  << " triangle 2 1 4\n triangle 2 4 5\n}\n";
	return s.str();
}

//! A closed 2-manifold mesh cube on [-1, 1]^3 with a zero-thickness coplanar
//! opposed pleat along edge (0,8) at z = -1, so DL-197 refuses
//! IsCertifiedWatertight() while TopologicalOpenness() is 0 (closed) and
//! every ray through the interior [-0.8, 0.8]^2 sees the unit cube [-1, 1]^3.
static std::string UncertifiedClosedMeshCube( const std::string& name, bool pleated = true )
{
	std::ostringstream s;
	s << "indexedmesh_geometry\n{\n name " << name << "\n double_sided TRUE\n face_normals TRUE\n"
	  << " vertex -1 -1 -1\n vertex 1 -1 -1\n vertex 1 1 -1\n vertex -1 1 -1\n"
	  << " vertex -1 -1 1\n vertex 1 -1 1\n vertex 1 1 1\n vertex -1 1 1\n"
	  << " vertex 0 " << ( pleated ? "-0.90" : "-0.95" ) << " -1\n"
	  << " vertex 0 " << ( pleated ? "-0.95" : "-0.90" ) << " -1\n"
	  << " uv 0 0\n uv 1 0\n uv 1 1\n uv 0 1\n uv 0 0\n uv 1 0\n uv 1 1\n uv 0 1\n uv 0.5 0\n uv 0.5 0\n"
	  // Bottom face with a flat fold back and forth across y = -0.90..-0.95 at z = -1 when pleated:
	  << " triangle 0 8 1\n"
	  << " triangle 0 9 8\n"
	  << " triangle 8 9 1\n"
	  << " triangle 0 2 9\n"
	  << " triangle 9 2 1\n"
	  << " triangle 0 3 2\n"
	  // Remaining 5 cube faces:
	  << " triangle 4 5 6\n triangle 4 6 7\n"
	  << " triangle 0 1 5\n triangle 0 5 4\n"
	  << " triangle 3 7 6\n triangle 3 6 2\n"
	  << " triangle 0 4 7\n triangle 0 7 3\n"
	  << " triangle 1 2 6\n triangle 1 6 5\n}\n";
	return s.str();
}

static TriangleMeshGeometry* MakeRawTriangleMeshCube( Scalar halfExtent, const Point3& center )
{
	TriangleMeshGeometry* g = new TriangleMeshGeometry( true );
	g->BeginTriangles();
	const Point3 v[8] = {
		Point3( center.x - halfExtent, center.y - halfExtent, center.z - halfExtent ),
		Point3( center.x + halfExtent, center.y - halfExtent, center.z - halfExtent ),
		Point3( center.x + halfExtent, center.y + halfExtent, center.z - halfExtent ),
		Point3( center.x - halfExtent, center.y + halfExtent, center.z - halfExtent ),
		Point3( center.x - halfExtent, center.y - halfExtent, center.z + halfExtent ),
		Point3( center.x + halfExtent, center.y - halfExtent, center.z + halfExtent ),
		Point3( center.x + halfExtent, center.y + halfExtent, center.z + halfExtent ),
		Point3( center.x - halfExtent, center.y + halfExtent, center.z + halfExtent )
	};
	const int tris[12][3] = {
		{0,2,1}, {0,3,2}, {4,5,6}, {4,6,7},
		{0,1,5}, {0,5,4}, {3,7,6}, {3,6,2},
		{0,4,7}, {0,7,3}, {1,2,6}, {1,6,5}
	};
	for( const auto& t : tris ) {
		Triangle tri;
		const Vector3 fn = Vector3Ops::Normalize( Vector3Ops::Cross(
			Vector3Ops::mkVector3( v[t[1]], v[t[0]] ),
			Vector3Ops::mkVector3( v[t[2]], v[t[0]] ) ) );
		for( int k = 0; k < 3; ++k ) {
			tri.vertices[k] = v[t[k]];
			tri.normals[k] = fn;
			tri.coords[k] = Point2( 0, 0 );
		}
		g->AddTriangle( tri );
	}
	g->DoneTriangles();
	return g;
}

static TriangleMeshGeometry* MakeRawTriangleMeshSheet( Scalar halfExtent, Scalar z )
{
	TriangleMeshGeometry* g = new TriangleMeshGeometry( true );
	g->BeginTriangles();
	const Point3 v[4] = {
		Point3( -halfExtent, -halfExtent, z ),
		Point3(  halfExtent, -halfExtent, z ),
		Point3(  halfExtent,  halfExtent, z ),
		Point3( -halfExtent,  halfExtent, z )
	};
	const int tris[2][3] = { {0,1,2}, {0,2,3} };
	for( const auto& t : tris ) {
		Triangle tri;
		for( int k = 0; k < 3; ++k ) {
			tri.vertices[k] = v[t[k]];
			tri.normals[k] = ( k == 0 ) ? Vector3( 0, 0, -1 ) : Vector3( 0, 0, 1 );
			tri.coords[k] = Point2( 0, 0 );
		}
		g->AddTriangle( tri );
	}
	g->DoneTriangles();
	return g;
}

static void TestMeshAndDisplacedCSG_DL621()
{
	std::cout << "I. DL-621: CSG over mesh and displaced operands" << std::endl;

	// 1. Direct GeometricUtilities::TriangleMeshOpenness classification.
	{
		std::vector<Point3> empty;
		Check( MeshOpenness( empty ) == -1,
			"I(a): empty corner list has unknown openness (-1)" );

		std::vector<Point3> openQuad = {
			Point3( 0, 0, 0 ), Point3( 1, 0, 0 ), Point3( 1, 1, 0 ),
			Point3( 0, 0, 0 ), Point3( 1, 1, 0 ), Point3( 0, 1, 0 )
		};
		Check( MeshOpenness( openQuad ) == 1,
			"I(a): 2-triangle quad is topologically open (+1)" );

		const Point3 cv[8] = {
			Point3(-1,-1,-1), Point3(1,-1,-1), Point3(1,1,-1), Point3(-1,1,-1),
			Point3(-1,-1,1), Point3(1,-1,1), Point3(1,1,1), Point3(-1,1,1)
		};
		const int ctris[12][3] = {
			{0,2,1},{0,3,2},{4,5,6},{4,6,7},
			{0,1,5},{0,5,4},{3,7,6},{3,6,2},{0,4,7},{0,7,3},{1,2,6},{1,6,5}
		};
		std::vector<Point3> closedCube, inwardCube;
		for( const auto& t : ctris ) {
			closedCube.push_back( cv[t[0]] );
			closedCube.push_back( cv[t[1]] );
			closedCube.push_back( cv[t[2]] );
			inwardCube.push_back( cv[t[0]] );
			inwardCube.push_back( cv[t[2]] );
			inwardCube.push_back( cv[t[1]] );
		}
		Check( MeshOpenness( closedCube ) == 0,
			"I(a): closed 12-triangle cube is topologically closed (0)" );
		Check( MeshOpenness( inwardCube ) == 0,
			"I(a): inward-wound 12-triangle cube is topologically closed (0)" );

		std::vector<Point3> openBox( closedCube.begin() + 6, closedCube.end() );
		Check( MeshOpenness( openBox ) == 1,
			"I(a): cube missing one face is topologically open (+1)" );

		std::vector<Point3> degen = {
			Point3( 0, 0, 0 ), Point3( 1, 0, 0 ), Point3( 2, 0, 0 )
		};
		Check( MeshOpenness( degen ) == -1,
			"I(a): collinear triangle is refused (-1)" );

		std::vector<Point3> pillow = {
			Point3( 0, 0, 0 ), Point3( 1, 0, 0 ), Point3( 0, 1, 0 ),
			Point3( 0, 0, 0 ), Point3( 0, 1, 0 ), Point3( 1, 0, 0 )
		};
		Check( MeshOpenness( pillow ) == -1,
			"I(a): 2-triangle coincident pillow is refused (-1), never closed" );

		std::vector<Point3> threeFin = {
			Point3( 0, 0, 0 ), Point3( 1, 0, 0 ), Point3( 0, 1, 0 ),
			Point3( 1, 0, 0 ), Point3( 0, 0, 0 ), Point3( 0, -1, 0 ),
			Point3( 0, 0, 0 ), Point3( 1, 0, 0 ), Point3( 0, 0, 1 )
		};
		Check( MeshOpenness( threeFin ) == -1,
			"I(a): non-manifold 3-fin edge is refused (-1)" );

		std::vector<Point3> orientConflict = {
			Point3( 0, 0, 0 ), Point3( 1, 0, 0 ), Point3( 0, 1, 0 ),
			Point3( 0, 0, 0 ), Point3( 1, 0, 0 ), Point3( 0, -1, 0 )
		};
		Check( MeshOpenness( orientConflict ) == -1,
			"I(a): orientation-conflicting shared edge is refused (-1)" );

		std::vector<Point3> cubePlusFlap = closedCube;
		cubePlusFlap.push_back( Point3( 5, 0, 0 ) );
		cubePlusFlap.push_back( Point3( 6, 0, 0 ) );
		cubePlusFlap.push_back( Point3( 5, 1, 0 ) );
		Check( MeshOpenness( cubePlusFlap ) == -1,
			"I(a): closed cube plus disjoint open triangle is refused (-1)" );
	}

	// 2. Scene fixtures covering DL-621(a), DL-621(b), and DL-621(c).
	std::string scene = Materials( false ) +
		OpenMeshSheet( "msheet", 0.5 ) +
		OpenMeshSheet( "msheet_edit", 0.5, false ) +
		UncertifiedOpenMeshSheet( "msheet_uncert", 0.5 ) +
		NamedMeshCube( "mcube_cert", false, true ) +
		UncertifiedClosedMeshCube( "mcube_edit", false ) +
		NamedMeshCube( "mcube_in", true, true ) +
		UncertifiedClosedMeshCube( "mcube_uncert", true ) +
		TwoLobeMesh( "lobes" ) +
		"clippedplane_geometry\n{\n name cplane\n pta -3 -3 0.5\n ptb 3 -3 0.5\n ptc 3 3 0.5\n ptd -3 3 0.5\n}\n"
		"box_geometry\n{\n name cube2\n width 2\n height 2\n depth 2\n}\n"
		"sphere_geometry\n{\n name sph1\n radius 1\n}\n"
		"displaced_geometry\n{\n name disp_sheet\n base_geometry cplane\n detail 4\n disp_scale 0\n}\n"
		"displaced_geometry\n{\n name disp_cube\n base_geometry cube2\n detail 4\n disp_scale 0\n}\n"
		"displaced_geometry\n{\n name disp_uncert_cube\n base_geometry mcube_uncert\n detail 2\n disp_scale 0\n}\n";

	auto obj = [&]( const std::string& name, const std::string& geom, const std::string& extra = "" ) {
		scene += "standard_object\n{\n name " + name + "\n geometry " + geom + "\n material glass\n" + extra + "}\n";
	};
	auto csg = [&]( const std::string& name, const std::string& a, const std::string& b, const std::string& op, const std::string& extra = "" ) {
		scene += "csg_object\n{\n name " + name + "\n obja " + a + "\n objb " + b + "\n operation " + op + "\n material glass\n" + extra + "}\n";
	};

	// DL-621(a): certified vs uncertified mesh sheets and mesh/displaced solids in trimming & union.
	obj( "o_ms_c1", "msheet" ); obj( "o_mc_c1", "mcube_cert" ); csg( "msheet_inter_mcert", "o_ms_c1", "o_mc_c1", "intersection" );
	obj( "o_ms_c2", "msheet" ); obj( "o_mc_c2", "mcube_cert" ); csg( "msheet_minus_mcert", "o_ms_c2", "o_mc_c2", "subtraction" );
	obj( "o_ms_u1", "msheet_uncert" ); obj( "o_mc_c3", "mcube_cert" ); csg( "muncert_inter_mcert", "o_ms_u1", "o_mc_c3", "intersection" );
	obj( "o_ms_u2", "msheet_uncert" ); obj( "o_mc_c4", "mcube_cert" ); csg( "muncert_minus_mcert", "o_ms_u2", "o_mc_c4", "subtraction" );
	obj( "o_ms_c5", "msheet" ); obj( "o_mc_u1", "mcube_uncert" ); csg( "msheet_inter_muncert", "o_ms_c5", "o_mc_u1", "intersection" );
	obj( "o_ms_c6", "msheet" ); obj( "o_mc_u2", "mcube_uncert" ); csg( "msheet_minus_muncert", "o_ms_c6", "o_mc_u2", "subtraction" );
	// Live deformation / normal-edit probes:
	obj( "o_ms_ed", "msheet_edit" ); obj( "o_mc_ed1", "mcube_cert" ); csg( "edit_sheet_csg", "o_ms_ed", "o_mc_ed1", "intersection" );
	obj( "o_ms_ed2", "msheet" ); obj( "o_mc_ed2", "mcube_edit" ); csg( "edit_solid_csg", "o_ms_ed2", "o_mc_ed2", "subtraction" );
	// Displaced sheet & displaced solid:
	obj( "o_ds1", "disp_sheet" ); obj( "o_dc1", "disp_cube" ); csg( "dsheet_inter_dcube", "o_ds1", "o_dc1", "intersection" );
	obj( "o_ds2", "disp_sheet" ); obj( "o_duc1", "disp_uncert_cube" ); csg( "dsheet_minus_duncert", "o_ds2", "o_duc1", "subtraction" );

	// DL-621(b): solid minus mesh/displaced solids from outside and inside, including uncertified & non-convex two-lobe meshes.
	obj( "o_mp_a", "cube2" ); obj( "o_mp_b", "mcube_cert", " position 0 0 1\n scale 0.5 0.5 0.5\n" ); csg( "mesh_pit", "o_mp_a", "o_mp_b", "subtraction" );
	obj( "o_mpi_a", "cube2" ); obj( "o_mpi_b", "mcube_in", " position 0 0 1\n scale 0.5 0.5 0.5\n" ); csg( "mesh_pit_in", "o_mpi_a", "o_mpi_b", "subtraction" );
	obj( "o_mpu_a", "cube2" ); obj( "o_mpu_b", "mcube_uncert", " position 0 0 1\n scale 0.5 0.5 0.5\n" ); csg( "mesh_pit_uncert", "o_mpu_a", "o_mpu_b", "subtraction" );
	obj( "o_dp_a", "cube2" ); obj( "o_dp_b", "disp_uncert_cube", " position 0 0 1\n scale 0.5 0.5 0.5\n" ); csg( "disp_pit_uncert", "o_dp_a", "o_dp_b", "subtraction" );
	// Non-convex TwoLobeMesh: lobe 0 is y in [-1, 1], lobe 1 is y in [-5, -3].
	// Place box at y in [-4, -2] (position 0 -3 0), so lobe 1 carves y in [-4, -3] out of the box.
	obj( "o_lb_box", "cube2", " position 0 -3 0\n" ); obj( "o_lb_m1", "lobes" ); csg( "box_minus_lobes", "o_lb_box", "o_lb_m1", "subtraction" );
	obj( "o_lbi_box", "cube2", " position 0 -3 0\n" ); obj( "o_lbi_m", "lobes" ); csg( "box_inter_lobes", "o_lbi_box", "o_lbi_m", "intersection" );
	// Sheet at y = -3.5 (inside lobe 1 of TwoLobeMesh):
	scene += "clippedplane_geometry\n{\n name sheet_y35\n pta -3 -3.5 -3\n ptb 3 -3.5 -3\n ptc 3 -3.5 3\n ptd -3 -3.5 3\n}\n";
	obj( "o_sy35", "sheet_y35" ); obj( "o_lb_m2", "lobes" ); csg( "sheet_minus_lobes", "o_sy35", "o_lb_m2", "subtraction" );

	// DL-621(c): sheet UNION solid (and solid UNION sheet) with mesh/plane sheets and analytic/mesh solids.
	obj( "o_su_s1", "msheet" ); obj( "o_su_b1", "mcube_cert" ); csg( "msheet_union_mcube", "o_su_s1", "o_su_b1", "union" );
	obj( "o_su_b2", "mcube_cert" ); obj( "o_su_s2", "msheet" ); csg( "mcube_union_msheet", "o_su_b2", "o_su_s2", "union" );
	obj( "o_su_s3", "msheet_uncert" ); obj( "o_su_b3", "mcube_uncert" ); csg( "muncert_union_muncert", "o_su_s3", "o_su_b3", "union" );
	// Sheet above solid (at z = 1.5, outside cube [-1, 1]^3):
	scene += OpenMeshSheet( "msheet_top", 1.5 );
	obj( "o_sut_s", "msheet_top" ); obj( "o_sut_b", "mcube_cert" ); csg( "msheet_top_union_mcube", "o_sut_s", "o_sut_b", "union" );

	LoadedScene loaded( scene );
	const Vector3 down( 0, 0, -1 ), up( 0, 0, 1 ), downY( 0, -1, 0 ), upY( 0, 1, 0 );

	// Verify all fixtures loaded.
	const char* const names[] = {
		"msheet_inter_mcert", "msheet_minus_mcert", "muncert_inter_mcert", "muncert_minus_mcert",
		"msheet_inter_muncert", "msheet_minus_muncert", "edit_sheet_csg", "edit_solid_csg",
		"dsheet_inter_dcube", "dsheet_minus_duncert",
		"mesh_pit", "mesh_pit_in", "mesh_pit_uncert", "disp_pit_uncert",
		"box_minus_lobes", "box_inter_lobes", "sheet_minus_lobes",
		"msheet_union_mcube", "mcube_union_msheet", "muncert_union_muncert", "msheet_top_union_mcube"
	};
	bool allLoaded = true;
	for( const char* n : names ) {
		if( !loaded.Object( n ) ) {
			std::cout << "    missing DL-621 fixture: " << n << std::endl;
			allLoaded = false;
		}
	}
	Check( allLoaded, "I: all DL-621 CSG fixtures load" );
	if( !allLoaded ) {
		return;
	}

	// DL-621(a): verify topological openness vs geometric certification on the operands and composites.
	{
		const IObject* cCert = loaded.Object( "msheet_inter_mcert" );
		const IObject* cUncertSheet = loaded.Object( "muncert_inter_mcert" );
		const IObject* cUncertSolid = loaded.Object( "msheet_inter_muncert" );
		const auto* gSheetCert = dynamic_cast<const TriangleMeshGeometryIndexed*>( AsCSG( cCert )->GetOperandA()->GetGeometry() );
		const auto* gSheetUncert = dynamic_cast<const TriangleMeshGeometryIndexed*>( AsCSG( cUncertSheet )->GetOperandA()->GetGeometry() );
		const auto* gSolidCert = dynamic_cast<const TriangleMeshGeometryIndexed*>( AsCSG( cCert )->GetOperandB()->GetGeometry() );
		const auto* gSolidUncert = dynamic_cast<const TriangleMeshGeometryIndexed*>( AsCSG( cUncertSolid )->GetOperandB()->GetGeometry() );

		Check( gSheetCert && gSheetCert->IsProvablyOpenSheet() && TopOpenness( gSheetCert ) == 1,
			"I(a): certified open mesh sheet has IsProvablyOpenSheet=true and TopologicalOpenness=1" );
		Check( gSheetUncert && !gSheetUncert->IsProvablyOpenSheet() && !gSheetUncert->IsCertifiedWatertight() &&
			TopOpenness( gSheetUncert ) == 1,
			"I(a): self-crossing open mesh sheet has IsProvablyOpenSheet=false and TopologicalOpenness=1" );
		Check( gSolidCert && gSolidCert->IsCertifiedWatertight() && TopOpenness( gSolidCert ) == 0,
			"I(a): certified mesh cube has IsCertifiedWatertight=true and TopologicalOpenness=0" );
		Check( gSolidUncert && !gSolidUncert->IsCertifiedWatertight() && !gSolidUncert->IsProvablyOpenSheet() &&
			TopOpenness( gSolidUncert ) == 0,
			"I(a): coplanar-pleated closed mesh cube is refused watertightness but has TopologicalOpenness=0" );

		Check( AsCSG( cCert )->TrimsSheet() && AsCSG( cCert )->IsProvablyOpenSheet(),
			"I(a): certified mesh sheet ∩ certified mesh solid both trims and certifies open" );
		Check( AsCSG( cUncertSheet )->TrimsSheet() && !AsCSG( cUncertSheet )->IsProvablyOpenSheet(),
			"I(a): uncertified mesh sheet ∩ certified mesh solid still trims the sheet (without open certificate)" );
		Check( AsCSG( cUncertSolid )->TrimsSheet() && !AsCSG( cUncertSolid )->IsProvablyOpenSheet(),
			"I(a): certified mesh sheet ∩ uncertified closed mesh solid still trims the sheet (without open certificate)" );

		for( const Scalar x : { 0.2, 0.7, 1.5 } ) {
			const RayIntersection hCert = Cast( *cCert, Point3( x, 0.1, 3 ), down );
			const RayIntersection hUSheet = Cast( *cUncertSheet, Point3( x, 0.1, 3 ), down );
			const RayIntersection hUSolid = Cast( *cUncertSolid, Point3( x, 0.1, 3 ), down );
			const bool expectHit = ( x < 1.0 );
			Check( hCert.geometric.bHit == expectHit && hUSheet.geometric.bHit == expectHit && hUSolid.geometric.bHit == expectHit &&
				( !expectHit || ( std::fabs( hCert.geometric.range - 2.5 ) < 1e-6 &&
				                  std::fabs( hUSheet.geometric.range - 2.5 ) < 1e-6 &&
				                  std::fabs( hUSolid.geometric.range - 2.5 ) < 1e-6 &&
				                  hUSheet.geometric.range2 == RISE_INFINITY &&
				                  hUSolid.geometric.range2 == RISE_INFINITY ) ),
				"I(a): sheet ∩ solid geometry is identical whether the sheet or solid mesh is certified or uncertified" );
		}

		const IObject* mCert = loaded.Object( "msheet_minus_mcert" );
		const IObject* mUSheet = loaded.Object( "muncert_minus_mcert" );
		const IObject* mUSolid = loaded.Object( "msheet_minus_muncert" );
		for( const Scalar x : { 0.2, 1.5 } ) {
			const RayIntersection hCert = Cast( *mCert, Point3( x, 0.1, 3 ), down );
			const RayIntersection hUSheet = Cast( *mUSheet, Point3( x, 0.1, 3 ), down );
			const RayIntersection hUSolid = Cast( *mUSolid, Point3( x, 0.1, 3 ), down );
			const bool expectHit = ( x > 1.0 );
			Check( hCert.geometric.bHit == expectHit && hUSheet.geometric.bHit == expectHit && hUSolid.geometric.bHit == expectHit &&
				( !expectHit || ( std::fabs( hUSheet.geometric.range - 2.5 ) < 1e-6 &&
				                  std::fabs( hUSolid.geometric.range - 2.5 ) < 1e-6 &&
				                  hUSheet.geometric.range2 == RISE_INFINITY &&
				                  hUSolid.geometric.range2 == RISE_INFINITY ) ),
				"I(a): sheet − solid geometry is identical whether the sheet or solid mesh is certified or uncertified" );
		}

		// Displaced sheet & displaced solid (certified and uncertified base):
		const IObject* dInter = loaded.Object( "dsheet_inter_dcube" );
		const IObject* dMinus = loaded.Object( "dsheet_minus_duncert" );
		const auto* gDispSheet = dynamic_cast<const DisplacedGeometry*>( AsCSG( dInter )->GetOperandA()->GetGeometry() );
		const auto* gDispCube = dynamic_cast<const DisplacedGeometry*>( AsCSG( dInter )->GetOperandB()->GetGeometry() );
		const auto* gDispUncert = dynamic_cast<const DisplacedGeometry*>( AsCSG( dMinus )->GetOperandB()->GetGeometry() );
		Check( gDispSheet && TopOpenness( gDispSheet ) == 1 &&
			gDispCube && TopOpenness( gDispCube ) == 0 &&
			gDispUncert && !gDispUncert->IsCertifiedWatertight() && TopOpenness( gDispUncert ) == 0,
			"I(a): DisplacedGeometry reports TopologicalOpenness from its tessellated/base topology even when uncertified" );
		Check( AsCSG( dInter )->TrimsSheet() && AsCSG( dMinus )->TrimsSheet() && !AsCSG( dMinus )->IsProvablyOpenSheet(),
			"I(a): DisplacedGeometry operands preserve their sheet/trimmer role regardless of watertight certification" );
		Check( Cast( *dInter, Point3( 0.2, 0.1, 3 ), down ).geometric.bHit &&
			!Cast( *dInter, Point3( 1.5, 0.1, 3 ), down ).geometric.bHit &&
			!Cast( *dMinus, Point3( 0.2, 0.1, 3 ), down ).geometric.bHit &&
			Cast( *dMinus, Point3( 1.5, 0.1, 3 ), down ).geometric.bHit,
			"I(a): displaced sheet trimmed by displaced solid keeps/removes the expected regions" );

		// Live mutation test: losing a mesh operand's certificate via UpdateVertices / ComputeVertexNormals
		// updates the CSG certificate epoch immediately without flipping the CSG's sheet-trimming role or hits.
		const IObject* edSheetCSG = loaded.Object( "edit_sheet_csg" );
		auto* edSheetGeom = const_cast<TriangleMeshGeometryIndexed*>(
			dynamic_cast<const TriangleMeshGeometryIndexed*>( AsCSG( edSheetCSG )->GetOperandA()->GetGeometry() ) );
		Check( edSheetGeom && edSheetGeom->IsProvablyOpenSheet() && AsCSG( edSheetCSG )->IsProvablyOpenSheet(),
			"I(a): editable mesh sheet composite starts certified open" );
		const RayIntersection preSheetIn = Cast( *edSheetCSG, Point3( 0.2, 0.1, 3 ), down );
		const RayIntersection preSheetOut = Cast( *edSheetCSG, Point3( 1.5, 0.1, 3 ), down );
		if( edSheetGeom ) {
			// Supply opposing vertex normals so Step 2 of IsEmbeddedOpenOrientedSurface refuses the sheet,
			// while vertex positions and welded-edge topology stay unchanged.
			const std::vector<Point3> pts = {
				Point3( -3, -3, 0.5 ), Point3( 3, -3, 0.5 ), Point3( 3, 3, 0.5 ), Point3( -3, 3, 0.5 )
			};
			const std::vector<Vector3> badNormals = {
				Vector3( 0, 0, -1 ), Vector3( 0, 0, 1 ), Vector3( 0, 0, 1 ), Vector3( 0, 0, 1 )
			};
			edSheetGeom->UpdateVertices( pts, badNormals );
			Check( !edSheetGeom->IsProvablyOpenSheet() && TopOpenness( edSheetGeom ) == 1,
				"I(a): after normal flip, mesh sheet loses its open-sheet certificate but keeps TopologicalOpenness=1" );
			Check( !AsCSG( edSheetCSG )->IsProvablyOpenSheet() && AsCSG( edSheetCSG )->TrimsSheet(),
				"I(a): composite drops its open-sheet certificate via epoch bump while keeping its sheet-trimming role" );
			const RayIntersection postSheetIn = Cast( *edSheetCSG, Point3( 0.2, 0.1, 3 ), down );
			const RayIntersection postSheetOut = Cast( *edSheetCSG, Point3( 1.5, 0.1, 3 ), down );
			Check( preSheetIn.geometric.bHit && postSheetIn.geometric.bHit &&
				std::fabs( preSheetIn.geometric.range - postSheetIn.geometric.range ) < 1e-9 &&
				postSheetIn.geometric.range2 == RISE_INFINITY &&
				!preSheetOut.geometric.bHit && !postSheetOut.geometric.bHit,
				"I(a): sheet ∩ solid geometry does not flip when the mesh sheet loses its certificate" );
			// Review P1-2: when m_topologicalOpenness is already cached >= 0 (here 1), a subsequent
			// UpdateVertices that changes vertex positions (collapsing a triangle, then restoring it)
			// must invalidate m_topologicalOpenness and refresh TrimsSheet().
			const std::vector<Point3> collapsedPts = {
				Point3( -3, -3, 0.5 ), Point3( -3, -3, 0.5 ), Point3( 3, 3, 0.5 ), Point3( -3, 3, 0.5 )
			};
			edSheetGeom->UpdateVertices( collapsedPts, badNormals );
			const bool collapsedInvalidated = ( TopOpenness( edSheetGeom ) == -1 && !AsCSG( edSheetCSG )->TrimsSheet() );
			edSheetGeom->UpdateVertices( pts, badNormals );
			Check( collapsedInvalidated && TopOpenness( edSheetGeom ) == 1 && AsCSG( edSheetCSG )->TrimsSheet(),
				"I(a): UpdateVertices invalidates cached TopologicalOpenness >= 0 when vertex positions change (review P1-2)" );
		}

		const IObject* edSolidCSG = loaded.Object( "edit_solid_csg" );
		auto* edSolidGeom = const_cast<TriangleMeshGeometryIndexed*>(
			dynamic_cast<const TriangleMeshGeometryIndexed*>( AsCSG( edSolidCSG )->GetOperandB()->GetGeometry() ) );
		Check( edSolidGeom && edSolidGeom->IsCertifiedWatertight() && AsCSG( edSolidCSG )->IsProvablyOpenSheet(),
			"I(a): editable mesh solid composite starts certified open" );
		const RayIntersection preSolidIn = Cast( *edSolidCSG, Point3( 0.2, 0.1, 3 ), down );
		const RayIntersection preSolidOut = Cast( *edSolidCSG, Point3( 1.5, 0.1, 3 ), down );
		if( edSolidGeom ) {
			const std::vector<Point3> pleatedPts = {
				Point3( -1, -1, -1 ), Point3( 1, -1, -1 ), Point3( 1, 1, -1 ), Point3( -1, 1, -1 ),
				Point3( -1, -1,  1 ), Point3( 1, -1,  1 ), Point3( 1, 1,  1 ), Point3( -1, 1,  1 ),
				Point3(  0, -0.90, -1 ), Point3( 0, -0.95, -1 )
			};
			edSolidGeom->UpdateVertices( pleatedPts, NormalsListType() );
			Check( !edSolidGeom->IsCertifiedWatertight() && TopOpenness( edSolidGeom ) == 0,
				"I(a): after bottom-face coplanar pleat, mesh solid loses watertight certificate but keeps TopologicalOpenness=0" );
			Check( !AsCSG( edSolidCSG )->IsProvablyOpenSheet() && AsCSG( edSolidCSG )->TrimsSheet(),
				"I(a): sheet − solid drops its open-sheet certificate via epoch bump while keeping its sheet-trimming role" );
			const RayIntersection postSolidIn = Cast( *edSolidCSG, Point3( 0.2, 0.1, 3 ), down );
			const RayIntersection postSolidOut = Cast( *edSolidCSG, Point3( 1.5, 0.1, 3 ), down );
			Check( !preSolidIn.geometric.bHit && !postSolidIn.geometric.bHit &&
				preSolidOut.geometric.bHit && postSolidOut.geometric.bHit &&
				std::fabs( preSolidOut.geometric.range - postSolidOut.geometric.range ) < 1e-9 &&
				postSolidOut.geometric.range2 == RISE_INFINITY,
				"I(a): sheet − solid geometry does not flip when the trimming mesh solid loses its watertight certificate" );
		}
	}

	// DL-621(b): exit completion (range2 and outward exit normals) for certified, uncertified,
	// inward-wound, non-indexed, and non-convex mesh & displaced solids.
	{
		for( const char* pitName : { "mesh_pit", "mesh_pit_in", "mesh_pit_uncert", "disp_pit_uncert" } ) {
			const IObject* mp = loaded.Object( pitName );
			const RayIntersection outsideHit = Cast( *mp, Point3( 0.1, 0.1, 3 ), down );
			Check( outsideHit.geometric.bHit &&
				std::fabs( outsideHit.geometric.ptIntersection.z - 0.5 ) < 1e-6 &&
				std::fabs( outsideHit.geometric.ptExit.z + 1.0 ) < 1e-6 &&
				Vector3Ops::Dot( outsideHit.geometric.vNormal, up ) > 0 &&
				Vector3Ops::Dot( outsideHit.geometric.UnflippedGeomNormal(), up ) > 0 &&
				Coherent( outsideHit ) &&
				Shadow( *mp, Point3( 0.1, 0.1, 3 ), down, 10 ) &&
				!Shadow( *mp, Point3( 0.1, 0.1, 3 ), down, 2.2 ),
				std::string( "I(b): outside ray into " ) + pitName + " completes range2 and hits the pit floor at z=0.5 with upward normal" );

			const RayIntersection insideHit = Cast( *mp, Point3( 0.1, 0.1, 0.8 ), down );
			Check( insideHit.geometric.bHit &&
				std::fabs( insideHit.geometric.ptIntersection.z - 0.5 ) < 1e-6 &&
				Vector3Ops::Dot( insideHit.geometric.vNormal, up ) > 0 &&
				Vector3Ops::Dot( insideHit.geometric.UnflippedGeomNormal(), up ) > 0 &&
				Coherent( insideHit ),
				std::string( "I(b): inside-both ray into " ) + pitName + " hits the pit floor at z=0.5 with upward normal" );
		}

		// Non-indexed TriangleMeshGeometry (always uncertified) in sheet-trimming and solid-minus-solid:
		TriangleMeshGeometry* rawCubeGeom = MakeRawTriangleMeshCube( 0.5, Point3( 0, 0, 1 ) );
		TriangleMeshGeometry* rawCubeOriginGeom = MakeRawTriangleMeshCube( 1.0, Point3( 0, 0, 0 ) );
		TriangleMeshGeometry* rawSheetGeom = MakeRawTriangleMeshSheet( 3.0, 0.2 );
		BoxGeometry* boxGeom = new BoxGeometry( 2, 2, 2 );
		Check( TopOpenness( rawCubeGeom ) == 0 &&
			!rawSheetGeom->IsProvablyOpenSheet() && TopOpenness( rawSheetGeom ) == 1,
			"I(a/b): non-indexed TriangleMeshGeometry cube/sheet report TopologicalOpenness 0 and 1" );

		Object* objBox = new Object( boxGeom );
		Object* objRawCube = new Object( rawCubeGeom );
		Object* objRawSheet = new Object( rawSheetGeom );
		CSGObject* rawPit = new CSGObject( CSG_SUBTRACTION );
		rawPit->AssignObjects( objBox, objRawCube );
		const RayIntersection rawPitOut = Cast( *rawPit, Point3( 0.1, 0.1, 3 ), down );
		const RayIntersection rawPitIn = Cast( *rawPit, Point3( 0.1, 0.1, 0.8 ), down );
		Check( rawPitOut.geometric.bHit &&
			std::fabs( rawPitOut.geometric.ptIntersection.z - 0.5 ) < 1e-6 &&
			std::fabs( rawPitOut.geometric.ptExit.z + 1.0 ) < 1e-6 &&
			Vector3Ops::Dot( rawPitOut.geometric.vNormal, up ) > 0 &&
			Vector3Ops::Dot( rawPitOut.geometric.UnflippedGeomNormal(), up ) > 0 &&
			Coherent( rawPitOut ) &&
			rawPitIn.geometric.bHit &&
			std::fabs( rawPitIn.geometric.ptIntersection.z - 0.5 ) < 1e-6 &&
			Vector3Ops::Dot( rawPitIn.geometric.vNormal, up ) > 0 &&
			Coherent( rawPitIn ),
			"I(b): box minus non-indexed TriangleMeshGeometry cube completes exit from outside and inside" );

		Object* objRawCube2 = new Object( rawCubeOriginGeom );
		CSGObject* rawTrim = new CSGObject( CSG_SUBTRACTION );
		rawTrim->AssignObjects( objRawSheet, objRawCube2 );
		Check( rawTrim->TrimsSheet() && !rawTrim->IsProvablyOpenSheet() &&
			!Cast( *rawTrim, Point3( 0.1, 0.1, 3 ), down ).geometric.bHit &&
			Cast( *rawTrim, Point3( 1.5, 0.1, 3 ), down ).geometric.bHit,
			"I(a/b): non-indexed TriangleMeshGeometry sheet minus non-indexed TriangleMeshGeometry cube trims the hole" );
		rawTrim->release();
		rawPit->release();
		rawSheetGeom->release();
		rawCubeOriginGeom->release();
		rawCubeGeom->release();
		boxGeom->release();

		// Non-convex TwoLobeMesh (lobe 0: y in [-1, 1], lobe 1: y in [-5, -3]; box: y in [-4, -2]):
		const IObject* bml = loaded.Object( "box_minus_lobes" );
		const IObject* bil = loaded.Object( "box_inter_lobes" );
		const IObject* sml = loaded.Object( "sheet_minus_lobes" );
		// Ray from y = 3 along -y passes through lobe 0 [-1, 1] in empty space before hitting the box [-4, -2]
		// where lobe 1 [-5, -3] carves [-4, -3], leaving [-3, -2].
		const RayIntersection hitAcrossLobe0 = Cast( *bml, Point3( 0.1, 3, 0.2 ), downY );
		Check( hitAcrossLobe0.geometric.bHit &&
			std::fabs( hitAcrossLobe0.geometric.ptIntersection.y + 2.0 ) < 1e-6 &&
			std::fabs( hitAcrossLobe0.geometric.ptExit.y + 3.0 ) < 1e-6 &&
			Vector3Ops::Dot( hitAcrossLobe0.geometric.vNormal2, downY ) > 0,
			"I(b): box minus TwoLobeMesh advances past the disjoint first lobe in empty space and carves the second lobe" );
		// Ray from y = -6 along +y enters lobe 1 at y = -5 (before box at y = -4) and exits lobe 1 at y = -3 (inside box):
		const RayIntersection hitFromBelow = Cast( *bml, Point3( 0.1, -6, 0.2 ), upY );
		Check( hitFromBelow.geometric.bHit &&
			std::fabs( hitFromBelow.geometric.ptIntersection.y + 3.0 ) < 1e-6 &&
			std::fabs( hitFromBelow.geometric.ptExit.y + 2.0 ) < 1e-6 &&
			Vector3Ops::Dot( hitFromBelow.geometric.vNormal, downY ) > 0 &&
			Vector3Ops::Dot( hitFromBelow.geometric.UnflippedGeomNormal(), downY ) > 0 &&
			Coherent( hitFromBelow ) &&
			Shadow( *bml, Point3( 0.1, -6, 0.2 ), upY, 10 ) &&
			!Shadow( *bml, Point3( 0.1, -6, 0.2 ), upY, 2.5 ),
			"I(b): box minus TwoLobeMesh seen through the carving lobe reports the cavity floor at y=-3 with outward normal" );
		// Box intersect TwoLobeMesh from y = 3 along -y: skips lobe 0 [-1, 1] and hits the overlap [-4, -3] at y = -3:
		const RayIntersection interHit = Cast( *bil, Point3( 0.1, 3, 0.2 ), downY );
		Check( interHit.geometric.bHit &&
			std::fabs( interHit.geometric.ptIntersection.y + 3.0 ) < 1e-6 &&
			std::fabs( interHit.geometric.ptExit.y + 4.0 ) < 1e-6 &&
			Shadow( *bil, Point3( 0.1, 3, 0.2 ), downY, 10 ) &&
			!Shadow( *bil, Point3( 0.1, 3, 0.2 ), downY, 5.5 ),
			"I(b): box intersect TwoLobeMesh advances past the disjoint first lobe to report the second lobe's overlap" );
		// Sheet at y = -3.5 minus TwoLobeMesh from y = 3 along -y:
		Check( !Cast( *sml, Point3( 0.1, 3, 0.2 ), downY ).geometric.bHit &&
			!Shadow( *sml, Point3( 0.1, 3, 0.2 ), downY, 10 ) &&
			Cast( *sml, Point3( 1.5, 3, 0.2 ), downY ).geometric.bHit &&
			Shadow( *sml, Point3( 1.5, 3, 0.2 ), downY, 10 ),
			"I(b): sheet minus TwoLobeMesh removes the sheet inside the second lobe and keeps it outside" );
	}

	// DL-621(c): sheet UNION solid (both operand orders, certified and uncertified).
	{
		for( const char* uName : { "msheet_union_mcube", "mcube_union_msheet", "muncert_union_muncert" } ) {
			const IObject* u = loaded.Object( uName );
			Check( !AsCSG( u )->IsCertifiedWatertight() && !AsCSG( u )->IsProvablyOpenSheet() && !AsCSG( u )->TrimsSheet(),
				std::string( "I(c): " ) + uName + " is neither purely watertight nor purely open" );

			// 1. Ray outside the solid (x = 1.5) hits the sheet at z = 0.5 as a zero-thickness hit:
			const RayIntersection hSheet = Cast( *u, Point3( 1.5, 0.1, 3 ), down );
			Check( hSheet.geometric.bHit &&
				std::fabs( hSheet.geometric.range - 2.5 ) < 1e-6 &&
				hSheet.geometric.range2 == RISE_INFINITY &&
				!hSheet.hasBoundaryRange2 &&
				Shadow( *u, Point3( 1.5, 0.1, 3 ), down, 10 ) &&
				!Shadow( *u, Point3( 1.5, 0.1, 3 ), down, 2.0 ),
				std::string( "I(c): " ) + uName + " outside the solid reports the zero-thickness sheet hit" );

			// 2. Ray hitting the solid [-1, 1]^3 first from z = 3 (with the sheet buried at z = 0.5 inside the solid)
			// must report the solid's entry at z = 1 AND its finite exit at z = -1 (range2 = 4, NOT RISE_INFINITY):
			const RayIntersection hSolid = Cast( *u, Point3( 0.2, 0.1, 3 ), down );
			Check( hSolid.geometric.bHit &&
				std::fabs( hSolid.geometric.ptIntersection.z - 1.0 ) < 1e-6 &&
				std::fabs( hSolid.geometric.ptExit.z + 1.0 ) < 1e-6 &&
				std::fabs( hSolid.geometric.range - 2.0 ) < 1e-6 &&
				std::fabs( hSolid.geometric.range2 - 4.0 ) < 1e-6 &&
				Shadow( *u, Point3( 0.2, 0.1, 3 ), down, 2.5 ),
				std::string( "I(c): " ) + uName + " entering the solid keeps the solid's finite exit range2 despite the buried sheet" );

			// 3. Ray starting INSIDE the solid at z = 0.8 (above the buried sheet at z = 0.5) looking down
			// must ignore the buried sheet at z = 0.5 and exit the solid at z = -1 with range2 = 0 and outward normal:
			const RayIntersection hInside = Cast( *u, Point3( 0.2, 0.1, 0.8 ), down );
			Check( hInside.geometric.bHit &&
				std::fabs( hInside.geometric.ptIntersection.z + 1.0 ) < 1e-6 &&
				hInside.geometric.range2 == 0 &&
				Vector3Ops::Dot( hInside.geometric.vNormal, down ) > 0 &&
				Vector3Ops::Dot( hInside.geometric.UnflippedGeomNormal(), down ) > 0 &&
				Coherent( hInside ) &&
				!Shadow( *u, Point3( 0.2, 0.1, 0.8 ), down, 0.5 ) &&
				Shadow( *u, Point3( 0.2, 0.1, 0.8 ), down, 2.5 ),
				std::string( "I(c): " ) + uName + " from inside the solid ignores the buried sheet and exits at z=-1" );
		}

		// 4. Ray hitting an unburied sheet at z = 1.5 BEFORE the solid [-1, 1]^3:
		const IObject* uTop = loaded.Object( "msheet_top_union_mcube" );
		const RayIntersection hBefore = Cast( *uTop, Point3( 0.2, 0.1, 3 ), down );
		Check( hBefore.geometric.bHit &&
			std::fabs( hBefore.geometric.ptIntersection.z - 1.5 ) < 1e-6 &&
			hBefore.geometric.range2 == RISE_INFINITY &&
			!hBefore.hasBoundaryRange2 &&
			Shadow( *uTop, Point3( 0.2, 0.1, 3 ), down, 1.6 ) &&
			!Shadow( *uTop, Point3( 0.2, 0.1, 3 ), down, 1.4 ),
			"I(c): sheet union solid when the sheet is in front of the solid reports the front sheet hit" );
	}
}

//////////////////////////////////////////////////////////////////////
// J. DL-624: polar patch sets (degenerate collapsed sides / pole fans)
//////////////////////////////////////////////////////////////////////

static BezierPatch RotateTestNet( const BezierPatch& q, int r )
{
	BezierPatch cur = q;
	for( int t = 0; t < ( r & 3 ); ++t ) {
		BezierPatch nxt;
		for( int i = 0; i < 4; ++i ) {
			for( int j = 0; j < 4; ++j ) {
				nxt.c[i].pts[j] = cur.c[3 - j].pts[i];
			}
		}
		cur = nxt;
	}
	return cur;
}

//! Builds an 8-patch polar UV Bezier sphere (4 northern wedges collapsed at
//! north pole (0,0,1) + 4 southern wedges collapsed at south pole (0,0,-1)),
//! or the 4-patch northern hemisphere dome when `northernHemisphereOnly` is true.
//! `rot` rotates each patch's parameter domain so collapsed sides on all four
//! parameter boundaries (s = 0, 1, 2, 3) are exercised.
static std::vector<BezierPatch> MakePolarUVSphere( bool outward, int rot = 0, bool northernHemisphereOnly = false )
{
	static const Vector3 kDir[4] = {
		Vector3( 1, 0, 0 ), Vector3( 0, 1, 0 ), Vector3( -1, 0, 0 ), Vector3( 0, -1, 0 )
	};
	const Scalar kappa = Scalar( 0.55 );
	const Point3 northPole( 0, 0, 1 );
	const Point3 southPole( 0, 0, -1 );
	std::vector<BezierPatch> out;
	for( int w = 0; w < 4; ++w ) {
		const Vector3 d0 = outward ? kDir[( w + 1 ) & 3] : kDir[w];
		const Vector3 d1 = outward ? kDir[w] : kDir[( w + 1 ) & 3];
		const Vector3 H[4] = {
			d0,
			d0 + d1 * kappa,
			d1 + d0 * kappa,
			d1
		};
		// Northern wedge: j = 0 collapsed at northPole, j = 3 on equator z = 0.
		BezierPatch north;
		for( int i = 0; i < 4; ++i ) {
			north.c[i].pts[0] = northPole;
			north.c[i].pts[1] = Point3( kappa * H[i].x, kappa * H[i].y, 1 );
			north.c[i].pts[2] = Point3( H[i].x, H[i].y, kappa );
			north.c[i].pts[3] = Point3( H[i].x, H[i].y, 0 );
		}
		out.push_back( RotateTestNet( north, rot ) );
		if( !northernHemisphereOnly ) {
			// Southern wedge: j = 0 on equator z = 0, j = 3 collapsed at southPole.
			BezierPatch south;
			for( int i = 0; i < 4; ++i ) {
				south.c[i].pts[0] = Point3( H[i].x, H[i].y, 0 );
				south.c[i].pts[1] = Point3( H[i].x, H[i].y, -kappa );
				south.c[i].pts[2] = Point3( kappa * H[i].x, kappa * H[i].y, -1 );
				south.c[i].pts[3] = southPole;
			}
			out.push_back( RotateTestNet( south, rot ) );
		}
	}
	return out;
}

//! Builds an 8-patch polar bilinear octahedron (4 top triangular bilinear
//! patches with a collapsed side at (0,0,1) + 4 bottom triangular bilinear
//! patches with a collapsed side at (0,0,-1)), or just the 4-patch top
//! pyramid when `topPyramidOnly` is true.
static std::vector<BilinearPatch> MakePolarBilinearOctahedron( bool outward, bool topPyramidOnly = false )
{
	static const Point3 kEq[4] = {
		Point3( 1, 0, 0 ), Point3( 0, 1, 0 ), Point3( -1, 0, 0 ), Point3( 0, -1, 0 )
	};
	const Point3 northPole( 0, 0, 1 );
	const Point3 southPole( 0, 0, -1 );
	std::vector<BilinearPatch> out;
	for( int w = 0; w < 4; ++w ) {
		const Point3& e0 = outward ? kEq[( w + 1 ) & 3] : kEq[w];
		const Point3& e1 = outward ? kEq[w] : kEq[( w + 1 ) & 3];
		// BilinearPatch layout: pts[0]=(0,0), pts[1]=(0,1), pts[2]=(1,0), pts[3]=(1,1).
		// Collapse v = 0 (pts[0] == pts[2] == northPole), v = 1 on equator (pts[1] = e0, pts[3] = e1).
		BilinearPatch top;
		top.pts[0] = northPole;
		top.pts[1] = e0;
		top.pts[2] = northPole;
		top.pts[3] = e1;
		out.push_back( top );
		if( !topPyramidOnly ) {
			// Collapse v = 1 (pts[1] == pts[3] == southPole), v = 0 on equator (pts[0] = e0, pts[2] = e1).
			BilinearPatch bot;
			bot.pts[0] = e0;
			bot.pts[1] = southPole;
			bot.pts[2] = e1;
			bot.pts[3] = southPole;
			out.push_back( bot );
		}
	}
	return out;
}

static void TestPolarPatchSets_DL624()
{
	std::cout << "J. DL-624: polar patch sets (degenerate collapsed sides / pole fans)" << std::endl;

	// 1. Closed polar UV Bezier sphere across all 4 collapsed-side rotations (s = 0, 1, 2, 3),
	//    plus inward-wound twin and open polar hemisphere / partial fan / single triangular patch.
	for( int rot = 0; rot < 4; ++rot ) {
		const std::vector<BezierPatch> sphOut = MakePolarUVSphere( true, rot, false );
		const Verdict vOut = CertBezier( sphOut );
		Check( vOut.watertight && !vOut.open && GeometricUtilities::BezierPatchSetOpenness( sphOut ) == 0,
			"J1: outward polar UV Bezier sphere certifies watertight (rot=" + std::to_string( rot ) + ")" );

		const std::vector<BezierPatch> sphIn = MakePolarUVSphere( false, rot, false );
		const Verdict vIn = CertBezier( sphIn );
		Check( !vIn.watertight && !vIn.open &&
			ReasonIs( vIn, "closed surface is inward-wound or encloses no resolvable volume" ) &&
			GeometricUtilities::BezierPatchSetOpenness( sphIn ) == 0,
			"J1: inward polar UV Bezier sphere is topologically closed (0) and refused by signed volume (rot=" + std::to_string( rot ) + ")" );

		const std::vector<BezierPatch> dome = MakePolarUVSphere( true, rot, true );
		const Verdict vDome = CertBezier( dome );
		Check( !vDome.watertight && vDome.open && GeometricUtilities::BezierPatchSetOpenness( dome ) == 1,
			"J1: polar UV Bezier hemisphere dome certifies as an open sheet (rot=" + std::to_string( rot ) + ")" );

		// Single triangular Bezier patch (1 collapsed side, 3 free sides):
		const std::vector<BezierPatch> tri( 1, dome[0] );
		const Verdict vTri = CertBezier( tri );
		Check( !vTri.watertight && vTri.open && GeometricUtilities::BezierPatchSetOpenness( tri ) == 1,
			"J1: single triangular Bezier patch with one collapsed side certifies open (rot=" + std::to_string( rot ) + ")" );
	}
	{
		// Open 3-wedge polar fan (radial boundary seams at the start and end of the fan are free):
		std::vector<BezierPatch> fan3 = MakePolarUVSphere( true, 0, true );
		fan3.pop_back();
		const Verdict vFan3 = CertBezier( fan3 );
		Check( !vFan3.watertight && vFan3.open && GeometricUtilities::BezierPatchSetOpenness( fan3 ) == 1,
			"J1: 3-wedge partial polar fan with free radial seams certifies as an open sheet" );
	}

	// 2. Polar bilinear patch sets: closed outward octahedron, inward octahedron, and open 4-wedge pyramid.
	{
		const std::vector<BilinearPatch> octOut = MakePolarBilinearOctahedron( true, false );
		const Verdict vOctOut = CertBilinear( octOut );
		Check( vOctOut.watertight && !vOctOut.open && GeometricUtilities::BilinearPatchSetOpenness( octOut ) == 0,
			"J2: outward polar bilinear octahedron certifies watertight" );

		const std::vector<BilinearPatch> octIn = MakePolarBilinearOctahedron( false, false );
		const Verdict vOctIn = CertBilinear( octIn );
		Check( !vOctIn.watertight && !vOctIn.open &&
			ReasonIs( vOctIn, "closed surface is inward-wound or encloses no resolvable volume" ) &&
			GeometricUtilities::BilinearPatchSetOpenness( octIn ) == 0,
			"J2: inward polar bilinear octahedron is topologically closed (0) and refused by signed volume" );

		const std::vector<BilinearPatch> pyr = MakePolarBilinearOctahedron( true, true );
		const Verdict vPyr = CertBilinear( pyr );
		Check( !vPyr.watertight && vPyr.open && GeometricUtilities::BilinearPatchSetOpenness( pyr ) == 1,
			"J2: open 4-wedge polar bilinear pyramid certifies as an open sheet" );

		// Mixed polar-side + regular-corner fan at the pole (0,0,1): replace wedge 0 with a 4-corner
		// bilinear quad whose regular corner pts[0] is at (0,0,1) while wedges 1..3 have collapsed sides at (0,0,1).
		std::vector<BilinearPatch> mixedPyr = pyr;
		mixedPyr[0].pts[0] = Point3( 0, 0, 1 );
		mixedPyr[0].pts[1] = Point3( 0, 1, 0 );
		mixedPyr[0].pts[2] = Point3( 1, 0, 0 );
		mixedPyr[0].pts[3] = Point3( 1, 1, -0.5 );
		const Verdict vMixed = CertBilinear( mixedPyr );
		Check( !vMixed.watertight && vMixed.open && GeometricUtilities::BilinearPatchSetOpenness( mixedPyr ) == 1,
			"J2: mixed fan of 3 collapsed-side wedges and 1 regular-corner quad at a shared pole certifies open" );
	}

	// 3. Shipped teapot polar lid (patches 20..27 of teapot.bilinear and teapot.bezier).
	{
		const std::vector<BilinearPatch> teaBil = ReadBilinearFile( "models/raw/teapot.bilinear" );
		Check( teaBil.size() == 28, "J3: models/raw/teapot.bilinear has 28 patches" );
		if( teaBil.size() == 28 ) {
			const std::vector<BilinearPatch> lidBil( teaBil.begin() + 20, teaBil.begin() + 28 );
			const Verdict vLidBil = CertBilinear( lidBil );
			std::cout << "    teapot.bilinear polar lid (patches 20..27): " << Describe( vLidBil ) << std::endl;
			Check( !vLidBil.watertight && vLidBil.open && GeometricUtilities::BilinearPatchSetOpenness( lidBil ) == 1,
				"J3: teapot.bilinear 8-patch polar lid (4-wedge pole fan at (0,3,0) + 4-patch skirt) certifies open" );
		}

		const std::vector<BezierPatch> teaBez = ReadBezierFile( "models/raw/teapot.bezier" );
		Check( teaBez.size() == 28, "J3: models/raw/teapot.bezier has 28 patches" );
		if( teaBez.size() == 28 ) {
			std::vector<BezierPatch> lidBez( teaBez.begin() + 20, teaBez.begin() + 28 );
			// Unmodified teapot.bezier lid has side j=0 (s=0) with c[0].pts[0]==c[3].pts[0]==(0,3,0) and
			// c[1].pts[0], c[2].pts[0] offset by 0.002 (a non-constant loop side):
			const Verdict vRawLid = CertBezier( lidBez );
			Check( !vRawLid.watertight && !vRawLid.open && ReasonIs( vRawLid, kCollapsed ) &&
				GeometricUtilities::BezierPatchSetOpenness( lidBez ) == -1,
				"J3: unmodified teapot.bezier lid is refused for its 0.002 non-constant loop side at the pole" );
			// Snapping side j=0 on the 4 lid knob patches to the exact pole (0,3,0) yields a valid 4-wedge
			// pole fan (even though column j=2 of the input control polygon sits on the axis at (0,2.7,0),
			// the cubic curve r(v) = 2.4*v*(1-v)^2 + 0.2*v^3 > 0 for all v in (0,1], so every sub-patch net
			// has strictly positive radius away from the pole and certifies as an open sheet!):
			for( int p = 0; p < 4; ++p ) {
				for( int i = 0; i < 4; ++i ) {
					lidBez[p].c[i].pts[0] = Point3( 0, 3, 0 );
				}
			}
			const Verdict vSnappedLid = CertBezier( lidBez );
			std::cout << "    teapot.bezier snapped polar lid (patches 20..27): " << Describe( vSnappedLid ) << std::endl;
			Check( !vSnappedLid.watertight && vSnappedLid.open && GeometricUtilities::BezierPatchSetOpenness( lidBez ) == 1,
				"J3: teapot.bezier 8-patch polar lid with exact pole at (0,3,0) certifies open" );
		}
	}

	// 4. Adversarial refusal cases for degenerate sides and pole fans.
	{
		// 4a. Patch with TWO collapsed sides (both j = 0 and j = 3 collapsed):
		BezierPatch twoPoles = MakePolarUVSphere( true, 0, true )[0];
		for( int i = 0; i < 4; ++i ) {
			twoPoles.c[i].pts[3] = Point3( 0, 0, -1 );
		}
		const std::vector<BezierPatch> twoPolesVec( 1, twoPoles );
		const Verdict vTwoPoles = CertBezier( twoPolesVec );
		Check( !vTwoPoles.watertight && !vTwoPoles.open && ReasonIs( vTwoPoles, kCollapsed ) &&
			GeometricUtilities::BezierPatchSetOpenness( twoPolesVec ) == -1,
			"J4a: a single patch with two collapsed sides is refused at kCollapsed" );

		// 4b. Patch with a non-constant loop side (c[0] == c[3] but c[1] != c[0]):
		BezierPatch loopSide = MakePolarUVSphere( true, 0, true )[0];
		loopSide.c[1].pts[0] = Point3( 0.01, 0, 1 );
		const std::vector<BezierPatch> loopVec( 1, loopSide );
		const Verdict vLoop = CertBezier( loopVec );
		Check( !vLoop.watertight && !vLoop.open && ReasonIs( vLoop, kCollapsed ) &&
			GeometricUtilities::BezierPatchSetOpenness( loopVec ) == -1,
			"J4b: a patch whose collapsed-side interior control point wanders off the pole is refused at kCollapsed" );

		// 4c. Polar patch where a control point outside the collapsed side coincides with the pole:
		BezierPatch interiorAtPole = MakePolarUVSphere( true, 0, true )[0];
		interiorAtPole.c[1].pts[1] = Point3( 0, 0, 1 );
		const std::vector<BezierPatch> intPoleVec( 1, interiorAtPole );
		const Verdict vIntPole = CertBezier( intPoleVec );
		Check( !vIntPole.watertight && !vIntPole.open && ReasonIs( vIntPole, kCollapsed ) &&
			GeometricUtilities::BezierPatchSetOpenness( intPoleVec ) == -1,
			"J4c: a polar patch with a non-pole control point equal to the pole is refused at kCollapsed" );

		// 4d. Tip-to-tip double cone (two closed 4-wedge cones sharing the pole (0,0,1)):
		std::vector<BezierPatch> doubleCone = MakePolarUVSphere( true, 0, true );
		const std::vector<BezierPatch> upperCone = MakePolarUVSphere( true, 0, true );
		for( BezierPatch p : upperCone ) {
			for( int i = 0; i < 4; ++i ) {
				for( int j = 1; j < 4; ++j ) {
					p.c[i].pts[j].z = Scalar( 2 ) - p.c[i].pts[j].z;
				}
			}
			doubleCone.push_back( p );
		}
		const Verdict vDoubleCone = CertBezier( doubleCone );
		Check( !vDoubleCone.watertight && !vDoubleCone.open &&
			ReasonIs( vDoubleCone, "non-manifold pole or pinch vertex" ) &&
			GeometricUtilities::BezierPatchSetOpenness( doubleCone ) == -1,
			"J4d: tip-to-tip double cone sharing a pole is refused as a non-manifold pole" );

		// 4e. Closed 4-wedge cone + extra 5th flap touching the pole:
		std::vector<BezierPatch> conePlusFlap = MakePolarUVSphere( true, 0, true );
		conePlusFlap.push_back( doubleCone[4] );
		const Verdict vFlap = CertBezier( conePlusFlap );
		Check( !vFlap.watertight && !vFlap.open &&
			ReasonIs( vFlap, "non-manifold pole or pinch vertex" ) &&
			GeometricUtilities::BezierPatchSetOpenness( conePlusFlap ) == -1,
			"J4e: closed polar cone with an extra flap at the pole is refused as a non-manifold pole" );

		// 4f. Bowtie polar fan (two opposite wedges sharing only the pole (0,0,1), no shared radial seam):
		const std::vector<BezierPatch> dome4 = MakePolarUVSphere( true, 0, true );
		const std::vector<BezierPatch> bowtie = { dome4[0], dome4[2] };
		const Verdict vBowtie = CertBezier( bowtie );
		Check( !vBowtie.watertight && !vBowtie.open &&
			ReasonIs( vBowtie, "non-manifold pole or pinch vertex" ) &&
			GeometricUtilities::BezierPatchSetOpenness( bowtie ) == -1,
			"J4f: two opposite polar wedges touching only at the pole are refused as a non-manifold pole" );

		// 4g. Polar cusp fold across a shared radial seam at the pole (polar analogue of MakeCuspFold):
		// Both patches have side s=3 (i=0) collapsed to the pole (0,0,0), and share the radial seam at
		// j=3 of `a` / j=0 of `b` along (u, u, 0); `b` turns straight back over `a` with z = c * u * v^2,
		// so non-seam sub-patches are strictly separated in z > 0 and only the paired CrossConeProof
		// along the shared radial seam refuses the cusp fold.
		{
			BezierPatch a, b;
			const Scalar c = Scalar( 0.2 );
			const Scalar zc[4] = { 0, 0, c / 3, c };
			for( int i = 0; i < 4; ++i ) {
				const Scalar u = Scalar( i ) / 3;
				for( int j = 0; j < 4; ++j ) {
					const Scalar v = Scalar( j ) / 3;
					a.c[i].pts[j] = Point3( u, u * v, 0 );
					b.c[i].pts[j] = Point3( u, u * ( 1 - Scalar( 0.5 ) * v ), u * zc[j] );
				}
			}
			const std::vector<BezierPatch> foldedSeam = { a, b };
			const Verdict vFoldedSeam = CertBezier( foldedSeam );
			Check( !vFoldedSeam.watertight && !vFoldedSeam.open && ReasonIs( vFoldedSeam, kConePair ) &&
				GeometricUtilities::BezierPatchSetOpenness( foldedSeam ) == 1,
				"J4g: two polar wedges with a cusp fold across their shared radial seam are refused by the paired CrossConeProof" );
		}

		// 4h. Polar helicoid fan around the pole O=(0,0,0) (polar analogue of Section D's MakeFan):
		// 5 quarter-turn polar bilinear wedges with v=0 collapsed at O; when `cross` is false the 1.25-turn
		// polar helicoid fan certifies open, and when `cross` is true the 5th wedge drops through the 1st wedge
		// at the pole and is refused by CornerPairSeparated.
		auto makePolarHelicoidFan = []( bool cross ) {
			const Scalar pi = 3.14159265358979323846;
			const Scalar R = 1, h = 0.1;
			const Point3 O( 0, 0, 0 );
			auto rim = [&]( int q, Scalar z ) {
				const Scalar th = Scalar( q ) * pi / 2;
				return Point3( R * std::cos( th ), R * std::sin( th ), z );
			};
			std::vector<BilinearPatch> f;
			for( int k = 0; k < 5; ++k ) {
				const Scalar z0 = h * Scalar( k ) * pi / 2 * R;
				Scalar z1 = h * Scalar( k + 1 ) * pi / 2 * R;
				if( k == 4 && cross ) {
					z1 = -0.8;
				}
				BilinearPatch w;
				w.pts[0] = O;
				w.pts[1] = rim( k, z0 );
				w.pts[2] = O;
				w.pts[3] = rim( k + 1, z1 );
				f.push_back( w );
			}
			return f;
		};
		const std::vector<BilinearPatch> polarFanOk = makePolarHelicoidFan( false );
		const Verdict vPolarFanOk = CertBilinear( polarFanOk );
		Check( !vPolarFanOk.watertight && vPolarFanOk.open &&
			GeometricUtilities::BilinearPatchSetOpenness( polarFanOk ) == 1,
			"J4h: embedded 1.25-turn polar bilinear helicoid fan around a pole certifies open" );

		const std::vector<BilinearPatch> polarFanCross = makePolarHelicoidFan( true );
		const Verdict vPolarFanCross = CertBilinear( polarFanCross );
		Check( !vPolarFanCross.watertight && !vPolarFanCross.open &&
			ReasonIs( vPolarFanCross, "sub-patches sharing one vertex are not provably separated there" ) &&
			GeometricUtilities::BilinearPatchSetOpenness( polarFanCross ) == 1,
			"J4h: polar bilinear fan whose 5th wedge crosses the 1st at the pole is refused by CornerPairSeparated" );
	}

	// 5. End-to-end CSG over polar Bezier and bilinear solids and sheets.
	{
		const std::string polarSpherePath = WriteBezier( "dl624_polar_sphere.bezier", MakePolarUVSphere( true, 0, false ) );
		const std::string polarDomePath = WriteBezier( "dl624_polar_dome.bezier", MakePolarUVSphere( true, 0, true ) );
		const std::string polarOctPath = WriteBilinear( "dl624_polar_oct.bilinear", MakePolarBilinearOctahedron( true, false ) );

		std::string scene = Materials( false ) +
			"bezierpatch_geometry\n{\n name g_psphere\n file " + polarSpherePath + "\n}\n"
			"bezierpatch_geometry\n{\n name g_pdome\n file " + polarDomePath + "\n}\n"
			"bilinearpatch_geometry\n{\n name g_poct\n file " + polarOctPath + "\n}\n"
			"box_geometry\n{\n name g_box\n width 2\n height 2\n depth 2\n}\n";
		auto obj = [&]( const std::string& name, const std::string& geom, const std::string& extra = "" ) {
			scene += "standard_object\n{\n name " + name + "\n geometry " + geom + "\n material glass\n" + extra + "}\n";
		};
		auto csg = [&]( const std::string& name, const std::string& a, const std::string& b, const std::string& op ) {
			scene += "csg_object\n{\n name " + name + "\n obja " + a + "\n objb " + b + "\n operation " + op + "\n material glass\n}\n";
		};
		// Solid box [-1,1]^3 minus polar Bezier sphere centred at (0, 0, 1): carves a polar-sphere dimple in the top face.
		obj( "box_a", "g_box" );
		obj( "psph_b", "g_psphere", " position 0 0 1\n" );
		csg( "box_minus_psphere", "box_a", "psph_b", "subtraction" );

		// Polar Bezier dome trimmed by a solid box shifted to x in [0, 2]:
		obj( "pdome_a", "g_pdome" );
		obj( "box_trim", "g_box", " position 1 0 0\n" );
		csg( "pdome_inter_box", "pdome_a", "box_trim", "intersection" );

		// Polar Bezier dome minus polar bilinear octahedron centered at the north pole (0, 0, 1) with scale 0.6:
		obj( "pdome_c", "g_pdome" );
		obj( "poct_punch", "g_poct", " position 0 0 1\n scale 0.6 0.6 0.6\n" );
		csg( "pdome_minus_poct", "pdome_c", "poct_punch", "subtraction" );

		LoadedScene loaded( scene );
		const IObject* bmp = loaded.Object( "box_minus_psphere" );
		const IObject* pib = loaded.Object( "pdome_inter_box" );
		const IObject* pmp = loaded.Object( "pdome_minus_poct" );

		Check( bmp && AsCSG( bmp )->IsCertifiedWatertight() && !AsCSG( bmp )->IsProvablyOpenSheet(),
			"J5: box minus polar Bezier sphere is a certified watertight solid" );
		Check( pib && AsCSG( pib )->IsProvablyOpenSheet() && !AsCSG( pib )->IsCertifiedWatertight(),
			"J5: polar Bezier dome intersect solid box is a certified open sheet" );
		Check( pmp && AsCSG( pmp )->IsProvablyOpenSheet() && !AsCSG( pmp )->IsCertifiedWatertight(),
			"J5: polar Bezier dome minus polar bilinear octahedron is a certified open sheet" );

		if( bmp && pib && pmp ) {
			const Vector3 down( 0, 0, -1 );
			// Ray down along (0.2, 0.1, 3) into box_minus_psphere hits the carved dimple floor near z ~ 0.025 (below z = 1):
			const RayIntersection hDimple = Cast( *bmp, Point3( 0.2, 0.1, 3 ), down );
			Check( hDimple.geometric.bHit &&
				hDimple.geometric.ptIntersection.z < 0.2 &&
				hDimple.geometric.ptIntersection.z > -0.1 &&
				std::fabs( hDimple.geometric.ptExit.z + 1.0 ) < 1e-6 &&
				Vector3Ops::Dot( hDimple.geometric.vNormal, down ) < 0 &&
				Coherent( hDimple ),
				"J5: ray into box minus polar Bezier sphere hits the carved dimple floor and exits the box bottom" );

			// Ray down into pdome_inter_box hits on the +x side (x = 0.4) and misses on the -x side (x = -0.4):
			const RayIntersection hKeep = Cast( *pib, Point3( 0.4, 0.1, 3 ), down );
			const RayIntersection hTrimmed = Cast( *pib, Point3( -0.4, 0.1, 3 ), down );
			Check( hKeep.geometric.bHit && hKeep.geometric.range2 == RISE_INFINITY && !hTrimmed.geometric.bHit,
				"J5: polar Bezier dome intersect box keeps the +x half and trims the -x half" );

			// Ray down into pdome_minus_poct misses near the pole (x = 0.1, y = 0.1, inside the octahedron punch)
			// and hits near the outer skirt (x = 0.7, y = 0.1, outside the octahedron punch):
			const RayIntersection hPunched = Cast( *pmp, Point3( 0.1, 0.1, 3 ), down );
			const RayIntersection hSkirt = Cast( *pmp, Point3( 0.7, 0.1, 3 ), down );
			Check( !hPunched.geometric.bHit && hSkirt.geometric.bHit && hSkirt.geometric.range2 == RISE_INFINITY,
				"J5: polar Bezier dome minus polar bilinear octahedron punches out the pole cap and keeps the skirt" );
		}

		std::remove( polarSpherePath.c_str() );
		std::remove( polarDomePath.c_str() );
		std::remove( polarOctPath.c_str() );
	}
}

#ifndef PATCHSET_NO_MAIN
int main()
{
	std::cout << "=== PatchSetCertificationTest (DL-616 / DL-617 / DL-621 / DL-624) ===" << std::endl;
	TestClosedSolids();
	TestCrossingStrip();
	TestTJunction();
	TestFanCrossingAtVertex();
	TestOpenSheetCSG();
	TestNonConvexMeshUnion();
	TestCostAndShippedModels();
	TestProofStepCoverage();
	TestMeshAndDisplacedCSG_DL621();
	TestPolarPatchSets_DL624();
	std::cout << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount ? 1 : 0;
}
#endif
