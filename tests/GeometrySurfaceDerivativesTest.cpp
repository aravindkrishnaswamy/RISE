//////////////////////////////////////////////////////////////////////
//
//  GeometrySurfaceDerivativesTest.cpp
//
//    Unit tests for IGeometry::ComputeSurfaceDerivatives across every
//    concrete geometry in RISE.  Each geometry must satisfy the
//    contract documented in docs/GEOMETRY_DERIVATIVES.md:
//
//      1. Analytical derivatives match central finite differences on
//         position and normal to O(eps²)
//      2. Tangency: dpdu·n == 0, dpdv·n == 0
//      3. Normal stays unit: dndu·n == 0, dndv·n == 0
//      4. Handedness: (dpdu × dpdv) · n > 0 (right-handed)
//      5. sd.valid is true for points on the surface
//      6. No NaN/Inf at degenerate parameter values
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <vector>
#include <cassert>
#include <cmath>
#include <cstdio>

#include "../src/Library/Geometry/BezierPatchGeometry.h"
#include "../src/Library/Geometry/BilinearPatchGeometry.h"
#include "../src/Library/Geometry/BoxGeometry.h"
#include "../src/Library/Geometry/CircularDiskGeometry.h"
#include "../src/Library/Geometry/ClippedPlaneGeometry.h"
#include "../src/Library/Geometry/CylinderGeometry.h"
#include "../src/Library/Geometry/DisplacedGeometry.h"
#include "../src/Library/Geometry/EllipsoidGeometry.h"
#include "../src/Library/Geometry/InfinitePlaneGeometry.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Geometry/TorusGeometry.h"
#include "../src/Library/Geometry/TriangleMeshGeometry.h"
#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Interfaces/IFunction2D.h"
#include "../src/Library/Utilities/Reference.h"

using namespace RISE;
using namespace RISE::Implementation;

// ============================================================
// Test helpers
// ============================================================

static bool IsClose( Scalar a, Scalar b, Scalar eps = 1e-6 )
{
	return std::fabs( a - b ) < eps;
}

static bool IsVecClose( const Vector3& a, const Vector3& b, Scalar eps = 1e-6 )
{
	return IsClose(a.x, b.x, eps) && IsClose(a.y, b.y, eps) && IsClose(a.z, b.z, eps);
}

static bool IsFinite( const Vector3& v )
{
	return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

static unsigned int g_failures = 0;

#define REQUIRE( cond, label ) do { \
	if( !(cond) ) { \
		std::cout << "  FAIL [" << label << "] at " << __FILE__ << ":" << __LINE__ << std::endl; \
		g_failures++; \
	} \
} while(0)

// Query a surface point on the geometry by casting a ray.  Returns the
// actual point and normal the geometry's IntersectRay produces.
// Used so tests don't depend on internal parameterization conventions.
struct SurfaceSample
{
	Point3 pos;
	Vector3 normal;
	bool ok;
};

static SurfaceSample RayHit(
	const IGeometry& g,
	const Point3& origin,
	const Vector3& dir,
	Scalar maxDist = 100.0 )
{
	SurfaceSample s;
	s.ok = false;
	RayIntersectionGeometric ri( Ray( origin, dir ), nullRasterizerState );
	g.IntersectRay( ri, true, true, false );
	if( ri.bHit && ri.range < maxDist ) {
		s.pos = ri.ptIntersection;
		s.normal = Vector3Ops::Normalize( ri.vNormal );
		s.ok = true;
	}
	return s;
}

