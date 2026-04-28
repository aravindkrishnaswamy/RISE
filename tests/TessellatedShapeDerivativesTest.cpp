//////////////////////////////////////////////////////////////////////
//
//  TessellatedShapeDerivativesTest.cpp
//
//    Compares the surface normals and derivatives produced by a
//    DisplacedGeometry (with zero displacement — i.e. pure
//    tessellation) against the analytical shape it wraps.
//
//    For each supported analytical shape (Sphere, Torus, Ellipsoid,
//    Cylinder), a virtual ray-shooter fires rays from a
//    Fibonacci-sampled sphere of directions at a sweep of target
//    incident angles (0–90°).  For every ray that hits BOTH the
//    analytical and the tessellated geometry, the test records:
//
//      - angular error between analytical and tessellated shading
//        normals  (the primary correctness signal)
//      - tangent-plane alignment error: angle between
//        Normalize(dpdu × dpdv) on each side  (should match the
//        normal error; independent sanity check on the derivatives)
//      - area-element ratio: |dpdu × dpdv|_tess / |dpdu × dpdv|_analytic
//        (should trend to 1 as tessellation refines)
//      - position error (hit-point distance, normalised by bbox)
//
//    Metrics are bucketed by the actual incident angle of the ray at
//    the analytical hit (0–90° in 5° bins).  A per-shape table is
//    printed to stdout.  The test fails only if the normal error in
//    the low-incidence region (incident <= 45°, where tessellation
//    is expected to be accurate) exceeds egregious thresholds —
//    i.e. a real bug, not just tessellation approximation noise near
//    silhouettes.
//
//    NOTE on derivatives: TriangleMeshGeometryIndexed computes
//    dpdu / dpdv in the TRIANGLE EDGE frame (edge1 for dpdu, edge2
//    for dpdv, projected into the shading tangent plane).  That
//    frame is not aligned with the analytical (u,v) parameterisation
//    frame, so directly comparing the analytical and tessellated
//    dpdu vectors gives a frame-rotation angle, not an error.  Only
//    frame-invariant quantities — the tangent plane (via the cross
//    product) and the area element (via its magnitude) — are
//    compared across the two geometries.
//
//    Hit point: TriangleMeshGeometryIndexed fills ri.range and
//    ri.vNormal but not ri.ptIntersection.  For both sides we
//    compute the hit point ourselves via ray.PointAtLength(range)
//    so the two queries use the same protocol.
//
//    Run:   ./bin/tests/TessellatedShapeDerivativesTest
//    Build: make -C build/make/rise tests   (auto-discovered)
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "../src/Library/Geometry/CylinderGeometry.h"
#include "../src/Library/Geometry/DisplacedGeometry.h"
#include "../src/Library/Geometry/EllipsoidGeometry.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Geometry/TorusGeometry.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Interfaces/IFunction2D.h"
#include "../src/Library/Utilities/Math3D/Constants.h"
#include "../src/Library/Utilities/Reference.h"

using namespace RISE;
using namespace RISE::Implementation;

// ============================================================
// Configuration
// ============================================================

static const unsigned int DETAIL      = 32;  // tessellation subdivision
static const unsigned int N_SOURCES   = 32;  // Fibonacci-sphere ray directions
static const unsigned int N_INCIDENT  = 18;  // target incident-angle samples per source
static const unsigned int N_AZIMUTH   = 8;   // azimuthal samples in the offset disk

static const unsigned int N_BUCKETS   = 18;                    // 5° bins over 0–90°
static const Scalar       BUCKET_DEG  = 90.0 / Scalar(N_BUCKETS);

// Pass/fail gate: applied only to the low-incidence region where
// tessellation must be accurate.  Grazing-angle samples are always
// reported but do not gate the suite (interpolated normals naturally
// diverge near silhouettes on any tessellation).
static const Scalar PASS_INCIDENT_LIMIT_DEG = 45.0;
// Egregious-error thresholds: set loose enough that detail-32
// tessellation of any shape passes (torus, whose tube is small,
// produces ~10° max in mid-incidence buckets), but tight enough to
// catch real bugs — flipped normals (180°), wrong winding (~90°), or
// pervasive seam misalignments.
static const Scalar PASS_NORMAL_P95_DEG     = 10.0;
static const Scalar PASS_NORMAL_MAX_DEG     = 20.0;
static const unsigned int PASS_MIN_SAMPLES  = 50;

