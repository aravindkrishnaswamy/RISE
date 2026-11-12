//////////////////////////////////////////////////////////////////////
//
//  LuminarySamplerAreaTest.cpp - DL-459 / DL-460: three analytic
//    luminary geometries whose OBJECT-space UniformRandomPoint was not
//    area-uniform and/or whose GetArea was not the area of the surface
//    they trace, so pdfPosition = 1/GetArea() was wrong at ANY transform.
//
//    ClippedPlaneGeometry (DL-460): GetArea was |e0| |e1| (exact for a
//      rectangle only) and the sampler uniform in (u, v) (area-uniform
//      for a parallelogram only).  A coplanar dart FOLDS: its folded
//      sheet covers points the positive sheet already covers, and the
//      intersector reports such a point once, so the luminary is the
//      IMAGE (area = integral of the positive part of the area element).
//    BezierPatchGeometry (DL-459): patch picked by its midpoint area
//      element, then (u, v) uniform; GetArea the midpoint estimate.
//    SDFGeometry (DL-459): chord-triangle sample projected onto the zero
//      set, CDF weighted by the per-triangle mean projection Jacobian,
//      leaving the within-triangle Jacobian (and the chord tilt) as a
//      density distortion.
//
//    A  GetArea against closed forms / independent quadrature (the dart:
//       an independent pixel-occupancy count of the image), and the
//       rectangle's legacy arithmetic bit for bit.
//    B  region shares of the samples against independent area shares
//       (5 sigma), the SDF by a chi-square over equal-area cells.
//    C  renders: PT vs BDPT vs VCM and vs the same surface authored as an
//       exactly-sampled triangle mesh where one exists (salted repeats,
//       3 combined standard errors).
//
//  Author: Claude (debt-dl459)
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cinttypes>
#include <cstdarg>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <cmath>
#include <string>
#include <functional>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <random>
#ifdef _WIN32
#include <process.h>
static int RiseTestPid() { return _getpid(); }
#else
#include <unistd.h>
static int RiseTestPid() { return static_cast<int>( ::getpid() ); }
#endif

#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IScene.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/Color/Color_Template.h"
#include "../src/Library/Utilities/SobolSampler.h"
#include "../src/Library/Geometry/ClippedPlaneGeometry.h"
#include "../src/Library/Geometry/BezierPatchGeometry.h"
#include "../src/Library/Geometry/SDFGeometry.h"
#include "../src/Library/Utilities/GeometricUtilities.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Rendering/Rasterizer.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Cameras/OrthographicCamera.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

static unsigned int g_seedBase = 4590u;
static unsigned int g_renderIndex = 0;
static int passCount = 0;
static int failCount = 0;
static const double kPi = 3.14159265358979323846;

static void Check( bool condition, const std::string& testName )
{
	if( condition ) {
		passCount++;
		std::cout << "  PASS: " << testName << std::endl;
	} else {
		failCount++;
		std::cout << "  FAIL: " << testName << std::endl;
	}
}

static std::string Fmt( const char* f, ... )
{
	char buf[16384];
	va_list ap;
	va_start( ap, f );
	std::vsnprintf( buf, sizeof(buf), f, ap );
	va_end( ap );
	return std::string( buf );
}

static double RadInv( unsigned b, unsigned i ) { double f = 1.0/b, r = 0; while( i ) { r += f*(i%b); i /= b; f /= b; } return r; }

static Point3 Prand( unsigned i, unsigned N ) { return Point3( ( i + 0.5 ) / N, RadInv( 3, i ), RadInv( 5, i ) ); }

//////////////////////////////////////////////////////////////////////
// Independent surface models (no library evaluation)
//////////////////////////////////////////////////////////////////////
typedef std::function<void( double u, double v, double* p )> Param;

static void Cross( const double* a, const double* b, double* c ) { c[0] = a[1]*b[2]-a[2]*b[1]; c[1] = a[2]*b[0]-a[0]*b[2]; c[2] = a[0]*b[1]-a[1]*b[0]; }
static double Norm( const double* a ) { return std::sqrt( a[0]*a[0]+a[1]*a[1]+a[2]*a[2] ); }

//! |dp/du x dp/dv| by central differences of the parametric map.
static double JacFD( const Param& p, double u, double v )
{
	const double h = 1e-6;
	double a[3], b[3], c[3], d[3];
	p( u + h, v, a ); p( u - h, v, b ); p( u, v + h, c ); p( u, v - h, d );
	double tu[3], tv[3], n[3];
	for( int k = 0; k < 3; k++ ) { tu[k] = ( a[k] - b[k] ) / ( 2*h ); tv[k] = ( c[k] - d[k] ) / ( 2*h ); }
	Cross( tu, tv, n );
	return Norm( n );
}

//! Midpoint quadrature of the area element, optionally restricted to a
//! world region; `signedPos` integrates only where n0 . (tu x tv) > 0.
static double ParamArea( const Param& p, int N, std::function<bool( const double* )> region = nullptr, double* regionArea = nullptr )
{
	double all = 0, reg = 0;
	for( int j = 0; j < N; j++ ) for( int i = 0; i < N; i++ ) {
		const double u = ( i + 0.5 ) / N, v = ( j + 0.5 ) / N;
		const double J = JacFD( p, u, v );
		all += J;
		if( region ) { double x[3]; p( u, v, x ); if( region( x ) ) reg += J; }
	}
	if( regionArea ) *regionArea = reg / ( double( N ) * N );
	return all / ( double( N ) * N );
}

static Param BilinearParam( const Point3 c[4] )
{
	return [c]( double u, double v, double* p ) {
		const double w0 = (1-u)*(1-v), w1 = u*(1-v), w2 = u*v, w3 = (1-u)*v;
		p[0] = w0*c[0].x + w1*c[1].x + w2*c[2].x + w3*c[3].x;
		p[1] = w0*c[0].y + w1*c[1].y + w2*c[2].y + w3*c[3].y;
		p[2] = w0*c[0].z + w1*c[1].z + w2*c[2].z + w3*c[3].z;
	};
}

static Param BezierParam( const BezierPatch& bp )
{
	return [bp]( double u, double v, double* p ) {
		const double bu[4] = { (1-u)*(1-u)*(1-u), 3*u*(1-u)*(1-u), 3*u*u*(1-u), u*u*u };
		const double bv[4] = { (1-v)*(1-v)*(1-v), 3*v*(1-v)*(1-v), 3*v*v*(1-v), v*v*v };
		p[0] = p[1] = p[2] = 0;
		for( int i = 0; i < 4; i++ ) for( int j = 0; j < 4; j++ ) {
			const double w = bu[i]*bv[j];
			p[0] += w*bp.c[i].pts[j].x; p[1] += w*bp.c[i].pts[j].y; p[2] += w*bp.c[i].pts[j].z;
		}
	};
}

static void CheckRel( double got, double expected, double tol, const std::string& label )
{
	const double rel = ( got - expected ) / expected;
	std::cout << "  A " << label << ": GetArea " << got << " expected " << expected << " rel " << rel << "\n";
	Check( std::fabs( rel ) <= tol, label );
}

static void CheckFraction( double got, double expected, unsigned N, const std::string& label )
{
	const double sigma = std::sqrt( expected * ( 1 - expected ) / N );
	std::cout << "  B " << label << ": sample share " << got << " area share " << expected
		<< " z " << ( got - expected ) / sigma << "\n";
	Check( std::fabs( got - expected ) < 5 * sigma, label );
}

template<class G, class Pred>
static double SampleShare( const G* g, Pred inRegion, unsigned N )
{
	unsigned hits = 0;
	for( unsigned i = 0; g && i < N; i++ ) {
		Point3 p; Vector3 n; Point2 uv;
		g->UniformRandomPoint( &p, &n, &uv, Prand( i, N ) );
		const double x[3] = { p.x, p.y, p.z };
		hits += inRegion( x ) ? 1u : 0u;
	}
	return double( hits ) / N;
}

//////////////////////////////////////////////////////////////////////
// Fixtures
//////////////////////////////////////////////////////////////////////
static ClippedPlaneGeometry* MakeQuad( const Point3 c[4] )
{
	const Point3 v[4] = { c[0], c[1], c[2], c[3] };
	return new ClippedPlaneGeometry( v, true );
}

static const Point3 kTrapezoid[4]     = { Point3(-1,-1,0), Point3(1,-1,0), Point3(0.3,1,0), Point3(-0.3,1,0) };
static const Point3 kParallelogram[4] = { Point3(0,0,0), Point3(2,0,0), Point3(3,1,0), Point3(1,1,0) };
static const Point3 kDart[4]          = { Point3(0,0,0), Point3(2,0,0), Point3(0.5,0.5,0), Point3(0,2,0) };
static const Point3 kTwisted[4]       = { Point3(-1,-1,0), Point3(1,-1,0.8), Point3(1.2,1,-0.3), Point3(-1,1,0.6) };
static const Point3 kRectangle[4]     = { Point3(-0.7,-0.4,0.1), Point3(0.9,-0.4,0.1), Point3(0.9,1.3,0.1), Point3(-0.7,1.3,0.1) };

//! A FLAT 2 x 2 square whose cubic parametrization is strongly non-uniform:
//! x(u) = -1 + 2u^3, y(v) = 1 - 2(1-v)^3 -- area 4, midpoint area element 2.25.
static BezierPatch WarpedSquarePatch( double z = 0 )
{
	const double xs[4] = { -1, -1, -1, 1 };
	const double ys[4] = { -1, 1, 1, 1 };
	BezierPatch p;
	for( int i = 0; i < 4; i++ ) for( int j = 0; j < 4; j++ ) p.c[i].pts[j] = Point3( xs[i], ys[j], z );
	return p;
}

//! A curved, unevenly spaced dome patch.
static BezierPatch DomePatch()
{
	const double xs[4] = { -1, -0.2, 0.1, 1 };
	const double ys[4] = { -1, -0.6, 0.5, 1 };
	const double zs[4][4] = { { 0, 0.3, 0.2, 0 }, { 0.2, 1.4, 0.9, 0.1 }, { 0.1, 0.8, 1.2, 0.3 }, { 0, 0.2, 0.4, 0 } };
	BezierPatch p;
	for( int i = 0; i < 4; i++ ) for( int j = 0; j < 4; j++ ) p.c[i].pts[j] = Point3( xs[i] + 3.0, ys[j], zs[i][j] );
	return p;
}