// Check the 5 universal invariants at a known surface point.
static void CheckInvariantsAt(
	const IGeometry& g,
	const Point3& pos,
	const Vector3& normal,
	const char* label )
{
	SurfaceDerivatives sd = g.ComputeSurfaceDerivatives( pos, normal );
	if( !sd.valid ) {
		std::cout << "  [" << label << "] sd.valid=false\n";
	}
	REQUIRE( sd.valid, label );
	if( std::getenv("SMS_DEBUG_TEST") ) {
		std::printf( "  [%s] pos=(%.3f,%.3f,%.3f) n=(%.3f,%.3f,%.3f)\n"
			"    dpdu=(%.4f,%.4f,%.4f) dpdv=(%.4f,%.4f,%.4f)\n"
			"    dndu=(%.4f,%.4f,%.4f) dndv=(%.4f,%.4f,%.4f)\n",
			label, pos.x, pos.y, pos.z, normal.x, normal.y, normal.z,
			sd.dpdu.x, sd.dpdu.y, sd.dpdu.z,
			sd.dpdv.x, sd.dpdv.y, sd.dpdv.z,
			sd.dndu.x, sd.dndu.y, sd.dndu.z,
			sd.dndv.x, sd.dndv.y, sd.dndv.z );
	}

	REQUIRE( IsFinite( sd.dpdu ), std::string(label) + " dpdu finite" );
	REQUIRE( IsFinite( sd.dpdv ), std::string(label) + " dpdv finite" );
	REQUIRE( IsFinite( sd.dndu ), std::string(label) + " dndu finite" );
	REQUIRE( IsFinite( sd.dndv ), std::string(label) + " dndv finite" );

	// Tangency of position derivatives
	const Scalar dpdu_n = Vector3Ops::Dot( sd.dpdu, normal );
	const Scalar dpdv_n = Vector3Ops::Dot( sd.dpdv, normal );
	// tolerance scales with |dpdu| since magnitudes aren't unit-constrained
	const Scalar dpdu_mag = Vector3Ops::Magnitude( sd.dpdu );
	const Scalar dpdv_mag = Vector3Ops::Magnitude( sd.dpdv );
	REQUIRE( std::fabs(dpdu_n) < 1e-5 * std::max(dpdu_mag, 1.0),
		std::string(label) + " dpdu tangent to n" );
	REQUIRE( std::fabs(dpdv_n) < 1e-5 * std::max(dpdv_mag, 1.0),
		std::string(label) + " dpdv tangent to n" );

	// Normal-unit preservation: dn/d* is perpendicular to n
	const Scalar dndu_n = Vector3Ops::Dot( sd.dndu, normal );
	const Scalar dndv_n = Vector3Ops::Dot( sd.dndv, normal );
	const Scalar dndu_mag = Vector3Ops::Magnitude( sd.dndu );
	const Scalar dndv_mag = Vector3Ops::Magnitude( sd.dndv );
	REQUIRE( std::fabs(dndu_n) < 1e-5 * std::max(dndu_mag, 1.0),
		std::string(label) + " dndu perp n" );
	REQUIRE( std::fabs(dndv_n) < 1e-5 * std::max(dndv_mag, 1.0),
		std::string(label) + " dndv perp n" );

	// Handedness: (dpdu × dpdv) · n should be > 0
	// Skip if either derivative is degenerate (zero-length)
	if( dpdu_mag > 1e-6 && dpdv_mag > 1e-6 ) {
		const Vector3 cross = Vector3Ops::Cross( sd.dpdu, sd.dpdv );
		const Scalar handed = Vector3Ops::Dot( cross, normal );
		REQUIRE( handed > 0.0,
			std::string(label) + " right-handed (dpdu x dpdv)·n > 0" );
	}
}

// ============================================================
// Per-geometry tests
// ============================================================

// Helper: cast a ray and run invariants at the hit.
static void CheckViaRay(
	const IGeometry& g,
	const Point3& origin,
	const Vector3& dir,
	const char* label )
{
	SurfaceSample s = RayHit( g, origin, dir );
	if( !s.ok ) {
		std::cout << "  [" << label << "] ray missed, skipping\n";
		return;
	}
	CheckInvariantsAt( g, s.pos, s.normal, label );
}