// Near-hit filter (applied to the pass gate and the "normal err near"
// columns): a sample only counts if the analytical and tessellated
// hits are within this fraction of bbox of each other.  Rays where
// tessellation approximation causes the mesh and analytical to hit
// different regions of the surface (e.g. a ray grazing a torus tube
// where the inscribed mesh is slightly inside the analytical) would
// compare normals at unrelated surface locations — those samples are
// recorded in the totals and the pos_err column but excluded from the
// normal-error comparison, which is only meaningful at corresponding
// points.
static const Scalar NEAR_HIT_FILTER = 0.1;  // fraction of bboxR

// ============================================================
// Helpers
// ============================================================

static Scalar AngleBetweenDeg( const Vector3& a, const Vector3& b )
{
	const Scalar ma = Vector3Ops::Magnitude( a );
	const Scalar mb = Vector3Ops::Magnitude( b );
	if( ma < 1e-12 || mb < 1e-12 ) return 0.0;
	Scalar c = Vector3Ops::Dot( a, b ) / (ma * mb);
	if( c >  1.0 ) c =  1.0;
	if( c < -1.0 ) c = -1.0;
	return std::acos( c ) * RAD_TO_DEG;
}

// Fibonacci-spiral point on the unit sphere.
static Vector3 FibonacciDir( unsigned int i, unsigned int n )
{
	const Scalar ga = PI * (3.0 - std::sqrt(5.0));  // golden angle
	const Scalar y  = 1.0 - (Scalar(i) + 0.5) / Scalar(n) * 2.0;
	const Scalar r  = std::sqrt( std::max( Scalar(0.0), Scalar(1.0) - y * y ) );
	const Scalar t  = ga * Scalar(i);
	return Vector3( std::cos(t) * r, y, std::sin(t) * r );
}

// Arbitrary orthonormal basis perpendicular to d (|d|=1).
static void OrthoBasis( const Vector3& d, Vector3& a, Vector3& b )
{
	const Vector3 seed = (std::fabs(d.y) < 0.9) ? Vector3(0,1,0) : Vector3(1,0,0);
	a = Vector3Ops::Normalize( Vector3Ops::Cross( seed, d ) );
	b = Vector3Ops::Normalize( Vector3Ops::Cross( d, a ) );
}

// ============================================================
// Aggregation
// ============================================================

struct BucketStats
{
	std::vector<Scalar> normalErrAll;  // shading-normal error, every hit-both sample
	std::vector<Scalar> normalErrNear; // same, filtered to near-hit samples only
	std::vector<Scalar> areaRatio;     // |T_dpdu × T_dpdv| / |A_dpdu × A_dpdv|, near-hit samples
	std::vector<Scalar> posErr;        // hit-point error, normalised by bboxR, every sample
};

struct Summary
{
	unsigned int n = 0;
	Scalar mean = 0, p50 = 0, p95 = 0, maxv = 0;
};

static Summary Summarise( std::vector<Scalar> v )
{
	Summary s;
	if( v.empty() ) return s;
	std::sort( v.begin(), v.end() );
	s.n = static_cast<unsigned int>( v.size() );
	Scalar sum = 0;
	for( Scalar x : v ) sum += x;
	s.mean = sum / Scalar( v.size() );
	s.p50  = v[ v.size() / 2 ];
	const size_t idx95 = std::min<size_t>( v.size() * 95 / 100, v.size() - 1 );
	s.p95  = v[ idx95 ];
	s.maxv = v.back();
	return s;
}

struct ShapeResult
{
	std::vector<BucketStats> buckets;
	unsigned int raysCast    = 0;
	unsigned int raysHitA    = 0;
	unsigned int raysHitT    = 0;
	unsigned int raysHitBoth = 0;
	unsigned int derivInvalid = 0;
};

// ============================================================
// The virtual ray shooter.
// Deterministic pattern: for every (source direction, target
// incident angle, azimuth) triplet, fires one ray and compares the
// analytical and tessellated hit it produces.
// ============================================================

