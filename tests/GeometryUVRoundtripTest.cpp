//////////////////////////////////////////////////////////////////////
//
//  GeometryUVRoundtripTest.cpp
//
//    Standalone regression test that verifies, for each primitive
//    geometry type with a TessellateToMesh override, that the
//    (u, v) emitted by IntersectRay AND UniformRandomPoint actually
//    parameterises the surface — i.e. feeding the returned UV back
//    through TessellateToMesh's parameterisation reconstructs the
//    surface point.
//
//    Why this test exists:
//      EllipsoidGeometry shipped two related bugs in IntersectRay's
//      and UniformRandomPoint's UV computation.  Both were calling
//      GeometricUtilities::SphereTextureCoord with non-unit "axis"
//      vectors of magnitude m_OVmaxRadius (≈ 0.005 for a unit-ish
//      ellipsoid).  Result: every dot product landed in a tiny band
//      around zero, so v collapsed to ≈ 0.5 for every hit.  Even
//      with correct unit vectors, SphereTextureCoord's gradient-based
//      derivation does not match the position-based parameterisation
//      that EllipsoidGeometry::TessellateToMesh uses (the gradient
//      direction is not the surface position direction when
//      a ≠ b ≠ c).  The fix replaces the call with an explicit
//      position-based inverse:
//
//          phi   = acos( P_y / b )
//          theta = atan2( P_z / c, -P_x / a )
//          u     = theta / 2π,    v = phi / π
//
//    This regression test then drove a wave of follow-up principled
//    fixes across the rest of the geometry hierarchy so that for
//    every primitive, IntersectRay's (u, v) is consistent with
//    TessellateToMesh's parameterisation:
//
//      cylinder: CylinderTextureCoord rewritten from acos(z/r) (with
//                a wrap-flip on x) to atan2(b, a) where (a, b) are
//                the two radial coordinates TessellateToMesh uses,
//                so u (the angular fraction) roundtrips through
//                theta = u·2π.
//
//      torus:    TorusTextureCoord rewritten to derive (u, v) from
//                the position alone (atan2 of x,z for the ring angle;
//                atan2 of y, dXZ-R for the tube angle), matching
//                TessellateToMesh's ring/tube convention.
//
//      disk:     CircularDiskGeometry now uses polar (u, v) ∈ [0, 1]²
//                everywhere (matching TessellateToMesh): u = θ/2π,
//                v = r/R.  Two issues were folded in: the in-disk
//                membership check now uses the two in-plane axes for
//                the disk's orientation rather than always (x, y),
//                fixing X/Y-axis disks that previously accepted hits
//                arbitrarily far along their out-of-plane radial
//                direction.
//
//      bilinear patch: TessellateToMesh's weight assignment was
//                fixed to match GeometricUtilities::EvaluateBilinearPatchAt
//                and RayBilinearPatchIntersection — pts[1] at (0, 1),
//                pts[2] at (1, 0), pts[3] at (1, 1).  The previous
//                row-major convention disagreed with IntersectRay,
//                so the surface emitted by tessellation differed from
//                the surface IntersectRay traced.
//
//      clipped plane: IntersectRay's per-triangle UV corner
//                assignments were updated to vP[0]=(0,0), vP[1]=(1,0),
//                vP[2]=(1,1), vP[3]=(0,1), matching TessellateToMesh.
//                For genuinely non-planar quads the two flat
//                triangles span a different surface than the bilinear
//                surface used by TessellateToMesh; corner UVs still
//                agree.  The strict roundtrip below uses a planar
//                quad to exercise the corner UV alignment.
//
//    All geometries below are now exercised with the same three
//    properties (no more "soft" coverage-only tests):
//
//      1. UV-position roundtrip via IntersectRay: the (u, v) returned
//         must reconstruct the hit position via the TessellateToMesh
//         forward formula.
//
//      2. UV coverage histogram: u and v each cover at least 6/16
//         non-empty bins, no single bin holding ≥85% of samples.
//         Catches "v ≈ 0.5 collapse" symptoms regardless of geometry.
//
//      3. UV-position roundtrip via UniformRandomPoint: same property
//         as (1) but driven by UniformRandomPoint instead of
//         IntersectRay.  Surface-membership invariants (point lies on
//         the implicit surface) are also asserted here.
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <vector>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../src/Library/Geometry/BilinearPatchGeometry.h"
#include "../src/Library/Geometry/BoxGeometry.h"
#include "../src/Library/Geometry/CircularDiskGeometry.h"
#include "../src/Library/Geometry/ClippedPlaneGeometry.h"
#include "../src/Library/Geometry/CylinderGeometry.h"
#include "../src/Library/Geometry/EllipsoidGeometry.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Geometry/TorusGeometry.h"
#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Intersection/RayPrimitiveIntersections.h"
#include "../src/Library/Functions/Polynomial.h"
#include "../src/Library/Utilities/GeometricUtilities.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Geometry/InfinitePlaneGeometry.h"

using namespace RISE;
using namespace RISE::Implementation;

// ============================================================
// Test infrastructure
// ============================================================

static unsigned int g_failures = 0;

#define REQUIRE( cond, label ) do { \
	if( !(cond) ) { \
		std::cout << "  FAIL [" << label << "] at " << __FILE__ << ":" << __LINE__ << std::endl; \
		g_failures++; \
	} \
} while(0)

static bool IsClose( Scalar a, Scalar b, Scalar eps )
{
	return std::fabs( a - b ) < eps;
}

static bool IsPointClose( const Point3& a, const Point3& b, Scalar eps )
{
	return IsClose( a.x, b.x, eps )
		&& IsClose( a.y, b.y, eps )
		&& IsClose( a.z, b.z, eps );
}

// Deterministic LCG so the test is reproducible across runs / platforms.
struct LCG
{
	unsigned long long state;
	explicit LCG( unsigned long long seed ) : state( seed ) {}

	Scalar next01()
	{
		state = state * 6364136223846793005ULL + 1442695040888963407ULL;
		return Scalar( (state >> 11) & ((1ULL << 53) - 1) ) / Scalar( 1ULL << 53 );
	}
};

// Generate a ray that hits the geometry from a uniformly random direction
// on a sphere enclosing it.  Aim every ray at the bounding-sphere centre
// (the geometry origin in object space) plus a small lateral jitter, so any
// shape containing that centre is hit broadly across its surface.
static bool ShootHit(
	const IGeometry& g,
	LCG& rng,
	RayIntersectionGeometric& ri )
{
	Point3 sphereCenter;
	Scalar sphereRadius;
	g.GenerateBoundingSphere( sphereCenter, sphereRadius );

	if( !std::isfinite( sphereRadius ) || sphereRadius > 1e6 ) {
		sphereRadius = 1.0;
	}

	const Scalar u = rng.next01();
	const Scalar v = rng.next01();
	const Scalar costheta = 1.0 - 2.0 * u;
	const Scalar sintheta = std::sqrt( std::max( 0.0, 1.0 - costheta * costheta ) );
	const Scalar phi      = 2.0 * PI * v;
	const Vector3 originDir( sintheta * std::cos( phi ),
	                         sintheta * std::sin( phi ),
	                         costheta );

	Vector3 axisU;
	if( std::fabs( originDir.x ) < 0.9 ) {
		axisU = Vector3Ops::Normalize( Vector3Ops::Cross( originDir, Vector3( 1, 0, 0 ) ) );
	} else {
		axisU = Vector3Ops::Normalize( Vector3Ops::Cross( originDir, Vector3( 0, 1, 0 ) ) );
	}
	const Vector3 axisV = Vector3Ops::Cross( originDir, axisU );

	const Scalar j1 = (rng.next01() * 2.0) - 1.0;
	const Scalar j2 = (rng.next01() * 2.0) - 1.0;
	const Scalar jr = sphereRadius * 0.3 * std::sqrt( j1 * j1 + j2 * j2 );
	const Scalar jt = std::atan2( j2, j1 );
	const Vector3 lateral = axisU * (jr * std::cos( jt ))
	                      + axisV * (jr * std::sin( jt ));

	const Point3 target(
		sphereCenter.x + lateral.x,
		sphereCenter.y + lateral.y,
		sphereCenter.z + lateral.z );

	const Point3 origin(
		sphereCenter.x + 4.0 * sphereRadius * originDir.x,
		sphereCenter.y + 4.0 * sphereRadius * originDir.y,
		sphereCenter.z + 4.0 * sphereRadius * originDir.z );

	const Vector3 rayDir = Vector3Ops::Normalize(
		Vector3Ops::mkVector3( target, origin ) );

	ri = RayIntersectionGeometric( Ray( origin, rayDir ), nullRasterizerState );
	g.IntersectRay( ri, /*bHitFrontFaces=*/true, /*bHitBackFaces=*/true,
		/*bComputeExitInfo=*/false );
	return ri.bHit;
}

// ============================================================
// Per-geometry "TessellateToMesh forward" formulas — the inverse
// of the TessellateToMesh code in src/Library/Geometry/*.cpp.
//
// Keep these MATCHED with the corresponding TessellateToMesh body.
// If TessellateToMesh changes, this file must change to match.
// ============================================================

static Point3 SphereTessParamToPos( Scalar r, Scalar u, Scalar v )
{
	// SphereGeometry::TessellateToMesh:
	//   phi = v*PI, theta = u*TWO_PI
	//   pos = r * (-sin(phi)*cos(theta), cos(phi), sin(phi)*sin(theta))
	const Scalar phi   = v * PI;
	const Scalar theta = u * TWO_PI;
	const Scalar sP = std::sin( phi );
	const Scalar cP = std::cos( phi );
	const Scalar sT = std::sin( theta );
	const Scalar cT = std::cos( theta );
	return Point3( -r * sP * cT, r * cP, r * sP * sT );
}

static Point3 EllipsoidTessParamToPos( const Vector3& radii, Scalar u, Scalar v )
{
	// EllipsoidGeometry::TessellateToMesh:
	//   pos = (a*-sin(phi)*cos(theta), b*cos(phi), c*sin(phi)*sin(theta))
	const Scalar a = radii.x;  // m_vRadius is the semi-axis
	const Scalar b = radii.y;
	const Scalar c = radii.z;
	const Scalar phi   = v * PI;
	const Scalar theta = u * TWO_PI;
	const Scalar sP = std::sin( phi );
	const Scalar cP = std::cos( phi );
	const Scalar sT = std::sin( theta );
	const Scalar cT = std::cos( theta );
	return Point3( -a * sP * cT, b * cP, c * sP * sT );
}

static Point3 BoxTessParamToPos(
	Scalar w, Scalar h, Scalar d, int faceIndex, Scalar u, Scalar v )
{
	const Scalar hw = w * 0.5;
	const Scalar hh = h * 0.5;
	const Scalar hd = d * 0.5;
	switch( faceIndex ) {
	case 0: return Point3( -hw,  hh - v * h, -hd + u * d );
	case 1: return Point3(  hw,  hh - v * h,  hd - u * d );
	case 2: return Point3( -hw + u * w, -hh,  hd - v * d );
	case 3: return Point3( -hw + u * w,  hh, -hd + v * d );
	case 4: return Point3(  hw - u * w,  hh - v * h, -hd );
	case 5: return Point3( -hw + u * w,  hh - v * h,  hd );
	}
	return Point3( 0, 0, 0 );
}

static int BoxFaceFromNormal( const Vector3& n )
{
	const Scalar ax = std::fabs( n.x );
	const Scalar ay = std::fabs( n.y );
	const Scalar az = std::fabs( n.z );
	if( ax >= ay && ax >= az ) return n.x < 0 ? 0 : 1;
	if( ay >= az )             return n.y < 0 ? 2 : 3;
	return n.z < 0 ? 4 : 5;
}

static Point3 CylinderTessParamToPos(
	char axis, Scalar radius, Scalar axisMin, Scalar axisMax, Scalar u, Scalar v )
{
	// CylinderGeometry::TessellateToMesh:
	//   axis 'x':  pos = (axial, r·cos(θ), r·sin(θ))
	//   axis 'y':  pos = (r·cos(θ), axial, r·sin(θ))
	//   axis 'z':  pos = (r·cos(θ), r·sin(θ), axial)
	// with θ = u·2π and axial = axisMin + v·(axisMax - axisMin).
	const Scalar theta = u * TWO_PI;
	const Scalar axial = axisMin + v * (axisMax - axisMin);
	const Scalar c = radius * std::cos( theta );
	const Scalar s = radius * std::sin( theta );
	switch( axis ) {
	case 'x': return Point3( axial, c, s );
	case 'y': return Point3( c, axial, s );
	default:
	case 'z': return Point3( c, s, axial );
	}
}

static Point3 TorusTessParamToPos(
	Scalar R, Scalar r, Scalar u, Scalar v )
{
	// TorusGeometry::TessellateToMesh:
	//   pos = ( (R + r·cos(V))·cos(U), r·sin(V), (R + r·cos(V))·sin(U) )
	// with U = u·2π and V = v·2π.
	const Scalar U = u * TWO_PI;
	const Scalar V = v * TWO_PI;
	const Scalar ringR = R + r * std::cos( V );
	return Point3(
		ringR * std::cos( U ),
		r * std::sin( V ),
		ringR * std::sin( U ) );
}

static Point3 DiskTessParamToPos(
	char axis, Scalar R, Scalar u, Scalar v )
{
	// CircularDiskGeometry::TessellateToMesh:
	//   axis 'x':  pos = (0, r·cos(θ), r·sin(θ))
	//   axis 'y':  pos = (r·sin(θ), 0, r·cos(θ))
	//   axis 'z':  pos = (r·cos(θ), r·sin(θ), 0)
	// with θ = u·2π and r = v·R.
	const Scalar theta = u * TWO_PI;
	const Scalar r = v * R;
	const Scalar c = r * std::cos( theta );
	const Scalar s = r * std::sin( theta );
	switch( axis ) {
	case 'x': return Point3( 0.0, c, s );
	case 'y': return Point3( s, 0.0, c );
	default:
	case 'z': return Point3( c, s, 0.0 );
	}
}

static Point3 ClippedPlaneTessParamToPos(
	const Point3 corners[4], Scalar u, Scalar v )
{
	// ClippedPlaneGeometry::TessellateToMesh: row-major corner UVs.
	//   vP[0] at (u=0, v=0), vP[1] at (1, 0), vP[2] at (1, 1), vP[3] at (0, 1).
	const Scalar w00 = (1.0 - u) * (1.0 - v);
	const Scalar w10 =        u  * (1.0 - v);
	const Scalar w11 =        u  *        v;
	const Scalar w01 = (1.0 - u) *        v;
	return Point3(
		w00 * corners[0].x + w10 * corners[1].x + w11 * corners[2].x + w01 * corners[3].x,
		w00 * corners[0].y + w10 * corners[1].y + w11 * corners[2].y + w01 * corners[3].y,
		w00 * corners[0].z + w10 * corners[1].z + w11 * corners[2].z + w01 * corners[3].z );
}

static Point3 BilinearPatchTessParamToPos(
	const BilinearPatch& p, Scalar u, Scalar v )
{
	// BilinearPatchGeometry::TessellateToMesh now matches the canonical
	// RISE convention used by EvaluateBilinearPatchAt:
	//   pts[0] at (u=0, v=0)
	//   pts[1] at (u=0, v=1)
	//   pts[2] at (u=1, v=0)
	//   pts[3] at (u=1, v=1)
	const Scalar w00 = (1.0 - u) * (1.0 - v);
	const Scalar w01 = (1.0 - u) *        v;
	const Scalar w10 =        u  * (1.0 - v);
	const Scalar w11 =        u  *        v;
	return Point3(
		w00 * p.pts[0].x + w01 * p.pts[1].x + w10 * p.pts[2].x + w11 * p.pts[3].x,
		w00 * p.pts[0].y + w01 * p.pts[1].y + w10 * p.pts[2].y + w11 * p.pts[3].y,
		w00 * p.pts[0].z + w01 * p.pts[1].z + w10 * p.pts[2].z + w11 * p.pts[3].z );
}

// ============================================================
// UV histogram — common coverage check.
//
// The ellipsoid v-collapse bug puts every hit's v in a single bin.
// We require:
//   * uHits and vHits have at least kMinNonEmptyBins non-empty bins.
//   * No single bin holds more than kMaxFractionInOneBin of the samples.
// ============================================================

struct CoverageStats
{
	std::vector<int> uHits;
	std::vector<int> vHits;
	int totalSamples;
};

static CoverageStats MakeCoverage( int kBins = 16 )
{
	CoverageStats c;
	c.uHits.assign( kBins, 0 );
	c.vHits.assign( kBins, 0 );
	c.totalSamples = 0;
	return c;
}

static void RecordCoverage(
	CoverageStats& c, Scalar u, Scalar v )
{
	const int kBins = static_cast<int>( c.uHits.size() );
	int ui = static_cast<int>( std::floor( u * kBins ) );
	int vi = static_cast<int>( std::floor( v * kBins ) );
	if( ui < 0 )       ui = 0;
	if( ui >= kBins )  ui = kBins - 1;
	if( vi < 0 )       vi = 0;
	if( vi >= kBins )  vi = kBins - 1;
	c.uHits[ui]++;
	c.vHits[vi]++;
	c.totalSamples++;
}

static void AssertCoverage(
	const CoverageStats& c, const char* label,
	int   kMinNonEmptyBinsU = 6,
	int   kMinNonEmptyBinsV = 6,
	Scalar kMaxFractionInOneBin = 0.85 )
{
	if( c.totalSamples == 0 ) {
		std::cout << "  [" << label << "] no samples — coverage unverifiable\n";
		REQUIRE( false, std::string(label) + " coverage" );
		return;
	}

	int nonEmptyU = 0, nonEmptyV = 0;
	int maxBinU = 0,    maxBinV = 0;
	for( int n : c.uHits ) { if( n > 0 ) nonEmptyU++; if( n > maxBinU ) maxBinU = n; }
	for( int n : c.vHits ) { if( n > 0 ) nonEmptyV++; if( n > maxBinV ) maxBinV = n; }

	const Scalar fracU = Scalar(maxBinU) / Scalar(c.totalSamples);
	const Scalar fracV = Scalar(maxBinV) / Scalar(c.totalSamples);

	if( std::getenv( "GEOM_UV_DEBUG" ) ) {
		std::printf( "  [%s] samples=%d  uBins=%d/%zu (max-frac=%.2f)  vBins=%d/%zu (max-frac=%.2f)\n",
			label, c.totalSamples,
			nonEmptyU, c.uHits.size(), fracU,
			nonEmptyV, c.vHits.size(), fracV );
	}

	REQUIRE( nonEmptyU >= kMinNonEmptyBinsU,
		std::string(label) + " u coverage (non-empty bins)" );
	REQUIRE( nonEmptyV >= kMinNonEmptyBinsV,
		std::string(label) + " v coverage (non-empty bins)" );
	REQUIRE( fracU < kMaxFractionInOneBin,
		std::string(label) + " u not collapsed to one bin" );
	REQUIRE( fracV < kMaxFractionInOneBin,
		std::string(label) + " v not collapsed to one bin" );
}