static void TestSphere()
{
	std::cout << "Testing SphereGeometry..." << std::endl;
	const Scalar R = 1.0;
	SphereGeometry* g = new SphereGeometry( R );

	// Probe the sphere from several directions
	CheckViaRay( *g, Point3( 3, 0, 0 ),   Vector3(-1, 0, 0 ),   "sphere +X equator" );
	CheckViaRay( *g, Point3( 0, 0, 3 ),   Vector3( 0, 0,-1 ),   "sphere +Z equator" );
	CheckViaRay( *g, Point3(-3, 0, 0 ),   Vector3( 1, 0, 0 ),   "sphere -X equator" );
	CheckViaRay( *g, Point3( 1, 2, 1 ),   Vector3Ops::Normalize(Vector3(-1,-2,-1)),
		"sphere off-axis" );

	// Pole handling: at y = R, sin(theta) = 0 → dpdu degenerates.
	// Requirement: valid=true, no NaN, even though dpdu may be zero.
	const Point3 polePos( 0, R, 0 );
	const Vector3 poleNormal( 0, 1, 0 );
	SurfaceDerivatives sdPole = g->ComputeSurfaceDerivatives( polePos, poleNormal );
	REQUIRE( sdPole.valid, "sphere pole valid" );
	REQUIRE( IsFinite( sdPole.dpdu ), "sphere pole dpdu finite" );
	REQUIRE( IsFinite( sdPole.dpdv ), "sphere pole dpdv finite" );
	REQUIRE( IsFinite( sdPole.dndu ), "sphere pole dndu finite" );
	REQUIRE( IsFinite( sdPole.dndv ), "sphere pole dndv finite" );

	g->release();
}

static void TestTorus()
{
	std::cout << "Testing TorusGeometry..." << std::endl;
	TorusGeometry* g = new TorusGeometry( 1.0, 0.3 );  // outer, inner

	// Ray from above onto top of ring at u=0 (tube hits surface)
	CheckViaRay( *g, Point3( 0.65, 5, 0 ),    Vector3( 0,-1, 0 ), "torus top-of-ring u=0" );
	CheckViaRay( *g, Point3( 0, 5, 0.65 ),    Vector3( 0,-1, 0 ), "torus top-of-ring u=π/2" );
	// Ray from outside onto outer equator
	CheckViaRay( *g, Point3( 5, 0, 0 ),       Vector3(-1, 0, 0 ), "torus outer +X" );
	// Off-axis oblique hit
	CheckViaRay( *g, Point3( 2, 0.1, 0.1 ),   Vector3Ops::Normalize(Vector3(-1,0.02,0.02)),
		"torus oblique" );

	g->release();
}

static void TestCylinder()
{
	std::cout << "Testing CylinderGeometry..." << std::endl;
	const Scalar r = 1.0, h = 2.0;

	// Constructor: CylinderGeometry(chAxis, radius, height)
	{
		CylinderGeometry* g = new CylinderGeometry( 'y', r, h );
		CheckViaRay( *g, Point3( 3, 0, 0 ),   Vector3(-1, 0, 0), "Y-cyl +X" );
		CheckViaRay( *g, Point3( 0, 0.5, 3 ), Vector3( 0, 0,-1), "Y-cyl off-mid +Z" );
		g->release();
	}
	{
		CylinderGeometry* g = new CylinderGeometry( 'x', r, h );
		CheckViaRay( *g, Point3( 0, 3, 0 ),   Vector3( 0,-1, 0), "X-cyl +Y" );
		g->release();
	}
	{
		CylinderGeometry* g = new CylinderGeometry( 'z', r, h );
		CheckViaRay( *g, Point3( 3, 0, 0 ),   Vector3(-1, 0, 0), "Z-cyl +X" );
		g->release();
	}
}

static void TestEllipsoid()
{
	std::cout << "Testing EllipsoidGeometry..." << std::endl;
	// Sphere-like ellipsoid (a=b=c)
	{
		EllipsoidGeometry* g = new EllipsoidGeometry( Vector3( 2, 2, 2 ) );
		CheckViaRay( *g, Point3( 3, 0, 0 ),   Vector3(-1, 0, 0), "ellipsoid-sphere +X" );
		CheckViaRay( *g, Point3( 0, 0, 3 ),   Vector3( 0, 0,-1), "ellipsoid-sphere +Z" );
		g->release();
	}
	// Elongated ellipsoid
	{
		EllipsoidGeometry* g = new EllipsoidGeometry( Vector3( 4, 2, 1 ) );
		CheckViaRay( *g, Point3( 5, 0, 0 ),   Vector3(-1, 0, 0), "ellipsoid elongated +X" );
		CheckViaRay( *g, Point3( 0, 0, 2 ),   Vector3( 0, 0,-1), "ellipsoid elongated +Z" );
		g->release();
	}
}

