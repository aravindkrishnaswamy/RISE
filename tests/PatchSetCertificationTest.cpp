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
#include "../src/Library/Geometry/BezierPatchGeometry.h"

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
	// P3-b: the same trimming geometry when the solid is closed but NOT
	// certified (the T-junction cube).
	obj( "ts1", "sheet_half" ); obj( "tc1", "tcube" ); csg( "inter_tcube", "ts1", "tc1", "intersection" );
	obj( "ts2", "sheet_half" ); obj( "tc2", "tcube" ); csg( "minus_tcube", "ts2", "tc2", "subtraction" );
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
		if( it && mt ) {
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
		Check( !p.empty() && !v.watertight && !v.open && ReasonIs( v, kCollapsed ),
			std::string( "G: shipped model refused at the collapsed-side topology step: " ) + m );
	}
	for( const char* m : { "models/raw/teapot.bilinear", "models/raw/test.bilinear" } ) {
		const std::vector<BilinearPatch> p = ReadBilinearFile( m );
		const Verdict v = CertBilinear( p );
		std::cout << "    " << m << " (" << p.size() << " patches): " << Describe( v ) << std::endl;
		const bool single = std::strcmp( m, "models/raw/test.bilinear" ) == 0;
		Check( !p.empty() && ( single ? ( v.open && v.negative && ReasonIs( v, "" ) )
		                              : ( !v.watertight && !v.open && ReasonIs( v, kCollapsed ) ) ),
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

#ifndef PATCHSET_NO_MAIN
int main()
{
	std::cout << "=== PatchSetCertificationTest (DL-616 / DL-617) ===" << std::endl;
	TestClosedSolids();
	TestCrossingStrip();
	TestTJunction();
	TestFanCrossingAtVertex();
	TestOpenSheetCSG();
	TestCostAndShippedModels();
	TestProofStepCoverage();
	std::cout << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount ? 1 : 0;
}
#endif