static void AssertUVInUnitSquare(
	Scalar u, Scalar v, const std::string& label )
{
	REQUIRE( u >= -1e-5 && u <= 1.0 + 1e-5, label + " u in [0,1]" );
	REQUIRE( v >= -1e-5 && v <= 1.0 + 1e-5, label + " v in [0,1]" );
}

// ============================================================
// Sphere
// ============================================================

static void TestSphere()
{
	std::cout << "Testing SphereGeometry UV roundtrip..." << std::endl;

	const Scalar r = 1.5;
	SphereGeometry* g = new SphereGeometry( r );

	const int N = 1000;
	const Scalar tol = 1e-3 * r;
	LCG rng( 12345 );
	CoverageStats cov = MakeCoverage();

	int nHits = 0;
	for( int i = 0; i < N; ++i ) {
		RayIntersectionGeometric ri( Ray(), nullRasterizerState );
		if( !ShootHit( *g, rng, ri ) ) continue;
		nHits++;

		AssertUVInUnitSquare( ri.ptCoord.x, ri.ptCoord.y, "sphere IntersectRay" );

		const Point3 reconstructed = SphereTessParamToPos( r, ri.ptCoord.x, ri.ptCoord.y );
		REQUIRE( IsPointClose( reconstructed, ri.ptIntersection, tol ),
			"sphere IntersectRay UV->pos roundtrip" );

		RecordCoverage( cov, ri.ptCoord.x, ri.ptCoord.y );
	}
	REQUIRE( nHits > N / 2, "sphere enough hits for stats" );
	AssertCoverage( cov, "sphere IntersectRay" );

	CoverageStats covRand = MakeCoverage();
	const int Mpoints = 1000;
	int nPts = 0;
	for( int i = 0; i < Mpoints; ++i ) {
		Point3  p; Vector3 n; Point2 uv;
		const Point3 prand( rng.next01(), rng.next01(), rng.next01() );
		g->UniformRandomPoint( &p, &n, &uv, prand );

		const Scalar pmag = std::sqrt( p.x*p.x + p.y*p.y + p.z*p.z );
		REQUIRE( IsClose( pmag, r, tol ), "sphere UniformRandomPoint on surface" );

		const Scalar dotPN = p.x*n.x + p.y*n.y + p.z*n.z;
		REQUIRE( dotPN > 0.0, "sphere UniformRandomPoint normal outward" );

		AssertUVInUnitSquare( uv.x, uv.y, "sphere UniformRandomPoint" );

		const Point3 reconstructed = SphereTessParamToPos( r, uv.x, uv.y );
		REQUIRE( IsPointClose( reconstructed, p, tol ),
			"sphere UniformRandomPoint UV->pos roundtrip" );
		nPts++;
		RecordCoverage( covRand, uv.x, uv.y );
	}
	REQUIRE( nPts > Mpoints / 2, "sphere enough random points" );
	AssertCoverage( covRand, "sphere UniformRandomPoint" );

	g->release();
	std::cout << "  sphere: " << nHits << " ray hits, " << nPts << " random points\n";
}

// ============================================================
// Ellipsoid
// ============================================================

static void TestEllipsoidOne( const Vector3& diameters, const char* label )
{
	std::cout << "Testing EllipsoidGeometry UV roundtrip [" << label << "] ..." << std::endl;

	EllipsoidGeometry* g = new EllipsoidGeometry( diameters );

	Point3 bsCenter; Scalar bsRadius;
	g->GenerateBoundingSphere( bsCenter, bsRadius );
	const Scalar tol = 1e-3 * bsRadius;

	const int N = 1500;
	LCG rng( 67890 );
	CoverageStats cov = MakeCoverage();

	int nHits = 0;
	for( int i = 0; i < N; ++i ) {
		RayIntersectionGeometric ri( Ray(), nullRasterizerState );
		if( !ShootHit( *g, rng, ri ) ) continue;
		nHits++;

		AssertUVInUnitSquare( ri.ptCoord.x, ri.ptCoord.y,
			std::string("ellipsoid[") + label + "] IntersectRay" );

		const Point3 reconstructed = EllipsoidTessParamToPos(
			diameters, ri.ptCoord.x, ri.ptCoord.y );
		REQUIRE( IsPointClose( reconstructed, ri.ptIntersection, tol ),
			std::string("ellipsoid[") + label + "] IntersectRay UV->pos roundtrip" );

		RecordCoverage( cov, ri.ptCoord.x, ri.ptCoord.y );
	}
	REQUIRE( nHits > N / 2, "ellipsoid enough hits for stats" );
	AssertCoverage( cov, (std::string("ellipsoid[") + label + "] IntersectRay").c_str() );

	CoverageStats covRand = MakeCoverage();
	const int Mpoints = 1000;
	int nPts = 0;
	for( int i = 0; i < Mpoints; ++i ) {
		Point3  p; Vector3 n; Point2 uv;
		const Point3 prand( rng.next01(), rng.next01(), rng.next01() );
		g->UniformRandomPoint( &p, &n, &uv, prand );

		const Scalar a = diameters.x;  // radii ARE the semi-axes now
		const Scalar b = diameters.y;
		const Scalar c = diameters.z;
		const Scalar implicit =
			(p.x * p.x) / (a * a) +
			(p.y * p.y) / (b * b) +
			(p.z * p.z) / (c * c);
		REQUIRE( IsClose( implicit, 1.0, 1e-5 ),
			std::string("ellipsoid[") + label + "] UniformRandomPoint on surface" );

		AssertUVInUnitSquare( uv.x, uv.y,
			std::string("ellipsoid[") + label + "] UniformRandomPoint" );

		const Point3 reconstructed = EllipsoidTessParamToPos( diameters, uv.x, uv.y );
		REQUIRE( IsPointClose( reconstructed, p, tol ),
			std::string("ellipsoid[") + label + "] UniformRandomPoint UV->pos roundtrip" );
		nPts++;
		RecordCoverage( covRand, uv.x, uv.y );
	}
	REQUIRE( nPts > Mpoints / 2, "ellipsoid enough random points" );
	AssertCoverage( covRand,
		(std::string("ellipsoid[") + label + "] UniformRandomPoint").c_str() );

	g->release();
	std::cout << "  ellipsoid[" << label << "]: " << nHits
		<< " ray hits, " << nPts << " random points\n";
}

static void TestEllipsoid()
{
	TestEllipsoidOne( Vector3( 2.0, 2.0, 2.0 ), "isotropic" );
	TestEllipsoidOne( Vector3( 3.0, 2.0, 1.5 ), "mild" );
	TestEllipsoidOne( Vector3( 4.0, 2.0, 1.0 ), "strong" );
}

// ============================================================
// Box
// ============================================================

static void TestBox()
{
	std::cout << "Testing BoxGeometry UV roundtrip..." << std::endl;

	const Scalar w = 2.0, h = 3.0, d = 4.0;
	BoxGeometry* g = new BoxGeometry( w, h, d );

	const Scalar tol = 1e-4 * std::sqrt( w*w + h*h + d*d );
	const int N = 1500;
	LCG rng( 24680 );

	std::vector<int> hitsPerFace( 6, 0 );
	std::vector<CoverageStats> covPerFace;
	for( int i = 0; i < 6; ++i ) covPerFace.push_back( MakeCoverage() );

	int nHits = 0;
	for( int i = 0; i < N; ++i ) {
		RayIntersectionGeometric ri( Ray(), nullRasterizerState );
		if( !ShootHit( *g, rng, ri ) ) continue;
		nHits++;

		const int face = BoxFaceFromNormal( ri.vNormal );
		REQUIRE( face >= 0 && face < 6, "box face index in range" );

		AssertUVInUnitSquare( ri.ptCoord.x, ri.ptCoord.y, "box IntersectRay" );

		const Point3 reconstructed = BoxTessParamToPos(
			w, h, d, face, ri.ptCoord.x, ri.ptCoord.y );
		REQUIRE( IsPointClose( reconstructed, ri.ptIntersection, tol ),
			"box IntersectRay UV->pos roundtrip" );

		hitsPerFace[face]++;
		RecordCoverage( covPerFace[face], ri.ptCoord.x, ri.ptCoord.y );
	}

	for( int f = 0; f < 6; ++f ) {
		REQUIRE( hitsPerFace[f] > 5,
			std::string("box face ") + std::to_string(f) + " hit at least once" );
	}

	const int Mpoints = 1500;
	int nPts = 0;
	for( int i = 0; i < Mpoints; ++i ) {
		Point3  p; Vector3 n; Point2 uv;
		const Point3 prand( rng.next01(), rng.next01(), rng.next01() );
		g->UniformRandomPoint( &p, &n, &uv, prand );

		AssertUVInUnitSquare( uv.x, uv.y, "box UniformRandomPoint" );

		const int face = BoxFaceFromNormal( n );
		const Point3 reconstructed = BoxTessParamToPos( w, h, d, face, uv.x, uv.y );
		REQUIRE( IsPointClose( reconstructed, p, tol ),
			"box UniformRandomPoint UV->pos roundtrip" );
		nPts++;
	}
	REQUIRE( nPts > Mpoints / 2, "box enough random points" );

	// Exact area-selection contract: UniformRandomPoint claims a 1/GetArea()
	// density, so its face PMF must equal each face's area fraction.  Use a
	// stratified z sequence whose 13,000 samples divide this 2x3x4 box's
	// total area 52 exactly: X faces have area 12 (3,000 each), Y faces area
	// 8 (2,000 each), and Z faces area 6 (1,500 each).  Uniform-by-face
	// selection would instead produce about 2,167 samples on every face.
	const int areaSampleCount = 13000;
	const int expectedAreaSamples[6] = { 3000, 3000, 2000, 2000, 1500, 1500 };
	int areaSamples[6] = { 0, 0, 0, 0, 0, 0 };
	for( int i = 0; i < areaSampleCount; ++i ) {
		Point3 p; Vector3 n; Point2 uv;
		const Scalar z = ( Scalar(i) + Scalar(0.5) ) / Scalar(areaSampleCount);
		g->UniformRandomPoint( &p, &n, &uv, Point3( Scalar(0.5), Scalar(0.5), z ) );
		const int face = BoxFaceFromNormal( n );
		REQUIRE( face >= 0 && face < 6, "box area sampler face index in range" );
		if( face >= 0 && face < 6 ) {
			areaSamples[face]++;
		}
	}
	for( int f = 0; f < 6; ++f ) {
		REQUIRE( areaSamples[f] == expectedAreaSamples[f],
			std::string("box area sampler face ") + std::to_string(f) +
			" matches its exact surface-area probability" );
	}

	g->release();
	std::cout << "  box: " << nHits << " ray hits, " << nPts
		<< " random points; per-face hits: ";
	for( int f = 0; f < 6; ++f ) std::cout << hitsPerFace[f] << " ";
	std::cout << "\n";
}

// ============================================================
// Cylinder
// ============================================================

static void TestCylinderOne( char axis )
{
	const Scalar r = 1.0, h = 2.5;
	// Side-wall UV roundtrip: use the open tube (capped=false) so every ray hit
	// lands on the side surface that CylinderTessParamToPos parameterises.
	CylinderGeometry* g = new CylinderGeometry( axis, r, h, false );

	// CylinderGeometry::RegenerateData (private) sets axisMin = -h/2, axisMax = h/2.
	const Scalar axisMin = -h * 0.5;
	const Scalar axisMax =  h * 0.5;

	const int N = 1500;
	const Scalar tol = 1e-4 * std::max( r, h );
	LCG rng( 13579 );
	CoverageStats cov = MakeCoverage();
	int nHits = 0;
	const std::string axisLabel = std::string("cylinder-") + axis;

	for( int i = 0; i < N; ++i ) {
		RayIntersectionGeometric ri( Ray(), nullRasterizerState );
		if( !ShootHit( *g, rng, ri ) ) continue;
		nHits++;

		AssertUVInUnitSquare( ri.ptCoord.x, ri.ptCoord.y, axisLabel + " IntersectRay" );

		const Point3 reconstructed = CylinderTessParamToPos(
			axis, r, axisMin, axisMax, ri.ptCoord.x, ri.ptCoord.y );
		REQUIRE( IsPointClose( reconstructed, ri.ptIntersection, tol ),
			axisLabel + " IntersectRay UV->pos roundtrip" );

		RecordCoverage( cov, ri.ptCoord.x, ri.ptCoord.y );
	}
	REQUIRE( nHits > N / 4, axisLabel + " enough hits" );
	AssertCoverage( cov, (axisLabel + " IntersectRay").c_str() );

	const int Mpoints = 1000;
	int nPts = 0;
	for( int i = 0; i < Mpoints; ++i ) {
		Point3  p; Vector3 n; Point2 uv;
		const Point3 prand( rng.next01(), rng.next01(), rng.next01() );
		g->UniformRandomPoint( &p, &n, &uv, prand );

		Scalar radial = 0.0;
		switch( axis ) {
		case 'x': radial = std::sqrt( p.y * p.y + p.z * p.z ); break;
		case 'y': radial = std::sqrt( p.x * p.x + p.z * p.z ); break;
		case 'z': radial = std::sqrt( p.x * p.x + p.y * p.y ); break;
		}
		REQUIRE( IsClose( radial, r, 1e-5 ),
			axisLabel + " UniformRandomPoint on side" );

		AssertUVInUnitSquare( uv.x, uv.y, axisLabel + " UniformRandomPoint" );

		const Point3 reconstructed = CylinderTessParamToPos(
			axis, r, axisMin, axisMax, uv.x, uv.y );
		REQUIRE( IsPointClose( reconstructed, p, tol ),
			axisLabel + " UniformRandomPoint UV->pos roundtrip" );
		nPts++;
	}
	REQUIRE( nPts > Mpoints / 2, axisLabel + " enough random points" );

	g->release();
	std::cout << "  " << axisLabel << ": " << nHits << " ray hits, "
		<< nPts << " random points\n";
}

static void TestCylinder()
{
	std::cout << "Testing CylinderGeometry UV roundtrip..." << std::endl;
	for( char axis : { 'x', 'y', 'z' } ) {
		TestCylinderOne( axis );
	}
}

// ============================================================
// Torus
// ============================================================

static void TestTorus()
{
	std::cout << "Testing TorusGeometry UV roundtrip..." << std::endl;

	const Scalar R = 1.0;
	const Scalar rTube = 0.3;
	TorusGeometry* g = new TorusGeometry( R, rTube );

	const int N = 1500;
	const Scalar tol = 1e-3 * (R + rTube);
	LCG rng( 24680 );
	CoverageStats cov = MakeCoverage();
	int nHits = 0;

	for( int i = 0; i < N; ++i ) {
		RayIntersectionGeometric ri( Ray(), nullRasterizerState );
		if( !ShootHit( *g, rng, ri ) ) continue;
		nHits++;

		AssertUVInUnitSquare( ri.ptCoord.x, ri.ptCoord.y, "torus IntersectRay" );

		// Surface invariant.
		const Scalar dXZ = std::sqrt(
			ri.ptIntersection.x * ri.ptIntersection.x +
			ri.ptIntersection.z * ri.ptIntersection.z );
		const Scalar implicit = (dXZ - R) * (dXZ - R)
			+ ri.ptIntersection.y * ri.ptIntersection.y;
		REQUIRE( IsClose( implicit, rTube * rTube, 1e-3 ),
			"torus hit on surface" );

		const Point3 reconstructed = TorusTessParamToPos(
			R, rTube, ri.ptCoord.x, ri.ptCoord.y );
		REQUIRE( IsPointClose( reconstructed, ri.ptIntersection, tol ),
			"torus IntersectRay UV->pos roundtrip" );

		RecordCoverage( cov, ri.ptCoord.x, ri.ptCoord.y );
	}
	REQUIRE( nHits > N / 4, "torus enough hits" );
	AssertCoverage( cov, "torus IntersectRay" );

	const int Mpoints = 800;
	int nPts = 0;
	for( int i = 0; i < Mpoints; ++i ) {
		Point3  p; Vector3 n; Point2 uv;
		const Point3 prand( rng.next01(), rng.next01(), rng.next01() );
		g->UniformRandomPoint( &p, &n, &uv, prand );

		const Scalar dXZ = std::sqrt( p.x * p.x + p.z * p.z );
		const Scalar implicit = (dXZ - R) * (dXZ - R) + p.y * p.y;
		REQUIRE( IsClose( implicit, rTube * rTube, 1e-5 ),
			"torus UniformRandomPoint on surface" );

		AssertUVInUnitSquare( uv.x, uv.y, "torus UniformRandomPoint" );

		const Point3 reconstructed = TorusTessParamToPos( R, rTube, uv.x, uv.y );
		REQUIRE( IsPointClose( reconstructed, p, tol ),
			"torus UniformRandomPoint UV->pos roundtrip" );
		nPts++;
	}
	REQUIRE( nPts > Mpoints / 2, "torus enough random points" );

	// ------------------------------------------------------------
	// Area-uniformity of UniformRandomPoint (regression, 2026-06-10).
	//
	// The tube-angle density must be p(v) = (R + r*cos v) / (2*PI*R)
	// -- uniform by AREA, not uniform by parameter.  A prior
	// implementation rejection-sampled this density but re-drew
	// rejected candidates through an integer-hash constant (2^-32)
	// applied to a [0,1) float, which collapsed every rejected first
	// draw onto the single tube angle v = 2*PI*0.618... -- a
	// point-mass holding r/(R+r) of ALL samples (23% at this R/r).
	// Estimators dividing by the claimed uniform 1/GetArea() pdf then
	// carried a circle-shaped spatial bias, and integrators mixing
	// light-sampled strategies with different MIS shares disagreed on
	// torus-emitter scenes (VCM floor pool ~0.90x of PT/BDPT).
	//
	// 64-bin histogram over the tube angle, plus a uniformity check
	// on the ring angle.  With 200k samples the sparsest bin holds
	// ~2200 samples (1 sigma ~ 2.1%); max-over-64-bins of healthy MC
	// noise stays under ~7%, while the point-mass bug overshoots its
	// bin by ~1400%.  Tolerance 10%.
	{
		const int NBINS = 64;
		const int NSAMP = 200000;
		std::vector<int> vbins( NBINS, 0 );
		std::vector<int> ubins( NBINS, 0 );
		for( int i = 0; i < NSAMP; ++i ) {
			Point3  p; Vector3 n; Point2 uv;
			const Point3 prand( rng.next01(), rng.next01(), rng.next01() );
			g->UniformRandomPoint( &p, &n, &uv, prand );
			const Scalar dXZ = std::sqrt( p.x * p.x + p.z * p.z );
			const Scalar vAng = std::atan2( p.y, dXZ - R );	// tube angle
			const Scalar uAng = std::atan2( p.z, p.x );		// ring angle
			int bv = static_cast<int>( ( vAng + PI ) / TWO_PI * NBINS );
			int bu = static_cast<int>( ( uAng + PI ) / TWO_PI * NBINS );
			if( bv < 0 ) bv = 0;
			if( bv >= NBINS ) bv = NBINS - 1;
			if( bu < 0 ) bu = 0;
			if( bu >= NBINS ) bu = NBINS - 1;
			vbins[bv]++;
			ubins[bu]++;
		}
		Scalar maxRelDevV = 0;
		Scalar maxRelDevU = 0;
		for( int b = 0; b < NBINS; ++b ) {
			const Scalar vc = -PI + ( b + Scalar( 0.5 ) ) * TWO_PI / NBINS;
			const Scalar expectV = ( ( R + rTube * std::cos( vc ) ) / ( TWO_PI * R ) )
				* ( TWO_PI / NBINS ) * NSAMP;
			const Scalar expectU = static_cast<Scalar>( NSAMP ) / NBINS;
			const Scalar devV = std::fabs( vbins[b] - expectV ) / expectV;
			const Scalar devU = std::fabs( ubins[b] - expectU ) / expectU;
			if( devV > maxRelDevV ) maxRelDevV = devV;
			if( devU > maxRelDevU ) maxRelDevU = devU;
		}
		std::cout << "  torus density: max tube-angle bin deviation "
			<< maxRelDevV * 100.0 << "%, ring-angle "
			<< maxRelDevU * 100.0 << "%\n";
		REQUIRE( maxRelDevV < 0.10,
			"torus UniformRandomPoint tube-angle density matches (R + r cos v) area weighting" );
		REQUIRE( maxRelDevU < 0.10,
			"torus UniformRandomPoint ring-angle density uniform" );
	}

	g->release();
	std::cout << "  torus: " << nHits << " ray hits, " << nPts
		<< " random points\n";
}