//! Image area of a planar quad by pixel occupancy (independent of any
//! area-element formula): map a dense (u, v) grid into a fine raster of
//! the plane and count occupied pixels.  Also counts the part outside the
//! polygon.
static double ImageAreaByOccupancy( const Point3 c[4], double* outsidePolygon,
	std::function<bool( double, double )> region = nullptr, double* regionArea = nullptr )
{
	const Param p = BilinearParam( c );
	double lo[2] = { 1e30, 1e30 }, hi[2] = { -1e30, -1e30 };
	for( int k = 0; k < 4; k++ ) { lo[0] = std::min( lo[0], c[k].x ); lo[1] = std::min( lo[1], c[k].y ); hi[0] = std::max( hi[0], c[k].x ); hi[1] = std::max( hi[1], c[k].y ); }
	const int R = 3000, S = 7000;
	const double px = ( hi[0] - lo[0] ) / R, py = ( hi[1] - lo[1] ) / R;
	std::vector<unsigned char> occ( size_t( R ) * R, 0 );
	for( int j = 0; j <= S; j++ ) for( int i = 0; i <= S; i++ ) {
		double x[3]; p( double( i ) / S, double( j ) / S, x );
		const int ix = std::min( R - 1, std::max( 0, int( ( x[0] - lo[0] ) / px ) ) );
		const int iy = std::min( R - 1, std::max( 0, int( ( x[1] - lo[1] ) / py ) ) );
		occ[size_t( iy ) * R + ix] = 1;
	}
	size_t n = 0, out = 0, inReg = 0;
	for( int iy = 0; iy < R; iy++ ) for( int ix = 0; ix < R; ix++ ) {
		if( !occ[size_t( iy ) * R + ix] ) continue;
		n++;
		const double X = lo[0] + ( ix + 0.5 ) * px, Y = lo[1] + ( iy + 0.5 ) * py;
		if( region && region( X, Y ) ) inReg++;
		bool inside = false;
		for( int a = 0, b = 3; a < 4; b = a++ ) {
			if( ( c[a].y > Y ) != ( c[b].y > Y ) && X < c[a].x + ( Y - c[a].y ) / ( c[b].y - c[a].y ) * ( c[b].x - c[a].x ) ) inside = !inside;
		}
		if( !inside ) out++;
	}
	if( outsidePolygon ) *outsidePolygon = double( out ) * px * py;
	if( regionArea ) *regionArea = double( inReg ) * px * py;
	return double( n ) * px * py;
}

static bool InPolygon( const Point3 c[4], double X, double Y )
{
	bool inside = false;
	for( int a = 0, b = 3; a < 4; b = a++ ) {
		if( ( c[a].y > Y ) != ( c[b].y > Y ) && X < c[a].x + ( Y - c[a].y ) / ( c[b].y - c[a].y ) * ( c[b].x - c[a].x ) ) inside = !inside;
	}
	return inside;
}

//////////////////////////////////////////////////////////////////////
// A: areas
//////////////////////////////////////////////////////////////////////
static void TestAreas()
{
	std::cout << "A: areas\n";
	{
		ClippedPlaneGeometry* g = MakeQuad( kRectangle );
		const Vector3 e0 = Vector3Ops::mkVector3( kRectangle[1], kRectangle[0] );
		const Vector3 e1 = Vector3Ops::mkVector3( kRectangle[3], kRectangle[0] );
		const Scalar legacy = Vector3Ops::Magnitude( e0 ) * Vector3Ops::Magnitude( e1 );
		Check( g->GetArea() == legacy, "DL-460 rectangle GetArea bit-identical to |e0| |e1|" );
		// The sampler must still be uniform (u, v): the (u, v) it reports is
		// prand's, and the point is the bilinear map's (to rounding -- the
		// library may contract differently).  Bit identity against the
		// pre-DL-460 build is the H hash below (A/B).
		bool same = true;
		for( unsigned i = 0; i < 1000; i++ ) {
			const Point3 r = Prand( i, 1000 );
			Point3 p; Vector3 n; Point2 uv;
			g->UniformRandomPoint( &p, &n, &uv, r );
			const Point3 q = GeometricUtilities::BilinearForward( kRectangle[0], kRectangle[1], kRectangle[2], kRectangle[3], r.x, r.y );
			same = same && std::fabs( p.x - q.x ) + std::fabs( p.y - q.y ) + std::fabs( p.z - q.z ) < 1e-14 && uv.x == r.x && uv.y == r.y;
		}
		Check( same, "DL-460 rectangle sampler is uniform (u, v)" );
		safe_release( g );
	}
	{
		ClippedPlaneGeometry* g = MakeQuad( kParallelogram );
		CheckRel( g->GetArea(), 2.0, 1e-12, "DL-460 parallelogram area = |e0 x e1|" );
		safe_release( g );
	}
	{
		ClippedPlaneGeometry* g = MakeQuad( kTrapezoid );
		CheckRel( g->GetArea(), 2.6, 1e-12, "DL-460 trapezoid area = shoelace" );
		safe_release( g );
	}
	{
		ClippedPlaneGeometry* g = MakeQuad( kDart );
		double out = 0;
		const double img = ImageAreaByOccupancy( kDart, &out );
		std::cout << "    dart: polygon area 1, image by occupancy " << img << " (outside polygon " << out << ")\n";
		CheckRel( g->GetArea(), img, 3e-3, "DL-460 dart area = image area (pixel occupancy)" );
		safe_release( g );
	}
	{
		ClippedPlaneGeometry* g = MakeQuad( kTwisted );
		CheckRel( g->GetArea(), ParamArea( BilinearParam( kTwisted ), 1500 ), 1e-6, "DL-460 twisted (non-planar) quad area" );
		safe_release( g );
	}
	{
		BezierPatchGeometry* g = new BezierPatchGeometry( 2, 8, false );
		g->AddPatch( WarpedSquarePatch() );
		g->Prepare();
		CheckRel( g->GetArea(), 4.0, 1e-9, "DL-459 flat warped Bezier square area" );
		safe_release( g );
	}
	{
		BezierPatchGeometry* g = new BezierPatchGeometry( 2, 8, false );
		g->AddPatch( WarpedSquarePatch() );
		g->AddPatch( DomePatch() );
		g->Prepare();
		CheckRel( g->GetArea(), 4.0 + ParamArea( BezierParam( DomePatch() ), 1500 ), 1e-6, "DL-459 warped square + dome Bezier area" );
		safe_release( g );
	}
	for( unsigned detail : { 8u, 64u } ) {
		std::vector<SDFGeometry::Part> parts;
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
			Point3( 0, 0, 0 ), 0, 0, 0, Vector3( 1, 1, 1 ), 1.0, 0, 0, 0 ) );
		SDFGeometry* g = new SDFGeometry( parts, 512, Scalar( 1e-5 ), detail );
		CheckRel( g->GetArea(), 4*kPi, detail == 8 ? 1e-3 : 3e-5, Fmt( "DL-459 SDF sphere area, sampling_detail %u", detail ) );
		safe_release( g );
	}
}

//////////////////////////////////////////////////////////////////////
// B: uniformity
//////////////////////////////////////////////////////////////////////
static void QuadShare( const Point3 c[4], std::function<bool( const double* )> region, const char* label, unsigned N )
{
	ClippedPlaneGeometry* g = MakeQuad( c );
	double reg = 0;
	const double all = ParamArea( BilinearParam( c ), 1200, region, &reg );
	CheckFraction( SampleShare( g, region, N ), reg / all, N, label );
	safe_release( g );
}

