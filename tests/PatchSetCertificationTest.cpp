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
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "../src/Library/Polygon.h"
#include "../src/Library/Utilities/GeometricUtilities.h"
#include "../src/Library/Objects/CSGObject.h"

using namespace RISE;
using namespace RISE::Implementation;

struct Verdict
{
	bool watertight = false;
	bool open = false;
	bool negative = false;
	const char* reason = "";
};

static Verdict CertBezier( const std::vector<BezierPatch>& p )
{
	Verdict v;
#ifdef DL616_BASELINE_SIGNATURE
	GeometricUtilities::CertifyBezierPatchSet( p, v.watertight, v.open, v.negative );
#else
	GeometricUtilities::CertifyBezierPatchSet( p, v.watertight, v.open, v.negative, &v.reason );
#endif
	return v;
}

static Verdict CertBilinear( const std::vector<BilinearPatch>& p )
{
	Verdict v;
#ifdef DL616_BASELINE_SIGNATURE
	GeometricUtilities::CertifyBilinearPatchSet( p, v.watertight, v.open, v.negative );
#else
	GeometricUtilities::CertifyBilinearPatchSet( p, v.watertight, v.open, v.negative, &v.reason );
#endif
	return v;
}

static std::string Describe( const Verdict& v )
{
	return std::string( v.watertight ? "watertight" : ( v.open ? ( v.negative ? "open(neg)" : "open" ) : "refused" ) ) +
		( v.reason && *v.reason ? std::string( " [" ) + v.reason + "]" : std::string() );
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

static void TestTJunction()
{
	std::cout << "C. T-junction" << std::endl;
	std::vector<BilinearPatch> c = MakeBilinearCube();
	// Replace +Z with two half quads split at x = 0.5: the -Y and +Y faces'
	// top sides each meet two half-sides -> T-junctions at (0.5, 0, 1) and
	// (0.5, 1, 1).  The body still encloses the cube.
	c[1] = MakeQuad( Point3(0,0,1), Point3(0.5,0,1), Point3(0.5,1,1), Point3(0,1,1) );
	c.push_back( MakeQuad( Point3(0.5,0,1), Point3(1,0,1), Point3(1,1,1), Point3(0.5,1,1) ) );
	const Verdict v = CertBilinear( c );
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
	const std::string sheetZ =
		"clippedplane_geometry\n{\n name sheet_z\n pta -3 -3 0\n ptb 3 -3 0\n ptc 3 3 0\n ptd -3 3 0\n}\n";
	const std::string sheetX =
		"clippedplane_geometry\n{\n name sheet_x\n pta -1.5 -3 -3\n ptb -1.5 3 -3\n ptc -1.5 3 3\n ptd -1.5 -3 3\n}\n";
	std::string scene = Materials( false ) + sheetZ + sheetX +
		"bezierpatch_geometry\n{\n name ball\n file " + spherePath + "\n}\n"
		"bilinearpatch_geometry\n{\n name frame\n file " + framePath + "\n}\n"
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
}

//////////////////////////////////////////////////////////////////////
// G. cost and shipped models
//////////////////////////////////////////////////////////////////////

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
	const char* models[] = { "models/raw/teapot.bezier", "models/bezier/aphrodite.bezier",
		"models/bezier/f16.bezier", "models/bezier/balanza.bezier" };
	for( const char* m : models ) {
		const std::vector<BezierPatch> p = ReadBezierFile( m );
		const auto t0 = std::chrono::steady_clock::now();
		const Verdict v = CertBezier( p );
		const auto t1 = std::chrono::steady_clock::now();
		const double ms = std::chrono::duration<double, std::milli>( t1 - t0 ).count();
		std::cout << "    " << m << " (" << p.size() << " patches): " << Describe( v ) << " in " << ms << " ms" << std::endl;
		Check( !p.empty(), std::string( "G: shipped model loads: " ) + m );
	}
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
	std::cout << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount ? 1 : 0;
}
#endif