// ============================================================
// Disk
// ============================================================

static void TestDiskOne( char axis )
{
	const Scalar R = 1.0;
	CircularDiskGeometry* g = new CircularDiskGeometry( R, axis );

	const int N = 1500;
	const Scalar tol = 1e-4 * R;
	LCG rng( 11111 );
	CoverageStats cov = MakeCoverage();
	int nHits = 0;
	const std::string axisLabel = std::string("disk-") + axis;

	for( int i = 0; i < N; ++i ) {
		RayIntersectionGeometric ri( Ray(), nullRasterizerState );
		if( !ShootHit( *g, rng, ri ) ) continue;
		nHits++;

		AssertUVInUnitSquare( ri.ptCoord.x, ri.ptCoord.y, axisLabel + " IntersectRay" );

		const Point3 reconstructed = DiskTessParamToPos(
			axis, R, ri.ptCoord.x, ri.ptCoord.y );
		REQUIRE( IsPointClose( reconstructed, ri.ptIntersection, tol ),
			axisLabel + " IntersectRay UV->pos roundtrip" );

		RecordCoverage( cov, ri.ptCoord.x, ri.ptCoord.y );
	}
	REQUIRE( nHits > N / 4, axisLabel + " enough hits" );
	AssertCoverage( cov, (axisLabel + " IntersectRay").c_str() );

	const int Mpoints = 1000;
	int nPts = 0;
	for( int i = 0; i < Mpoints; ++i ) {
		Point3  p; Vector3 n; Point2 uv;
		const Point3 prand( rng.next01(), rng.next01(), rng.next01() );
		g->UniformRandomPoint( &p, &n, &uv, prand );

		AssertUVInUnitSquare( uv.x, uv.y, axisLabel + " UniformRandomPoint" );

		switch( axis ) {
		case 'x': REQUIRE( IsClose( p.x, 0.0, 1e-12 ),
			axisLabel + " UniformRandomPoint on plane" ); break;
		case 'y': REQUIRE( IsClose( p.y, 0.0, 1e-12 ),
			axisLabel + " UniformRandomPoint on plane" ); break;
		case 'z': REQUIRE( IsClose( p.z, 0.0, 1e-12 ),
			axisLabel + " UniformRandomPoint on plane" ); break;
		}

		const Point3 reconstructed = DiskTessParamToPos( axis, R, uv.x, uv.y );
		REQUIRE( IsPointClose( reconstructed, p, tol ),
			axisLabel + " UniformRandomPoint UV->pos roundtrip" );
		nPts++;
	}
	REQUIRE( nPts > Mpoints / 2, axisLabel + " enough random points" );

	g->release();
	std::cout << "  " << axisLabel << ": " << nHits << " ray hits, "
		<< nPts << " random points\n";
}

static void TestDisk()
{
	std::cout << "Testing CircularDiskGeometry UV roundtrip..." << std::endl;
	for( char axis : { 'x', 'y', 'z' } ) {
		TestDiskOne( axis );
	}
}

// ============================================================
// Clipped plane
//
// ClippedPlaneGeometry now traces the bilinear surface defined by
// its four corners (via RayBilinearPatchIntersection) and uses
// GeometricUtilities::BilinearInverse for ComputeSurfaceDerivatives.
// The strict roundtrip below therefore holds for ALL three regimes:
//   1. planar parallelogram  (the unit-square base case)
//   2. planar non-parallelogram (a trapezoid)
//   3. non-planar (saddle) quad with one corner lifted off the plane
// ============================================================

static void TestClippedPlaneOne(
	const Point3 (&corners)[4],
	const char*  label,
	Scalar       tol = 1e-4 )
{
	ClippedPlaneGeometry* g = new ClippedPlaneGeometry( corners, /*bDoubleSided=*/true );

	Point3 bsCenter; Scalar bsRadius;
	g->GenerateBoundingSphere( bsCenter, bsRadius );

	const int N = 2000;
	LCG rng( 33333 );
	CoverageStats cov = MakeCoverage();
	int nHits = 0;
	for( int i = 0; i < N; ++i ) {
		RayIntersectionGeometric ri( Ray(), nullRasterizerState );
		// Shoot from a wide enclosing sphere toward a random target near
		// the patch centre.  Use ShootHit's logic so coverage is broad.
		if( !ShootHit( *g, rng, ri ) ) continue;
		nHits++;

		AssertUVInUnitSquare( ri.ptCoord.x, ri.ptCoord.y,
			std::string("clippedplane[") + label + "] IntersectRay" );

		const Point3 reconstructed = ClippedPlaneTessParamToPos(
			corners, ri.ptCoord.x, ri.ptCoord.y );
		REQUIRE( IsPointClose( reconstructed, ri.ptIntersection, tol ),
			std::string("clippedplane[") + label
				+ "] IntersectRay UV->pos roundtrip" );

		RecordCoverage( cov, ri.ptCoord.x, ri.ptCoord.y );
	}
	REQUIRE( nHits > N / 4,
		std::string("clippedplane[") + label + "] enough hits" );
	AssertCoverage( cov,
		(std::string("clippedplane[") + label + "] IntersectRay").c_str() );

	const int Mpoints = 800;
	int nPts = 0;
	for( int i = 0; i < Mpoints; ++i ) {
		Point3  p; Vector3 n; Point2 uv;
		const Point3 prand( rng.next01(), rng.next01(), rng.next01() );
		g->UniformRandomPoint( &p, &n, &uv, prand );

		AssertUVInUnitSquare( uv.x, uv.y,
			std::string("clippedplane[") + label + "] UniformRandomPoint" );

		// UniformRandomPoint pulls the point off the surface by a tiny
		// epsilon along the normal so back-facing luminaries occlude
		// correctly; allow that offset in the tolerance.
		const Point3 reconstructed = ClippedPlaneTessParamToPos( corners, uv.x, uv.y );
		REQUIRE( IsPointClose( reconstructed, p, 1e-3 ),
			std::string("clippedplane[") + label
				+ "] UniformRandomPoint UV->pos roundtrip" );
		nPts++;
	}
	REQUIRE( nPts > Mpoints / 2,
		std::string("clippedplane[") + label + "] enough random points" );

	g->release();
	std::cout << "  clipped-plane[" << label << "]: " << nHits
		<< " ray hits, " << nPts << " random points\n";
}

static void TestClippedPlane()
{
	std::cout << "Testing ClippedPlaneGeometry UV roundtrip..." << std::endl;

	// 1. Planar parallelogram (unit square).
	{
		const Point3 corners[4] = {
			Point3( 0, 0, 0 ),
			Point3( 1, 0, 0 ),
			Point3( 1, 1, 0 ),
			Point3( 0, 1, 0 )
		};
		TestClippedPlaneOne( corners, "planar parallelogram" );
	}

	// 2. Planar non-parallelogram (trapezoid).  vP[2] is shifted along
	//    +X so the top edge is longer than the bottom — exercises the
	//    bilinear surface's saddle term D in the XY plane (D ≠ 0 even
	//    though all corners are coplanar).
	{
		const Point3 corners[4] = {
			Point3( 0.0, 0.0, 0.0 ),
			Point3( 1.0, 0.0, 0.0 ),
			Point3( 1.5, 1.0, 0.0 ),
			Point3( 0.0, 1.0, 0.0 )
		};
		TestClippedPlaneOne( corners, "planar trapezoid" );
	}

	// 3. Non-planar (saddle) quad.  vP[2] lifted off the XY plane by
	//    +Z; vP[0] and vP[3] sit at z=0 — a true bilinear saddle.
	//    This is the case where the legacy two-flat-triangle path
	//    diverged from the bilinear surface; the bilinear refactor
	//    now produces a smooth surface and a roundtrip-consistent UV.
	{
		const Point3 corners[4] = {
			Point3( 0.0, 0.0, 0.0 ),
			Point3( 1.0, 0.0, 0.2 ),
			Point3( 1.0, 1.0, 0.5 ),
			Point3( 0.0, 1.0, 0.0 )
		};
		TestClippedPlaneOne( corners, "non-planar saddle" );
	}

	// 4. Both at once: non-planar trapezoid (twisted + non-parallelogram).
	{
		const Point3 corners[4] = {
			Point3( 0.0, 0.0, 0.0 ),
			Point3( 1.0, 0.0, 0.1 ),
			Point3( 1.5, 1.0, 0.4 ),
			Point3( 0.0, 1.0, 0.0 )
		};
		TestClippedPlaneOne( corners, "non-planar trapezoid" );
	}
}

// ============================================================
// Direct unit test for GeometricUtilities::BilinearInverse —
// exercises the solver across configurations that cause the
// quadratic-in-v reduction to take different branches.
// ============================================================

static void TestBilinearInverseOne(
	const Point3& c00, const Point3& c10,
	const Point3& c11, const Point3& c01,
	Scalar uTrue, Scalar vTrue,
	const char* label )
{
	const Point3 P = GeometricUtilities::BilinearForward( c00, c10, c11, c01, uTrue, vTrue );

	Scalar uRec = -1.0, vRec = -1.0;
	const bool ok = GeometricUtilities::BilinearInverse(
		c00, c10, c11, c01, P, uRec, vRec );

	REQUIRE( ok, std::string(label) + " inverse converged" );

	if( ok ) {
		// Tolerance: accept up to 1e-4 in (u, v) since the quadratic
		// solve loses precision near the boundary and for nearly
		// degenerate axis pairs.
		REQUIRE( std::fabs( uRec - uTrue ) < 1e-4,
			std::string(label) + " u recovered" );
		REQUIRE( std::fabs( vRec - vTrue ) < 1e-4,
			std::string(label) + " v recovered" );

		// Verify forward formula at recovered (u, v) returns P.
		const Point3 recon = GeometricUtilities::BilinearForward( c00, c10, c11, c01, uRec, vRec );
		REQUIRE( IsPointClose( recon, P, 1e-5 ),
			std::string(label) + " forward(inverse(P)) == P" );
	}
}

static void TestBilinearInverse()
{
	std::cout << "Testing GeometricUtilities::BilinearInverse direct unit cases..." << std::endl;

	// Fixed corners, sweep (u, v) on a 5x5 grid.
	auto sweep = [&](
		const Point3& c00, const Point3& c10,
		const Point3& c11, const Point3& c01,
		const char* label )
	{
		for( int j = 0; j <= 4; ++j ) {
			for( int i = 0; i <= 4; ++i ) {
				const Scalar u = Scalar(i) * 0.25;
				const Scalar v = Scalar(j) * 0.25;
				char tag[64];
				std::snprintf( tag, sizeof(tag), "%s [%d,%d]", label, i, j );
				TestBilinearInverseOne( c00, c10, c11, c01, u, v, tag );
			}
		}
	};

	// Planar unit square (D = 0).
	sweep(
		Point3( 0, 0, 0 ), Point3( 1, 0, 0 ),
		Point3( 1, 1, 0 ), Point3( 0, 1, 0 ),
		"unit square" );

	// Planar trapezoid (saddle term in-plane).
	sweep(
		Point3( 0.0, 0.0, 0.0 ), Point3( 1.0, 0.0, 0.0 ),
		Point3( 1.5, 1.0, 0.0 ), Point3( 0.0, 1.0, 0.0 ),
		"trapezoid" );

	// Non-planar (saddle).
	sweep(
		Point3( 0.0, 0.0, 0.0 ), Point3( 1.0, 0.0, 0.2 ),
		Point3( 1.0, 1.0, 0.5 ), Point3( 0.0, 1.0, 0.0 ),
		"saddle" );

	// Twisted + non-parallelogram + non-planar.
	sweep(
		Point3( 0.0, 0.0, 0.0 ), Point3( 1.0, 0.0, 0.1 ),
		Point3( 1.5, 1.0, 0.4 ), Point3( 0.0, 1.0, 0.0 ),
		"twisted trapezoid" );

	// Off-axis quad (rotated arbitrarily — exercises all three axis
	// pair candidates inside BilinearInverse).
	sweep(
		Point3( 0.10, 0.20, 0.30 ),
		Point3( 0.80, 0.40, 0.10 ),
		Point3( 0.90, 1.10, 0.45 ),
		Point3( 0.30, 1.05, 0.60 ),
		"oblique quad" );

	// Off-surface rejection: pick a point that's clearly NOT on the
	// surface; BilinearInverse must return false.
	{
		Scalar u = 0, v = 0;
		const bool ok = GeometricUtilities::BilinearInverse(
			Point3( 0, 0, 0 ), Point3( 1, 0, 0 ),
			Point3( 1, 1, 0 ), Point3( 0, 1, 0 ),
			Point3( 0.5, 0.5, 5.0 ),  // far above the unit square
			u, v );
		REQUIRE( !ok, "BilinearInverse rejects off-surface point" );
	}

	std::cout << "  BilinearInverse direct cases: 5 patches x 25 (u, v) samples + 1 rejection\n";
}

// ============================================================
// Bilinear patch
// ============================================================

//! Area-light contract for BilinearPatchGeometry (2026-07).
//!
//! UniformRandomPoint was an unimplemented stub that wrote NOTHING, and
//! GetArea() returned a hardcoded 1.0, while CanBeAreaLight() inherited
//! IGeometry's `true` default -- so an emissive bilinearpatch_geometry was a
//! silently broken area light: every NEE sample landed on the local origin
//! (Point3's default ctor zero-inits), and the emitted power was wrong by the
//! patch's true area.
//!
//! Two properties are asserted, because passing one without the other is
//! exactly how the BoxGeometry defect (098d2565) survived for so long:
//!   (1) GetArea() matches independently-computed ground truth;
//!   (2) samples are uniform in SURFACE AREA, not in (u, v) parameter space.
//! A trapezoid separates them: parameter-uniform sampling puts equal numbers
//! of samples in the narrow and wide halves, area-uniform does not.
static void TestBilinearPatchAreaLightContract()
{
	std::cout << "Testing BilinearPatchGeometry area-light contract..." << std::endl;

	// PLANAR TRAPEZOID in z=0.  Canonical corner mapping (NOT row-major):
	//   pts[0]=(u0,v0)  pts[1]=(u0,v1)  pts[2]=(u1,v0)  pts[3]=(u1,v1)
	// Width at v=0 is 4 (x from 0..4); at v=1 it is 1 (x from 0..1); height 1.
	// Exact area of that trapezoid = (4 + 1)/2 * 1 = 2.5.
	BilinearPatchGeometry* g = new BilinearPatchGeometry( 10, 8, /*bUseBSP=*/false );
	BilinearPatch patch;
	patch.pts[0] = Point3( 0, 0, 0 );   // (u=0, v=0)
	patch.pts[1] = Point3( 0, 1, 0 );   // (u=0, v=1)
	patch.pts[2] = Point3( 4, 0, 0 );   // (u=1, v=0)
	patch.pts[3] = Point3( 1, 1, 0 );   // (u=1, v=1)
	g->AddPatch( patch );
	g->Prepare();

	// (1) AREA.  Ground truth 2.5 computed analytically above -- deliberately
	// NOT by calling the same quadrature the implementation uses, which would
	// only prove self-consistency.
	const Scalar area = g->GetArea();
	std::cout << "  bilinear trapezoid GetArea=" << area << " (exact 2.5)" << std::endl;
	REQUIRE( std::fabs( area - 2.5 ) < 0.02,
		"bilinear patch GetArea matches the trapezoid's exact area (was hardcoded 1.0)" );
	REQUIRE( g->CanBeAreaLight(),
		"a bilinear patch with real area can be an area light" );

	// (2) AREA-UNIFORMITY.  Split at v=0.5.  The v<0.5 half spans x-width
	// 4.0..2.5 (area 1.625); the v>0.5 half spans 2.5..1.0 (area 0.875).
	// So 65% of area-uniform samples must land in the wide half.  A
	// parameter-uniform sampler -- the naive implementation -- would put 50%
	// in each, which this assertion rejects.
	const int NS = 40000;
	LCG rng( 24680 );
	int lowerHalf = 0;
	int offSurface = 0, atOrigin = 0;
	for( int i = 0; i < NS; ++i ) {
		Point3 pt; Vector3 nrm; Point2 uv;
		const Point3 prand( rng.next01(), rng.next01(), rng.next01() );
		g->UniformRandomPoint( &pt, &nrm, &uv, prand );

		// The stub's signature failure: every sample at the local origin.
		if( std::fabs(pt.x) < 1e-12 && std::fabs(pt.y) < 1e-12 && std::fabs(pt.z) < 1e-12 ) {
			atOrigin++;
		}
		// Every sample must lie ON the patch (z=0 plane, inside the trapezoid).
		const Scalar xMaxAtY = 4.0 - 3.0 * pt.y;
		if( std::fabs(pt.z) > 1e-9 || pt.y < -1e-9 || pt.y > 1.0 + 1e-9 ||
		    pt.x < -1e-9 || pt.x > xMaxAtY + 1e-9 ) {
			offSurface++;
		}
		if( pt.y < 0.5 ) lowerHalf++;
	}

	REQUIRE( atOrigin < NS / 100,
		"bilinear UniformRandomPoint does not collapse to the local origin "
		"(the unimplemented-stub signature: it wrote nothing at all)" );
	REQUIRE( offSurface == 0,
		"every bilinear UniformRandomPoint sample lies on the patch surface" );

	const double wideFrac = double(lowerHalf) / double(NS);
	std::cout << "  wide-half sample fraction=" << wideFrac
	          << " (area-uniform 0.65, parameter-uniform 0.50)" << std::endl;
	REQUIRE( std::fabs( wideFrac - 0.65 ) < 0.02,
		"bilinear UniformRandomPoint is uniform in SURFACE AREA -- 65% of samples "
		"fall in the wide half.  Parameter-uniform sampling would give 50%, which "
		"disagrees with the 1/GetArea() density its consumers assume" );

	g->release();
}