static void TestUniformity()
{
	std::cout << "B: area-uniform samples\n";
	const unsigned N = 60000;
	QuadShare( kTrapezoid, []( const double* x ) { return x[1] > 0.4; }, "DL-460 trapezoid, y > 0.4", N );
	QuadShare( kTwisted, []( const double* x ) { return x[0] + 0.5*x[1] > 0.5; }, "DL-460 twisted quad, x + y/2 > 0.5", N );
	{
		// Dart: the share OUTSIDE the polygon is the folded-over region,
		// covered once in the image; and every sample comes from the
		// positive sheet.
		ClippedPlaneGeometry* g = MakeQuad( kDart );
		double out = 0;
		const double img = ImageAreaByOccupancy( kDart, &out );
		CheckFraction( SampleShare( g, []( const double* x ) { return !InPolygon( kDart, x[0], x[1] ); }, N ), out / img, N, "DL-460 dart, share outside the polygon" );
		double reg = 0;
		const double imgAll = ImageAreaByOccupancy( kDart, nullptr, []( double X, double Y ) { return X + Y < 0.8; }, &reg );
		CheckFraction( SampleShare( g, []( const double* x ) { return x[0] + x[1] < 0.8; }, N ), reg / imgAll, N, "DL-460 dart, x + y < 0.8" );
		bool positive = true;
		for( unsigned i = 0; i < 4000; i++ ) {
			Point3 p; Vector3 n; Point2 uv;
			g->UniformRandomPoint( &p, &n, &uv, Prand( i, 4000 ) );
			const Vector3 tu = GeometricUtilities::BilinearTangentU( kDart[0], kDart[1], kDart[2], kDart[3], uv.y );
			const Vector3 tv = GeometricUtilities::BilinearTangentV( kDart[0], kDart[1], kDart[2], kDart[3], uv.x );
			positive = positive && Vector3Ops::Cross( tu, tv ).z > 0;
		}
		Check( positive, "DL-460 dart samples only the positive (unfolded) sheet" );
		safe_release( g );
	}
	{
		// The INTERSECTOR on a folded dart: over the overshoot the two
		// sheets meet a ray at the same distance, so the reported root must
		// be the positive one (the luminary the sampler draws) -- otherwise
		// a single-sided dart culls / shows the wrong face there and a
		// textured one reads the folded (u, v).  Rays straight down on a
		// grid: every hit must carry a positive-sheet (u, v) and the +z
		// normal, the hit area must be the image area, and rays straight
		// up must all be culled.
		const Point3 v[4] = { kDart[0], kDart[1], kDart[2], kDart[3] };
		ClippedPlaneGeometry* g = new ClippedPlaneGeometry( v, false );
		double out = 0;
		const double img = ImageAreaByOccupancy( kDart, &out );
		const int R = 600;
		const double px = 2.0 / R;
		// Grid points that land EXACTLY on the fold line (area element 0,
		// both preimages coincide; measure zero) are skipped: neither sheet
		// is preferred there and the normal degenerates.
		auto onFold = [&]( const Point2& uv ) {
			const Vector3 tu = GeometricUtilities::BilinearTangentU( kDart[0], kDart[1], kDart[2], kDart[3], uv.y );
			const Vector3 tv = GeometricUtilities::BilinearTangentV( kDart[0], kDart[1], kDart[2], kDart[3], uv.x );
			return std::fabs( Vector3Ops::Cross( tu, tv ).z ) < 1e-12;
		};
		unsigned down = 0, up = 0, wrong = 0, downOut = 0;
		for( int iy = 0; iy < R; iy++ ) for( int ix = 0; ix < R; ix++ ) {
			const double X = ( ix + 0.5 ) * px, Y = ( iy + 0.5 ) * px;
			RayIntersectionGeometric rd( Ray( Point3( X, Y, 1 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
			g->IntersectRay( rd, true, true, false );
			if( rd.bHit ) {
				down++;
				if( !InPolygon( kDart, X, Y ) ) downOut++;
				const Vector3 tu = GeometricUtilities::BilinearTangentU( kDart[0], kDart[1], kDart[2], kDart[3], rd.ptCoord.y );
				const Vector3 tv = GeometricUtilities::BilinearTangentV( kDart[0], kDart[1], kDart[2], kDart[3], rd.ptCoord.x );
				if( !onFold( rd.ptCoord ) && ( !( Vector3Ops::Cross( tu, tv ).z > 0 ) || !( rd.vNormal.z > 0 ) ) ) {
					if( getenv( "DL460_DEBUG" ) ) std::printf( "    wrong X=%.6f Y=%.6f uv=(%.9f %.9f) Jz=%g nz=%g\n", X, Y, rd.ptCoord.x, rd.ptCoord.y, Vector3Ops::Cross( tu, tv ).z, rd.vNormal.z );
					wrong++;
				}
			}
			RayIntersectionGeometric ru( Ray( Point3( X, Y, -1 ), Vector3( 0, 0, 1 ) ), nullRasterizerState );
			g->IntersectRay( ru, true, true, false );
			if( ru.bHit && !onFold( ru.ptCoord ) ) {
				if( getenv( "DL460_DEBUG" ) ) std::printf( "    up X=%.6f Y=%.6f uv=(%.9f %.9f)\n", X, Y, ru.ptCoord.x, ru.ptCoord.y );
				up++;
			}
		}
		const double hitArea = down * px * px, outArea = downOut * px * px;
		std::cout << "  B DL-460 single-sided dart hits from above: area " << hitArea << " (image " << img << "), outside polygon "
			<< outArea << " (" << out << "), wrong-sheet hits " << wrong << ", hits from below " << up << "\n";
		Check( std::fabs( hitArea - img ) < 0.01 * img, "DL-460 single-sided dart: front hits cover the image" );
		Check( std::fabs( outArea - out ) < 0.05 * out, "DL-460 single-sided dart: front hits cover the overshoot" );
		Check( wrong == 0, "DL-460 dart hits report the positive sheet's (u, v) and normal" );
		Check( up == 0, "DL-460 single-sided dart: rays from behind are culled" );
		safe_release( g );
	}
	{
		BezierPatchGeometry* g = new BezierPatchGeometry( 2, 8, false );
		g->AddPatch( WarpedSquarePatch() );
		g->Prepare();
		// Flat 2x2 square: the quadrant x > 0, y > 0 holds 1/4 of the area.
		CheckFraction( SampleShare( g, []( const double* x ) { return x[0] > 0 && x[1] > 0; }, N ), 0.25, N, "DL-459 flat warped Bezier square, quadrant" );
		CheckFraction( SampleShare( g, []( const double* x ) { return x[0] < -0.8; }, N ), 0.1, N, "DL-459 flat warped Bezier square, x < -0.8 strip" );
		safe_release( g );
	}
	{
		BezierPatchGeometry* g = new BezierPatchGeometry( 2, 8, false );
		g->AddPatch( WarpedSquarePatch() );
		g->AddPatch( DomePatch() );
		g->Prepare();
		double reg = 0;
		const double dome = ParamArea( BezierParam( DomePatch() ), 1200, []( const double* x ) { return x[2] > 0.5; }, &reg );
		const double total = 4.0 + dome;
		CheckFraction( SampleShare( g, []( const double* x ) { return x[0] > 1.5; }, N ), dome / total, N, "DL-459 two patches, dome share" );
		CheckFraction( SampleShare( g, []( const double* x ) { return x[0] > 1.5 && x[2] > 0.5; }, N ), reg / total, N, "DL-459 two patches, dome z > 0.5" );
		safe_release( g );
	}
	{
		// SDF unit sphere at the coarsest sampling detail: chi-square of
		// the samples over 24 x 48 EQUAL-AREA cells (uniform in z and phi).
		std::vector<SDFGeometry::Part> parts;
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
			Point3( 0, 0, 0 ), 0, 0, 0, Vector3( 1, 1, 1 ), 1.0, 0, 0, 0 ) );
		SDFGeometry* g = new SDFGeometry( parts, 512, Scalar( 1e-5 ), 8 );
		const int NZ = 24, NP = 48;
		const unsigned M = getenv( "DL459_SDF_M" ) ? unsigned( atoi( getenv( "DL459_SDF_M" ) ) ) : 48000000u;
		std::vector<double> cnt( NZ * NP, 0 );
		unsigned cap = 0;
		// Independent pseudo-random variates: a chi-square needs iid
		// samples (a low-discrepancy set is under-dispersed).
		std::mt19937_64 rng( 0x459 );
		std::uniform_real_distribution<double> U( 0.0, 1.0 );
		for( unsigned i = 0; i < M; i++ ) {
			Point3 p; Vector3 n;
			const double r0 = U( rng ), r1 = U( rng ), r2 = U( rng );
			g->UniformRandomPoint( &p, &n, nullptr, Point3( r0, r1, r2 ) );
			const double r = std::sqrt( p.x*p.x + p.y*p.y + p.z*p.z );
			const double z = std::max( -1.0, std::min( 1.0, p.z / r ) );
			const double ph = std::atan2( p.y, p.x ) + kPi;
			const int iz = std::min( NZ - 1, int( ( z + 1 ) / 2 * NZ ) );
			const int ip = std::min( NP - 1, int( ph / ( 2*kPi ) * NP ) );
			cnt[iz * NP + ip] += 1;
			if( z > 0.5 ) cap++;
		}
		const double e = double( M ) / ( NZ * NP );
		double chi = 0;
		for( double c : cnt ) chi += ( c - e ) * ( c - e ) / e;
		const double dof = NZ * NP - 1;
		const double z = ( chi - dof ) / std::sqrt( 2 * dof );
		std::cout << "  B DL-459 SDF sphere (detail 8): chi2 " << chi << " dof " << dof << " z " << z << "\n";
		Check( std::fabs( z ) < 5, "DL-459 SDF sphere (detail 8) equal-area chi-square" );
		CheckFraction( double( cap ) / M, 0.25, M, "DL-459 SDF sphere (detail 8), cap z > 0.5" );
		safe_release( g );
	}
}

//////////////////////////////////////////////////////////////////////
// C: renders
//////////////////////////////////////////////////////////////////////
class CapturingRasterizerOutput : public virtual IRasterizerOutput, public virtual Reference
{
public:
	struct IntermCapture
	{
		bool fullFrame;
		Rect region;
		double meanAll;
		double meanInterior;
	};
	std::vector<RISEColor> pixels;
	std::vector<IntermCapture> intermediateCaptures;
	bool captureIntermediate = false;
	std::mutex intermediateMutex;
protected:
	virtual ~CapturingRasterizerOutput() {}
public:
	virtual void OutputIntermediateImage( const IRasterImage& img, const Rect* rcRegion ) override
	{
		if( !captureIntermediate ) return;
		const unsigned int x0 = rcRegion ? rcRegion->left : 0;
		const unsigned int y0 = rcRegion ? rcRegion->top : 0;
		const unsigned int x1 = rcRegion ? std::min<unsigned int>( rcRegion->right + 1, img.GetWidth() ) : img.GetWidth();
		const unsigned int y1 = rcRegion ? std::min<unsigned int>( rcRegion->bottom + 1, img.GetHeight() ) : img.GetHeight();
		if( x1 <= x0 || y1 <= y0 ) return;
		double sumAll = 0.0, sumInt = 0.0;
		unsigned int cntAll = 0, cntInt = 0;
		const unsigned int ix0 = rcRegion ? ( x0 + 5 ) : x0;
		const unsigned int iy0 = rcRegion ? ( y0 + 5 ) : y0;
		const unsigned int ix1 = ( rcRegion && x1 > 5 ) ? ( x1 - 5 ) : x0;
		const unsigned int iy1 = ( rcRegion && y1 > 5 ) ? ( y1 - 5 ) : y0;
		for( unsigned int y = y0; y < y1; ++y ) {
			for( unsigned int x = x0; x < x1; ++x ) {
				const RISEColor c = img.GetPEL( x, y );
				const double v = ( c.base.r + c.base.g + c.base.b ) * c.a / 3.0;
				sumAll += v;
				++cntAll;
				if( !rcRegion || ( x >= ix0 && x < ix1 && y >= iy0 && y < iy1 ) ) {
					sumInt += v;
					++cntInt;
				}
			}
		}
		if( cntAll > 0 ) {
			const double mAll = sumAll / double( cntAll );
			const double mInt = ( cntInt > 0 ) ? ( sumInt / double( cntInt ) ) : mAll;
			std::lock_guard<std::mutex> lk( intermediateMutex );
			intermediateCaptures.push_back( IntermCapture{
				rcRegion == nullptr,
				rcRegion ? *rcRegion : Rect( 0, 0, 0, 0 ),
				mAll,
				mInt } );
		}
	}
	virtual void OutputImage( const IRasterImage& img, const Rect*, const unsigned int ) override
	{
		pixels.resize( img.GetWidth() * img.GetHeight() );
		for( unsigned int y = 0; y < img.GetHeight(); y++ )
			for( unsigned int x = 0; x < img.GetWidth(); x++ )
				pixels[y * img.GetWidth() + x] = img.GetPEL( x, y );
	}
};

class CancellingProgressCallback : public IProgressCallback
{
public:
	double cancelFraction = 2.0;
	bool cancelled = false;
	bool Progress( const double progress, const double total ) override
	{
		if( total > 0.0 && ( progress / total ) >= cancelFraction ) {
			cancelled = true;
			return false;
		}
		return true;
	}
	void SetTitle( const char* ) override {}
	bool IsCancelled() const override { return cancelled; }
};

static std::string TempPath( const char* tag, const char* ext )
{
	std::error_code ec;
	std::filesystem::path dir = std::filesystem::temp_directory_path( ec );
	if( ec || dir.empty() ) dir = ".";
	return ( dir / Fmt( "dl459_%s_%d.%s", tag, RiseTestPid(), ext ) ).generic_string();
}

//! Renders `sceneText` once.  By default single-threaded (deterministic,
//! DL-625) with the next seed of the g_seedBase sequence; `multiThreaded`
//! keeps the rasterizer's own worker count, `fixedSeed` replaces the
//! sequence, and `outPixels` receives the captured pixels.
static bool RenderOnce( const std::string& sceneText, double& mean, bool multiThreaded = false,
	const unsigned int* fixedSeed = nullptr, std::vector<RISEColor>* outPixels = nullptr, const unsigned int* libcSeed = nullptr )
{
	const std::string path = TempPath( "render", "RISEscene" );
	{ std::ofstream ofs( path ); ofs << sceneText; }
	bool ok = false;
	IJobPriv* pJob = nullptr;
	if( RISE_CreateJobPriv( &pJob ) && pJob ) {
		if( pJob->LoadAsciiSceneViaCst( path.c_str() ) ) {
			pJob->RemoveRasterizerOutputs();
			CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
			GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "capture" );
			pJob->GetRasterizer()->AddRasterizerOutput( pCap );
			if( !multiThreaded ) {
				if( Rasterizer* pRas = dynamic_cast<Rasterizer*>( pJob->GetRasterizer() ) ) {
					pRas->ForTest_SetThreadCountOverride( 1 );
				}
			}
			const unsigned int renderSeed = fixedSeed ? *fixedSeed : g_seedBase + g_renderIndex++;
			SobolSamplerTestHooks::ValueSalt().store( SobolSequence::HashCombine( renderSeed, 0x459u ) );
			std::srand( libcSeed ? *libcSeed : renderSeed );
			GlobalRNG().Reseed( libcSeed ? *libcSeed : renderSeed );
			const bool bRendered = pJob->Rasterize();
			SobolSamplerTestHooks::ValueSalt().store( 0u );
			if( bRendered && !pCap->pixels.empty() ) {
				double sum = 0; ok = true;
				for( const RISEColor& c : pCap->pixels ) {
					const double v = ( c.base.r + c.base.g + c.base.b ) * c.a / 3.0;
					if( !std::isfinite( v ) ) { ok = false; break; }
					sum += v;
				}
				mean = sum / double( pCap->pixels.size() );
				if( outPixels ) *outPixels = pCap->pixels;
			}
			safe_release( pCap );
		}
		safe_release( pJob );
	}
	std::remove( path.c_str() );
	return ok;
}

struct Stat { double mean, se; bool ok; };