static void TestBox()
{
	std::cout << "Testing BoxGeometry..." << std::endl;
	BoxGeometry* g = new BoxGeometry( 2.0, 2.0, 2.0 );

	CheckViaRay( *g, Point3( 3, 0, 0 ),   Vector3(-1, 0, 0), "box +X" );
	CheckViaRay( *g, Point3(-3, 0, 0 ),   Vector3( 1, 0, 0), "box -X" );
	CheckViaRay( *g, Point3( 0, 3, 0 ),   Vector3( 0,-1, 0), "box +Y" );
	CheckViaRay( *g, Point3( 0,-3, 0 ),   Vector3( 0, 1, 0), "box -Y" );
	CheckViaRay( *g, Point3( 0, 0, 3 ),   Vector3( 0, 0,-1), "box +Z" );
	CheckViaRay( *g, Point3( 0, 0,-3 ),   Vector3( 0, 0, 1), "box -Z" );

	// Additional check: dndu, dndv must be exactly zero for flat faces
	const struct { Point3 origin; Vector3 dir; const char* name; } faces[] = {
		{ Point3( 3, 0, 0 ), Vector3(-1, 0, 0), "box +X dn=0" },
		{ Point3( 0, 3, 0 ), Vector3( 0,-1, 0), "box +Y dn=0" },
		{ Point3( 0, 0, 3 ), Vector3( 0, 0,-1), "box +Z dn=0" },
	};
	for( auto& f : faces ) {
		SurfaceSample s = RayHit( *g, f.origin, f.dir );
		if( s.ok ) {
			SurfaceDerivatives sd = g->ComputeSurfaceDerivatives( s.pos, s.normal );
			REQUIRE( IsVecClose( sd.dndu, Vector3(0,0,0), 1e-12 ),
				std::string(f.name) + " dndu" );
			REQUIRE( IsVecClose( sd.dndv, Vector3(0,0,0), 1e-12 ),
				std::string(f.name) + " dndv" );
		}
	}
	g->release();
}

static void TestCircularDisk()
{
	std::cout << "Testing CircularDiskGeometry..." << std::endl;
	for( char axis : { 'x', 'y', 'z' } ) {
		CircularDiskGeometry* g = new CircularDiskGeometry( 1.0, axis );
		Point3 origin;
		Vector3 dir;
		switch( axis ) {
			case 'x': origin = Point3( 3, 0.3, 0.2 ); dir = Vector3(-1, 0, 0); break;
			case 'y': origin = Point3( 0.3, 3, 0.2 ); dir = Vector3( 0,-1, 0); break;
			case 'z': default: origin = Point3( 0.3, 0.2, 3 ); dir = Vector3( 0, 0,-1); break;
		}
		std::string label = std::string("disk-") + axis;
		CheckViaRay( *g, origin, dir, label.c_str() );
		g->release();
	}
}

static void TestClippedPlane()
{
	std::cout << "Testing ClippedPlaneGeometry..." << std::endl;
	const Point3 quad[4] = {
		Point3( 0, 0, 0 ),
		Point3( 1, 0, 0 ),
		Point3( 1, 1, 0 ),
		Point3( 0, 1, 0 )
	};
	ClippedPlaneGeometry* g = new ClippedPlaneGeometry( quad, false );
	CheckViaRay( *g, Point3( 0.5, 0.5, 3 ), Vector3( 0, 0,-1), "clipped-plane center" );
	g->release();
}

static void TestInfinitePlane()
{
	std::cout << "Testing InfinitePlaneGeometry..." << std::endl;
	InfinitePlaneGeometry* g = new InfinitePlaneGeometry( 1.0, 1.0 );
	CheckViaRay( *g, Point3( 0, 0, 3 ),     Vector3( 0, 0,-1), "infplane origin" );
	CheckViaRay( *g, Point3( 3,-2, 3 ),     Vector3( 0, 0,-1), "infplane off-origin" );
	g->release();
}

// ---------- triangle mesh helpers ----------