static void TestBilinearPatch()
{
	std::cout << "Testing BilinearPatchGeometry UV roundtrip..." << std::endl;

	BilinearPatchGeometry* g = new BilinearPatchGeometry( 10, 8, /*bUseBSP=*/false );
	BilinearPatch patch;
	patch.pts[0] = Point3( 0, 0, 0 );
	patch.pts[1] = Point3( 0, 1, 0.2 );  // pts[1] at (u=0, v=1)
	patch.pts[2] = Point3( 1, 0, 0.0 );  // pts[2] at (u=1, v=0)
	patch.pts[3] = Point3( 1, 1, 0.2 );  // pts[3] at (u=1, v=1)
	g->AddPatch( patch );
	g->Prepare();

	const int N = 1500;
	const Scalar tol = 1e-4;
	LCG rng( 55555 );
	CoverageStats cov = MakeCoverage();
	int nHits = 0;

	for( int i = 0; i < N; ++i ) {
		const Scalar zSide = (rng.next01() < 0.5) ? -2.0 : 2.0;
		const Point3 origin( rng.next01() * 1.5 - 0.25, rng.next01() * 1.5 - 0.25, zSide );
		const Point3 target( rng.next01(), rng.next01(), rng.next01() * 0.2 );
		const Vector3 dir = Vector3Ops::Normalize(
			Vector3Ops::mkVector3( target, origin ) );
		RayIntersectionGeometric ri( Ray( origin, dir ), nullRasterizerState );
		g->IntersectRay( ri, true, true, false );
		if( !ri.bHit ) continue;
		nHits++;

		AssertUVInUnitSquare( ri.ptCoord.x, ri.ptCoord.y, "bilinear-patch IntersectRay" );

		const Point3 reconstructed = BilinearPatchTessParamToPos(
			patch, ri.ptCoord.x, ri.ptCoord.y );
		REQUIRE( IsPointClose( reconstructed, ri.ptIntersection, tol ),
			"bilinear-patch IntersectRay UV->pos roundtrip" );

		RecordCoverage( cov, ri.ptCoord.x, ri.ptCoord.y );
	}
	REQUIRE( nHits > N / 4, "bilinear-patch enough hits" );
	AssertCoverage( cov, "bilinear-patch IntersectRay" );

	// BilinearPatchGeometry::UniformRandomPoint is currently a stub
	// (commented "@ To be implemented").  Skip its driving here — the
	// IntersectRay roundtrip is already the load-bearing assertion.

	g->release();
	std::cout << "  bilinear-patch: " << nHits << " ray hits\n";
}

// ============================================================
// Main
// ============================================================

// ============================================================
// Object world-area Jacobian
//
// Object::UniformRandomPoint() returns WORLD-space samples
// (transformed through m_mxFinalTrans), so Object::GetArea() must
// report the WORLD-space area or every consumer claiming
// pdfPosition = 1/GetArea() (LightSampler NEE, BDPT/VCM InitLight,
// photon-emission normalization) is off by the transform's area
// scaling.  Regression (2026-08-13): GetArea() returned the raw
// object-space geometry area, so a `scale 2` emissive sphere lit its
// surroundings at ~0.39x of the identical unscaled sphere (NEE
// under-counted 4x, partially MIS-compensated by BSDF-hit emission).
// ============================================================

// ============================================================
// Bilinear-patch ELIMINATION-AXIS regression
//   (docs/TEXTURE_FOOTPRINT_ANALYTIC_DESIGN.md section 10.6)
// ============================================================
//
// THE BUG, ONE SENTENCE.  RayBilinearPatchIntersection eliminated the
// ray parameter `t` from the three component equations of
// `P(u,v) = origin + t*q` by always dividing out the Z axis
// (`A1 = ax*qz - az*qx`, and the same shape for B/C/D), so for any ray
// direction with `q.z == 0` -- every ray travelling in the XY plane,
// including a camera looking straight down -Y -- the two eliminated
// equations collapsed to the SAME equation up to scale, every
// coefficient of the quadratic in v cancelled to identically zero,
// SolveQuadricWithinRange reported no roots, and the patch was MISSED.
//
// This is a formulation degeneracy, not a threshold: a rank-1 2x2
// system has no epsilon that rescues it.  The fix picks the elimination
// axis as the LARGEST |q| component -- the choice `computet` in the same
// file already made for the t recovery -- so |q[w]| >= |q|/sqrt(3) for
// every non-zero direction, and reduces textually to the old algebra
// when Z is dominant.
//
// RED-PROOF (2026-09-06): with the elimination axis forced back to a
// hard-coded `w = 2` in RayBilinearPatchIntersection.cpp and everything
// rebuilt, this binary reports 22 failed assertions -- in part (a) all
// four +/-X and +/-Y rays miss on the raw patch AND through
// clippedplane_geometry (8), and in part (b) the curved saddle plus all
// 12 of the twisted patch's +/-X and +/-Y rays miss (14).  Not one of
// the +/-Z rays fails, which is the point: the bug was never about the
// patch, only about the direction.  Part (c) also stays green under that
// revert, but NOT by construction: it asserts `bHit` on every ray it does
// not skip, and the legacy solver loses a ray only when `q.z` is EXACTLY
// zero, which a uniformly random direction never is.  It is a parity
// proof (the new axis pick is the identity where the old code was valid),
// and it happens not to double as a bug detector -- an accident of the
// sampling, not a property of the test.  With the fix in place the whole
// file passes.

// Verbatim copy of the PRE-FIX solver body (hard-coded z elimination),
// kept here so part (c) can prove the new axis pick is the IDENTITY
// wherever |q.z| is the dominant component -- i.e. that the permutation
// changed nothing for the rays that already worked.  Do NOT "fix" this
// copy; being wrong for q.z == 0 is exactly what it is here to be.
static Scalar LegacyGetU(
	const Scalar v,
	const Scalar M1, const Scalar M2,
	const Scalar J1, const Scalar J2,
	const Scalar K1, const Scalar K2,
	const Scalar R1, const Scalar R2 )
{
	const Scalar denom = (v*(M1-M2)+J1-J2);
	const Scalar d2 = (v*M1+J1);
	if( std::fabs(denom) > std::fabs(d2) ) {
		return (v*(K2-K1)+R2-R1)/denom;
	}
	return -(v*K1+R1)/d2;
}

static Scalar LegacyComputeT( const Ray& ray, const Point3& srfpos )
{
	if( std::fabs(ray.Dir().x) >= std::fabs(ray.Dir().y) && std::fabs(ray.Dir().x) >= std::fabs(ray.Dir().z) ) {
		return (srfpos.x - ray.origin.x) / ray.Dir().x;
	} else if( std::fabs(ray.Dir().y) >= std::fabs(ray.Dir().z) ) {
		return (srfpos.y - ray.origin.y) / ray.Dir().y;
	}
	return (srfpos.z - ray.origin.z) / ray.Dir().z;
}

static void LegacyFixedZBilinearIntersection(
	const Ray& ray, BILINEAR_HIT& hit, const BilinearPatch& patch )
{
	hit.bHit = false;
	hit.dRange = RISE_INFINITY;
	hit.dRange2 = RISE_INFINITY;

	Scalar coordScale = std::fabs(ray.origin.x) + std::fabs(ray.origin.y) + std::fabs(ray.origin.z);
	for( int ci = 0; ci < 4; ci++ ) {
		const Scalar s = std::fabs(patch.pts[ci].x) + std::fabs(patch.pts[ci].y) + std::fabs(patch.pts[ci].z);
		if( s > coordScale ) coordScale = s;
	}
	const Scalar tMin = NEARZERO * ( Scalar(1) + coordScale );

	const Scalar ax = patch.pts[3].x - patch.pts[2].x - patch.pts[1].x + patch.pts[0].x;
	const Scalar ay = patch.pts[3].y - patch.pts[2].y - patch.pts[1].y + patch.pts[0].y;
	const Scalar az = patch.pts[3].z - patch.pts[2].z - patch.pts[1].z + patch.pts[0].z;
	const Scalar bx = patch.pts[2].x - patch.pts[0].x;
	const Scalar by = patch.pts[2].y - patch.pts[0].y;
	const Scalar bz = patch.pts[2].z - patch.pts[0].z;
	const Scalar cx = patch.pts[1].x - patch.pts[0].x;
	const Scalar cy = patch.pts[1].y - patch.pts[0].y;
	const Scalar cz = patch.pts[1].z - patch.pts[0].z;
	const Scalar qx = ray.Dir().x, qy = ray.Dir().y, qz = ray.Dir().z;
	const Scalar dx = patch.pts[0].x - ray.origin.x;
	const Scalar dy = patch.pts[0].y - ray.origin.y;
	const Scalar dz = patch.pts[0].z - ray.origin.z;

	const Scalar A1 = ax*qz - az*qx;   const Scalar A2 = ay*qz - az*qy;
	const Scalar B1 = bx*qz - bz*qx;   const Scalar B2 = by*qz - bz*qy;
	const Scalar C1 = cx*qz - cz*qx;   const Scalar C2 = cy*qz - cz*qy;
	const Scalar D1 = dx*qz - dz*qx;   const Scalar D2 = dy*qz - dz*qy;

	Scalar coeff[3] = {0};
	coeff[0] = A2*C1 - A1*C2;
	coeff[1] = A2*D1 - A1*D2 + B2*C1 - B1*C2;
	coeff[2] = B2*D1 - B1*D2;

	hit.u = hit.v = hit.dRange = -2;

	Scalar sol[2] = {0};
	const int numSol = Polynomial::SolveQuadricWithinRange( coeff, sol, -NEARZERO, 1.0+NEARZERO );

	switch( numSol )
	{
	case 0:
		break;
	case 1:
		{
			hit.u = LegacyGetU(sol[0],A2,A1,B2,B1,C2,C1,D2,D1);
			hit.v = sol[0];
			const Point3 pos1 = GeometricUtilities::EvaluateBilinearPatchAt( patch, hit.u, hit.v );
			hit.dRange = LegacyComputeT(ray,pos1);
			if( hit.u < 1+NEARZERO && hit.u > -NEARZERO && hit.dRange > tMin ) {
				hit.bHit = true;
			}
		}
		break;
	case 2:
		{
			hit.v = sol[0];
			hit.u = LegacyGetU(sol[0],A2,A1,B2,B1,C2,C1,D2,D1);
			const Point3 pos1 = GeometricUtilities::EvaluateBilinearPatchAt( patch, hit.u, hit.v );
			hit.dRange = LegacyComputeT(ray,pos1);
			if( hit.u < 1+NEARZERO && hit.u > -NEARZERO && hit.dRange > tMin ) {
				hit.bHit = true;
				const Scalar u = LegacyGetU(sol[1],A2,A1,B2,B1,C2,C1,D2,D1);
				if( u < 1+NEARZERO && u > NEARZERO ) {
					const Point3 pos2 = GeometricUtilities::EvaluateBilinearPatchAt( patch, u, sol[1] );
					const Scalar t2 = LegacyComputeT(ray,pos2);
					if( t2 < tMin || hit.dRange < t2 ) {
						return;
					}
					hit.v = sol[1];
					hit.u = u;
					hit.dRange = t2;
				}
			}
			else
			{
				hit.u = LegacyGetU(sol[1],A2,A1,B2,B1,C2,C1,D2,D1);
				hit.v = sol[1];
				const Point3 pos1b = GeometricUtilities::EvaluateBilinearPatchAt( patch, hit.u, hit.v );
				hit.dRange = LegacyComputeT(ray,pos1b);
				if( hit.u < 1+NEARZERO && hit.u > -NEARZERO && hit.dRange > tMin ) {
					hit.bHit = true;
				}
			}
		}
		break;
	};
}

// Independent brute-force root finder, sharing NO code with the analytic
// solver: coarse 129x129 grid over (u, v) picking the sample whose
// P(u, v) is closest to the ray LINE at positive t, then a full 3x3
// Newton on F(u, v, t) = P(u, v) - (origin + t*q) = 0 solved by Cramer's
// rule.  This is the oracle part (b) checks the analytic (u, v, t)
// against; it never touches the quadratic elimination at all.
static bool BruteForceBilinearRayRoot(
	const Ray& ray, const BilinearPatch& patch,
	Scalar& uOut, Scalar& vOut, Scalar& tOut )
{
	const Vector3 A( patch.pts[3].x - patch.pts[2].x - patch.pts[1].x + patch.pts[0].x,
	                 patch.pts[3].y - patch.pts[2].y - patch.pts[1].y + patch.pts[0].y,
	                 patch.pts[3].z - patch.pts[2].z - patch.pts[1].z + patch.pts[0].z );
	const Vector3 B( patch.pts[2].x - patch.pts[0].x,
	                 patch.pts[2].y - patch.pts[0].y,
	                 patch.pts[2].z - patch.pts[0].z );
	const Vector3 C( patch.pts[1].x - patch.pts[0].x,
	                 patch.pts[1].y - patch.pts[0].y,
	                 patch.pts[1].z - patch.pts[0].z );

	const Vector3& q = ray.Dir();
	const Scalar qq = Vector3Ops::Dot( q, q );

	const int N = 129;
	Scalar bestDist = 1e30, bu = 0, bv = 0, bt = 0;
	bool found = false;
	for( int iu = 0; iu <= N; ++iu ) {
		const Scalar u = Scalar(iu) / Scalar(N);
		for( int iv = 0; iv <= N; ++iv ) {
			const Scalar v = Scalar(iv) / Scalar(N);
			const Point3 P = GeometricUtilities::EvaluateBilinearPatchAt( patch, u, v );
			const Vector3 w( P.x - ray.origin.x, P.y - ray.origin.y, P.z - ray.origin.z );
			const Scalar t = Vector3Ops::Dot( w, q ) / qq;
			if( t <= 0.0 ) continue;
			const Vector3 e( w.x - t*q.x, w.y - t*q.y, w.z - t*q.z );
			const Scalar dist = Vector3Ops::Dot( e, e );
			if( dist < bestDist ) { bestDist = dist; bu = u; bv = v; bt = t; found = true; }
		}
	}
	if( !found ) return false;

	Scalar u = bu, v = bv, t = bt;
	for( int it = 0; it < 100; ++it ) {
		const Point3 P = GeometricUtilities::EvaluateBilinearPatchAt( patch, u, v );
		const Scalar F[3] = { P.x - ray.origin.x - t*q.x,
		                      P.y - ray.origin.y - t*q.y,
		                      P.z - ray.origin.z - t*q.z };
		if( std::fabs(F[0]) < 1e-15 && std::fabs(F[1]) < 1e-15 && std::fabs(F[2]) < 1e-15 ) break;

		// Columns of the Jacobian: dP/du, dP/dv, -q.
		const Scalar J[3][3] = {
			{ B.x + A.x*v, C.x + A.x*u, -q.x },
			{ B.y + A.y*v, C.y + A.y*u, -q.y },
			{ B.z + A.z*v, C.z + A.z*u, -q.z } };
		auto det3 = []( const Scalar m[3][3] ) -> Scalar {
			return m[0][0]*(m[1][1]*m[2][2] - m[1][2]*m[2][1])
			     - m[0][1]*(m[1][0]*m[2][2] - m[1][2]*m[2][0])
			     + m[0][2]*(m[1][0]*m[2][1] - m[1][1]*m[2][0]);
		};
		const Scalar D = det3( J );
		if( std::fabs(D) < 1e-16 ) return false;
		Scalar Ju[3][3], Jv[3][3], Jt[3][3];
		for( int r = 0; r < 3; ++r ) for( int cc = 0; cc < 3; ++cc ) {
			Ju[r][cc] = Jv[r][cc] = Jt[r][cc] = J[r][cc];
		}
		for( int r = 0; r < 3; ++r ) { Ju[r][0] = F[r]; Jv[r][1] = F[r]; Jt[r][2] = F[r]; }
		u -= det3(Ju)/D;
		v -= det3(Jv)/D;
		t -= det3(Jt)/D;
	}

	uOut = u; vOut = v; tOut = t;
	return true;
}

// Signed distance from the ray line to P(u, v) at the reported hit --
// the invariant every accepted root must satisfy regardless of which
// axis was eliminated.
static Scalar HitResidual( const Ray& ray, const BilinearPatch& patch, const BILINEAR_HIT& h )
{
	const Point3 P = GeometricUtilities::EvaluateBilinearPatchAt( patch, h.u, h.v );
	const Point3 R = ray.PointAtLength( h.dRange );
	return std::sqrt( (P.x-R.x)*(P.x-R.x) + (P.y-R.y)*(P.y-R.y) + (P.z-R.z)*(P.z-R.z) );
}