static Stat RenderN( const std::string& text, int n )
{
	Stat s{ 0, 0, true };
	std::vector<double> v;
	for( int i = 0; i < n; i++ ) {
		double m = 0;
		if( !RenderOnce( text, m ) ) { s.ok = false; return s; }
		v.push_back( m );
	}
	for( double x : v ) s.mean += x;
	s.mean /= n;
	double ss = 0;
	for( double x : v ) ss += ( x - s.mean ) * ( x - s.mean );
	s.se = std::sqrt( ss / ( n - 1 ) / n );
	return s;
}

static int g_spp = 1024;
static int g_repeats = 4;
static int g_trapRepeats = 12;
static std::string RasPT() { return "pathtracing_pel_rasterizer\n{\n\tsamples " + std::to_string( g_spp ) + "\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n\n"; }
static std::string RasBDPT() { return "bdpt_pel_rasterizer\n{\n\tmax_eye_depth 4\n\tmax_light_depth 4\n\tsamples " + std::to_string( g_spp ) + "\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n\n"; }
static std::string RasVCM() { return "vcm_pel_rasterizer\n{\n\tmax_eye_depth 4\n\tmax_light_depth 4\n\tsamples " + std::to_string( g_spp ) + "\n\toidn_denoise FALSE\n\tpixel_filter box\n\tmerge_radius 0.0\n\tvc_enabled true\n\tvm_enabled false\n}\n\n"; }

//! Independent 4D Gauss-Legendre quadrature of the exact rendering-equation
//! integral for the DL-460 planar trapezoid luminary scene:
//!   mean = (1 / A_film) * int_{[-2,2]^2} dx int_{Trap} dy (rho / pi) * L_e * cos(theta_x) * cos(theta_y) / |x - y|^2
//! With rho = 0.5, L_e = 10 / pi, A_film = 16, and Trap at z = 2.6:
//!   mean = (5 / (16 * pi^2)) * int_{[-2,2]^2} dx int_{Trap} dy z^2 / |x - y|^4.
static double TrapezoidSceneExactQuadrature()
{
	const int N = 32;
	double xi[N], wi[N];
	for( int i = 0; i < N; i++ ) {
		double z = std::cos( kPi * ( i + 0.75 ) / ( N + 0.5 ) );
		double dp = 0.0;
		for( int it = 0; it < 100; it++ ) {
			double p0 = 1.0, p1 = z;
			for( int k = 2; k <= N; k++ ) {
				const double p2 = ( ( 2.0 * k - 1.0 ) * z * p1 - ( k - 1.0 ) * p0 ) / k;
				p0 = p1;
				p1 = p2;
			}
			dp = N * ( z * p1 - p0 ) / ( z * z - 1.0 );
			const double dz = p1 / dp;
			z -= dz;
			if( std::fabs( dz ) < 1e-15 ) break;
		}
		xi[i] = z;
		wi[i] = 2.0 / ( ( 1.0 - z * z ) * dp * dp );
	}
	const double ze = 2.6;
	const double ze2 = ze * ze;
	double total = 0.0;
	for( int iv = 0; iv < N; iv++ ) {
		const double y_loc = xi[iv];
		const double wy = wi[iv];
		const double half_w = 0.65 - 0.35 * y_loc;
		const double ye = y_loc - 0.1;
		for( int iu = 0; iu < N; iu++ ) {
			const double x_loc = half_w * xi[iu];
			const double wx = wi[iu] * half_w;
			const double xe = x_loc + 0.2;
			const double w_lum = wx * wy;
			for( int iy = 0; iy < N; iy++ ) {
				const double yf = 2.0 * xi[iy];
				const double wyf = 2.0 * wi[iy];
				const double dy2 = ( yf - ye ) * ( yf - ye ) + ze2;
				for( int ix = 0; ix < N; ix++ ) {
					const double xf = 2.0 * xi[ix];
					const double wxf = 2.0 * wi[ix];
					const double r2 = ( xf - xe ) * ( xf - xe ) + dy2;
					total += w_lum * wxf * wyf * ( ze2 / ( r2 * r2 ) );
				}
			}
		}
	}
	return total * ( 0.5 * 10.0 / ( kPi * kPi ) ) / 16.0;
}

//! Floor (rho 0.5) at z = 0 seen by an orthographic camera at z = 1
//! looking down; the emitter (geometry chunk named geo_e) hangs at
//! z = 2.6 above the camera, so only emitter -> floor -> camera transport
//! is imaged.
static std::string RenderScene( const std::string& emitterGeometry, const std::string& rasterizer )
{
	return std::string( "RISE ASCII SCENE 7\n\n" ) +
		"film\n{\n\twidth 24\n\theight 24\n}\n\n"
		"orthographic_camera\n{\n\tlocation 0 0 1\n\tlookat 0 0 0\n\tup 0 1 0\n\tviewport_scale 4 4\n}\n\n"
		"uniformcolor_painter\n{\n\tname white\n\tcolor 1 1 1\n}\n\n"
		"uniformcolor_painter\n{\n\tname grey\n\tcolor 0.5 0.5 0.5\n}\n\n"
		"lambertian_material\n{\n\tname floor_mat\n\treflectance grey\n}\n\n"
		"lambertian_luminaire_material\n{\n\tname lum\n\texitance white\n\tscale 10\n\tmaterial none\n}\n\n"
		"clippedplane_geometry\n{\n\tname geo_floor\n\tpta -200 -200 0\n\tptb 200 -200 0\n\tptc 200 200 0\n\tptd -200 200 0\n\tdoublesided FALSE\n}\n\n"
		"standard_object\n{\n\tname floor\n\tgeometry geo_floor\n\tmaterial floor_mat\n}\n\n"
		+ emitterGeometry +
		"standard_object\n{\n\tname e\n\tgeometry geo_e\n\tmaterial lum\n\tposition 0.2 -0.1 2.6\n}\n\n"
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n" + rasterizer;
}

static std::string QuadChunk( const Point3 c[4] )
{
	return Fmt( "clippedplane_geometry\n{\n\tname geo_e\n\tpta %.17g %.17g %.17g\n\tptb %.17g %.17g %.17g\n\tptc %.17g %.17g %.17g\n\tptd %.17g %.17g %.17g\n\tdoublesided TRUE\n}\n\n",
		c[0].x, c[0].y, c[0].z, c[1].x, c[1].y, c[1].z, c[2].x, c[2].y, c[2].z, c[3].x, c[3].y, c[3].z );
}

static std::string MeshChunk( const std::vector<Point3>& v, const std::vector<int>& tris )
{
	std::string s = "indexedmesh_geometry\n{\n\tname geo_e\n";
	for( const Point3& p : v ) s += Fmt( "\tvertex %.17g %.17g %.17g\n", p.x, p.y, p.z );
	for( size_t i = 0; i + 2 < tris.size(); i += 3 ) s += Fmt( "\ttriangle %d %d %d\n", tris[i], tris[i+1], tris[i+2] );
	s += "\tdouble_sided TRUE\n}\n\n";
	return s;
}

static std::string BezierChunk( const std::vector<BezierPatch>& patches, const std::string& file )
{
	{
		std::ofstream out( file );
		out << patches.size() << "\n";
		char buf[256];
		for( const BezierPatch& p : patches ) for( int i = 0; i < 4; i++ ) for( int j = 0; j < 4; j++ ) {
			std::snprintf( buf, sizeof(buf), "%.17g %.17g %.17g\n", p.c[i].pts[j].x, p.c[i].pts[j].y, p.c[i].pts[j].z );
			out << buf;
		}
	}
	return Fmt( "bezierpatch_geometry\n{\n\tname geo_e\n\tfile %s\n}\n\n", file.c_str() );
}

static bool Agree( const Stat& a, const Stat& b )
{
	const double se = std::sqrt( a.se * a.se + b.se * b.se );
	return std::fabs( a.mean - b.mean ) <= 3 * se + 1e-12;
}

static void Report( const char* label, const Stat& s ) { std::printf( "    %s %.9f +/- %.3g\n", label, s.mean, s.se ); }

static void RenderCase( const char* label, const std::string& emitter, const std::string* reference, int repeatsOverride = 0, const double* exactOracle = nullptr )
{
	std::cout << "  C " << label << "\n";
	const int n = repeatsOverride > 0 ? repeatsOverride : g_repeats;
	const Stat pt = RenderN( RenderScene( emitter, RasPT() ), n );
	const Stat bd = RenderN( RenderScene( emitter, RasBDPT() ), n );
	const Stat vc = RenderN( RenderScene( emitter, RasVCM() ), n );
	Report( "PT  ", pt ); Report( "BDPT", bd ); Report( "VCM ", vc );
	Check( pt.ok && bd.ok && vc.ok, std::string( "renders: " ) + label );
	Check( Agree( pt, bd ), std::string( "PT vs BDPT: " ) + label );
	Check( Agree( pt, vc ), std::string( "PT vs VCM: " ) + label );
	if( exactOracle ) {
		const Stat ex{ *exactOracle, 0.0, true };
		std::printf( "    exact quadrature %.12f\n", *exactOracle );
		Check( Agree( pt, ex ), std::string( "PT vs exact quadrature: " ) + label );
		Check( Agree( bd, ex ), std::string( "BDPT vs exact quadrature: " ) + label );
		Check( Agree( vc, ex ), std::string( "VCM vs exact quadrature: " ) + label );
		// DL-625 review: what these 3-sigma gates can see.  The renders are
		// deterministic for a seed base, so each gate is ONE draw: a bias b
		// in an estimator with standard error se is caught with probability
		// Phi( b / se - 3 ), i.e. reliably (95 %) only from b >= 4.64 se.
		const double z95 = 3.0 + 1.645;
		std::cout << "    bias caught with 95 % power vs exact: PT >= " << z95 * pt.se << ", BDPT >= " << z95 * bd.se
			<< ", VCM >= " << z95 * vc.se << " (PT vs VCM >= " << z95 * std::sqrt( pt.se * pt.se + vc.se * vc.se ) << ")\n";
	}
	// With an exact oracle a second noisy reference adds nothing the exact
	// checks do not already gate (DL-625 review: it cost a quarter of the
	// trapezoid case's renders).
	if( reference && !exactOracle ) {
		const Stat ref = RenderN( RenderScene( *reference, RasPT() ), n );
		Report( "mesh reference PT", ref );
		Check( ref.ok && Agree( pt, ref ), std::string( "PT vs mesh reference: " ) + label );
		Check( ref.ok && Agree( bd, ref ), std::string( "BDPT vs mesh reference: " ) + label );
		Check( ref.ok && Agree( vc, ref ), std::string( "VCM vs mesh reference: " ) + label );
	}
}