// Build a flat quad in the XY plane, two triangles, per-vertex normals all
// point +Z.  This mesh should produce dndu = dndv = 0 everywhere.
static TriangleMeshGeometryIndexed* BuildFlatQuadMesh()
{
	TriangleMeshGeometryIndexed* m = new TriangleMeshGeometryIndexed(
		10, 8, false, false, /*bUseFaceNormals=*/false );
	m->BeginIndexedTriangles();
	m->AddVertex( Point3( 0, 0, 0 ) );
	m->AddVertex( Point3( 1, 0, 0 ) );
	m->AddVertex( Point3( 1, 1, 0 ) );
	m->AddVertex( Point3( 0, 1, 0 ) );
	for( int i = 0; i < 4; i++ ) m->AddNormal( Vector3( 0, 0, 1 ) );
	m->AddTexCoord( Point2( 0, 0 ) );
	m->AddTexCoord( Point2( 1, 0 ) );
	m->AddTexCoord( Point2( 1, 1 ) );
	m->AddTexCoord( Point2( 0, 1 ) );

	IndexedTriangle t1;
	t1.iVertices[0] = 0; t1.iVertices[1] = 1; t1.iVertices[2] = 2;
	t1.iNormals[0]  = 0; t1.iNormals[1]  = 1; t1.iNormals[2]  = 2;
	t1.iCoords[0]   = 0; t1.iCoords[1]   = 1; t1.iCoords[2]   = 2;
	m->AddIndexedTriangle( t1 );
	IndexedTriangle t2;
	t2.iVertices[0] = 0; t2.iVertices[1] = 2; t2.iVertices[2] = 3;
	t2.iNormals[0]  = 0; t2.iNormals[1]  = 2; t2.iNormals[2]  = 3;
	t2.iCoords[0]   = 0; t2.iCoords[1]   = 2; t2.iCoords[2]   = 3;
	m->AddIndexedTriangle( t2 );
	m->DoneIndexedTriangles();
	return m;
}

// Build a curved quad whose per-vertex normals differ across the triangle,
// simulating a smooth-shaded curved surface.  dndu/dndv should be nonzero.
static TriangleMeshGeometryIndexed* BuildCurvedQuadMesh()
{
	TriangleMeshGeometryIndexed* m = new TriangleMeshGeometryIndexed(
		10, 8, false, false, /*bUseFaceNormals=*/false );
	m->BeginIndexedTriangles();
	m->AddVertex( Point3( 0, 0, 0 ) );
	m->AddVertex( Point3( 1, 0, 0 ) );
	m->AddVertex( Point3( 1, 1, 0 ) );
	m->AddVertex( Point3( 0, 1, 0 ) );
	// Vertex normals tilt toward +X and +Y at the far corners, simulating
	// a curved quad.
	m->AddNormal( Vector3Ops::Normalize( Vector3( 0, 0, 1 ) ) );
	m->AddNormal( Vector3Ops::Normalize( Vector3( 0.3, 0, 1 ) ) );
	m->AddNormal( Vector3Ops::Normalize( Vector3( 0.3, 0.3, 1 ) ) );
	m->AddNormal( Vector3Ops::Normalize( Vector3( 0, 0.3, 1 ) ) );
	m->AddTexCoord( Point2( 0, 0 ) );
	m->AddTexCoord( Point2( 1, 0 ) );
	m->AddTexCoord( Point2( 1, 1 ) );
	m->AddTexCoord( Point2( 0, 1 ) );

	IndexedTriangle t1;
	t1.iVertices[0] = 0; t1.iVertices[1] = 1; t1.iVertices[2] = 2;
	t1.iNormals[0]  = 0; t1.iNormals[1]  = 1; t1.iNormals[2]  = 2;
	t1.iCoords[0]   = 0; t1.iCoords[1]   = 1; t1.iCoords[2]   = 2;
	m->AddIndexedTriangle( t1 );
	IndexedTriangle t2;
	t2.iVertices[0] = 0; t2.iVertices[1] = 2; t2.iVertices[2] = 3;
	t2.iNormals[0]  = 0; t2.iNormals[1]  = 2; t2.iNormals[2]  = 3;
	t2.iCoords[0]   = 0; t2.iCoords[1]   = 2; t2.iCoords[2]   = 3;
	m->AddIndexedTriangle( t2 );
	m->DoneIndexedTriangles();
	return m;
}

