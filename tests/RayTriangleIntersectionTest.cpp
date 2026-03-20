#include <cassert>
#include <cmath>
#include <iostream>

#include "../src/Library/Intersection/RayPrimitiveIntersections.h"

using namespace RISE;

namespace
{
	bool IsClose( const Scalar a, const Scalar b, const Scalar epsilon = 1e-6 )
	{
		return std::fabs( a - b ) <= epsilon;
	}

	void TestInteriorHit()
	{
		std::cout << "Testing RayTriangleIntersection interior hit..." << std::endl;

		const Point3 v0( 0, 0, 0 );
		const Vector3 edgeA( 1, 0, 0 );
		const Vector3 edgeB( 0, 1, 0 );
		const Ray ray( Point3( 0.25, 0.25, 1.0 ), Vector3( 0, 0, -1 ) );

		TRIANGLE_HIT hit;
		RayTriangleIntersection( ray, hit, v0, edgeA, edgeB );

		assert( hit.bHit );
		assert( IsClose( hit.dRange, 1.0 ) );
		assert( IsClose( hit.dRange2, 1.0 ) );
		assert( IsClose( hit.alpha, 0.25 ) );
		assert( IsClose( hit.beta, 0.25 ) );

		std::cout << "RayTriangleIntersection interior hit Passed!" << std::endl;
	}

	void TestBoundaryHits()
	{
		std::cout << "Testing RayTriangleIntersection edge and vertex hits..." << std::endl;

		const Point3 v0( 0, 0, 0 );
		const Vector3 edgeA( 1, 0, 0 );
		const Vector3 edgeB( 0, 1, 0 );

		{
			const Ray ray( Point3( 0.5, 0.5, 2.0 ), Vector3( 0, 0, -2 ) );
			TRIANGLE_HIT hit;
			RayTriangleIntersection( ray, hit, v0, edgeA, edgeB );

			assert( hit.bHit );
			assert( IsClose( hit.dRange, 1.0 ) );
			assert( IsClose( hit.alpha, 0.5 ) );
			assert( IsClose( hit.beta, 0.5 ) );
		}

		{
			const Ray ray( Point3( 1.0, 0.0, 3.0 ), Vector3( 0, 0, -3 ) );
			TRIANGLE_HIT hit;
			RayTriangleIntersection( ray, hit, v0, edgeA, edgeB );

			assert( hit.bHit );
			assert( IsClose( hit.dRange, 1.0 ) );
			assert( IsClose( hit.alpha, 1.0 ) );
			assert( IsClose( hit.beta, 0.0 ) );
		}

		std::cout << "RayTriangleIntersection edge and vertex hits Passed!" << std::endl;
	}

	void TestMissCases()
	{
		std::cout << "Testing RayTriangleIntersection miss cases..." << std::endl;

		const Point3 v0( 0, 0, 0 );
		const Vector3 edgeA( 1, 0, 0 );
		const Vector3 edgeB( 0, 1, 0 );

		{
			const Ray ray( Point3( 0.75, 0.75, 1.0 ), Vector3( 0, 0, -1 ) );
			TRIANGLE_HIT hit;
			RayTriangleIntersection( ray, hit, v0, edgeA, edgeB );
			assert( !hit.bHit );
		}

		{
			const Ray ray( Point3( 0.25, 0.25, -1.0 ), Vector3( 0, 0, -1 ) );
			TRIANGLE_HIT hit;
			RayTriangleIntersection( ray, hit, v0, edgeA, edgeB );
			assert( !hit.bHit );
		}

		{
			const Ray ray( Point3( 0.25, 0.25, 1.0 ), Vector3( 1, 0, 0 ) );
			TRIANGLE_HIT hit;
			RayTriangleIntersection( ray, hit, v0, edgeA, edgeB );
			assert( !hit.bHit );
		}

		std::cout << "RayTriangleIntersection miss cases Passed!" << std::endl;
	}

	void TestWindingIndependence()
	{
		std::cout << "Testing RayTriangleIntersection winding independence..." << std::endl;

		const Point3 v0( 0, 0, 0 );
		const Ray ray( Point3( 0.2, 0.3, 1.0 ), Vector3( 0, 0, -1 ) );

		TRIANGLE_HIT forwardHit;
		RayTriangleIntersection( ray, forwardHit, v0, Vector3( 1, 0, 0 ), Vector3( 0, 1, 0 ) );
		assert( forwardHit.bHit );

		TRIANGLE_HIT reversedHit;
		RayTriangleIntersection( ray, reversedHit, v0, Vector3( 0, 1, 0 ), Vector3( 1, 0, 0 ) );
		assert( reversedHit.bHit );
		assert( IsClose( reversedHit.dRange, forwardHit.dRange ) );

		std::cout << "RayTriangleIntersection winding independence Passed!" << std::endl;
	}
}

int main()
{
	TestInteriorHit();
	TestBoundaryHits();
	TestMissCases();
	TestWindingIndependence();
	return 0;
}