// (a) Axis-aligned rays at an axis-aligned unit patch, all three
//     elimination branches (dominant q = X, Y, Z) and both signs.
static void TestBilinearAxisAlignedRays()
{
	std::cout << "Testing bilinear-patch axis-aligned rays (elimination-axis pick)..." << std::endl;

	// BilinearPatch convention: pts[0]->(0,0), pts[1]->(0,1),
	// pts[2]->(1,0), pts[3]->(1,1).  Each patch below is the unit square
	// in one coordinate plane, laid out so P(u, v) is exactly (u, v) in
	// that plane's two axes -- the closed form the assertions use.
	struct Case {
		const char* label;
		Point3      pts[4];
		Point3      origin;
		Vector3     dir;
	};

	const Scalar U = 0.3, V = 0.7, T = 5.0;

	const Case cases[6] = {
		// Patch in the XZ plane (y = 0): P(u,v) = (u, 0, v).  Rays along
		// -/+Y -- q.z == 0 AND q.x == 0, the exact case the hard-coded z
		// elimination lost.  Dominant axis is Y (w = 1).
		{ "XZ patch, ray -Y",
		  { Point3(0,0,0), Point3(0,0,1), Point3(1,0,0), Point3(1,0,1) },
		  Point3(U, T, V), Vector3(0,-1,0) },
		{ "XZ patch, ray +Y",
		  { Point3(0,0,0), Point3(0,0,1), Point3(1,0,0), Point3(1,0,1) },
		  Point3(U, -T, V), Vector3(0,1,0) },
		// Patch in the YZ plane (x = 0): P(u,v) = (0, u, v).  Rays along
		// -/+X -- q.z == 0 again.  Dominant axis is X (w = 0).
		{ "YZ patch, ray -X",
		  { Point3(0,0,0), Point3(0,0,1), Point3(0,1,0), Point3(0,1,1) },
		  Point3(T, U, V), Vector3(-1,0,0) },
		{ "YZ patch, ray +X",
		  { Point3(0,0,0), Point3(0,0,1), Point3(0,1,0), Point3(0,1,1) },
		  Point3(-T, U, V), Vector3(1,0,0) },
		// Patch in the XY plane (z = 0): P(u,v) = (u, v, 0).  Rays along
		// -/+Z -- the case the legacy code already handled; the new axis
		// pick must be the identity here (w = 2).
		{ "XY patch, ray -Z",
		  { Point3(0,0,0), Point3(0,1,0), Point3(1,0,0), Point3(1,1,0) },
		  Point3(U, V, T), Vector3(0,0,-1) },
		{ "XY patch, ray +Z",
		  { Point3(0,0,0), Point3(0,1,0), Point3(1,0,0), Point3(1,1,0) },
		  Point3(U, V, -T), Vector3(0,0,1) }
	};

	for( int k = 0; k < 6; ++k ) {
		const Case& cs = cases[k];
		BilinearPatch patch;
		for( int p = 0; p < 4; ++p ) patch.pts[p] = cs.pts[p];

		BILINEAR_HIT h;
		RayBilinearPatchIntersection( Ray( cs.origin, cs.dir ), h, patch );

		REQUIRE( h.bHit, std::string("axis-aligned[") + cs.label + "] hit" );
		if( !h.bHit ) continue;

		REQUIRE( IsClose( h.dRange, T, 1e-12 ),
			std::string("axis-aligned[") + cs.label + "] t == distance to the plane" );
		REQUIRE( IsClose( h.u, U, 1e-12 ),
			std::string("axis-aligned[") + cs.label + "] u closed form" );
		REQUIRE( IsClose( h.v, V, 1e-12 ),
			std::string("axis-aligned[") + cs.label + "] v closed form" );
		REQUIRE( HitResidual( Ray( cs.origin, cs.dir ), patch, h ) < 1e-9,
			std::string("axis-aligned[") + cs.label + "] P(u,v) lies on the ray" );

		// Discriminating half: the pre-fix hard-coded-z solver MISSES the
		// four rays whose direction lies in the XY plane and still hits the
		// two travelling along +/-Z.  Without this the six assertions above
		// would pass on a solver that never had the bug, so this is what
		// makes the case above a regression guard rather than a smoke test.
		BILINEAR_HIT hLegacy;
		LegacyFixedZBilinearIntersection( Ray( cs.origin, cs.dir ), hLegacy, patch );
		const bool zTravelling = ( k >= 4 );
		REQUIRE( hLegacy.bHit == zTravelling,
			std::string("axis-aligned[") + cs.label
				+ "] pre-fix hard-coded-z solver misses iff q.z == 0 (red-proof)" );
	}

	// Same six rays end-to-end through ClippedPlaneGeometry -- the
	// user-visible symptom was a `clipped_plane` viewed dead-on from
	// above being invisible.  ClippedPlaneGeometry's corner convention is
	// row-major (vP[0]->(0,0), vP[1]->(1,0), vP[2]->(1,1), vP[3]->(0,1)),
	// so its ptCoord is (u, v) in that order after ToBilinearPatch's
	// remap.
	struct GeomCase {
		const char* label;
		Point3      vP[4];
		Point3      origin;
		Vector3     dir;
	};
	const GeomCase gcases[6] = {
		{ "XZ quad, camera above looking -Y",
		  { Point3(0,0,0), Point3(1,0,0), Point3(1,0,1), Point3(0,0,1) },
		  Point3(U, T, V), Vector3(0,-1,0) },
		{ "XZ quad, ray +Y",
		  { Point3(0,0,0), Point3(1,0,0), Point3(1,0,1), Point3(0,0,1) },
		  Point3(U, -T, V), Vector3(0,1,0) },
		{ "YZ quad, ray -X",
		  { Point3(0,0,0), Point3(0,1,0), Point3(0,1,1), Point3(0,0,1) },
		  Point3(T, U, V), Vector3(-1,0,0) },
		{ "YZ quad, ray +X",
		  { Point3(0,0,0), Point3(0,1,0), Point3(0,1,1), Point3(0,0,1) },
		  Point3(-T, U, V), Vector3(1,0,0) },
		{ "XY quad, ray -Z",
		  { Point3(0,0,0), Point3(1,0,0), Point3(1,1,0), Point3(0,1,0) },
		  Point3(U, V, T), Vector3(0,0,-1) },
		{ "XY quad, ray +Z",
		  { Point3(0,0,0), Point3(1,0,0), Point3(1,1,0), Point3(0,1,0) },
		  Point3(U, V, -T), Vector3(0,0,1) }
	};

	for( int k = 0; k < 6; ++k ) {
		const GeomCase& cs = gcases[k];
		ClippedPlaneGeometry* g = new ClippedPlaneGeometry( cs.vP, /*bDoubleSided=*/true );
		RayIntersectionGeometric ri( Ray( cs.origin, cs.dir ), nullRasterizerState );
		g->IntersectRay( ri, true, true, false );

		REQUIRE( ri.bHit, std::string("clippedplane[") + cs.label + "] hit" );
		if( ri.bHit ) {
			REQUIRE( IsClose( ri.range, T, 1e-12 ),
				std::string("clippedplane[") + cs.label + "] range == distance to the plane" );
			REQUIRE( IsClose( ri.ptCoord.x, U, 1e-12 ),
				std::string("clippedplane[") + cs.label + "] u closed form" );
			REQUIRE( IsClose( ri.ptCoord.y, V, 1e-12 ),
				std::string("clippedplane[") + cs.label + "] v closed form" );
		}
		g->release();
	}

	std::cout << "  12 axis-aligned cases (6 raw patch + 6 clipped_plane) done\n";
}

// (b) CURVED (non-planar) patches struck by axis-aligned rays the fixed-z
//     form cannot solve, checked against the brute-force Newton oracle.
static void TestBilinearCurvedAxisAlignedRays()
{
	std::cout << "Testing curved bilinear patch vs brute-force Newton oracle..." << std::endl;

	// A genuinely twisted patch: `a = pts[3]-pts[2]-pts[1]+pts[0]` is
	// (0, -1.2, 0), so the surface is the saddle
	// P(u,v) = (u, 0.3u + 0.5v - 1.2uv, v) -- non-planar, and its x/z
	// still parameterise directly so the closed form is available as a
	// second, independent check on the oracle itself.
	BilinearPatch saddle;
	saddle.pts[0] = Point3( 0.0,  0.0, 0.0 );
	saddle.pts[1] = Point3( 0.0,  0.5, 1.0 );
	saddle.pts[2] = Point3( 1.0,  0.3, 0.0 );
	saddle.pts[3] = Point3( 1.0, -0.4, 1.0 );

	{
		const Scalar u = 0.3, v = 0.7;
		const Scalar ySurf = 0.3*u + 0.5*v - 1.2*u*v;
		const Ray ray( Point3( u, 5.0, v ), Vector3( 0, -1, 0 ) );

		BILINEAR_HIT h;
		RayBilinearPatchIntersection( ray, h, saddle );
		REQUIRE( h.bHit, "curved saddle, ray -Y: hit" );

		BILINEAR_HIT hLegacy;
		LegacyFixedZBilinearIntersection( ray, hLegacy, saddle );
		REQUIRE( !hLegacy.bHit,
			"curved saddle, ray -Y: pre-fix hard-coded-z solver misses (red-proof)" );
		if( h.bHit ) {
			REQUIRE( IsClose( h.u, u, 1e-9 ), "curved saddle, ray -Y: u closed form" );
			REQUIRE( IsClose( h.v, v, 1e-9 ), "curved saddle, ray -Y: v closed form" );
			REQUIRE( IsClose( h.dRange, 5.0 - ySurf, 1e-9 ), "curved saddle, ray -Y: t closed form" );

			Scalar bu, bv, bt;
			const bool ok = BruteForceBilinearRayRoot( ray, saddle, bu, bv, bt );
			REQUIRE( ok, "curved saddle, ray -Y: oracle converged" );
			if( ok ) {
				REQUIRE( std::fabs( h.u - bu ) < 1e-9, "curved saddle, ray -Y: u matches oracle" );
				REQUIRE( std::fabs( h.v - bv ) < 1e-9, "curved saddle, ray -Y: v matches oracle" );
				REQUIRE( std::fabs( h.dRange - bt ) < 1e-9, "curved saddle, ray -Y: t matches oracle" );
			}
		}
	}

	// A patch twisted in ALL THREE axes, with no closed form available.
	// Rays are built by choosing a target (u*, v*) on the surface and
	// backing the origin off along the axis direction, so the expected
	// root is known exactly and the oracle is a third opinion.
	BilinearPatch twisted;
	twisted.pts[0] = Point3(  0.10,  0.05, -0.20 );
	twisted.pts[1] = Point3( -0.15,  0.90,  0.35 );
	twisted.pts[2] = Point3(  1.05, -0.10,  0.30 );
	twisted.pts[3] = Point3(  0.80,  1.15,  1.10 );

	const Vector3 dirs[6] = {
		Vector3(-1,0,0), Vector3(1,0,0),
		Vector3(0,-1,0), Vector3(0,1,0),
		Vector3(0,0,-1), Vector3(0,0,1) };
	const char* dirNames[6] = { "-X", "+X", "-Y", "+Y", "-Z", "+Z" };

	const Scalar targets[3][2] = { {0.25, 0.40}, {0.55, 0.65}, {0.80, 0.20} };

	int checked = 0;
	for( int di = 0; di < 6; ++di ) {
		for( int ti = 0; ti < 3; ++ti ) {
			const Scalar uT = targets[ti][0], vT = targets[ti][1];
			const Point3 P = GeometricUtilities::EvaluateBilinearPatchAt( twisted, uT, vT );
			const Scalar t0 = 4.0;
			const Ray ray( Point3( P.x - t0*dirs[di].x, P.y - t0*dirs[di].y, P.z - t0*dirs[di].z ),
			               dirs[di] );

			char label[128];
			std::snprintf( label, sizeof(label), "twisted patch, ray %s at (%.2f, %.2f)",
				dirNames[di], (double)uT, (double)vT );

			BILINEAR_HIT h;
			RayBilinearPatchIntersection( ray, h, twisted );
			REQUIRE( h.bHit, std::string(label) + ": hit" );
			if( !h.bHit ) continue;
			checked++;

			// The reported root must lie on the ray to full precision --
			// this holds even if the solver legitimately picks a NEARER
			// second root of the twisted surface.
			REQUIRE( HitResidual( ray, twisted, h ) < 1e-9,
				std::string(label) + ": P(u,v) lies on the ray" );

			Scalar bu, bv, bt;
			const bool ok = BruteForceBilinearRayRoot( ray, twisted, bu, bv, bt );
			REQUIRE( ok, std::string(label) + ": oracle converged" );
			if( ok && std::fabs( bt - h.dRange ) < 1e-6 ) {
				// Same root -- must agree to 1e-9 in every coordinate.
				REQUIRE( std::fabs( h.u - bu ) < 1e-9, std::string(label) + ": u matches oracle" );
				REQUIRE( std::fabs( h.v - bv ) < 1e-9, std::string(label) + ": v matches oracle" );
				REQUIRE( std::fabs( h.dRange - bt ) < 1e-9, std::string(label) + ": t matches oracle" );
			}
		}
	}
	REQUIRE( checked == 18, "twisted patch: all 18 axis-aligned rays hit" );

	std::cout << "  curved-patch oracle agreement on " << checked << " rays\n";
}

// (c) 1000 random directions: the new solver's root always lies on the
//     ray, and wherever |q.z| is the dominant component the new axis pick
//     reproduces the legacy fixed-z result to FP-CONTRACTION NOISE (the
//     assertion tolerance is 1e-12; measured max delta 8.9e-16), proving
//     the permutation is the identity exactly where the old code was
//     valid.  Bit equality is deliberately NOT asserted -- see the note at
//     the comparison below.
static void TestBilinearRandomDirectionParity()
{
	std::cout << "Testing bilinear-patch random-direction parity + on-ray invariant..." << std::endl;

	BilinearPatch patch;
	patch.pts[0] = Point3(  0.00,  0.00,  0.00 );
	patch.pts[1] = Point3(  0.05,  0.15,  1.00 );
	patch.pts[2] = Point3(  1.00, -0.10,  0.10 );
	patch.pts[3] = Point3(  0.95,  0.35,  1.05 );

	LCG rng( 987654321ULL );
	const int N = 1000;
	int nHit = 0, nZDominant = 0, nParity = 0, nSkippedGrazing = 0;
	Scalar maxDelta = 0.0;

	for( int k = 0; k < N; ++k ) {
		const Scalar uT = 0.05 + 0.90 * rng.next01();
		const Scalar vT = 0.05 + 0.90 * rng.next01();
		const Point3 P = GeometricUtilities::EvaluateBilinearPatchAt( patch, uT, vT );

		// Uniform direction on the sphere.
		const Scalar z = 2.0 * rng.next01() - 1.0;
		const Scalar phi = 2.0 * 3.14159265358979323846 * rng.next01();
		const Scalar r = std::sqrt( std::fmax( 0.0, 1.0 - z*z ) );
		const Vector3 dir( r * std::cos(phi), r * std::sin(phi), z );

		// Skip near-tangent directions: the root is genuinely
		// ill-conditioned there for ANY elimination axis, so it is not
		// what this test is measuring.
		const Vector3 dpdu(
			(patch.pts[2].x - patch.pts[0].x) + (patch.pts[3].x - patch.pts[2].x - patch.pts[1].x + patch.pts[0].x)*vT,
			(patch.pts[2].y - patch.pts[0].y) + (patch.pts[3].y - patch.pts[2].y - patch.pts[1].y + patch.pts[0].y)*vT,
			(patch.pts[2].z - patch.pts[0].z) + (patch.pts[3].z - patch.pts[2].z - patch.pts[1].z + patch.pts[0].z)*vT );
		const Vector3 dpdv(
			(patch.pts[1].x - patch.pts[0].x) + (patch.pts[3].x - patch.pts[2].x - patch.pts[1].x + patch.pts[0].x)*uT,
			(patch.pts[1].y - patch.pts[0].y) + (patch.pts[3].y - patch.pts[2].y - patch.pts[1].y + patch.pts[0].y)*uT,
			(patch.pts[1].z - patch.pts[0].z) + (patch.pts[3].z - patch.pts[2].z - patch.pts[1].z + patch.pts[0].z)*uT );
		const Vector3 nrm = Vector3Ops::Normalize( Vector3Ops::Cross( dpdu, dpdv ) );
		if( std::fabs( Vector3Ops::Dot( nrm, dir ) ) < 0.15 ) { nSkippedGrazing++; continue; }

		const Scalar t0 = 3.0;
		const Ray ray( Point3( P.x - t0*dir.x, P.y - t0*dir.y, P.z - t0*dir.z ), dir );

		BILINEAR_HIT hNew;
		RayBilinearPatchIntersection( ray, hNew, patch );
		REQUIRE( hNew.bHit, "random direction: new solver hits the constructed root" );
		if( !hNew.bHit ) continue;
		nHit++;

		REQUIRE( HitResidual( ray, patch, hNew ) < 1e-9,
			"random direction: new solver's P(u,v) lies on the ray" );

		// The elimination axis the fix picks is the largest |q|; when that
		// is z the permutation is (x, y, z) -> identity, so the algebra is
		// textually the legacy one and the results must be identical bits.
		const Scalar qx = std::fabs(dir.x), qy = std::fabs(dir.y), qz = std::fabs(dir.z);
		if( qz >= qx && qz >= qy ) {
			nZDominant++;
			BILINEAR_HIT hOld;
			LegacyFixedZBilinearIntersection( ray, hOld, patch );

			// The two are ALGEBRAICALLY the same expression here, so they
			// agree to FP-contraction noise -- measured 0-3 ulp on each of
			// u, v and dRange.  Bit equality is deliberately NOT asserted:
			// this file necessarily carries its own copy of the solver, the
			// whole build runs `-ffast-math` with LTO, and FP contraction
			// is free to fuse the multiply-subtract pairs differently in
			// each copy (measured: whether it does depends on the
			// surrounding call sites, so bit equality is not even stable
			// across edits to THIS file).  1e-12 is nine orders below any
			// real disagreement -- as parts (a) and (b) show, eliminating on
			// the WRONG axis does not shift a root by an ulp, it loses the
			// root entirely.
			maxDelta = std::fmax( maxDelta, std::fabs( hOld.u - hNew.u ) );
			maxDelta = std::fmax( maxDelta, std::fabs( hOld.v - hNew.v ) );
			maxDelta = std::fmax( maxDelta, std::fabs( hOld.dRange - hNew.dRange ) );
			const bool same = ( hOld.bHit == hNew.bHit )
				&& ( std::fabs( hOld.u - hNew.u ) < 1e-12 )
				&& ( std::fabs( hOld.v - hNew.v ) < 1e-12 )
				&& ( std::fabs( hOld.dRange - hNew.dRange ) < 1e-12 );
			REQUIRE( same, "random direction, |q.z| dominant: new axis pick == legacy fixed-z" );
			if( same ) nParity++;
		}
	}

	REQUIRE( nHit > 800, "random direction: the vast majority of constructed roots are found" );
	REQUIRE( nZDominant > 200, "random direction: enough |q.z|-dominant samples to prove identity" );
	REQUIRE( nParity == nZDominant, "random direction: every |q.z|-dominant sample matched legacy" );

	std::cout << "  " << nHit << " hits / " << N << " rays ("
		<< nSkippedGrazing << " near-tangent skipped), "
		<< nParity << "/" << nZDominant << " |q.z|-dominant matched legacy"
		<< " (max |delta| = " << maxDelta << ")\n";
}