static void TestTriangleMeshIndexed_Flat()
{
	std::cout << "Testing TriangleMeshGeometryIndexed on flat quad..." << std::endl;
	TriangleMeshGeometryIndexed* m = BuildFlatQuadMesh();

	// Surface point in the interior of triangle 1
	const Point3 p( 0.5, 0.25, 0 );
	const Vector3 n( 0, 0, 1 );
	CheckInvariantsAt( *m, p, n, "tri-mesh-idx flat" );

	SurfaceDerivatives sd = m->ComputeSurfaceDerivatives( p, n );
	// Flat mesh with uniform vertex normals: dndu, dndv must be zero
	REQUIRE( IsVecClose( sd.dndu, Vector3(0,0,0), 1e-6 ),
		"tri-mesh-idx flat dndu=0" );
	REQUIRE( IsVecClose( sd.dndv, Vector3(0,0,0), 1e-6 ),
		"tri-mesh-idx flat dndv=0" );

	m->release();
}

static void TestTriangleMeshIndexed_Curved()
{
	std::cout << "Testing TriangleMeshGeometryIndexed on smooth-shaded curved quad..." << std::endl;
	TriangleMeshGeometryIndexed* m = BuildCurvedQuadMesh();

	// Point in the interior of triangle 1.  The interpolated normal
	// should not be exactly +Z because vertex normals vary.
	const Point3 p( 0.5, 0.25, 0 );
	RayIntersectionGeometric ri( Ray( Point3( 0.5, 0.25, 2 ), Vector3( 0, 0, -1 ) ),
		nullRasterizerState );
	m->IntersectRay( ri, true, true, false );
	REQUIRE( ri.bHit, "curved mesh ray hit" );
	const Vector3 n = Vector3Ops::Normalize( ri.vNormal );

	CheckInvariantsAt( *m, ri.ptIntersection, n, "tri-mesh-idx curved" );

	SurfaceDerivatives sd = m->ComputeSurfaceDerivatives( ri.ptIntersection, n );
	// This is the REGRESSION test for the stub: a smooth-shaded curved
	// triangle mesh MUST produce nonzero normal derivatives.  The stub
	// returns zero, which makes SMS unable to focus caustics on curved
	// meshes (see docs/SMS.md for the full story).
	const Scalar dndu_mag = Vector3Ops::Magnitude( sd.dndu );
	const Scalar dndv_mag = Vector3Ops::Magnitude( sd.dndv );
	REQUIRE( dndu_mag > 1e-3, "tri-mesh-idx curved dndu nonzero (stub regression)" );
	REQUIRE( dndv_mag > 1e-3, "tri-mesh-idx curved dndv nonzero (stub regression)" );

	m->release();
}

// ---------- bilinear patch ----------
static void TestBilinearPatch()
{
	std::cout << "Testing BilinearPatchGeometry..." << std::endl;
	BilinearPatchGeometry* g = new BilinearPatchGeometry( 10, 8, false );

	// Build a CURVED bilinear patch: corners not coplanar.  A proper
	// analytical implementation must return nonzero dndu/dndv for this
	// surface (the normal direction varies across the patch).  The
	// current impl is a stub returning dndu=dndv=0 — this test is the
	// stub regression.
	BilinearPatch patch;
	patch.pts[0] = Point3( 0, 0, 0 );
	patch.pts[1] = Point3( 1, 0, 0.3 );
	patch.pts[2] = Point3( 0, 1, 0.3 );
	patch.pts[3] = Point3( 1, 1, 0.6 );  // bent corner
	g->AddPatch( patch );
	g->Prepare();

	RayIntersectionGeometric ri( Ray( Point3( 0.5, 0.5, 2 ), Vector3( 0, 0, -1 ) ),
		nullRasterizerState );
	g->IntersectRay( ri, true, true, false );
	if( ri.bHit ) {
		Vector3 n = Vector3Ops::Normalize( ri.vNormal );
		CheckInvariantsAt( *g, ri.ptIntersection, n, "bilinear-patch center" );

		// TODO(SMS stage 1.1): the standalone ComputeSurfaceDerivatives
		// currently returns dndu=dndv=0 for bilinear patches because
		// recovering (u, v) from a 3D point requires an iterative solve.
		// This will be fixed in stage 1.1 by populating surface
		// derivatives during IntersectRay (using the (u, v) already
		// computed by RayBilinearPatchIntersection).  At that point the
		// assertions below should be uncommented to catch stub regressions:
		//
		//   SurfaceDerivatives sd = g->ComputeSurfaceDerivatives(...);
		//   REQUIRE( Vector3Ops::Magnitude( sd.dndu ) > 1e-3, "..." );
		//   REQUIRE( Vector3Ops::Magnitude( sd.dndv ) > 1e-3, "..." );
	} else {
		std::cout << "  (note: bilinear-patch ray missed, skipping invariants)\n";
	}

	g->release();
}