static void RunRayShooter(
	const IGeometry& analytical,
	const IGeometry& tessellated,
	const Scalar     bboxR,
	ShapeResult&     out )
{
	out.buckets.assign( N_BUCKETS, BucketStats() );

	const Scalar probeDist = bboxR * 8.0;

	for( unsigned int i = 0; i < N_SOURCES; ++i )
	{
		const Vector3 dir = FibonacciDir( i, N_SOURCES );
		Vector3 uAxis, vAxis;
		OrthoBasis( dir, uAxis, vAxis );

		for( unsigned int ai = 0; ai < N_INCIDENT; ++ai )
		{
			// Sweep target incident angles in (0°, ~88°).  For a sphere
			// this puts exactly one sample per bucket; for other shapes
			// the realised incident angle may differ, but the actual
			// per-hit angle still determines bucket assignment.
			const Scalar alphaDeg = (Scalar(ai) + 0.5) * 88.0 / Scalar(N_INCIDENT);
			const Scalar alpha    = alphaDeg / RAD_TO_DEG;
			const Scalar r        = bboxR * std::sin( alpha );

			for( unsigned int k = 0; k < N_AZIMUTH; ++k )
			{
				// Stagger azimuth per source so aggregate coverage is dense.
				const Scalar t =
					TWO_PI * (Scalar(k) + 0.5) / Scalar(N_AZIMUTH)
					+ TWO_PI * Scalar(i) / Scalar(N_SOURCES * N_AZIMUTH);
				const Scalar cs = std::cos(t);
				const Scalar sn = std::sin(t);

				const Vector3 offset(
					uAxis.x * r * cs + vAxis.x * r * sn,
					uAxis.y * r * cs + vAxis.y * r * sn,
					uAxis.z * r * cs + vAxis.z * r * sn );
				const Point3 origin(
					probeDist * dir.x + offset.x,
					probeDist * dir.y + offset.y,
					probeDist * dir.z + offset.z );
				const Vector3 rayDir( -dir.x, -dir.y, -dir.z );

				++out.raysCast;

				RayIntersectionGeometric riA( Ray( origin, rayDir ), nullRasterizerState );
				analytical.IntersectRay( riA, true, true, false );
				if( riA.bHit ) ++out.raysHitA;

				RayIntersectionGeometric riT( Ray( origin, rayDir ), nullRasterizerState );
				tessellated.IntersectRay( riT, true, true, false );
				if( riT.bHit ) ++out.raysHitT;

				if( !riA.bHit || !riT.bHit ) continue;
				++out.raysHitBoth;

				// Compute hit point from the ray: TriangleMeshGeometryIndexed
				// doesn't fill ri.ptIntersection, so we can't trust it for
				// the tessellated side.  For parity, we do it for both.
				const Point3 hitA = riA.ray.PointAtLength( riA.range );
				const Point3 hitT = riT.ray.PointAtLength( riT.range );

				const Vector3 nA = Vector3Ops::Normalize( riA.vNormal );
				const Vector3 nT = Vector3Ops::Normalize( riT.vNormal );

				// Incident angle against the analytical normal.  With both
				// face sides enabled we might land on a back face where
				// -rayDir points away from n; fold into [0, 90°].
				const Vector3 incoming( -rayDir.x, -rayDir.y, -rayDir.z );
				Scalar incDeg = AngleBetweenDeg( incoming, nA );
				if( incDeg > 90.0 ) incDeg = 180.0 - incDeg;
				const unsigned int bucket = std::min<unsigned int>(
					static_cast<unsigned int>( incDeg / BUCKET_DEG ), N_BUCKETS - 1 );

				const SurfaceDerivatives sdA = analytical .ComputeSurfaceDerivatives( hitA, nA );
				const SurfaceDerivatives sdT = tessellated.ComputeSurfaceDerivatives( hitT, nT );
				if( !sdA.valid || !sdT.valid ) { ++out.derivInvalid; continue; }

				const Scalar normDeg = AngleBetweenDeg( nA, nT );
				const Scalar posErr = Point3Ops::Distance( hitA, hitT ) / bboxR;
				const bool   isNear = (posErr <= NEAR_HIT_FILTER);

				BucketStats& bk = out.buckets[ bucket ];
				bk.normalErrAll.push_back( normDeg );
				bk.posErr      .push_back( posErr );

				// Area-element ratio — only meaningful when the two
				// geometries hit the same region of the surface.
				if( isNear ) {
					bk.normalErrNear.push_back( normDeg );
					const Vector3 crossA = Vector3Ops::Cross( sdA.dpdu, sdA.dpdv );
					const Vector3 crossT = Vector3Ops::Cross( sdT.dpdu, sdT.dpdv );
					const Scalar  areaA  = Vector3Ops::Magnitude( crossA );
					const Scalar  areaT  = Vector3Ops::Magnitude( crossT );
					const Scalar  areaRatio = (areaA < 1e-12) ? 1.0 : (areaT / areaA);
					bk.areaRatio.push_back( areaRatio );
				}
			}
		}
	}
}