// (d) OFF-RAY ROOT REJECTION.
//
// THE BUG, ONE SENTENCE.  The elimination in RayBilinearPatchIntersection
// solves only TWO of the three component equations of
// `P(u,v) = origin + t*q` and `computet` recovers `t` from ONE axis, so
// nothing in the chain ever asked whether the accepted `(u, v, t)` puts
// `P(u,v)` on the ray at all -- and when the patch is PLANAR in an axis
// `j` with `q[j] == 0`, row `j` is the INCONSISTENT equation `0 = D2`
// (`D2 != 0`), `coeff[0]` vanishes with it, `SolveQuadricWithinRange`
// takes its `a == 0` linear branch and hands back a `v` that solves
// nothing, and both of `getu`'s denominators are round-off residues of
// the same cancelling expression -- a near-0/0 ratio that lands in [0, 1]
// often enough to be accepted.
//
// Such a ray travels inside a plane PARALLEL to the patch's own and can
// never meet it, so the ground truth is analytic and needs no oracle:
// the answer is always MISS.  Part 2b below builds 100k of them.
//
// The fix is the residual gate `RootLiesOnRay` -- reconstruct P(u,v),
// compare |P(u,v) - (origin + t*q)| against a scale-relative floor, the
// same check `GeometricUtilities::BilinearInverse` already applies to its
// own two-axis solve.  It also closes the `SolveQuadricWithinRange`
// double-root misroot (`-b/a` for `-b/(2a)`, fixed the same round) for
// this caller, since a doubled `v` lands off the ray too.
static void TestBilinearOffRayRootRejection()
{
	std::cout << "Testing bilinear-patch off-ray root rejection..." << std::endl;

	// ---- Part 1: the reviewer's exact reproduction ---------------------
	//
	// A PLANAR, NON-PARALLELOGRAM quad in x = -2 struck from x = -1 by a
	// ray travelling in +Y, i.e. entirely inside the plane x = -1.  It
	// cannot touch the quad.  Pre-gate this reported a hit at the point
	// (-1, 0.333, 1) -- one whole unit off the quad's own plane -- with
	// (u, v, t) = (1, 0.667, 0.222).  (Non-parallelogram matters: for a
	// parallelogram `a[j] == 0` kills `coeff[1]` as well, the linear
	// branch reports no root, and the class stays invisible.)
	//
	// Corners are given in ClippedPlaneGeometry's row-major order
	// (vP[0]->(0,0), vP[1]->(1,0), vP[2]->(1,1), vP[3]->(0,1)); the raw
	// BilinearPatch below is that order under ToBilinearPatch's remap
	// pts = { vP[0], vP[3], vP[1], vP[2] }.
	const Point3 kQuad[4] = {
		Point3( -2.0,  0.0,  1.0 ),
		Point3( -2.0,  1.0,  0.0 ),
		Point3( -2.0,  0.0,  0.0 ),
		Point3( -2.0, -1.5, -0.5 ) };

	BilinearPatch reviewerPatch;
	reviewerPatch.pts[0] = kQuad[0];
	reviewerPatch.pts[1] = kQuad[3];
	reviewerPatch.pts[2] = kQuad[1];
	reviewerPatch.pts[3] = kQuad[2];

	{
		const Ray ray( Point3( -1.0, 0.0, 1.0 ), Vector3( 0.0, 1.5, 0.0 ) );

		BILINEAR_HIT h;
		RayBilinearPatchIntersection( ray, h, reviewerPatch );
		REQUIRE( !h.bHit,
			"off-ray: reviewer's planar non-parallelogram patch, ray in a parallel plane, MISSES" );
		if( h.bHit ) {
			std::cout << "    reported (u,v,t) = (" << h.u << ", " << h.v << ", "
				<< h.dRange << "), residual = "
				<< HitResidual( ray, reviewerPatch, h ) << "\n";
		}

		// Same ray, same corners, through the user-visible geometry.
		ClippedPlaneGeometry* g = new ClippedPlaneGeometry( kQuad, /*bDoubleSided=*/true );
		RayIntersectionGeometric ri( ray, nullRasterizerState );
		g->IntersectRay( ri, true, true, false );
		REQUIRE( !ri.bHit, "off-ray: the same case through clippedplane_geometry MISSES" );
		g->release();
	}

	// The same quad struck by a ray that DOES cross its plane must still
	// hit -- the gate must not have turned into a blanket rejection of
	// planar patches.  Aim at P(0.25, 0.25), which is inside the patch.
	{
		const Point3 P = GeometricUtilities::EvaluateBilinearPatchAt( reviewerPatch, 0.25, 0.25 );
		const Ray ray( Point3( P.x + 3.0, P.y, P.z ), Vector3( -1.0, 0.0, 0.0 ) );

		BILINEAR_HIT h;
		RayBilinearPatchIntersection( ray, h, reviewerPatch );
		REQUIRE( h.bHit, "off-ray: the same patch is still hit by a ray that crosses its plane" );
		if( h.bHit ) {
			REQUIRE( IsClose( h.u, 0.25, 1e-9 ), "off-ray: crossing ray u" );
			REQUIRE( IsClose( h.v, 0.25, 1e-9 ), "off-ray: crossing ray v" );
			REQUIRE( IsClose( h.dRange, 3.0, 1e-9 ), "off-ray: crossing ray t" );
		}
	}

	// ---- Part 2: randomized sweep -------------------------------------
	//
	// Coordinates are DYADIC (k/4 for integer k), so the differences and
	// products the elimination forms are exact in binary FP and the
	// degenerate branches are reached exactly rather than approximately.
	// Two halves, both seeded:
	//
	//   (a) 100k rays constructed to hit -- ground truth is the
	//       construction, so a miss is a LOST TRUE HIT.  A subsample is
	//       additionally checked against the brute-force grid + Newton
	//       oracle above (which shares no code with the analytic solver).
	//   (b) 100k rays in a plane parallel to a planar patch's own -- they
	//       cannot hit, so ANY accepted hit is a PHANTOM.
	//
	// RED-PROOF: dropping the four `RootLiesOnRay(...)` conjuncts from
	// RayBilinearPatchIntersection.cpp turns (b) red (and part 1 above
	// with it); the counts are printed either way.
	const int NHalf = 100000;
	LCG rng( 20260906ULL );

	auto dyadic = [&rng]( int lo, int hi ) -> Scalar {
		// Uniform over { lo/4, (lo+1)/4, ..., hi/4 }.
		const int span = hi - lo + 1;
		int k = lo + int( rng.next01() * Scalar(span) );
		if( k > hi ) k = hi;
		return Scalar(k) * 0.25;
	};
	auto setAxis = []( Point3& p, int k, Scalar val ) {
		if( k == 0 ) p.x = val; else if( k == 1 ) p.y = val; else p.z = val;
	};
	// World scale of the configuration, cycled over three decades.  The
	// producer's floor is scale-RELATIVE (`NEARZERO * (1 + coordScale +
	// |t|*|q|_1)`); an absolute epsilon would start losing true hits at
	// the top of this range and start admitting phantoms at the bottom.
	// The factors are powers of two so the dyadic coordinates stay exact.
	const Scalar kWorldScales[3] = { 1.0, 256.0, 65536.0 };

	// --- (a) constructed-to-hit ---
	int aHit = 0, aLost = 0, aSkipped = 0, aBadResidual = 0;
	int aOracleChecked = 0, aOracleDisagreed = 0;
	Scalar aMaxResidual = 0.0;

	for( int k = 0; k < NHalf; ++k ) {
		const Scalar ws = kWorldScales[ k % 3 ];
		BilinearPatch patch;
		for( int p = 0; p < 4; ++p ) {
			patch.pts[p] = Point3( ws*dyadic(-8, 8), ws*dyadic(-8, 8), ws*dyadic(-8, 8) );
		}

		const Scalar uT = 0.05 + 0.90 * rng.next01();
		const Scalar vT = 0.05 + 0.90 * rng.next01();
		const Point3 P = GeometricUtilities::EvaluateBilinearPatchAt( patch, uT, vT );

		// Uniform direction on the sphere.
		const Scalar z = 2.0 * rng.next01() - 1.0;
		const Scalar phi = 2.0 * 3.14159265358979323846 * rng.next01();
		const Scalar r = std::sqrt( std::fmax( 0.0, 1.0 - z*z ) );
		const Vector3 dir( r * std::cos(phi), r * std::sin(phi), z );

		// Surface tangents at the target, for the near-tangent skip: the
		// root is genuinely ill-conditioned there for ANY elimination
		// axis, and a degenerate patch has no normal at all.
		const Vector3 A( patch.pts[3].x - patch.pts[2].x - patch.pts[1].x + patch.pts[0].x,
		                 patch.pts[3].y - patch.pts[2].y - patch.pts[1].y + patch.pts[0].y,
		                 patch.pts[3].z - patch.pts[2].z - patch.pts[1].z + patch.pts[0].z );
		const Vector3 dpdu( (patch.pts[2].x - patch.pts[0].x) + A.x*vT,
		                    (patch.pts[2].y - patch.pts[0].y) + A.y*vT,
		                    (patch.pts[2].z - patch.pts[0].z) + A.z*vT );
		const Vector3 dpdv( (patch.pts[1].x - patch.pts[0].x) + A.x*uT,
		                    (patch.pts[1].y - patch.pts[0].y) + A.y*uT,
		                    (patch.pts[1].z - patch.pts[0].z) + A.z*uT );
		const Vector3 nrmRaw = Vector3Ops::Cross( dpdu, dpdv );
		if( Vector3Ops::SquaredModulus( nrmRaw ) < 1e-12 ) { aSkipped++; continue; }
		const Vector3 nrm = Vector3Ops::Normalize( nrmRaw );
		if( std::fabs( Vector3Ops::Dot( nrm, dir ) ) < 0.15 ) { aSkipped++; continue; }

		const Scalar t0 = 3.0 * ws;
		const Ray ray( Point3( P.x - t0*dir.x, P.y - t0*dir.y, P.z - t0*dir.z ), dir );

		BILINEAR_HIT h;
		RayBilinearPatchIntersection( ray, h, patch );
		if( !h.bHit ) { aLost++; continue; }
		aHit++;

		// Every accepted root must lie on the ray.  Scale-relative,
		// matching the producer's own floor derivation.
		Scalar scale = std::fabs(ray.origin.x) + std::fabs(ray.origin.y) + std::fabs(ray.origin.z);
		for( int ci = 0; ci < 4; ++ci ) {
			const Scalar s = std::fabs(patch.pts[ci].x) + std::fabs(patch.pts[ci].y) + std::fabs(patch.pts[ci].z);
			if( s > scale ) scale = s;
		}
		const Scalar res = HitResidual( ray, patch, h );
		if( res > aMaxResidual ) aMaxResidual = res;
		if( res > 1e-9 * ( 1.0 + scale ) ) aBadResidual++;

		// Oracle cross-check on a thin subsample (the oracle is a
		// 130x130 grid plus a 3x3 Newton, far too slow for all 100k).
		if( (k % 250) == 0 ) {
			Scalar bu, bv, bt;
			// The oracle's Newton is unconstrained, so it can converge to a
			// root of the INFINITE bilinear surface outside the (u, v) unit
			// square, or behind the origin.  Only a root that is actually on
			// the patch, in front of the ray, and on the ray is an opinion
			// worth comparing against.
			bool oracleValid = BruteForceBilinearRayRoot( ray, patch, bu, bv, bt );
			if( oracleValid ) {
				const Point3 bP = GeometricUtilities::EvaluateBilinearPatchAt( patch, bu, bv );
				const Point3 bR = ray.PointAtLength( bt );
				const Scalar bRes = std::sqrt( (bP.x-bR.x)*(bP.x-bR.x)
					+ (bP.y-bR.y)*(bP.y-bR.y) + (bP.z-bR.z)*(bP.z-bR.z) );
				oracleValid = ( bu >= -1e-9 && bu <= 1.0 + 1e-9
					&& bv >= -1e-9 && bv <= 1.0 + 1e-9
					&& bt > 0.0 && bRes < 1e-9 * ( 1.0 + scale ) );
			}
			if( oracleValid ) {
				aOracleChecked++;
				// The solver may legitimately report a NEARER second root
				// of the same patch; only demand agreement when the oracle
				// converged to the same t.
				const bool sameRoot = std::fabs( bt - h.dRange ) < 1e-6;
				const bool nearerRoot = ( h.dRange < bt + 1e-6 );
				const bool agree = sameRoot
					? ( std::fabs( h.u - bu ) <= 1e-6 && std::fabs( h.v - bv ) <= 1e-6 )
					: nearerRoot;
				if( !agree ) {
					aOracleDisagreed++;
					if( aOracleDisagreed <= 3 ) {
						std::cout << "    ORACLE DISAGREE at k=" << k
							<< ": solver (u,v,t) = (" << h.u << ", " << h.v << ", " << h.dRange
							<< "), oracle (" << bu << ", " << bv << ", " << bt << ")\n";
					}
				}
			}
		}
	}

	// --- (b) rays that cannot possibly hit ---
	int bPhantom = 0, bTested = 0;
	Scalar bWorstResidual = 0.0;

	for( int k = 0; k < NHalf; ++k ) {
		// Planar patch: every corner shares coordinate `kAxis`.
		const Scalar ws = kWorldScales[ k % 3 ];
		const int kAxis = int( rng.next01() * 3.0 ) % 3;
		const Scalar planeAt = ws * dyadic(-8, 8);

		BilinearPatch patch;
		for( int p = 0; p < 4; ++p ) {
			patch.pts[p] = Point3( ws*dyadic(-8, 8), ws*dyadic(-8, 8), ws*dyadic(-8, 8) );
			setAxis( patch.pts[p], kAxis, planeAt );
		}

		// Ray origin strictly OFF that plane, direction strictly INSIDE a
		// parallel one (q[kAxis] == 0 exactly).  Such a ray stays in the
		// plane `kAxis = origin[kAxis] != planeAt` forever.
		Point3 origin( ws*dyadic(-8, 8), ws*dyadic(-8, 8), ws*dyadic(-8, 8) );
		Scalar off = ws * dyadic(-8, 8);
		if( off == planeAt ) off = planeAt + ws;
		setAxis( origin, kAxis, off );

		Vector3 dir( dyadic(-4, 4), dyadic(-4, 4), dyadic(-4, 4) );
		if( kAxis == 0 ) dir.x = 0.0; else if( kAxis == 1 ) dir.y = 0.0; else dir.z = 0.0;
		if( Vector3Ops::SquaredModulus( dir ) == 0.0 ) continue;

		bTested++;

		const Ray ray( origin, dir );
		BILINEAR_HIT h;
		RayBilinearPatchIntersection( ray, h, patch );
		if( h.bHit ) {
			bPhantom++;
			const Scalar res = HitResidual( ray, patch, h );
			if( res > bWorstResidual ) bWorstResidual = res;
			// The patch plane is `kAxis == planeAt`; the ray never leaves
			// `kAxis == off`.  |off - planeAt| is the distance the
			// "intersection" is wrong by.
			if( bPhantom <= 3 ) {
				std::cout << "    PHANTOM: axis " << kAxis << ", patch plane " << planeAt
					<< ", ray plane " << off << ", (u,v,t) = (" << h.u << ", " << h.v
					<< ", " << h.dRange << "), residual = " << res << "\n";
			}
		}
	}

	REQUIRE( aLost == 0, "off-ray sweep: no constructed true hit was lost" );
	REQUIRE( aBadResidual == 0, "off-ray sweep: every accepted root lies on the ray" );
	REQUIRE( aOracleDisagreed == 0, "off-ray sweep: analytic solver agrees with the brute-force oracle" );
	REQUIRE( aOracleChecked > 100, "off-ray sweep: enough oracle cross-checks to be meaningful" );
	REQUIRE( aHit > NHalf / 2, "off-ray sweep: the constructed-to-hit half really does hit" );
	REQUIRE( bPhantom == 0, "off-ray sweep: no phantom hit on a ray parallel to the patch's plane" );
	REQUIRE( bTested > NHalf / 2, "off-ray sweep: enough parallel-plane rays built" );

	std::cout << "  constructed-to-hit: " << aHit << " hit, " << aLost << " lost, "
		<< aSkipped << " near-tangent/degenerate skipped, "
		<< aOracleChecked << " oracle cross-checks (" << aOracleDisagreed << " disagreed)"
		<< ", max on-ray residual " << aMaxResidual << "\n";
	std::cout << "  parallel-plane (cannot hit): " << bTested << " rays, "
		<< bPhantom << " phantoms";
	if( bPhantom ) std::cout << " (worst off-ray distance " << bWorstResidual << ")";
	std::cout << "\n";
}

static void TestBilinearEliminationAxis()
{
	TestBilinearAxisAlignedRays();
	TestBilinearCurvedAxisAlignedRays();
	TestBilinearRandomDirectionParity();
	TestBilinearOffRayRootRejection();
}