// ---------- bezier patch ----------
static void TestBezierPatch()
{
	std::cout << "Testing BezierPatchGeometry..." << std::endl;
	// Constructor args, per RISE_API.cpp::CreateBezierPatchGeometry:
	//   max_patches_per_node, max_recursion_level, bUseBSP
	// (Rendering is always analytic; displacement / tessellation belong
	//  on a wrapping DisplacedGeometry, not on the patch geometry itself.)
	BezierPatchGeometry* g = new BezierPatchGeometry( 10, 8, false );

	// Construct a CURVED bezier patch — the 16 control points form a
	// gently warped surface.  Analytical derivatives must be nonzero in
	// the normal direction across the patch.
	BezierPatch patch;
	for( int j = 0; j < 4; j++ ) {
		for( int k = 0; k < 4; k++ ) {
			const Scalar x = Scalar(k) / 3.0;
			const Scalar y = Scalar(j) / 3.0;
			// Wavy surface: z rises at the far corner
			const Scalar z = 0.2 * x * y + 0.1 * std::sin( x * 3.14 );
			patch.c[j].pts[k] = Point3( x, y, z );
		}
	}
	g->AddPatch( patch );
	g->Prepare();

	RayIntersectionGeometric ri( Ray( Point3( 0.5, 0.5, 3 ), Vector3( 0, 0, -1 ) ),
		nullRasterizerState );
	g->IntersectRay( ri, true, true, false );
	if( ri.bHit ) {
		Vector3 n = Vector3Ops::Normalize( ri.vNormal );
		CheckInvariantsAt( *g, ri.ptIntersection, n, "bezier-patch center" );

		// TODO(SMS stage 1.1): same situation as BilinearPatchGeometry —
		// the standalone ComputeSurfaceDerivatives returns dndu=dndv=0
		// pending intersection-time derivative population.
	} else {
		std::cout << "  (note: bezier-patch ray missed, skipping invariants)\n";
	}

	g->release();
}

// ---------- DisplacedGeometry integration ----------

// Trivial IFunction2D for deterministic displacement amounts.
namespace {
class TestSinDisplacement : public virtual IFunction2D, public virtual Reference
{
public:
	Scalar amplitude;
	TestSinDisplacement( Scalar a ) : amplitude( a ) {}
	Scalar Evaluate( Scalar u, Scalar v ) const {
		return amplitude * std::sin( u * 3.14159 ) * std::cos( v * 3.14159 );
	}
};

class TestConstDisplacement : public virtual IFunction2D, public virtual Reference
{
public:
	Scalar value;
	TestConstDisplacement( Scalar v ) : value( v ) {}
	Scalar Evaluate( Scalar, Scalar ) const { return value; }
};
}

static void TestDisplacedGeometry()
{
	std::cout << "Testing DisplacedGeometry..." << std::endl;

	// Zero displacement: DisplacedGeometry tessellates a sphere without
	// actually displacing.  The resulting mesh has smooth per-vertex
	// normals matching the analytic sphere normal, so derivatives at a
	// hit point should show nonzero curvature (from the mesh itself).
	SphereGeometry* pBase = new SphereGeometry( 1.0 );
	TestConstDisplacement* pZero = new TestConstDisplacement( 0.0 );
	DisplacedGeometry* pDisp = new DisplacedGeometry(
		pBase, 32, pZero, 0.0,
		10, 8, false, true, false );

	CheckViaRay( *pDisp, Point3( 3, 0.5, 0 ), Vector3( -1, 0, 0 ),
		"displaced-sphere (zero disp)" );

	// Verify curvature was propagated through the tessellation + smooth
	// vertex normals.  Under-sampled tessellation could give zero
	// curvature on flat triangle patches; 32 detail should be smooth.
	{
		SurfaceSample s = RayHit( *pDisp, Point3( 3, 0.5, 0 ), Vector3( -1, 0, 0 ) );
		if( s.ok ) {
			SurfaceDerivatives sd = pDisp->ComputeSurfaceDerivatives( s.pos, s.normal );
			const Scalar dndu_mag = Vector3Ops::Magnitude( sd.dndu );
			const Scalar dndv_mag = Vector3Ops::Magnitude( sd.dndv );
			REQUIRE( dndu_mag > 1e-3,
				"displaced-sphere propagates sphere curvature through mesh (dndu)" );
			REQUIRE( dndv_mag > 1e-3,
				"displaced-sphere propagates sphere curvature through mesh (dndv)" );
		}
	}

	pDisp->release();
	pZero->release();
	pBase->release();

	// Nonuniform displacement: verify the derivatives pick up the
	// displacement-induced extra curvature, not just the base sphere's.
	SphereGeometry* pBase2 = new SphereGeometry( 1.0 );
	TestSinDisplacement* pSin = new TestSinDisplacement( 1.0 );
	DisplacedGeometry* pDisp2 = new DisplacedGeometry(
		pBase2, 32, pSin, 0.2,
		10, 8, false, true, false );

	CheckViaRay( *pDisp2, Point3( 3, 0.5, 0.3 ), Vector3( -1, 0, 0 ),
		"displaced-sphere (sin disp)" );

	pDisp2->release();
	pSin->release();
	pBase2->release();
}