static void TestRenders( bool trapOnly = false )
{
	std::cout << "C: renders\n";
	{
		// DL-625: with only n = 4 salted repeats (3 degrees of freedom), the
		// fixed Sobol realization draw at g_seedBase = 4590 (VCM 0.103733 vs
		// PT 0.103721, +1.4 true sigma) failed ~10-20% of runs whenever
		// multi-threaded sub-pixel screen jitter nudged the 3-df VCM sample
		// SE below ~2.5e-6 (< 0.5x the true 4-repeat SE ~7.6e-6).  Use 12
		// salted repeats (11 df per estimator) and also check PT, BDPT, and
		// VCM against independent 4D Gauss-Legendre quadrature (0.10372060).
		// Renders are single-threaded and deterministic per seed base, so at
		// the default base each gate is one fixed realization; the printed
		// 95 %-power bias floors say what it can and cannot catch (VCM vs
		// exact ~1.6e-5 at n = 12).  The mesh-luminary reference the case
		// used before the exact oracle existed is no longer rendered.
		const double exact = TrapezoidSceneExactQuadrature();
		RenderCase( "DL-460 trapezoid clippedplane luminary", QuadChunk( kTrapezoid ), nullptr, std::max( g_repeats, g_trapRepeats ), &exact );
	}
	if( trapOnly ) return;
	{
		const std::string mesh = MeshChunk( { Point3(-1,-1,0), Point3(1,-1,0), Point3(1,1,0), Point3(-1,1,0) }, { 0, 1, 2, 0, 2, 3 } );
		const std::string file = TempPath( "warped", "bezier" );
		RenderCase( "DL-459 flat warped Bezier luminary", BezierChunk( { WarpedSquarePatch() }, file ), &mesh );
		std::remove( file.c_str() );
	}
	{
		BezierPatch d = DomePatch();
		for( int i = 0; i < 4; i++ ) for( int j = 0; j < 4; j++ ) { d.c[i].pts[j].x -= 3.0; d.c[i].pts[j].z *= -0.5; }
		const std::string file = TempPath( "dome", "bezier" );
		RenderCase( "DL-459 curved Bezier dome luminary", BezierChunk( { d }, file ), nullptr );
		std::remove( file.c_str() );
	}
}

//! D (DL-625 review): a MULTI-threaded pixel-based render seeded the same
//! way twice must give the same pixels.  Each RasterizeBlockDispatcher
//! worker used to seed its RandomNumberGenerator from libc rand(), called
//! concurrently by every worker (a data race), and which worker -- hence
//! which seed -- took which tile depended on scheduling; the generator now
//! reseeds per tile, keyed on the tile's pixel corner (the default block
//! sequence is shuffled from std::random_device), from one value read on
//! the dispatching thread.  The VCM light pass (VCMRasterizerBase's
//! LightPassDispatcher) had the same defect and the same fix; only the
//! rwsss VCM+VM row sees it, because the light pass consumes that
//! generator only inside a random walk.  Each row also checks that
//! another libc seed (same Sobol' salt) changes the pixels, so a row
//! whose generators never reach the image cannot pass vacuously.
//! One discarded warm-up render first: the first
//! render in a process also draws libc rand() for one-time statics, so it
//! sees a different libc state after the same std::srand (not a race).
static void TestMultiThreadDeterminism()
{
	std::cout << "D: multi-threaded render determinism\n";
	const std::string pt = "pathtracing_pel_rasterizer\n{\n\tsamples 16\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n\n";
	const std::string ptSpec = "pathtracing_spectral_rasterizer\n{\n\tsamples 16\n\tnmbegin 400\n\tnmend 700\n\tnum_wavelengths 1\n\tspectral_samples 1\n\thwss TRUE\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n\n";
	// VCM with merging on: its light pass (VCMRasterizerBase's
	// LightPassDispatcher) seeded its workers the same racy way.
	const std::string vcm = "vcm_pel_rasterizer\n{\n\tmax_eye_depth 4\n\tmax_light_depth 4\n\tsamples 16\n\toidn_denoise FALSE\n\tpixel_filter box\n\tvc_enabled true\n\tvm_enabled true\n}\n\n";
	const std::string bdpt = "bdpt_pel_rasterizer\n{\n\tmax_eye_depth 4\n\tmax_light_depth 4\n\tsamples 16\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n\n";
	{
		const unsigned int seed = 777u;
		double mean = 0;
		RenderOnce( RenderScene( QuadChunk( kTrapezoid ), pt ), mean, true, &seed );
	}
	const char* fogSnippet =
		"uniformcolor_painter\n{\n\tname fog_dense\n\tcolor 1 1 1\n}\n\n"
		"uniformcolor_painter\n{\n\tname fog_sparse\n\tcolor 0 0 0\n}\n\n"
		"wavelet3d_painter\n{\n\tname fog_density\n\ttile_size 32\n\tpersistence 0.65\n\toctaves 3\n\tcolora fog_dense\n\tcolorb fog_sparse\n\tscale 1.5 1.5 1.5\n\tshift 0 0 0\n}\n\n"
		"painter_heterogeneous_medium\n{\n\tname fog\n\tabsorption 0.3 0.3 0.3\n\tscattering 1.5 1.5 1.5\n\tphase isotropic\n\tdensity_painter fog_density\n\tresolution 16\n\tcolor_to_scalar luminance\n\tbbox_min -3 -3 0.05\n\tbbox_max 3 3 0.85\n}\n\n"
		"perfectrefractor_material\n{\n\tname fog_air\n\trefractance white\n\tior 1.0\n}\n\n"
		"box_geometry\n{\n\tname geo_fog\n\twidth 6\n\theight 6\n\tdepth 0.8\n}\n\n"
		"standard_object\n{\n\tname fogbox\n\tgeometry geo_fog\n\tmaterial fog_air\n\tposition 0 0 0.45\n\tinterior_medium fog\n\tcasts_shadows FALSE\n}\n\n";
	// The rwsss rows hang a random-walk SSS slab between the emitter and
	// the camera (which looks down and never sees it): every light path to
	// the floor walks through it, and the walk draws from the RuntimeContext
	// generator -- for VCM+VM the LIGHT pass's, whose vertices the floor's
	// eye vertices merge.  The Lambertian scene never consumes it there.
	// The fog rows (DL-628) exercise HeterogeneousMedium::EvalTransmittance
	// (Pel) and EvalTransmittanceNM (spectral/HWSS) across multi-tile passes.
	struct Row { const char* name; const std::string* ras; int res; int extra; };
	const Row rows[] = {
		{ "PT", &pt, 24, 0 }, { "PT", &pt, 96, 0 }, { "BDPT", &bdpt, 96, 0 },
		{ "VCM+VM", &vcm, 96, 0 }, { "PT rwsss", &pt, 96, 1 }, { "VCM+VM rwsss", &vcm, 96, 1 },
		{ "PT fog", &pt, 96, 2 }, { "PT spectral fog", &ptSpec, 96, 2 }, { "VCM+VM fog", &vcm, 96, 2 }
	};
	for( const Row& row : rows ) {
		std::string scene = RenderScene( QuadChunk( kTrapezoid ), *row.ras );
		if( row.extra == 1 ) {
			const std::string at = "standard_shader\n";
			scene.insert( scene.find( at ),
				"randomwalk_sss_material\n{\n\tname slab_mat\n\tior 1.0\n\tabsorption 0.05 0.05 0.05\n\tscattering 4\n\tg 0.0\n\troughness 0.05\n\tmax_bounces 64\n}\n\n"
				"box_geometry\n{\n\tname geo_slab\n\twidth 8\n\theight 8\n\tdepth 0.4\n}\n\n"
				"standard_object\n{\n\tname slab\n\tgeometry geo_slab\n\tmaterial slab_mat\n\tposition 0.2 -0.1 1.9\n}\n\n" );
		} else if( row.extra == 2 ) {
			scene.insert( scene.find( "standard_shader\n" ), fogSnippet );
		}
		const std::string from = "width 24\n\theight 24";
		scene.replace( scene.find( from ), from.size(), Fmt( "width %d\n\theight %d", row.res, row.res ) );
		std::vector<RISEColor> first;
		bool ok = true, same = true;
		double maxRel = 0;
		for( int r = 0; r < 6 && ok; ++r ) {
			const unsigned int seed = 777u;
			std::vector<RISEColor> px;
			double mean = 0;
			ok = RenderOnce( scene, mean, true, &seed, &px );
			if( !ok ) break;
			if( r == 0 ) {
				first = px;
				continue;
			}
			for( std::size_t i = 0; i < px.size(); ++i ) {
				const double d[4] = { px[i].base.r - first[i].base.r, px[i].base.g - first[i].base.g,
					px[i].base.b - first[i].base.b, px[i].a - first[i].a };
				for( const double v : d ) {
					if( v != 0 ) {
						same = false;
						maxRel = std::max( maxRel, std::fabs( v ) / std::max( 1e-30, double( std::fabs( first[i].base.g ) ) ) );
					}
				}
			}
		}
		// The row means something only if the libc-seeded generators reach
		// the pixels at all: another libc seed (same Sobol salt) must change
		// them.
		bool seedSensitive = false;
		{
			const unsigned int seed = 777u, other = 778u;
			std::vector<RISEColor> px;
			double mean = 0;
			if( ok && RenderOnce( scene, mean, true, &seed, &px, &other ) ) {
				for( std::size_t i = 0; i < px.size() && !seedSensitive; ++i ) {
					seedSensitive = px[i].base.r != first[i].base.r || px[i].base.g != first[i].base.g || px[i].base.b != first[i].base.b;
				}
			}
		}
		std::cout << "    " << row.name << " " << row.res << "x" << row.res << ": 6 multi-threaded renders at one seed "
			<< ( same ? "bit-identical" : Fmt( "DIFFER (max rel pixel diff %.3g)", maxRel ) )
			<< ( seedSensitive ? "; another seed changes it" : "; ANOTHER SEED GIVES THE SAME PIXELS" ) << "\n";
		Check( ok && same, Fmt( "D: multi-threaded %s %dx%d render is a function of the seed", row.name, row.res, row.res ) );
		Check( seedSensitive, Fmt( "D: multi-threaded %s %dx%d render depends on the libc seed (the row is not vacuous)", row.name, row.res, row.res ) );
	}
}

//! F (DL-625 review round 2): the same, in a FRESH process with no warm-up
//! render, through several progressive passes, in scenes whose workers seed
//! generators on first use (SMS's per-call generators via ThreadLocalSeed;
//! HeterogeneousMedium's thread_local ratio-tracking generators).  Those
//! first-use seeds used to read libc, and how many workers render in a pass
//! is scheduling-dependent (9 tiles of a 24 x 24 film, more workers than
//! that), so the libc stream position -- what the next pass's or render's
//! tile seeds read -- varied run to run.  Mode F runs F_child_<scene> as
//! three separate processes per scene and compares what they print
//! (pixel hash, and rand() right after the render); F_child_<scene> alone
//! renders once and prints it.  D's warm-up render does not hide this
//! here: every child is a fresh process.
static std::string GlassScene()
{
	std::string scene = RenderScene( QuadChunk( kTrapezoid ),
		"pathtracing_pel_rasterizer\n{\n\tsamples 96\n\toidn_denoise FALSE\n\tpixel_filter box\n\tsms_enabled TRUE\n\tsms_biased TRUE\n}\n\n" );
	scene.insert( scene.find( "standard_shader\n" ),
		"perfectrefractor_material\n{\n\tname glass_mat\n\trefractance white\n\tior 1.5\n}\n\n"
		"sphere_geometry\n{\n\tname geo_glass\n\tradius 0.3\n}\n\n"
		"standard_object\n{\n\tname glass\n\tgeometry geo_glass\n\tmaterial glass_mat\n\tposition 0.2 -0.1 0.5\n}\n\n" );
	return scene;
}