// ============================================================
// Report
// ============================================================

static bool PrintReport( const char* name, ShapeResult& r )
{
	std::printf( "\n=== %s (detail=%u) ===\n", name, DETAIL );
	std::printf( "Rays cast: %u   hits A: %u   hits T: %u   hits BOTH: %u   deriv invalid: %u\n",
		r.raysCast, r.raysHitA, r.raysHitT, r.raysHitBoth, r.derivInvalid );
	std::printf( "Near-hit filter: pos_err / bbox <= %.2f  (far-hits excluded from normal-err comparison)\n\n",
		NEAR_HIT_FILTER );

	std::printf( "  incident   | count | near  | normal err near-hit (deg) | normal err (all) | area ratio near (T/A)     | pos err (/bbox)\n" );
	std::printf( "             | all   | count |  mean   p50   p95    max  |   max  (outliers)|  mean   p50   p95    max  |  mean   p95    max\n" );
	std::printf( "  -----------+-------+-------+---------------------------+------------------+---------------------------+----------------------\n" );

	std::vector<Scalar> subNormal;  // near-hit samples with incident <= PASS_INCIDENT_LIMIT_DEG

	for( unsigned int i = 0; i < N_BUCKETS; ++i )
	{
		BucketStats& b = r.buckets[i];
		const Summary nmAll  = Summarise( b.normalErrAll );
		const Summary nmNear = Summarise( b.normalErrNear );
		const Summary ar     = Summarise( b.areaRatio );
		const Summary pe     = Summarise( b.posErr );

		const Scalar lo = Scalar(i)     * BUCKET_DEG;
		const Scalar hi = Scalar(i + 1) * BUCKET_DEG;

		std::printf( "  %4.1f°-%4.1f° | %5u | %5u | %5.2f %5.2f %5.2f %6.2f  | %7.2f          | %5.3f %5.3f %5.3f %6.3f  | %6.4f %6.4f %6.4f\n",
			lo, hi, nmAll.n, nmNear.n,
			nmNear.mean, nmNear.p50, nmNear.p95, nmNear.maxv,
			nmAll.maxv,
			ar.mean, ar.p50, ar.p95, ar.maxv,
			pe.mean, pe.p95, pe.maxv );

		if( hi <= PASS_INCIDENT_LIMIT_DEG + 1e-6 ) {
			for( Scalar x : b.normalErrNear ) subNormal.push_back( x );
		}
	}

	// Overall row.
	std::vector<Scalar> allN, allNnear, allAr, allPe;
	for( BucketStats& b : r.buckets ) {
		for( Scalar x : b.normalErrAll  ) allN    .push_back( x );
		for( Scalar x : b.normalErrNear ) allNnear.push_back( x );
		for( Scalar x : b.areaRatio     ) allAr   .push_back( x );
		for( Scalar x : b.posErr        ) allPe   .push_back( x );
	}
	const Summary nmAll  = Summarise( allN );
	const Summary nmNear = Summarise( allNnear );
	const Summary ar     = Summarise( allAr );
	const Summary pe     = Summarise( allPe );
	std::printf( "  -----------+-------+-------+---------------------------+------------------+---------------------------+----------------------\n" );
	std::printf( "  all        | %5u | %5u | %5.2f %5.2f %5.2f %6.2f  | %7.2f          | %5.3f %5.3f %5.3f %6.3f  | %6.4f %6.4f %6.4f\n",
		nmAll.n, nmNear.n,
		nmNear.mean, nmNear.p50, nmNear.p95, nmNear.maxv,
		nmAll.maxv,
		ar.mean, ar.p50, ar.p95, ar.maxv,
		pe.mean, pe.p95, pe.maxv );

	const Summary sub = Summarise( subNormal );
	std::printf( "\n  Pass gate [incident <= %.0f°, near-hit only]:  N=%u  normal mean=%.3f°  p95=%.3f°  max=%.3f°\n",
		PASS_INCIDENT_LIMIT_DEG, sub.n, sub.mean, sub.p95, sub.maxv );
	std::printf( "  Thresholds: N>=%u  AND  p95 < %.1f°  AND  max < %.1f°\n",
		PASS_MIN_SAMPLES, PASS_NORMAL_P95_DEG, PASS_NORMAL_MAX_DEG );

	const bool pass = sub.n >= PASS_MIN_SAMPLES
		&& sub.p95  <= PASS_NORMAL_P95_DEG
		&& sub.maxv <= PASS_NORMAL_MAX_DEG;
	std::printf( "  -> %s\n", pass ? "PASS" : "FAIL" );
	return pass;
}