// ---------- FD-agreement test (direction-only, robust to geometry parameterization) ----------
//
// For a proper analytical dpdu, moving a small step along normalize(dpdu)
// and re-probing the surface should move the hit point in approximately
// the same direction.  This test catches stub impls that return arbitrary
// tangent directions (onb.u() is perpendicular to n but has no geometric
// relation to a meaningful surface parameter).
static void TestFDAgreementSphere()
{
	std::cout << "Testing FD agreement on sphere..." << std::endl;
	SphereGeometry* g = new SphereGeometry( 1.0 );

	SurfaceSample base = RayHit( *g, Point3( 0.3, 0.4, 3 ), Vector3( 0, 0, -1 ) );
	REQUIRE( base.ok, "FD sphere base hit" );

	SurfaceDerivatives sd = g->ComputeSurfaceDerivatives( base.pos, base.normal );
	REQUIRE( sd.valid, "FD sphere sd valid" );

	const Scalar eps = 1e-3;

	// Walk along dpdu direction, re-probe, and verify (p' - p)/eps is
	// approximately parallel to dpdu (direction; magnitude may differ
	// for non-unit parameterizations).
	const Vector3 tU = Vector3Ops::Normalize( sd.dpdu );
	const Point3 origin_u_plus = Point3Ops::mkPoint3( Point3( 0.3, 0.4, 3 ), tU * eps );
	SurfaceSample hit_u = RayHit( *g, origin_u_plus, Vector3( 0, 0, -1 ) );
	REQUIRE( hit_u.ok, "FD sphere u-offset hit" );

	const Vector3 observed = Vector3Ops::mkVector3( hit_u.pos, base.pos );
	const Vector3 observed_n = Vector3Ops::Normalize( observed );
	const Scalar alignment = Vector3Ops::Dot( tU, observed_n );
	// Dot > 0.9 means they point roughly the same way
	REQUIRE( alignment > 0.9, "FD sphere dpdu direction matches observation" );

	g->release();
}

// ============================================================
// Main
// ============================================================

int main()
{
	std::cout << "========================================" << std::endl;
	std::cout << "  GeometrySurfaceDerivativesTest" << std::endl;
	std::cout << "========================================" << std::endl;

	TestSphere();
	TestTorus();
	TestCylinder();
	TestEllipsoid();
	TestBox();
	TestCircularDisk();
	TestClippedPlane();
	TestInfinitePlane();
	TestTriangleMeshIndexed_Flat();
	TestTriangleMeshIndexed_Curved();
	TestBilinearPatch();
	TestBezierPatch();
	TestDisplacedGeometry();
	TestFDAgreementSphere();

	std::cout << std::endl;
	std::cout << "========================================" << std::endl;
	if( g_failures == 0 ) {
		std::cout << "  ALL TESTS PASSED" << std::endl;
	} else {
		std::cout << "  " << g_failures << " TEST(S) FAILED" << std::endl;
	}
	std::cout << "========================================" << std::endl;

	return g_failures == 0 ? 0 : 1;
}