static void TestObjectWorldArea()
{
	std::cout << "Testing Object::GetArea world-area Jacobian..." << std::endl;

	SphereGeometry* g = new SphereGeometry( 1.5 );
	const Scalar geomArea = g->GetArea();
	REQUIRE( geomArea > 0.0, "object-area: geometry area positive" );

	{
		// identity transform: world area == object area
		Implementation::Object* o = new Implementation::Object( g );
		o->FinalizeTransformations();
		REQUIRE( IsClose( o->GetArea(), geomArea, 1e-9 * geomArea ),
			"object-area: identity transform preserves area" );
		o->release();
	}
	{
		// uniform scale s: world area == s^2 * object area (exact)
		Implementation::Object* o = new Implementation::Object( g );
		o->SetScale( 2.0 );
		o->FinalizeTransformations();
		REQUIRE( IsClose( o->GetArea(), 4.0 * geomArea, 1e-9 * geomArea ),
			"object-area: uniform scale 2 gives 4x area" );
		o->release();
	}
	{
		// rotation + translation: area invariant
		Implementation::Object* o = new Implementation::Object( g );
		o->SetOrientation( Vector3( 0.3, 1.1, -0.7 ) );
		o->TranslateObject( Vector3( 5, -2, 3 ) );
		o->FinalizeTransformations();
		REQUIRE( IsClose( o->GetArea(), geomArea, 1e-9 * geomArea ),
			"object-area: rotation+translation preserve area" );
		o->release();
	}

	{
		// non-uniform scale: |det|^(2/3) geometric-mean APPROXIMATION.
		// This pins the documented current behaviour (not ground truth —
		// the true world area of a stretched sphere differs; see the
		// comment in Object::GetArea and the LuminaryManager warn-once).
		Implementation::Object* o = new Implementation::Object( g );
		o->SetStretch( Vector3( 4.0, 0.05, 4.0 ) );
		o->FinalizeTransformations();
		const Scalar expect = geomArea * pow( 4.0 * 0.05 * 4.0, 2.0 / 3.0 );
		REQUIRE( IsClose( o->GetArea(), expect, 1e-9 * geomArea ),
			"object-area: non-uniform scale uses |det|^(2/3) approximation" );
		o->release();
	}
	{
		// RISE_INFINITY sentinel (infinite plane): must pass through
		// UNTOUCHED — multiplying DBL_MAX by any factor > 1 overflows to
		// +inf, which would NaN the light-selection alias table
		// (TotalWeight=inf → pdf=inf/inf).
		InfinitePlaneGeometry* plane = new InfinitePlaneGeometry( 1.0, 1.0 );
		Implementation::Object* o = new Implementation::Object( plane );
		o->SetScale( 2.0 );
		o->SetOrientation( Vector3( 0.2, 0.5, 0.1 ) );
		o->FinalizeTransformations();
		const Scalar a = o->GetArea();
		REQUIRE( a == plane->GetArea(),
			"object-area: RISE_INFINITY sentinel passes through unscaled" );
		REQUIRE( a <= RISE_INFINITY && a == a,
			"object-area: infinite-plane area stays finite (no inf/NaN)" );
		o->release();
		plane->release();
	}

	g->release();
	std::cout << "  object world-area Jacobian checks done\n";
}

// ============================================================
// Object world-space DERIVATIVE transform (dndu/dndv quotient rule)
//
// Regression for two sites that promote a geometry's OBJECT-space
// dndu/dndv to WORLD space: Object::IntersectRay's derivatives block and
// Object::ComputeAnalyticalDerivatives (both in src/Library/Objects/
// Object.cpp).  dndu/dndv are derivatives of the SHADING NORMAL, i.e. of
// a normalized (unit) vector field -- a plain inverse-transpose transform
// (correct for dpdu/dpdv, and for the normal ITSELF once renormalized)
// under-corrects dndu/dndv by an extra factor of the local scale whenever
// the transform isn't rigid, because it skips the renormalization the
// normal FIELD requires.  The correct transform is the quotient rule:
//   dn_w/du = (I - n_w n_w^T) . (M^-T dndu_obj) / ||M^-T n_obj||
// For a uniform scale s this makes world mean curvature H_world = H_obj/s;
// the pre-fix code gave H_obj/s^2.  See docs/GEOMETRY_DERIVATIVES.md
// "World-space transform" for the derivation.
//
// Exercises BOTH fixed call sites:
//   Part A -- Object::IntersectRay's derivatives block, via a tessellated
//             triangle-mesh sphere (the only geometry kind that populates
//             ri.geometric.derivatives during IntersectRay -- analytic
//             primitives like SphereGeometry never do, so they can't
//             exercise this specific code path).
//   Part B -- Object::ComputeAnalyticalDerivatives, via EllipsoidGeometry
//             (a=b=c gives an exact closed-form sphere curvature 1/r for
//             absolute-tolerance checks; a genuinely non-uniform stretch
//             gives an exact closed-form ellipsoid ground truth).
// ============================================================

// Mean curvature from the standard second-fundamental-form ratio.  Sign
// convention follows whatever the record's (dpdu, dpdv, dndu, dndv, n)
// happen to use -- callers compare |H| to sidestep sign-convention traps.
static Scalar RecordMeanCurvatureH(
	const Vector3& dpdu, const Vector3& dpdv,
	const Vector3& dndu, const Vector3& dndv )
{
	const Scalar E = Vector3Ops::Dot( dpdu, dpdu );
	const Scalar F = Vector3Ops::Dot( dpdu, dpdv );
	const Scalar G = Vector3Ops::Dot( dpdv, dpdv );
	const Scalar e = Vector3Ops::Dot( dndu, dpdu );
	const Scalar f = 0.5 * ( Vector3Ops::Dot( dndu, dpdv ) + Vector3Ops::Dot( dndv, dpdu ) );
	const Scalar g = Vector3Ops::Dot( dndv, dpdv );
	return ( e * G - 2.0 * f * F + g * E ) / ( 2.0 * ( E * G - F * F ) );
}

// Independent closed-form mean curvature of an ellipsoid
// x^2/a^2 + y^2/b^2 + z^2/c^2 = 1 at object-space surface point (x, y, z).
// Derived from H = (1/2) div(grad F / |grad F|) for the quadric
// F = x^2/a^2+y^2/b^2+z^2/c^2 - 1, whose Hessian is the CONSTANT diagonal
// matrix diag(2/a^2, 2/b^2, 2/c^2):
//   div(grad F / |grad F|)
//     = ( |grad F|^2 * trace(Hess F) - (grad F)^T Hess(F) (grad F) ) / |grad F|^3
// Substituting grad F = 2*(x/a^2, y/b^2, z/c^2) and simplifying:
//   S = x^2/a^4 + y^2/b^4 + z^2/c^4        (= |grad F|^2 / 4)
//   T = 1/a^2 + 1/b^2 + 1/c^2              (= trace(Hess F) / 2)
//   U = x^2/a^6 + y^2/b^6 + z^2/c^6        (= (grad F)^T Hess(F) (grad F) / 8)
//   H = (S*T - U) / (2 * S^1.5)
// Sanity-checked in the a=b=c=r special case: S=1/r^2, T=3/r^2, U=1/r^4
// (using x^2+y^2+z^2=r^2 on the surface) gives
// H = (3/r^4 - 1/r^4) / (2/r^3) = (2/r^4)*(r^3/2) = 1/r, the known sphere
// mean curvature -- this formula is used independently of, and predates,
// any code in Object.cpp, so it is a real ground truth rather than a
// restatement of the code under test.
static Scalar EllipsoidClosedFormH(
	Scalar a, Scalar b, Scalar c, Scalar x, Scalar y, Scalar z )
{
	const Scalar a2 = a * a, b2 = b * b, c2 = c * c;
	const Scalar S = x*x / (a2*a2) + y*y / (b2*b2) + z*z / (c2*c2);
	const Scalar T = 1.0/a2 + 1.0/b2 + 1.0/c2;
	const Scalar U = x*x / (a2*a2*a2) + y*y / (b2*b2*b2) + z*z / (c2*c2*c2);
	return ( S * T - U ) / ( 2.0 * pow( S, 1.5 ) );
}

// Builds a smooth-shaded (per-vertex analytic normal) triangle-mesh
// approximation of a sphere by reusing SphereGeometry::TessellateToMesh --
// deliberately NOT hand-deriving the spherical trig here, so the mesh's
// vertex positions/normals are exactly what the already-tested tessellator
// produces.  useFaceNormals=false so TriangleMeshGeometryIndexedSpecializations'
// per-triangle UV-Jacobian inversion sees non-degenerate per-vertex normal
// differences and populates non-zero dndu/dndv -- a flat-shaded / face-normal
// mesh would give dndu=dndv=(0,0,0) identically, testing nothing.
static Implementation::TriangleMeshGeometryIndexed* BuildTessellatedSphereMesh(
	Scalar radius, unsigned int detail )
{
	SphereGeometry* g = new SphereGeometry( radius );
	IndexTriangleListType tris;
	VerticesListType verts;
	NormalsListType norms;
	TexCoordsListType coords;
	const bool ok = g->TessellateToMesh( tris, verts, norms, coords, detail );
	g->release();
	if( !ok ) {
		return 0;
	}

	Implementation::TriangleMeshGeometryIndexed* pMesh =
		new Implementation::TriangleMeshGeometryIndexed( true, false );
	pMesh->BeginIndexedTriangles();
	pMesh->AddVertices( verts );
	pMesh->AddNormals( norms );
	pMesh->AddTexCoords( coords );
	pMesh->AddIndexedTriangles( tris );
	pMesh->DoneIndexedTriangles();
	return pMesh;
}

// Fires the SAME object-space ray (expressed once in local coordinates,
// then promoted through the object's own GetFinalTransformMatrix() so it
// lands on the IDENTICAL local hit point regardless of the object's own
// transform -- mirrors the technique CsgSurfacePayloadTest.cpp uses to
// compare a rotated CSG against a standalone rotated reference) and
// returns the populated derivatives from Object::IntersectRay.  Returns
// false if the ray missed or the geometry didn't populate valid
// derivatives.
static bool HitMeshDerivatives(
	Implementation::Object* obj,
	const Point3& localOrigin,
	const Vector3& localDir,
	Vector3& worldNormal,
	Vector3& dpdu, Vector3& dpdv, Vector3& dndu, Vector3& dndv )
{
	const Matrix4 mxFinal = obj->GetFinalTransformMatrix();
	const Point3 worldOrigin = Point3Ops::Transform( mxFinal, localOrigin );
	const Vector3 worldDir = Vector3Ops::Transform( mxFinal, localDir );

	RayIntersection ri( Ray( worldOrigin, worldDir ), nullRasterizerState );
	obj->IntersectRay( ri, RISE_INFINITY, true, true, false );
	if( !ri.geometric.bHit || !ri.geometric.derivatives.valid ) {
		return false;
	}
	worldNormal = ri.geometric.vNormal;
	dpdu = ri.geometric.derivatives.dpdu;
	dpdv = ri.geometric.derivatives.dpdv;
	dndu = ri.geometric.derivatives.dndu;
	dndv = ri.geometric.derivatives.dndv;
	return true;
}

