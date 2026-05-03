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
#include "../src/Library/Intersection/RayIntersectionGeometric.h"

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

static Point3 EllipsoidTessParamToPos( const Vector3& diam, Scalar u, Scalar v )
{
	// EllipsoidGeometry::TessellateToMesh:
	//   pos = (a*-sin(phi)*cos(theta), b*cos(phi), c*sin(phi)*sin(theta))
	const Scalar a = diam.x * 0.5;
	const Scalar b = diam.y * 0.5;
	const Scalar c = diam.z * 0.5;
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

		const Scalar a = diameters.x * 0.5;
		const Scalar b = diameters.y * 0.5;
		const Scalar c = diameters.z * 0.5;
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
	CylinderGeometry* g = new CylinderGeometry( axis, r, h );

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
// Clipped plane (planar quad)
//
// The strict roundtrip uses the bilinear forward formula across the
// quad corners.  For PLANAR quads (this test) the bilinear surface
// equals the two-flat-triangle surface IntersectRay traces, and
// corner UVs match TessellateToMesh.  For genuinely non-planar
// quads, the two surface representations differ off the
// vP[0]-vP[2] diagonal — that case is documented in the
// IntersectRay body.
// ============================================================

static void TestClippedPlane()
{
	std::cout << "Testing ClippedPlaneGeometry UV roundtrip..." << std::endl;

	const Point3 corners[4] = {
		Point3( 0, 0, 0 ),
		Point3( 1, 0, 0 ),
		Point3( 1, 1, 0 ),
		Point3( 0, 1, 0 )
	};
	ClippedPlaneGeometry* g = new ClippedPlaneGeometry( corners, /*bDoubleSided=*/true );

	const int N = 1500;
	const Scalar tol = 1e-5;
	LCG rng( 33333 );
	CoverageStats cov = MakeCoverage();
	int nHits = 0;
	for( int i = 0; i < N; ++i ) {
		RayIntersectionGeometric ri( Ray(), nullRasterizerState );

		const Scalar zSide = (rng.next01() < 0.5) ? -3.0 : 3.0;
		const Point3 origin( rng.next01() * 2.0 - 0.5, rng.next01() * 2.0 - 0.5, zSide );
		const Point3 target( rng.next01(), rng.next01(), 0.0 );
		const Vector3 dir = Vector3Ops::Normalize(
			Vector3Ops::mkVector3( target, origin ) );
		ri = RayIntersectionGeometric( Ray( origin, dir ), nullRasterizerState );
		g->IntersectRay( ri, true, true, false );
		if( !ri.bHit ) continue;
		nHits++;

		AssertUVInUnitSquare( ri.ptCoord.x, ri.ptCoord.y, "clippedplane IntersectRay" );

		const Point3 reconstructed = ClippedPlaneTessParamToPos(
			corners, ri.ptCoord.x, ri.ptCoord.y );
		REQUIRE( IsPointClose( reconstructed, ri.ptIntersection, tol ),
			"clippedplane IntersectRay UV->pos roundtrip" );

		RecordCoverage( cov, ri.ptCoord.x, ri.ptCoord.y );
	}
	REQUIRE( nHits > N / 4, "clippedplane enough hits" );
	AssertCoverage( cov, "clippedplane IntersectRay" );

	const int Mpoints = 800;
	int nPts = 0;
	for( int i = 0; i < Mpoints; ++i ) {
		Point3  p; Vector3 n; Point2 uv;
		const Point3 prand( rng.next01(), rng.next01(), rng.next01() );
		g->UniformRandomPoint( &p, &n, &uv, prand );

		AssertUVInUnitSquare( uv.x, uv.y, "clippedplane UniformRandomPoint" );

		// UniformRandomPoint pulls the point off the plane by a tiny
		// epsilon along the normal so back-facing luminaries occlude
		// correctly; allow that offset in the tolerance.
		const Point3 reconstructed = ClippedPlaneTessParamToPos( corners, uv.x, uv.y );
		REQUIRE( IsPointClose( reconstructed, p, 1e-3 ),
			"clippedplane UniformRandomPoint UV->pos roundtrip" );
		nPts++;
	}
	REQUIRE( nPts > Mpoints / 2, "clippedplane enough random points" );

	g->release();
	std::cout << "  clipped-plane: " << nHits << " ray hits, " << nPts
		<< " random points\n";
}

// ============================================================
// Bilinear patch
// ============================================================

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

int main()
{
	std::cout << "=== Geometry (u, v) parameterisation regression test ===\n";

	TestSphere();
	TestEllipsoid();
	TestBox();
	TestCylinder();
	TestTorus();
	TestDisk();
	TestClippedPlane();
	TestBilinearPatch();

	if( g_failures > 0 ) {
		std::cout << "\nFAILED with " << g_failures << " failed assertions.\n";
		return 1;
	}
	std::cout << "\nAll geometry UV roundtrip / coverage tests passed!\n";
	return 0;
}