//! A box of heterogeneous fog over the floor: every worker that renders a
//! fog pixel constructs HeterogeneousMedium's thread_local generators.
static std::string FogScene( bool spectral = false )
{
	const char* ras = spectral ?
		"pathtracing_spectral_rasterizer\n{\n\tsamples 64\n\tnmbegin 400\n\tnmend 700\n\tnum_wavelengths 1\n\tspectral_samples 1\n\thwss TRUE\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n\n" :
		"pathtracing_pel_rasterizer\n{\n\tsamples 96\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n\n";
	std::string scene = RenderScene( QuadChunk( kTrapezoid ), ras );
	scene.insert( scene.find( "standard_shader\n" ),
		"uniformcolor_painter\n{\n\tname fog_dense\n\tcolor 1 1 1\n}\n\n"
		"uniformcolor_painter\n{\n\tname fog_sparse\n\tcolor 0 0 0\n}\n\n"
		"wavelet3d_painter\n{\n\tname fog_density\n\ttile_size 32\n\tpersistence 0.65\n\toctaves 3\n\tcolora fog_dense\n\tcolorb fog_sparse\n\tscale 1.5 1.5 1.5\n\tshift 0 0 0\n}\n\n"
		"painter_heterogeneous_medium\n{\n\tname fog\n\tabsorption 0.3 0.3 0.3\n\tscattering 1.5 1.5 1.5\n\tphase isotropic\n\tdensity_painter fog_density\n\tresolution 16\n\tcolor_to_scalar luminance\n\tbbox_min -3 -3 0.05\n\tbbox_max 3 3 0.85\n}\n\n"
		"perfectrefractor_material\n{\n\tname fog_air\n\trefractance white\n\tior 1.0\n}\n\n"
		"box_geometry\n{\n\tname geo_fog\n\twidth 6\n\theight 6\n\tdepth 0.8\n}\n\n"
		"standard_object\n{\n\tname fogbox\n\tgeometry geo_fog\n\tmaterial fog_air\n\tposition 0 0 0.45\n\tinterior_medium fog\n\tcasts_shadows FALSE\n}\n\n" );
	if( spectral ) {
		const std::string from = "width 24\n\theight 24";
		scene.replace( scene.find( from ), from.size(), "width 64\n\theight 64" );
	} else {
		const std::string from = "width 24\n\theight 24";
		scene.replace( scene.find( from ), from.size(), "width 96\n\theight 96" );
	}
	return scene;
}

static void FreshProcessChild( const std::string& which )
{
	const unsigned int seed = 777u;
	std::vector<RISEColor> px;
	double mean = 0;
	const std::string scene =
		( which == "fog" ) ? FogScene( false ) :
		( which == "fog_spectral" ) ? FogScene( true ) :
		GlassScene();
	const bool ok = RenderOnce( scene, mean, true, &seed, &px );
	// The libc stream position after the render: what the NEXT render's (or
	// pass's) seeding sites would read.
	const int next = std::rand();
	unsigned long long h = 1469598103934665603ull;
	for( const RISEColor& c : px ) {
		const double v[4] = { c.base.r, c.base.g, c.base.b, c.a };
		for( const double d : v ) {
			unsigned long long b;
			std::memcpy( &b, &d, 8 );
			h = ( h ^ b ) * 1099511628211ull;
		}
	}
	std::printf( "FCHILD ok=%d next=%d mean=%.17g hash=%016llx\n", ok ? 1 : 0, next, mean, h );
}

static std::FILE* OpenReadPipe( const char* command )
{
#if defined( _WIN32 )
	return _popen( command, "r" );
#else
	return popen( command, "r" );
#endif
}

static int ClosePipe( std::FILE* pipe )
{
#if defined( _WIN32 )
	return _pclose( pipe );
#else
	return pclose( pipe );
#endif
}

static std::string FieldOf( const std::string& line, const char* key )
{
	const std::size_t p = line.find( key );
	if( p == std::string::npos ) return std::string();
	const std::size_t e = line.find_first_of( " \r\n", p );
	return line.substr( p, e == std::string::npos ? std::string::npos : e - p );
}

static void TestFreshProcessDeterminism( const char* self )
{
	std::cout << "F: fresh-process multi-threaded determinism (progressive passes, first-use worker generators)\n";
	for( const char* which : { "glass", "fog", "fog_spectral" } ) {
		std::vector<std::string> lines;
		for( int r = 0; r < 3; ++r ) {
			const std::string cmd = std::string( "\"" ) + self + "\" F_child_" + which;
			std::FILE* f = OpenReadPipe( cmd.c_str() );
			std::string line;
			if( f ) {
				char buf[512];
				while( std::fgets( buf, sizeof( buf ), f ) ) {
					if( std::strncmp( buf, "FCHILD", 6 ) == 0 ) line = buf;
				}
				ClosePipe( f );
			}
			std::cout << "    " << which << " process " << r << ": " << ( line.empty() ? std::string( "(no result)\n" ) : line );
			lines.push_back( line );
		}
		const bool ok = !lines[0].empty() && lines[0].find( "ok=1" ) != std::string::npos;
		bool sameNext = ok, sameHash = ok;
		for( int r = 1; r < 3; ++r ) {
			sameNext = sameNext && FieldOf( lines[r], "next=" ) == FieldOf( lines[0], "next=" );
			sameHash = sameHash && FieldOf( lines[r], "hash=" ) == FieldOf( lines[0], "hash=" );
		}
		// Every scene: no worker-side first use may move the libc stream.
		Check( sameNext, Fmt( "F: %s: the libc stream position after a multi-threaded render is a function of std::srand", which ) );
		// DL-628: both SMS glass and heterogeneous fog (Pel and spectral/HWSS)
		// render bit-identically across fresh multi-threaded processes under a
		// fixed std::srand.
		Check( sameHash, Fmt( "F: %s: three fresh processes render bit-identically after the same std::srand", which ) );
	}
}

//! Q (DL-629): high-power multi-threaded quadrature check on the DL-460
//! trapezoid clipped-plane luminary scene.  At 96x96 (same [-2,2]^2
//! orthographic viewport_scale 4x4 with box filter, so the exact film-mean
//! integral is identical to 24x24 while 9 tiles run concurrently across
//! workers and each pass averages 16x as many pixels), 32 salted 256-spp
//! repeats reach ~2-4e-6 relative SE -- 95 % power below DL-629's 2-4e-5
//! relative residual -- in seconds.
static void TestHighPowerTrapezoidQuadrature( int spp, int repeats )
{
	std::cout << "Q: high-power multi-threaded trapezoid quadrature (96x96, spp=" << spp << ", repeats=" << repeats << ")\n";
	const double exact = TrapezoidSceneExactQuadrature();
	const int savedSpp = g_spp;
	g_spp = spp;
	const std::string emit = QuadChunk( kTrapezoid );
	const std::pair<const char*, std::string> kinds[3] = {
		{ "PT", RasPT() }, { "BDPT", RasBDPT() }, { "VCM", RasVCM() }
	};
	for( int k = 0; k < 3; ++k ) {
		std::string text = RenderScene( emit, kinds[k].second );
		const std::string from = "width 24\n\theight 24";
		text.replace( text.find( from ), from.size(), "width 96\n\theight 96" );
		Stat st{ 0, 0, true };
		std::vector<double> vals;
		vals.reserve( repeats );
		for( int r = 0; r < repeats; ++r ) {
			const unsigned int seed = g_seedBase + static_cast<unsigned int>( k * 10000 + r * 97 + 1 );
			double m = 0;
			if( !RenderOnce( text, m, /*multiThreaded=*/true, &seed ) ) {
				st.ok = false;
				break;
			}
			vals.push_back( m );
		}
		if( st.ok ) {
			for( double v : vals ) st.mean += v;
			st.mean /= double( repeats );
			double ss = 0;
			for( double v : vals ) ss += ( v - st.mean ) * ( v - st.mean );
			st.se = std::sqrt( ss / double( repeats - 1 ) / double( repeats ) );
		}
		const double diff = st.mean - exact;
		const double relDiff = diff / exact;
		const double relSe = st.se / exact;
		const double power95Rel = ( 3.0 + 1.645 ) * relSe;
		const double z = st.se > 0 ? diff / st.se : 0.0;
		std::printf( "    %-4s mean=%.12f  diff=%+.3e (rel %+.3e)  se=%.3e (rel %.3e)  z=%+.2f  95%%-power=%.3e rel\n",
			kinds[k].first, st.mean, diff, relDiff, st.se, relSe, z, power95Rel );
		Check( st.ok && std::fabs( diff ) <= 3.0 * st.se + 1e-12,
			Fmt( "Q: %s matches exact trapezoid quadrature (z=%+.2f, 95%%-power=%.2e rel)", kinds[k].first, z, power95Rel ) );
	}
	g_spp = savedSpp;
}