static void TestObjectWorldDerivatives()
{
	std::cout << "Testing Object world-space dndu/dndv quotient-rule transform..." << std::endl;

	const Scalar r = 1.5;
	const unsigned int detail = 40;   // 40x40 grid -> smooth-shaded curvature approx well within 5%

	// A local-frame ray hitting a well-conditioned, off-axis equatorial-ish
	// point -- far from both poles, and not landing exactly on a mesh grid
	// vertex/edge.
	const Point3 localOrigin( 0.35, 0.22, 10.0 );
	const Vector3 localDir( 0, 0, -1 );

	// ---- Part A: Object::IntersectRay's derivatives block (mesh path) ----

	Scalar H_identity_mesh = 0.0;
	{
		Implementation::TriangleMeshGeometryIndexed* mesh = BuildTessellatedSphereMesh( r, detail );
		REQUIRE( mesh != 0, "derivatives: tessellated sphere mesh built" );
		Implementation::Object* o = new Implementation::Object( mesh );
		mesh->release();
		o->FinalizeTransformations();

		Vector3 n, dpdu, dpdv, dndu, dndv;
		const bool hit = HitMeshDerivatives( o, localOrigin, localDir, n, dpdu, dpdv, dndu, dndv );
		REQUIRE( hit, "derivatives: identity mesh hit with valid derivatives" );
		if( hit ) {
			H_identity_mesh = fabs( RecordMeanCurvatureH( dpdu, dpdv, dndu, dndv ) );
			// Absolute sanity vs the true sphere curvature 1/r -- loose
			// tolerance because this is a genuinely discretized mesh,
			// unlike the exact ratio checks below.
			REQUIRE( IsClose( H_identity_mesh, 1.0 / r, 0.05 * (1.0/r) ),
				"derivatives: identity-mesh |H| close to analytic sphere 1/r" );

			// Invariants 1+2 (docs/GEOMETRY_DERIVATIVES.md).
			REQUIRE( fabs( Vector3Ops::Dot( dndu, n ) ) < 1e-6, "derivatives: identity dndu . n ~= 0" );
			REQUIRE( fabs( Vector3Ops::Dot( dndv, n ) ) < 1e-6, "derivatives: identity dndv . n ~= 0" );
			REQUIRE( fabs( Vector3Ops::Dot( dpdu, n ) ) < 1e-6, "derivatives: identity dpdu . n ~= 0" );
			REQUIRE( fabs( Vector3Ops::Dot( dpdv, n ) ) < 1e-6, "derivatives: identity dpdv . n ~= 0" );
			REQUIRE( std::isfinite( H_identity_mesh ), "derivatives: identity |H| finite" );
		}
		o->release();
	}

	// Uniform scale s=2: SAME mesh construction, SAME local hit point (the
	// ray is re-promoted through THIS object's own transform).  The
	// object-space dpdu/dpdv/dndu/dndv the geometry hands back are
	// BYTE-IDENTICAL to the identity case (same triangle, same barycentric
	// point) -- so this is an EXACT algebraic check, not a discretization-
	// limited one: the fixed quotient-rule transform gives
	// H_world = H_obj/s (ratio == s between identity and scaled); the
	// pre-fix plain inverse-transpose gave H_obj/s^2 (ratio == s^2). s=2
	// separates those by 2x, far outside FP noise, so this catches a
	// regression to the old bug at a tight tolerance.
	{
		const Scalar s = 2.0;
		Implementation::TriangleMeshGeometryIndexed* mesh = BuildTessellatedSphereMesh( r, detail );
		Implementation::Object* o = new Implementation::Object( mesh );
		mesh->release();
		o->SetScale( s );
		o->FinalizeTransformations();

		Vector3 n, dpdu, dpdv, dndu, dndv;
		const bool hit = HitMeshDerivatives( o, localOrigin, localDir, n, dpdu, dpdv, dndu, dndv );
		REQUIRE( hit, "derivatives: scaled mesh hit with valid derivatives" );
		if( hit ) {
			const Scalar H_scaled = fabs( RecordMeanCurvatureH( dpdu, dpdv, dndu, dndv ) );
			REQUIRE( H_scaled > 0.0, "derivatives: scaled |H| non-degenerate" );

			const Scalar ratio = H_identity_mesh / H_scaled;
			REQUIRE( IsClose( ratio, s, 1e-6 * s ),
				"derivatives: H_identity/H_scaled == s (quotient-rule fix; old bug gave s^2)" );
			// Explicitly confirm the OLD buggy ratio (s^2 = 4) is excluded --
			// s vs s^2 differ by 2x here, far outside 1e-6 relative tolerance
			// either way, so this is a clean, loud failure if the bug returns.
			REQUIRE( !IsClose( ratio, s * s, 1e-6 * s * s ),
				"derivatives: ratio does NOT match the old buggy s^2 behaviour" );

			REQUIRE( fabs( Vector3Ops::Dot( dndu, n ) ) < 1e-6, "derivatives: scaled dndu . n ~= 0" );
			REQUIRE( fabs( Vector3Ops::Dot( dndv, n ) ) < 1e-6, "derivatives: scaled dndv . n ~= 0" );
		}
		o->release();
	}

	// Rigid transform (rotation + translation, no scale): |H| and the
	// dndu/dndv MAGNITUDES must be unchanged from identity -- the quotient
	// rule is a provable no-op under a rigid transform (||M^-T n|| == 1,
	// and the (I - n n^T) projection removes a component that's already
	// ~0 by the object-space tangency contract).
	{
		Implementation::TriangleMeshGeometryIndexed* meshId = BuildTessellatedSphereMesh( r, detail );
		Implementation::Object* oId = new Implementation::Object( meshId );
		meshId->release();
		oId->FinalizeTransformations();
		Vector3 nId, dpduId, dpdvId, dnduId, dndvId;
		const bool hitId = HitMeshDerivatives( oId, localOrigin, localDir, nId, dpduId, dpdvId, dnduId, dndvId );
		REQUIRE( hitId, "derivatives: (control) identity hit for rigid comparison" );

		Implementation::TriangleMeshGeometryIndexed* meshRigid = BuildTessellatedSphereMesh( r, detail );
		Implementation::Object* oRigid = new Implementation::Object( meshRigid );
		meshRigid->release();
		oRigid->SetOrientation( Vector3( 0.3, 1.1, -0.7 ) );
		oRigid->TranslateObject( Vector3( 5, -2, 3 ) );
		oRigid->FinalizeTransformations();
		Vector3 nR, dpduR, dpdvR, dnduR, dndvR;
		const bool hitR = HitMeshDerivatives( oRigid, localOrigin, localDir, nR, dpduR, dpdvR, dnduR, dndvR );
		REQUIRE( hitR, "derivatives: rigid-transform hit" );

		if( hitId && hitR ) {
			const Scalar H_id = fabs( RecordMeanCurvatureH( dpduId, dpdvId, dnduId, dndvId ) );
			const Scalar H_rigid = fabs( RecordMeanCurvatureH( dpduR, dpdvR, dnduR, dndvR ) );
			REQUIRE( IsClose( H_rigid, H_id, 1e-6 * H_id ),
				"derivatives: rigid transform preserves |H| exactly (no-op)" );

			REQUIRE( IsClose( Vector3Ops::Magnitude( dnduR ), Vector3Ops::Magnitude( dnduId ), 1e-6 ),
				"derivatives: rigid transform preserves |dndu|" );
			REQUIRE( IsClose( Vector3Ops::Magnitude( dndvR ), Vector3Ops::Magnitude( dndvId ), 1e-6 ),
				"derivatives: rigid transform preserves |dndv|" );
		}
		oId->release();
		oRigid->release();
	}

	// Non-uniform stretch: post-transform invariants (dndu . n ~= 0,
	// dndv . n ~= 0, dpdu . n ~= 0, dpdv . n ~= 0, all finite).  The
	// absolute/ratio ground truth for a non-uniform transform is checked
	// against a real closed-form ellipsoid in Part B below.
	{
		Implementation::TriangleMeshGeometryIndexed* mesh = BuildTessellatedSphereMesh( r, detail );
		Implementation::Object* o = new Implementation::Object( mesh );
		mesh->release();
		o->SetStretch( Vector3( 2.0, 0.5, 1.25 ) );
		o->FinalizeTransformations();

		Vector3 n, dpdu, dpdv, dndu, dndv;
		const bool hit = HitMeshDerivatives( o, localOrigin, localDir, n, dpdu, dpdv, dndu, dndv );
		REQUIRE( hit, "derivatives: non-uniform-stretch mesh hit" );
		if( hit ) {
			REQUIRE( fabs( Vector3Ops::Dot( dndu, n ) ) < 1e-5, "derivatives: stretch dndu . n ~= 0" );
			REQUIRE( fabs( Vector3Ops::Dot( dndv, n ) ) < 1e-5, "derivatives: stretch dndv . n ~= 0" );
			REQUIRE( fabs( Vector3Ops::Dot( dpdu, n ) ) < 1e-5, "derivatives: stretch dpdu . n ~= 0" );
			REQUIRE( fabs( Vector3Ops::Dot( dpdv, n ) ) < 1e-5, "derivatives: stretch dpdv . n ~= 0" );
			REQUIRE( std::isfinite( n.x ) && std::isfinite( n.y ) && std::isfinite( n.z ),
				"derivatives: stretch world normal finite" );
			REQUIRE( std::isfinite( dndu.x ) && std::isfinite( dndu.y ) && std::isfinite( dndu.z ),
				"derivatives: stretch dndu finite" );
			REQUIRE( std::isfinite( dndv.x ) && std::isfinite( dndv.y ) && std::isfinite( dndv.z ),
				"derivatives: stretch dndv finite" );
		}
		o->release();
	}

	// ---- Part B: Object::ComputeAnalyticalDerivatives (EllipsoidGeometry) ----
	// EllipsoidGeometry(a,b,c) with a=b=c=r is mathematically a sphere of
	// radius r; its ComputeAnalyticalDerivatives gives EXACT closed-form
	// (u,v)-parameterised derivatives with no mesh-discretization noise, so
	// these checks compare against absolute closed-form values instead of
	// the loose 5% mesh tolerance used in Part A.

	// Get a valid (u, v) near the equator by ray-hitting an identity
	// ellipsoid once (EllipsoidGeometry populates ri.geometric.ptCoord in
	// IntersectRay, in OBJECT-space parameter terms -- unaffected by the
	// wrapping Object's world transform, and the theta/phi -> (u,v) map is
	// identical regardless of the semi-axis values, so this (u,v) is
	// reusable for every variant below, including the differently-scaled
	// unit-sphere base in the non-uniform ground-truth check).
	Point2 hitUV;
	{
		EllipsoidGeometry* g = new EllipsoidGeometry( Vector3( r, r, r ) );
		Implementation::Object* o = new Implementation::Object( g );
		g->release();
		o->FinalizeTransformations();

		RayIntersection ri( Ray( localOrigin, localDir ), nullRasterizerState );
		o->IntersectRay( ri, RISE_INFINITY, true, true, false );
		REQUIRE( ri.geometric.bHit, "derivatives(B): identity ellipsoid ray hit for UV" );
		hitUV = ri.geometric.ptCoord;
		o->release();
	}

	// Tolerance for comparisons against the closed-form 1/r (etc.):
	// EllipsoidGeometry::ComputeAnalyticalDerivatives computes dN/du, dN/dv
	// via CENTRAL FINITE DIFFERENCE on the unit gradient-normal (epsUv =
	// 1e-3; see the comment at EllipsoidGeometry.cpp's ComputeAnalyticalDerivatives
	// "dN/du, dN/dv via central FD"), so its outputs carry an inherent
	// O(epsUv^2) ~ 1e-6 relative discretization error even for the
	// identity/no-bug case -- confirmed empirically (~2-4e-6 relative on
	// |H| at this test's hit point).  1e-4 gives >25x margin over that
	// floor while remaining >1000x tighter than the old bug's ~100%
	// (s or s^2) error, so it still fails loudly on a regression.
	const Scalar kClosedFormRelTol = 1e-4;

	// Identity: |H| must equal 1/r (closed form, within FD-noise tolerance).
	{
		EllipsoidGeometry* g = new EllipsoidGeometry( Vector3( r, r, r ) );
		Implementation::Object* o = new Implementation::Object( g );
		g->release();
		o->FinalizeTransformations();

		Point3 pos; Vector3 n, dpdu, dpdv, dndu, dndv;
		const bool ok = o->ComputeAnalyticalDerivatives( hitUV, 0.0, pos, n, dpdu, dpdv, dndu, dndv );
		REQUIRE( ok, "derivatives(B): identity ComputeAnalyticalDerivatives succeeds" );
		if( ok ) {
			const Scalar H = fabs( RecordMeanCurvatureH( dpdu, dpdv, dndu, dndv ) );
			REQUIRE( IsClose( H, 1.0 / r, kClosedFormRelTol * (1.0/r) ),
				"derivatives(B): identity |H| == 1/r (closed form)" );
		}
		o->release();
	}

	// Uniform scale s: |H| must equal 1/(r*s), ruling out the old buggy
	// 1/(r*s^2) which differs by 2x -- far outside kClosedFormRelTol.
	{
		const Scalar s = 2.0;
		EllipsoidGeometry* g = new EllipsoidGeometry( Vector3( r, r, r ) );
		Implementation::Object* o = new Implementation::Object( g );
		g->release();
		o->SetScale( s );
		o->FinalizeTransformations();

		Point3 pos; Vector3 n, dpdu, dpdv, dndu, dndv;
		const bool ok = o->ComputeAnalyticalDerivatives( hitUV, 0.0, pos, n, dpdu, dpdv, dndu, dndv );
		REQUIRE( ok, "derivatives(B): scaled ComputeAnalyticalDerivatives succeeds" );
		if( ok ) {
			const Scalar H = fabs( RecordMeanCurvatureH( dpdu, dpdv, dndu, dndv ) );
			REQUIRE( IsClose( H, 1.0 / (r * s), kClosedFormRelTol * (1.0/(r*s)) ),
				"derivatives(B): scaled |H| == 1/(r*s) (closed form; old bug gave 1/(r*s^2))" );
			REQUIRE( !IsClose( H, 1.0 / (r * s * s), kClosedFormRelTol * (1.0/(r*s*s)) ),
				"derivatives(B): scaled |H| does NOT match the old buggy 1/(r*s^2)" );
		}
		o->release();
	}

	// Rigid transform: |H| unchanged, dndu/dndv magnitudes unchanged.
	{
		EllipsoidGeometry* gId = new EllipsoidGeometry( Vector3( r, r, r ) );
		Implementation::Object* oId = new Implementation::Object( gId );
		gId->release();
		oId->FinalizeTransformations();
		Point3 posId; Vector3 nId, dpduId, dpdvId, dnduId, dndvId;
		const bool okId = oId->ComputeAnalyticalDerivatives( hitUV, 0.0, posId, nId, dpduId, dpdvId, dnduId, dndvId );
		REQUIRE( okId, "derivatives(B): (control) identity for rigid comparison" );

		EllipsoidGeometry* gR = new EllipsoidGeometry( Vector3( r, r, r ) );
		Implementation::Object* oR = new Implementation::Object( gR );
		gR->release();
		oR->SetOrientation( Vector3( -0.4, 0.6, 1.2 ) );
		oR->TranslateObject( Vector3( -3, 8, 1 ) );
		oR->FinalizeTransformations();
		Point3 posR; Vector3 nR, dpduR, dpdvR, dnduR, dndvR;
		const bool okR = oR->ComputeAnalyticalDerivatives( hitUV, 0.0, posR, nR, dpduR, dpdvR, dnduR, dndvR );
		REQUIRE( okR, "derivatives(B): rigid ComputeAnalyticalDerivatives succeeds" );

		if( okId && okR ) {
			const Scalar H_id = fabs( RecordMeanCurvatureH( dpduId, dpdvId, dnduId, dndvId ) );
			const Scalar H_r = fabs( RecordMeanCurvatureH( dpduR, dpdvR, dnduR, dndvR ) );
			// A tighter tolerance than kClosedFormRelTol is valid here: both
			// sides run the SAME (u,v), SAME central-FD step, and differ
			// only by a rotation applied AFTER the FD evaluation, so the FD
			// noise cancels almost exactly rather than accumulating (empirically ~0).
			REQUIRE( IsClose( H_r, H_id, 1e-5 * H_id ), "derivatives(B): rigid transform preserves |H| (no-op)" );
			REQUIRE( IsClose( Vector3Ops::Magnitude( dnduR ), Vector3Ops::Magnitude( dnduId ), 1e-9 ),
				"derivatives(B): rigid transform preserves |dndu|" );
			REQUIRE( IsClose( Vector3Ops::Magnitude( dndvR ), Vector3Ops::Magnitude( dndvId ), 1e-9 ),
				"derivatives(B): rigid transform preserves |dndv|" );
		}
		oId->release();
		oR->release();
	}

	// Non-uniform ground truth: stretch (a, b, c) applied to a UNIT sphere
	// makes an ellipsoid with semi-axes exactly (a, b, c) -- SetStretch
	// alone, with no rotation/position, is a pure diagonal linear map
	// (Transformable::FinalizeTransformations composes
	// Position*Orientation*Stretch*Scale*Mirror, and all but Stretch are
	// identity here).  The record-derived |H| at the transformed world
	// point must match EllipsoidClosedFormH evaluated at that SAME world
	// point with those semi-axes: H is parameterization-invariant, so
	// comparing a record-derived value against an independently-derived
	// closed form is valid regardless of how the record's own (u, v)
	// chart is laid out.
	{
		const Vector3 stretch( 2.0, 0.5, 1.25 );
		EllipsoidGeometry* g = new EllipsoidGeometry( Vector3( 1.0, 1.0, 1.0 ) );  // unit sphere
		Implementation::Object* o = new Implementation::Object( g );
		g->release();
		o->SetStretch( stretch );
		o->FinalizeTransformations();

		Point3 pos; Vector3 n, dpdu, dpdv, dndu, dndv;
		const bool ok = o->ComputeAnalyticalDerivatives( hitUV, 0.0, pos, n, dpdu, dpdv, dndu, dndv );
		REQUIRE( ok, "derivatives(B): non-uniform ComputeAnalyticalDerivatives succeeds" );
		if( ok ) {
			const Scalar H_record = fabs( RecordMeanCurvatureH( dpdu, dpdv, dndu, dndv ) );
			const Scalar H_closed = fabs( EllipsoidClosedFormH(
				stretch.x, stretch.y, stretch.z, pos.x, pos.y, pos.z ) );
			REQUIRE( IsClose( H_record, H_closed, kClosedFormRelTol * H_closed ),
				"derivatives(B): non-uniform |H| matches independent closed-form ellipsoid curvature" );

			// Same invariants as Part A, at a tighter tolerance since these
			// derivatives are exact/analytic, not mesh-discretized.
			REQUIRE( fabs( Vector3Ops::Dot( dndu, n ) ) < 1e-9, "derivatives(B): stretch dndu . n ~= 0" );
			REQUIRE( fabs( Vector3Ops::Dot( dndv, n ) ) < 1e-9, "derivatives(B): stretch dndv . n ~= 0" );
			REQUIRE( fabs( Vector3Ops::Dot( dpdu, n ) ) < 1e-9, "derivatives(B): stretch dpdu . n ~= 0" );
			REQUIRE( fabs( Vector3Ops::Dot( dpdv, n ) ) < 1e-9, "derivatives(B): stretch dpdv . n ~= 0" );
			REQUIRE( std::isfinite( H_record ), "derivatives(B): stretch |H| finite" );
		}
		o->release();
	}

	std::cout << "  object world-derivative quotient-rule checks done\n";
}

// Two-triangle flat quad in the local z=0 plane, constant vertex normal
// (0,0,1) -- deliberately NOT a curved/tessellated shape.  Unlike
// BuildTessellatedSphereMesh's near-pole triangles (interpolated, never
// EXACTLY axis-aligned), every point on this quad has a byte-exact
// axis-aligned normal, which TestSingularNormalTransformGuard() below needs:
// after a huge single-axis stretch, ||M^-T n|| must land under NEARZERO
// (1e-12), and that requires the OTHER two normal components to be exactly
// zero, not just close to it.
static Implementation::TriangleMeshGeometryIndexed* BuildFlatQuadMeshNormalZ()
{
	VerticesListType verts;
	NormalsListType norms;
	TexCoordsListType coords;
	IndexTriangleListType tris;

	verts.push_back( Point3( -1, -1, 0 ) );
	verts.push_back( Point3(  1, -1, 0 ) );
	verts.push_back( Point3(  1,  1, 0 ) );
	verts.push_back( Point3( -1,  1, 0 ) );
	for( int i = 0; i < 4; i++ ) {
		norms.push_back( Vector3( 0, 0, 1 ) );
	}
	coords.push_back( Point2( 0, 0 ) );
	coords.push_back( Point2( 1, 0 ) );
	coords.push_back( Point2( 1, 1 ) );
	coords.push_back( Point2( 0, 1 ) );

	IndexedTriangle t0, t1;
	t0.iVertices[0] = 0; t0.iVertices[1] = 1; t0.iVertices[2] = 2;
	t0.iNormals[0]  = 0; t0.iNormals[1]  = 1; t0.iNormals[2]  = 2;
	t0.iCoords[0]   = 0; t0.iCoords[1]   = 1; t0.iCoords[2]   = 2;
	t1.iVertices[0] = 0; t1.iVertices[1] = 2; t1.iVertices[2] = 3;
	t1.iNormals[0]  = 0; t1.iNormals[1]  = 2; t1.iNormals[2]  = 3;
	t1.iCoords[0]   = 0; t1.iCoords[1]   = 2; t1.iCoords[2]   = 3;
	tris.push_back( t0 );
	tris.push_back( t1 );

	Implementation::TriangleMeshGeometryIndexed* pMesh =
		new Implementation::TriangleMeshGeometryIndexed( true, false );
	pMesh->BeginIndexedTriangles();
	pMesh->AddVertices( verts );
	pMesh->AddNormals( norms );
	pMesh->AddTexCoords( coords );
	pMesh->AddIndexedTriangles( tris );
	pMesh->DoneIndexedTriangles();
	return pMesh;
}

// P2-2: regression coverage for the NEARZERO (1e-12) singular-transform
// guards added alongside the dndu/dndv quotient-rule transform (3f495c25)
// -- previously untested at either Object.cpp site.  A single-axis stretch
// of s >= 1e13 makes ||M^-T n|| = 1/s <= 1e-12 for a normal EXACTLY aligned
// with the stretched axis (M^-T is diagonal for an axis-aligned stretch, so
// M^-T n just divides that one component by s).
static void TestSingularNormalTransformGuard()
{
	std::cout << "Testing NEARZERO singular-transform guards (P2-2)..." << std::endl;

	// ---- Part A: Object::IntersectRay's derivatives block (mesh path) ----
	// Before P1-5 the guard's OWN behaviour was correctly conservative here
	// (it always marked derivatives.valid = false); P1-5 only corrected a
	// false comment about *why*.  This still deserves first-time coverage
	// since nothing previously exercised the guard at all.
	{
		Implementation::TriangleMeshGeometryIndexed* mesh = BuildFlatQuadMeshNormalZ();
		REQUIRE( mesh != 0, "singular-guard: flat quad mesh built" );
		Implementation::Object* o = new Implementation::Object( mesh );
		mesh->release();
		o->SetStretch( Vector3( 1.0, 1.0, 1.0e13 ) );   // huge stretch on Z
		o->FinalizeTransformations();

		// Local ray straight down the (huge-scaled) Z axis onto the quad --
		// mirrors HitMeshDerivatives's technique (promote a LOCAL-frame ray
		// through the object's own final transform) but inlined here so we
		// can distinguish "missed" from "hit with invalidated derivatives"
		// (HitMeshDerivatives's single bool return conflates the two).
		const Matrix4 mxFinal = o->GetFinalTransformMatrix();
		const Point3 localOrigin( 0.1, 0.1, 5.0 );
		const Vector3 localDir( 0, 0, -1 );
		const Point3 worldOrigin = Point3Ops::Transform( mxFinal, localOrigin );
		const Vector3 worldDir = Vector3Ops::Transform( mxFinal, localDir );

		RayIntersection ri( Ray( worldOrigin, worldDir ), nullRasterizerState );
		o->IntersectRay( ri, RISE_INFINITY, true, true, false );
		REQUIRE( ri.geometric.bHit, "singular-guard: (control) huge-Z-stretch quad hit" );
		if( ri.geometric.bHit ) {
			REQUIRE( !ri.geometric.derivatives.valid,
				"singular-guard: IntersectRay invalidates derivatives when ||M^-T n|| <= NEARZERO" );
		}
		o->release();
	}

	// ---- Part B: Object::ComputeAnalyticalDerivatives (P2-1) ----
	// EllipsoidGeometry's pole (uv.y = 0, i.e. phi = 0) has an EXACT
	// object-space normal of (0,1,0) -- see EllipsoidGeometry.cpp's
	// pos = (-a*sin(phi)*cos(theta), b*cos(phi), c*sin(phi)*sin(theta))
	// convention: phi=0 gives pos=(0,b,0), gradient-normal (0, 1/b, 0),
	// i.e. the pole sits on the Y axis, not Z.  Stretch Y (not Z) so the
	// pole normal is the one that collapses under ||M^-T n||.
	{
		EllipsoidGeometry* g = new EllipsoidGeometry( Vector3( 1.5, 1.5, 1.5 ) );
		Implementation::Object* o = new Implementation::Object( g );
		g->release();
		o->SetStretch( Vector3( 1.0, 1.0e13, 1.0 ) );   // huge stretch on Y
		o->FinalizeTransformations();

		Point3 pos; Vector3 n, dpdu, dpdv, dndu, dndv;
		const bool ok = o->ComputeAnalyticalDerivatives(
			Point2( 0.0, 0.0 ), 0.0, pos, n, dpdu, dpdv, dndu, dndv );
		REQUIRE( !ok,
			"singular-guard: ComputeAnalyticalDerivatives returns false when ||M^-T n|| <= NEARZERO (P2-1: was a fabricated true)" );
		o->release();
	}

	std::cout << "  singular-transform guard checks done\n";
}

int main()
{
	std::cout << "=== Geometry (u, v) parameterisation regression test ===\n";

	TestObjectWorldArea();
	TestObjectWorldDerivatives();
	TestSingularNormalTransformGuard();
	TestSphere();
	TestEllipsoid();
	TestBox();
	TestCylinder();
	TestTorus();
	TestDisk();
	TestClippedPlane();
	TestBilinearPatch();
	TestBilinearPatchAreaLightContract();
	TestBilinearInverse();
	TestBilinearEliminationAxis();

	if( g_failures > 0 ) {
		std::cout << "\nFAILED with " << g_failures << " failed assertions.\n";
		return 1;
	}
	std::cout << "\nAll geometry UV roundtrip / coverage tests passed!\n";
	return 0;
}
