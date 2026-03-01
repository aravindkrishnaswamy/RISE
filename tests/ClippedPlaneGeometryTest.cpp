#include <iostream>
#include <cassert>
#include <cmath>
#include "../src/Library/Geometry/ClippedPlaneGeometry.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"

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

void TestNonCoplanarQuadUsesPerTriangleNormals()
{
	std::cout << "Testing ClippedPlaneGeometry non-coplanar per-triangle normals..." << std::endl;

	const Point3 points[4] = {
		Point3(0, 0, 0),
		Point3(1, 0, 0),
		Point3(1, 1, 0.1),
		Point3(0, 1, 0)
	};

	ClippedPlaneGeometry* pGeom = new ClippedPlaneGeometry( points, false );

	const Vector3 normalA = Vector3Ops::Normalize(
		Vector3Ops::Cross(
			Vector3Ops::mkVector3(points[1], points[0]),
			Vector3Ops::mkVector3(points[2], points[0])
			)
		);

	const Vector3 normalB = Vector3Ops::Normalize(
		Vector3Ops::Cross(
			Vector3Ops::mkVector3(points[2], points[0]),
			Vector3Ops::mkVector3(points[3], points[0])
			)
		);

	assert( !IsVectorClose(normalA, normalB, 1e-4) );

	{
		RayIntersectionGeometric ri = MakeIntersection( Point3(0.75, 0.25, 1.0), Vector3(0, 0, -1) );
		pGeom->IntersectRay( ri, true, false, false );
		assert( ri.bHit );
		assert( IsVectorClose(ri.vNormal, normalA, 1e-5) );
	}

	{
		RayIntersectionGeometric ri = MakeIntersection( Point3(0.25, 0.75, 1.0), Vector3(0, 0, -1) );
		pGeom->IntersectRay( ri, true, false, false );
		assert( ri.bHit );
		assert( IsVectorClose(ri.vNormal, normalB, 1e-5) );
	}

	safe_release( pGeom );

	std::cout << "ClippedPlaneGeometry non-coplanar per-triangle normals Passed!" << std::endl;
}

int main()
{
	TestCoplanarQuadTrianglesShareExpectedNormal();
	TestNonCoplanarQuadUsesPerTriangleNormals();
	return 0;
}