//! P (DL-629):
//!   1. Progressive multi-pass RGB accumulation must not drift via repeated
//!      XYZ->Rec709->XYZ round-trips across passes (pre-fix: 32 progressive
//!      passes drifted by ~16.5 * 0.733 ppm = 1.21e-5 in R and 1.78e-5 in G on
//!      an achromatic scene, and left unequal R/G/B channels on white).
//!   2. OrthographicCamera odd-dimension films (1x1, 3x3, 5x5) must place the
//!      screen-center ray at the camera origin (pre-fix integer division
//!      `frame.GetWidth()/2` shifted 1x1 center to (2, 2, 1)).
static void TestProgressiveColorAndOrthoCamera()
{
	std::cout << "P: progressive RGB round-trip identity + orthographic odd-dim centering (DL-629)\n";

	// 1. Odd-dimension OrthographicCamera center-ray check.
	for( unsigned int dim : { 1u, 3u, 5u } ) {
		OrthographicCamera* pCam = new OrthographicCamera(
			Point3( 0, 0, 1 ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ),
			dim, dim, Vector2( 4.0, 4.0 ), 1.0, 0.0, 0.0, 0.0,
			Vector3( 0, 0, 0 ), Vector2( 0, 0 ) );
		RuntimeContext rc( GlobalRNG(), RuntimeContext::PASS_NORMAL, false );
		Ray ray;
		const bool genOk = pCam->GenerateRay( rc, ray, Point2( Scalar( 0.5 ) * Scalar( dim ), Scalar( 0.5 ) * Scalar( dim ) ) );
		const double err = std::hypot( ray.origin.x, std::hypot( ray.origin.y, ray.origin.z - 1.0 ) );
		Check( genOk && err < 1e-14,
			Fmt( "P: OrthographicCamera %ux%u center ray origin error %.3e < 1e-14", dim, dim, err ) );
		safe_release( pCam );
	}

	// 2. Progressive 32-pass (1024 spp at default 32 spp/pass) vs single-pass
	//    (progressive_rendering FALSE) on a 1x1 odd-dim film with uniform pixel
	//    sampling across PT, BDPT, VCM:
	//    - Progressive multi-pass accumulation must NOT compound XYZ<->Rec709
	//      round-trip drift across passes: 32-pass progressive output must match
	//      a single RISEPel(XYZPel(singlePass)) conversion to IEEE-754 summation
	//      precision (< 1e-13, vs ~5e-6 pre-fix from 32 compounded conversions);
	//    - Progressive vs non-progressive mean parity stays within a single
	//      XYZ<->Rec709 conversion (< 5e-7, vs ~1e-5 pre-fix).
	const std::string emit = QuadChunk( kTrapezoid );
	const char* rasts[3][2] = {
		{ "PT",   "pathtracing_pel_rasterizer\n{\n\tsamples 1024\n\tpixel_sampler uniform\n\toidn_denoise FALSE\n\tpixel_filter box\n" },
		{ "BDPT", "bdpt_pel_rasterizer\n{\n\tmax_eye_depth 4\n\tmax_light_depth 4\n\tsamples 1024\n\tpixel_sampler uniform\n\toidn_denoise FALSE\n\tpixel_filter box\n" },
		{ "VCM",  "vcm_pel_rasterizer\n{\n\tmax_eye_depth 4\n\tmax_light_depth 4\n\tsamples 1024\n\tpixel_sampler uniform\n\toidn_denoise FALSE\n\tpixel_filter box\n\tmerge_radius 0.0\n\tvc_enabled true\n\tvm_enabled false\n" },
	};
	for( int r = 0; r < 3; ++r ) {
		auto makeProgScene = [&]( bool progressive ) {
			const std::string rasChunk = std::string( rasts[r][1] ) +
				"\tprogressive_rendering " + ( progressive ? "TRUE" : "FALSE" ) + "\n}\n\n";
			std::string text = RenderScene( emit, rasChunk );
			const std::string from = "width 24\n\theight 24";
			text.replace( text.find( from ), from.size(), "width 1\n\theight 1" );
			const std::string vpFrom = "viewport_scale 4 4";
			text.replace( text.find( vpFrom ), vpFrom.size(), "viewport_scale 0 0" );
			return text;
		};
		const unsigned int seed = 4242u;
		double meanProg = 0.0, meanSingle = 0.0;
		std::vector<RISEColor> pxProg, pxSingle;
		const bool okProg = RenderOnce( makeProgScene( true ), meanProg, false, &seed, &pxProg, &seed );
		const bool okSingle = RenderOnce( makeProgScene( false ), meanSingle, false, &seed, &pxSingle, &seed );
		double maxSingleRoundTripDiff = 1.0;
		if( okProg && okSingle && !pxProg.empty() && !pxSingle.empty() ) {
			const RISEPel expectedOneRoundTrip( XYZPel( pxSingle[0].base ) );
			maxSingleRoundTripDiff = std::max( {
				double( std::fabs( pxProg[0].base.r - expectedOneRoundTrip.r ) ),
				double( std::fabs( pxProg[0].base.g - expectedOneRoundTrip.g ) ),
				double( std::fabs( pxProg[0].base.b - expectedOneRoundTrip.b ) ) } );
		}
		const double passParityErr = std::fabs( meanProg - meanSingle );
		Check( okProg && okSingle && maxSingleRoundTripDiff < 1e-13,
			Fmt( "P: %s 32-pass progressive matches single XYZ round-trip to %.3e < 1e-13 (zero multi-pass compounding)", rasts[r][0], maxSingleRoundTripDiff ) );
		Check( okProg && okSingle && passParityErr < 5e-7,
			Fmt( "P: %s 32-pass progressive vs single-pass mean diff %.3e < 5e-7", rasts[r][0], passParityErr ) );

		// 3. Live intermediate tile previews (OutputIntermediateImage on passes 1..3)
		//    and cancelled mid-pass-3 progressive renders must retain full cumulative
		//    luminance (regression guard against 1/(k+1) pass-delta darkening).
		const char* liveRasts[3] = {
			"pathtracing_pel_rasterizer\n{\n\tsamples 16\n\tprogressive_samples_per_pass 4\n\tprogressive_rendering TRUE\n\tpixel_sampler uniform\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n\n",
			"bdpt_pel_rasterizer\n{\n\tmax_eye_depth 4\n\tmax_light_depth 4\n\tsamples 16\n\tprogressive_samples_per_pass 4\n\tprogressive_rendering TRUE\n\tpixel_sampler uniform\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n\n",
			"vcm_pel_rasterizer\n{\n\tmax_eye_depth 4\n\tmax_light_depth 4\n\tsamples 16\n\tprogressive_samples_per_pass 4\n\tprogressive_rendering TRUE\n\tpixel_sampler uniform\n\toidn_denoise FALSE\n\tpixel_filter box\n\tmerge_radius 0.0\n\tvc_enabled true\n\tvm_enabled false\n}\n\n"
		};
		std::string liveScene = RenderScene( emit, liveRasts[r] );
		{
			const std::string from = "width 24\n\theight 24";
			liveScene.replace( liveScene.find( from ), from.size(), "width 64\n\theight 64" );
		}
		auto runLiveOrCancel = [&]( double cancelFrac, std::vector<CapturingRasterizerOutput::IntermCapture>& outInterm, std::vector<RISEColor>& outPx, double& outFinalMean, bool& didCancel ) -> bool {
			const std::string pth = TempPath( "prog_live", "RISEscene" );
			{ std::ofstream ofs( pth ); ofs << liveScene; }
			bool okRun = false;
			IJobPriv* pJob = nullptr;
			if( RISE_CreateJobPriv( &pJob ) && pJob && pJob->LoadAsciiSceneViaCst( pth.c_str() ) ) {
				pJob->RemoveRasterizerOutputs();
				CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
				pCap->captureIntermediate = true;
				GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "capture" );
				pJob->GetRasterizer()->AddRasterizerOutput( pCap );
				if( Rasterizer* pRas = dynamic_cast<Rasterizer*>( pJob->GetRasterizer() ) ) {
					pRas->ForTest_SetThreadCountOverride( 1 );
				}
				CancellingProgressCallback cb;
				cb.cancelFraction = cancelFrac;
				if( cancelFrac <= 1.0 ) {
					pJob->SetProgress( &cb );
				}
				SobolSamplerTestHooks::ValueSalt().store( SobolSequence::HashCombine( 4242u, 0x459u ) );
				std::srand( 4242u );
				GlobalRNG().Reseed( 4242u );
				pJob->Rasterize();
				SobolSamplerTestHooks::ValueSalt().store( 0u );
				pJob->SetProgress( nullptr );
				didCancel = cb.cancelled;
				outInterm = pCap->intermediateCaptures;
				outPx = pCap->pixels;
				if( !pCap->pixels.empty() ) {
					double sum = 0.0;
					for( const RISEColor& c : pCap->pixels ) {
						sum += ( c.base.r + c.base.g + c.base.b ) * c.a / 3.0;
					}
					outFinalMean = sum / double( pCap->pixels.size() );
					okRun = std::isfinite( outFinalMean ) && outFinalMean > 0.0;
				}
				safe_release( pCap );
			}
			safe_release( pJob );
			std::remove( pth.c_str() );
			return okRun;
		};
		std::vector<CapturingRasterizerOutput::IntermCapture> intermFull, intermCancel;
		std::vector<RISEColor> fullPx, cancelPx;
		double fullMean = 0.0, cancelMean = 0.0;
		bool dummyCancel = false, wasCancelled = false;
		const bool okLiveFull = runLiveOrCancel( 2.0, intermFull, fullPx, fullMean, dummyCancel );
		const bool okLiveCancel = runLiveOrCancel( 0.60, intermCancel, cancelPx, cancelMean, wasCancelled );
		auto regionRefMean = [&]( const CapturingRasterizerOutput::IntermCapture& cap, bool interiorOnly ) -> double {
			if( cap.fullFrame || fullPx.size() != 64u * 64u ) return fullMean;
			unsigned int x0 = cap.region.left, y0 = cap.region.top;
			unsigned int x1 = std::min<unsigned int>( cap.region.right + 1, 64u );
			unsigned int y1 = std::min<unsigned int>( cap.region.bottom + 1, 64u );
			if( interiorOnly && x1 > x0 + 10 && y1 > y0 + 10 ) {
				x0 += 5; y0 += 5; x1 -= 5; y1 -= 5;
			}
			double s = 0.0;
			unsigned int n = 0;
			for( unsigned int y = y0; y < y1; ++y ) {
				for( unsigned int x = x0; x < x1; ++x ) {
					const RISEColor& c = fullPx[y * 64u + x];
					s += ( c.base.r + c.base.g + c.base.b ) * c.a / 3.0;
					++n;
				}
			}
			return n > 0 ? ( s / double( n ) ) : fullMean;
		};
		// Index 0 is PrepareImageForNewRender's pre-render pastel clear.
		// Thereafter, PT/BDPT emit strictly paired per-block notifications
		// (even blockNotifyIdx = pre-block with 5-px DimTileBorder, odd =
		// post-block freshly written cret) plus full-frame pass previews;
		// VCM skips per-block output and emits full-frame pass previews
		// (ti >= 1).  Without cumulative StorePelPassDelta, late-pass
		// post-block cret drops to 1/(k+1) = 0.25x of its final tile mean.
		const bool expectsBlockFlushes = ( r < 2 );
		bool allLateTilesFullBrightness = expectsBlockFlushes ? ( intermFull.size() >= 17 ) : ( intermFull.size() >= 2 );
		double minLateRatio = 1.0, maxLateRatio = 1.0;
		const std::size_t startIdx = expectsBlockFlushes ? ( intermFull.size() / 2 ) : 1;
		std::size_t blockNotifyIdx = 0;
		for( std::size_t ti = 1; ti < intermFull.size() && fullMean > 0.0; ++ti ) {
			const auto& cap = intermFull[ti];
			const bool isPostBlock = !cap.fullFrame && ( ( blockNotifyIdx++ % 2 ) == 1 );
			if( ti < startIdx ) continue;
			const bool fullTile = ( cap.fullFrame || isPostBlock );
			const double sampleMean = fullTile ? cap.meanAll : cap.meanInterior;
			const double refMean = regionRefMean( cap, !fullTile );
			const double ratio = ( refMean > 0.0 ) ? ( sampleMean / refMean ) : 0.0;
			minLateRatio = std::min( minLateRatio, ratio );
			maxLateRatio = std::max( maxLateRatio, ratio );
			if( ratio < 0.90 || ratio > 1.10 ) {
				allLateTilesFullBrightness = false;
			}
		}
		Check( okLiveFull && allLateTilesFullBrightness,
			Fmt( "P: %s multi-pass OutputIntermediateImage retains full luminance (n=%zu, late-pass ratio [%.3f, %.3f] in [0.90, 1.10])",
				rasts[r][0], intermFull.size(), minLateRatio, maxLateRatio ) );
		const double cancelRatio = ( fullMean > 0.0 ) ? ( cancelMean / fullMean ) : 0.0;
		Check( okLiveCancel && wasCancelled && cancelRatio > 0.85 && cancelRatio < 1.15,
			Fmt( "P: %s mid-pass-3 cancelled progressive output retains full luminance (ratio %.3f in [0.85, 1.15])", rasts[r][0], cancelRatio ) );
	}

	// 4. Perspective (`pinhole_camera`) + `spot_light` -> `perfectreflector_material`
	//    mirror -> `lambertian_material` wall non-adaptive progressive BDPT across
	//    Pel, Spectral (NM), and Spectral HWSS: `s=0/1/2` are identically zero on
	//    this caustic (`spot_light` has zero area and points away from the wall;
	//    `perfectreflector_material` has delta BSDF), so 100% of image radiance
	//    comes from `(s = 3, t = 1)` light-to-camera splats in `pSplatFilm`.
	//    Cancelling after pass 1/4 must normalize `pSplatFilm` by completed samples
	//    (4 SPP) rather than full-render target SPP (16 SPP, which darkened by 4x).
	{
		struct PerspCase { const char* label; const char* rast; };
		const PerspCase perspCases[3] = {
			{ "bdpt_pel", "bdpt_pel_rasterizer\n{\n\tmax_eye_depth 4\n\tmax_light_depth 4\n\tsamples 16\n\tprogressive_samples_per_pass 4\n\tprogressive_rendering TRUE\n\tadaptive_max_samples 0\n\tpixel_sampler uniform\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n\n" },
			{ "bdpt_spectral_nm", "bdpt_spectral_rasterizer\n{\n\tmax_eye_depth 4\n\tmax_light_depth 4\n\tsamples 16\n\tprogressive_samples_per_pass 4\n\tprogressive_rendering TRUE\n\tadaptive_max_samples 0\n\thwss FALSE\n\tnmbegin 500\n\tnmend 600\n\tnum_wavelengths 16\n\tpixel_sampler uniform\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n\n" },
			{ "bdpt_spectral_hwss", "bdpt_spectral_rasterizer\n{\n\tmax_eye_depth 4\n\tmax_light_depth 4\n\tsamples 16\n\tprogressive_samples_per_pass 4\n\tprogressive_rendering TRUE\n\tadaptive_max_samples 0\n\thwss TRUE\n\tnmbegin 500\n\tnmend 600\n\tnum_wavelengths 16\n\tpixel_sampler uniform\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n\n" }
		};
		for( int pc = 0; pc < 3; ++pc ) {
			std::ostringstream sc;
			sc << "RISE ASCII SCENE 7\n"
			   << "film\n{\n\twidth 32\n\theight 32\n\tpixelAR 1.0\n}\n\n"
			   << "pinhole_camera\n{\n\tlocation 0 0 2.0\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 45.0\n}\n\n"
			   << "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
			   << perspCases[pc].rast
			   << "uniformcolor_painter\n{\n\tname p_white\n\tcolor 0.9 0.9 0.9\n\tcolorspace RISERGB\n}\n\n"
			   << "lambertian_material\n{\n\tname m_wall\n\treflectance p_white\n}\n\n"
			   << "perfectreflector_material\n{\n\tname m_mirror\n\treflectance p_white\n}\n\n"
			   << "clippedplane_geometry\n{\n\tname g_wall\n\tpta -4 -4 0\n\tptb 4 -4 0\n\tptc 4 4 0\n\tptd -4 4 0\n\tdoublesided TRUE\n}\n\n"
			   << "standard_object\n{\n\tname wall\n\tgeometry g_wall\n\tmaterial m_wall\n}\n\n"
			   << "clippedplane_geometry\n{\n\tname g_mirror\n\tpta -4 -4 3.5\n\tptb 4 -4 3.5\n\tptc 4 4 3.5\n\tptd -4 4 3.5\n\tdoublesided TRUE\n}\n\n"
			   << "standard_object\n{\n\tname mirror\n\tgeometry g_mirror\n\tmaterial m_mirror\n}\n\n"
			   << "spot_light\n{\n\tname key\n\tpower 40.0\n\tinner 20.0\n\touter 30.0\n\tposition 0 0 2.5\n\ttarget 0 0 3.5\n\tcolor 1.0 1.0 1.0\n}\n";
			const std::string scText = sc.str();
			auto runPersp = [&]( double cancelFrac, double& outMean, bool& outCancelled ) -> bool {
				const std::string pth = TempPath( "prog_persp_bdpt", "RISEscene" );
				{ std::ofstream ofs( pth ); ofs << scText; }
				bool okRun = false;
				IJobPriv* pJob = nullptr;
				if( RISE_CreateJobPriv( &pJob ) && pJob && pJob->LoadAsciiSceneViaCst( pth.c_str() ) ) {
					pJob->RemoveRasterizerOutputs();
					CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
					GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "capture" );
					pJob->GetRasterizer()->AddRasterizerOutput( pCap );
					if( Rasterizer* pRas = dynamic_cast<Rasterizer*>( pJob->GetRasterizer() ) ) {
						pRas->ForTest_SetThreadCountOverride( 1 );
					}
					CancellingProgressCallback cb;
					cb.cancelFraction = cancelFrac;
					if( cancelFrac <= 1.0 ) {
						pJob->SetProgress( &cb );
					}
					SobolSamplerTestHooks::ValueSalt().store( SobolSequence::HashCombine( 7777u, 0x459u ) );
					std::srand( 7777u );
					GlobalRNG().Reseed( 7777u );
					pJob->Rasterize();
					SobolSamplerTestHooks::ValueSalt().store( 0u );
					pJob->SetProgress( nullptr );
					outCancelled = cb.cancelled;
					if( !pCap->pixels.empty() ) {
						double sum = 0.0;
						for( const RISEColor& c : pCap->pixels ) {
							sum += ( c.base.r + c.base.g + c.base.b ) * c.a / 3.0;
						}
						outMean = sum / double( pCap->pixels.size() );
						okRun = std::isfinite( outMean ) && outMean > 0.0;
					}
					safe_release( pCap );
				}
				safe_release( pJob );
				std::remove( pth.c_str() );
				return okRun;
			};
			double meanFull16 = 0.0, meanCancelPass1 = 0.0;
			bool dummyC = false, didC = false;
			const bool ok16 = runPersp( 2.0, meanFull16, dummyC );
			// Cancel at 0.25 (cleanly after pass 1 of 4 completes = 4 of 16 SPP): without
			// unconditional AddAdaptiveSamples, t==1 splats divide by 16 instead of 4,
			// dropping total output luminance to ~0.625x of full brightness!
			const bool okC1 = runPersp( 0.25, meanCancelPass1, didC );
			const double ratioP1 = ( meanFull16 > 0.0 ) ? ( meanCancelPass1 / meanFull16 ) : 0.0;
			Check( ok16 && okC1 && didC && ratioP1 > 0.88 && ratioP1 < 1.12,
				Fmt( "P.4: %s pinhole+omni progressive cancel after pass 1/4 retains full t==1 splat luminance (ratio %.3f in [0.88, 1.12])",
					perspCases[pc].label, ratioP1 ) );
		}
	}
}