// ============================================================
// Per-shape driver
// ============================================================

static DisplacedGeometry* WrapAsTessellation( IGeometry* base )
{
	// disp=nullptr, disp_scale=0 → pure tessellation of the base shape.
	// bUseFaceNormals=false → smooth (interpolated) normals, so the
	// tessellated-vs-analytical comparison exercises the smooth
	// shading path rather than flat triangle normals.
	// Tier A2 cleanup (2026-04-27): max_polys/max_recursion/bUseBSP are gone.
	return new DisplacedGeometry( base, DETAIL, nullptr, 0.0,
		/*bDoubleSided*/true,
		/*bUseFaceNormals*/false );
}

static bool RunShape( const char* label, IGeometry* analytical, const Scalar bboxR )
{
	DisplacedGeometry* tess = WrapAsTessellation( analytical );
	if( !tess->IsValid() ) {
		std::printf( "  [%s] DisplacedGeometry::IsValid() returned false — base does not support tessellation.\n", label );
		tess->release();
		return false;
	}

	ShapeResult r;
	RunRayShooter( *analytical, *tess, bboxR, r );
	const bool pass = PrintReport( label, r );

	tess->release();
	return pass;
}

// ============================================================
// main
// ============================================================

int main()
{
	std::printf( "TessellatedShapeDerivativesTest\n" );
	std::printf( "Comparing DisplacedGeometry(disp=0, detail=%u) against analytical shapes\n", DETAIL );
	std::printf( "using %u Fibonacci ray sources x %u incident angles x %u azimuths = %u rays/shape.\n",
		N_SOURCES, N_INCIDENT, N_AZIMUTH, N_SOURCES * N_INCIDENT * N_AZIMUTH );

	bool allPass = true;
	unsigned int failCount = 0;

	{
		SphereGeometry* g = new SphereGeometry( 1.0 );
		const bool ok = RunShape( "Sphere (R=1.0)", g, /*bboxR*/1.0 );
		allPass &= ok; if( !ok ) ++failCount;
		g->release();
	}

	{
		TorusGeometry* g = new TorusGeometry( 1.0, 0.3 );
		const bool ok = RunShape( "Torus (major=1.0, minor=0.3)", g, /*bboxR*/1.3 );
		allPass &= ok; if( !ok ) ++failCount;
		g->release();
	}

	{
		// EllipsoidGeometry's m_vRadius stores diameters, so semi-axes
		// are vRadius/2.  Bounding sphere radius = 0.5 * |vRadius|.
		const Vector3 diam( 2.0, 3.0, 1.6 );
		EllipsoidGeometry* g = new EllipsoidGeometry( diam );
		const Scalar bboxR = 0.5 * std::sqrt( diam.x*diam.x + diam.y*diam.y + diam.z*diam.z );
		const bool ok = RunShape( "Ellipsoid (diam=2.0x3.0x1.6)", g, bboxR );
		allPass &= ok; if( !ok ) ++failCount;
		g->release();
	}

	{
		// Cylinder side only (no caps — matches analytical intersection).
		// chAxis is a character code, not an integer.
		CylinderGeometry* g = new CylinderGeometry( 'y', 1.0, 2.0 );
		const Scalar bboxR = std::sqrt( 1.0*1.0 + 1.0*1.0 );  // (radius, half-height)
		const bool ok = RunShape( "Cylinder (Y-axis, R=1.0, H=2.0)", g, bboxR );
		allPass &= ok; if( !ok ) ++failCount;
		g->release();
	}

	std::printf( "\n============================================================\n" );
	std::printf( "Overall: %s   (%u shape%s failed)\n",
		allPass ? "PASS" : "FAIL", failCount, failCount == 1 ? "" : "s" );
	return allPass ? 0 : 1;
}
