#include <iostream>
#include <cassert>
#include <cmath>
#include "../src/Library/Geometry/ClippedPlaneGeometry.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Utilities/GeometricUtilities.h"

using namespace RISE;
using namespace RISE::Implementation;

bool IsClose( const Scalar a, const Scalar b, const Scalar epsilon = 1e-6 )
{
	return std::fabs(a - b) <= epsilon;
}

bool IsVectorClose( const Vector3& a, const Vector3& b, const Scalar epsilon = 1e-6 )
{
	return IsClose(a.x, b.x, epsilon) &&
		IsClose(a.y, b.y, epsilon) &&
		IsClose(a.z, b.z, epsilon);
}

RayIntersectionGeometric MakeIntersection( const Point3& origin, const Vector3& dir )
{
	return RayIntersectionGeometric( Ray(origin, dir), nullRasterizerState );
}

void TestCoplanarQuadTrianglesShareExpectedNormal()
{
	std::cout << "Testing ClippedPlaneGeometry coplanar triangle behavior..." << std::endl;

	const Point3 points[4] = {
		Point3(0, 0, 0),
		Point3(1, 0, 0),
		Point3(1, 1, 0),
		Point3(0, 1, 0)
	};

	ClippedPlaneGeometry* pGeom = new ClippedPlaneGeometry( points, false );
	const Vector3 expectedNormal(0, 0, 1);

	{
		RayIntersectionGeometric ri = MakeIntersection( Point3(0.75, 0.25, 1.0), Vector3(0, 0, -1) );
		pGeom->IntersectRay( ri, true, false, false );
		assert( ri.bHit );
		assert( IsClose(ri.range, 1.0) );
		assert( IsVectorClose(ri.vNormal, expectedNormal) );
	}

	{
		RayIntersectionGeometric ri = MakeIntersection( Point3(0.25, 0.75, 1.0), Vector3(0, 0, -1) );
		pGeom->IntersectRay( ri, true, false, false );
		assert( ri.bHit );
		assert( IsClose(ri.range, 1.0) );
		assert( IsVectorClose(ri.vNormal, expectedNormal) );
	}

	{
		const bool triAHit = pGeom->IntersectRay_IntersectionOnly(
			Ray(Point3(0.75, 0.25, 1.0), Vector3(0, 0, -1)), 10.0, true, false
			);
		const bool triBHit = pGeom->IntersectRay_IntersectionOnly(
			Ray(Point3(0.25, 0.75, 1.0), Vector3(0, 0, -1)), 10.0, true, false
			);
		assert( triAHit );
		assert( triBHit );
	}

	safe_release( pGeom );

	std::cout << "ClippedPlaneGeometry coplanar triangle behavior Passed!" << std::endl;
}

// Returns the analytical bilinear surface normal at parametric (u, v),
// matching the formula ClippedPlaneGeometry::IntersectRay computes
// internally (cross of GeometricUtilities::BilinearTangentU/V).
static Vector3 BilinearNormalAt(
	const Point3 corners[4], Scalar u, Scalar v )
{
	const Vector3 dpdu = GeometricUtilities::BilinearTangentU(
		corners[0], corners[1], corners[2], corners[3], v );
	const Vector3 dpdv = GeometricUtilities::BilinearTangentV(
		corners[0], corners[1], corners[2], corners[3], u );
	return Vector3Ops::Normalize( Vector3Ops::Cross( dpdu, dpdv ) );
}

void TestNonCoplanarQuadUsesBilinearSurfaceNormal()
{
	// Pre-2026 ClippedPlaneGeometry traced two flat triangles and
	// emitted per-triangle constant normals.  It now traces the
	// canonical bilinear surface defined by the four corners (matching
	// TessellateToMesh), so the normal varies smoothly with (u, v) on
	// non-planar quads — equal to the cross of the analytical bilinear
	// tangents.  This test checks the new contract.
	std::cout << "Testing ClippedPlaneGeometry non-coplanar bilinear surface normal..." << std::endl;

	const Point3 points[4] = {
		Point3(0, 0, 0),
		Point3(1, 0, 0),
		Point3(1, 1, 0.1),
		Point3(0, 1, 0)
	};

	ClippedPlaneGeometry* pGeom = new ClippedPlaneGeometry( points, false );

	// Two probe rays at well-separated (u, v) on the surface.  For a
	// genuinely non-planar quad the bilinear normal differs at the two
	// sample points, so this still verifies that the geometry doesn't
	// emit a single constant normal (the planar-quad failure mode).
	{
		RayIntersectionGeometric ri = MakeIntersection( Point3(0.75, 0.25, 1.0), Vector3(0, 0, -1) );
		pGeom->IntersectRay( ri, true, false, false );
		assert( ri.bHit );
		const Vector3 expected = BilinearNormalAt( points, ri.ptCoord.x, ri.ptCoord.y );
		assert( IsVectorClose( ri.vNormal, expected, 1e-5 ) );
	}

	{
		RayIntersectionGeometric ri = MakeIntersection( Point3(0.25, 0.75, 1.0), Vector3(0, 0, -1) );
		pGeom->IntersectRay( ri, true, false, false );
		assert( ri.bHit );
		const Vector3 expected = BilinearNormalAt( points, ri.ptCoord.x, ri.ptCoord.y );
		assert( IsVectorClose( ri.vNormal, expected, 1e-5 ) );
	}

	// Sanity: the two probes produce DIFFERENT normals (they're at
	// opposite corners of the bilinear surface; the lifted vP[2]
	// makes the surface curve).
	{
		RayIntersectionGeometric riA = MakeIntersection( Point3(0.75, 0.25, 1.0), Vector3(0, 0, -1) );
		RayIntersectionGeometric riB = MakeIntersection( Point3(0.25, 0.75, 1.0), Vector3(0, 0, -1) );
		pGeom->IntersectRay( riA, true, false, false );
		pGeom->IntersectRay( riB, true, false, false );
		assert( riA.bHit && riB.bHit );
		assert( !IsVectorClose( riA.vNormal, riB.vNormal, 1e-4 ) );
	}

	safe_release( pGeom );

	std::cout << "ClippedPlaneGeometry non-coplanar bilinear surface normal Passed!" << std::endl;
}

int main()
{
	TestCoplanarQuadTrianglesShareExpectedNormal();
	TestNonCoplanarQuadUsesBilinearSurfaceNormal();
	return 0;
}