//! H: hashes of GetArea and 4096 samples of shapes whose arithmetic must
//! not change (rectangle, parallelogram sampler) -- compare across builds.
static void PrintHashes()
{
	std::cout << "H: bit-identity hashes (compare against the pre-DL-460 build)\n";
	for( int which = 0; which < 2; which++ ) {
		ClippedPlaneGeometry* g = MakeQuad( which == 0 ? kRectangle : kParallelogram );
		unsigned long long h = 1469598103934665603ull;
		auto mix = [&h]( double d ) { unsigned long long b; std::memcpy( &b, &d, 8 ); h = ( h ^ b ) * 1099511628211ull; };
		for( unsigned i = 0; i < 4096; i++ ) {
			Point3 p; Vector3 n; Point2 uv;
			g->UniformRandomPoint( &p, &n, &uv, Prand( i, 4096 ) );
			mix( p.x ); mix( p.y ); mix( p.z ); mix( n.x ); mix( n.y ); mix( n.z ); mix( uv.x ); mix( uv.y );
		}
		std::printf( "  H %s area=%.17g samples=%016llx\n", which == 0 ? "rectangle" : "parallelogram", double( g->GetArea() ), h );
		safe_release( g );
	}
}

//! T: per-sample UniformRandomPoint cost of an SDF luminary (not a gate;
//! compare across builds).
static void PrintSdfCost()
{
	std::cout << "T: SDF UniformRandomPoint cost (ns per sample)\n";
	struct Case { const char* name; std::vector<SDFGeometry::Part> parts; };
	std::vector<Case> cases;
	{
		Case c{ "sphere r1", {} };
		c.parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0, Point3( 0, 0, 0 ), 0, 0, 0, Vector3( 1, 1, 1 ), 1.0, 0, 0, 0 ) );
		cases.push_back( c );
	}
	{
		Case c{ "smin blob of 3 + box", {} };
		c.parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0, Point3( 0, 0, 0 ), 0, 0, 0, Vector3( 1, 1, 1 ), 1.0, 0, 0, 0 ) );
		c.parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpSmin, 0.3, Point3( 1.2, 0, 0 ), 0, 0, 0, Vector3( 1, 1, 1 ), 0.7, 0, 0, 0 ) );
		c.parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpSmin, 0.3, Point3( 0, 1.1, 0.3 ), 0, 0, 0, Vector3( 1, 1, 1 ), 0.6, 0, 0, 0 ) );
		c.parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpSubtract, 0.1, Point3( 0, -0.8, 0 ), 20, 0, 10, Vector3( 1, 1, 1 ), 0.5, 0.3, 0.5, 0 ) );
		cases.push_back( c );
	}
	for( const Case& c : cases ) {
		SDFGeometry* g = new SDFGeometry( c.parts, 512, Scalar( 0 ), 64 );
		g->GetArea();	// build the sampling structure outside the timing
		const unsigned M = 400000;
		double sink = 0;
		const auto t0 = std::chrono::steady_clock::now();
		for( unsigned i = 0; i < M; i++ ) {
			Point3 p; Vector3 n;
			g->UniformRandomPoint( &p, &n, nullptr, Prand( i, M ) );
			sink += p.x + n.y;
		}
		const double ns = std::chrono::duration<double, std::nano>( std::chrono::steady_clock::now() - t0 ).count() / M;
		std::printf( "  T %s: %.0f ns/sample (checksum %g)\n", c.name, ns, sink );
		safe_release( g );
	}
}

int main( int argc, char** argv )
{
	std::setvbuf( stdout, nullptr, _IONBF, 0 );
	int argIdx = 1;
	if( argc > argIdx ) {
		char* end = nullptr;
		const unsigned long parsed = std::strtoul( argv[argIdx], &end, 10 );
		if( end != argv[argIdx] && *end == '\0' ) {
			g_seedBase = unsigned( parsed );
			argIdx++;
		}
	}
	const std::string only = argc > argIdx ? argv[argIdx++] : "all";
	const bool hasExplicitSpp = argc > argIdx;
	if( hasExplicitSpp ) g_spp = std::atoi( argv[argIdx++] );
	const bool hasExplicitRepeats = argc > argIdx;
	if( hasExplicitRepeats ) { g_repeats = std::atoi( argv[argIdx++] ); g_trapRepeats = g_repeats; }
	const char* extEnv = std::getenv( "RISE_TEST_EXTENDED" );
	const bool extended = ( only == "Q" ) || ( extEnv != nullptr && std::strcmp( extEnv, "1" ) == 0 );
	const int qSpp = ( only == "Q" && hasExplicitSpp ) ? g_spp : ( extended ? 1024 : 256 );
	const int qRepeats = ( only == "Q" && hasExplicitRepeats ) ? g_repeats : ( extended ? 64 : 16 );
	std::cout << "LuminarySamplerAreaTest (DL-459 / DL-460)\n";
	if( only == "all" || only == "A" ) TestAreas();
	if( only == "all" || only == "B" ) TestUniformity();
	if( only == "all" || only == "C" ) TestRenders( false );
	if( only == "C_trap" ) TestRenders( true );
	if( only == "all" || only == "P" ) TestProgressiveColorAndOrthoCamera();
	if( only == "all" || only == "Q" ) TestHighPowerTrapezoidQuadrature( qSpp, qRepeats );
	if( only == "all" || only == "D" ) TestMultiThreadDeterminism();
	if( only == "all" || only == "F" ) TestFreshProcessDeterminism( argv[0] );
	if( only.rfind( "F_child_", 0 ) == 0 ) FreshProcessChild( only.substr( 8 ) );
	if( only == "all" || only == "H" ) PrintHashes();
	if( only == "T" ) PrintSdfCost();
	std::cout << "\n" << passCount << " passed, " << failCount << " failed\n";
	return failCount == 0 ? 0 : 1;
}
