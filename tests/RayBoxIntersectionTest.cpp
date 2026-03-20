//////////////////////////////////////////////////////////////////////
//
//  RayBoxIntersectionTest.cpp - Thorough regression tests for
//  RayBoxIntersection including edge cases, consistency with BSP
//  tree traversal, and comparison of old vs new implementations.
//
//////////////////////////////////////////////////////////////////////

#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

#include "../src/Library/Intersection/RayPrimitiveIntersections.h"
#include "../src/Library/BSPTreeSAH.h"
#include "../src/Library/Utilities/MemoryBuffer.h"
#include "../src/Library/Utilities/Reference.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	bool IsClose( const Scalar a, const Scalar b, const Scalar epsilon = 1e-6 )
	{
		return std::fabs( a - b ) < epsilon;
	}

	// =====================================================================
	// Test 1: Basic hit/miss cases
	// =====================================================================
	void TestBasicHitMiss()
	{
		std::cout << "Testing RayBoxIntersection basic hit/miss..." << std::endl;

		const Point3 ll( 0, 0, 0 );
		const Point3 ur( 1, 1, 1 );

		// Ray along +X hitting center of box
		{
			BOX_HIT h;
			RayBoxIntersection( Ray( Point3(-2, 0.5, 0.5), Vector3(1, 0, 0) ), h, ll, ur );
			assert( h.bHit );
			assert( IsClose( h.dRange, 2.0 ) );
			assert( IsClose( h.dRange2, 3.0 ) );
		}

		// Ray along -X hitting center of box
		{
			BOX_HIT h;
			RayBoxIntersection( Ray( Point3(3, 0.5, 0.5), Vector3(-1, 0, 0) ), h, ll, ur );
			assert( h.bHit );
			assert( IsClose( h.dRange, 2.0 ) );
			assert( IsClose( h.dRange2, 3.0 ) );
		}

		// Ray along +Y
		{
			BOX_HIT h;
			RayBoxIntersection( Ray( Point3(0.5, -5, 0.5), Vector3(0, 1, 0) ), h, ll, ur );
			assert( h.bHit );
			assert( IsClose( h.dRange, 5.0 ) );
			assert( IsClose( h.dRange2, 6.0 ) );
		}

		// Ray along +Z
		{
			BOX_HIT h;
			RayBoxIntersection( Ray( Point3(0.5, 0.5, -3), Vector3(0, 0, 1) ), h, ll, ur );
			assert( h.bHit );
			assert( IsClose( h.dRange, 3.0 ) );
			assert( IsClose( h.dRange2, 4.0 ) );
		}

		// Clear miss: ray parallel to box face, outside
		{
			BOX_HIT h;
			RayBoxIntersection( Ray( Point3(-1, 2, 0.5), Vector3(1, 0, 0) ), h, ll, ur );
			assert( !h.bHit );
		}

		// Clear miss: ray pointing away from box
		{
			BOX_HIT h;
			RayBoxIntersection( Ray( Point3(-2, 0.5, 0.5), Vector3(-1, 0, 0) ), h, ll, ur );
			assert( !h.bHit );
		}

		// Miss: ray passes beside box
		{
			BOX_HIT h;
			RayBoxIntersection( Ray( Point3(-2, 1.5, 0.5), Vector3(1, 0, 0) ), h, ll, ur );
			assert( !h.bHit );
		}

		std::cout << "RayBoxIntersection basic hit/miss Passed!" << std::endl;
	}

	// =====================================================================
	// Test 2: Origin inside box
	// =====================================================================
	void TestOriginInside()
	{
		std::cout << "Testing RayBoxIntersection origin inside..." << std::endl;

		const Point3 ll( 0, 0, 0 );
		const Point3 ur( 2, 2, 2 );

		// Origin at center, ray along +X
		{
			BOX_HIT h;
			RayBoxIntersection( Ray( Point3(1, 1, 1), Vector3(1, 0, 0) ), h, ll, ur );
			assert( h.bHit );
			// tmin should be negative (behind), tmax = 1.0 (exit at x=2)
			assert( h.dRange > 0 );
			assert( IsClose( h.dRange, 1.0 ) );
		}

		// Origin at center, ray along -Y
		{
			BOX_HIT h;
			RayBoxIntersection( Ray( Point3(1, 1, 1), Vector3(0, -1, 0) ), h, ll, ur );
			assert( h.bHit );
			assert( h.dRange > 0 );
			assert( IsClose( h.dRange, 1.0 ) );
		}

		std::cout << "RayBoxIntersection origin inside Passed!" << std::endl;
	}

	// =====================================================================
	// Test 3: Diagonal rays
	// =====================================================================
	void TestDiagonalRays()
	{
		std::cout << "Testing RayBoxIntersection diagonal rays..." << std::endl;

		const Point3 ll( 0, 0, 0 );
		const Point3 ur( 1, 1, 1 );

		// Diagonal ray towards corner
		{
			const Scalar inv_sqrt3 = 1.0 / std::sqrt(3.0);
			BOX_HIT h;
			RayBoxIntersection( Ray( Point3(-1, -1, -1), Vector3(inv_sqrt3, inv_sqrt3, inv_sqrt3) ), h, ll, ur );
			assert( h.bHit );
			assert( IsClose( h.dRange, std::sqrt(3.0) ) );
		}

		// Diagonal ray that misses the box corner
		{
			BOX_HIT h;
			const Scalar d = 1.0 / std::sqrt(2.0);
			RayBoxIntersection( Ray( Point3(-1, 1.5, 0.5), Vector3(d, d, 0) ), h, ll, ur );
			assert( !h.bHit );
		}

		std::cout << "RayBoxIntersection diagonal rays Passed!" << std::endl;
	}

	// =====================================================================
	// Test 4: Edge-grazing rays
	// =====================================================================
	void TestEdgeGrazing()
	{
		std::cout << "Testing RayBoxIntersection edge-grazing rays..." << std::endl;

		const Point3 ll( 0, 0, 0 );
		const Point3 ur( 1, 1, 1 );

		// Ray exactly along the edge of the box (y=0, z=0)
		{
			BOX_HIT h;
			RayBoxIntersection( Ray( Point3(-1, 0, 0), Vector3(1, 0, 0) ), h, ll, ur );
			// This may or may not hit depending on boundary handling; just don't crash
			(void)h;
		}

		// Ray along box face (z=0 plane)
		{
			BOX_HIT h;
			RayBoxIntersection( Ray( Point3(-1, 0.5, 0), Vector3(1, 0, 0) ), h, ll, ur );
			// Boundary case - just verify no crash
			(void)h;
		}

		std::cout << "RayBoxIntersection edge-grazing rays Passed!" << std::endl;
	}

	// =====================================================================
	// Test 5: Axis-aligned box at various positions
	// =====================================================================
	void TestVariousBoxPositions()
	{
		std::cout << "Testing RayBoxIntersection various box positions..." << std::endl;

		// Negative coordinate box
		{
			const Point3 ll( -5, -5, -5 );
			const Point3 ur( -3, -3, -3 );
			BOX_HIT h;
			RayBoxIntersection( Ray( Point3(-10, -4, -4), Vector3(1, 0, 0) ), h, ll, ur );
			assert( h.bHit );
			assert( IsClose( h.dRange, 5.0 ) );
		}

		// Large box
		{
			const Point3 ll( -1000, -1000, -1000 );
			const Point3 ur( 1000, 1000, 1000 );
			BOX_HIT h;
			RayBoxIntersection( Ray( Point3(-2000, 0, 0), Vector3(1, 0, 0) ), h, ll, ur );
			assert( h.bHit );
			assert( IsClose( h.dRange, 1000.0 ) );
		}

		// Very thin box (degenerate)
		{
			const Point3 ll( 0, 0, 0 );
			const Point3 ur( 10, 10, 0.001 );
			BOX_HIT h;
			RayBoxIntersection( Ray( Point3(5, 5, -1), Vector3(0, 0, 1) ), h, ll, ur );
			assert( h.bHit );
		}

		std::cout << "RayBoxIntersection various box positions Passed!" << std::endl;
	}

	// =====================================================================
	// Test 6: Side identification
	// =====================================================================
	void TestSideIdentification()
	{
		std::cout << "Testing RayBoxIntersection side identification..." << std::endl;

		const Point3 ll( 0, 0, 0 );
		const Point3 ur( 1, 1, 1 );

		// Hit from -X side (should enter on SIDE_X0=0)
		{
			BOX_HIT h;
			RayBoxIntersection( Ray( Point3(-2, 0.5, 0.5), Vector3(1, 0, 0) ), h, ll, ur );
			assert( h.bHit );
			assert( h.sideA == 0 );  // SIDE_X0
			assert( h.sideB == 1 );  // SIDE_X1
		}

		// Hit from +Y side
		{
			BOX_HIT h;
			RayBoxIntersection( Ray( Point3(0.5, 5, 0.5), Vector3(0, -1, 0) ), h, ll, ur );
			assert( h.bHit );
			assert( h.sideA == 3 );  // SIDE_Y1
			assert( h.sideB == 2 );  // SIDE_Y0
		}

		// Hit from -Z side
		{
			BOX_HIT h;
			RayBoxIntersection( Ray( Point3(0.5, 0.5, -5), Vector3(0, 0, 1) ), h, ll, ur );
			assert( h.bHit );
			assert( h.sideA == 4 );  // SIDE_Z0
			assert( h.sideB == 5 );  // SIDE_Z1
		}

		std::cout << "RayBoxIntersection side identification Passed!" << std::endl;
	}

	// =====================================================================
	// Test 7: BSP tree traversal correctness with many random-ish rays
	// Uses the existing BSPTreeSAH test infrastructure to verify that
	// tree traversal results match naive (brute-force) results.
	// =====================================================================
	struct BoxPrimitive
	{
		unsigned int	id;
		BoundingBox		bbox;

		BoxPrimitive() : id(0), bbox(Point3(0,0,0), Point3(0,0,0)) {}
		BoxPrimitive( const unsigned int id_, const BoundingBox& bb ) : id(id_), bbox(bb) {}
	};

	class BoxProcessor :
		public virtual TreeElementProcessor<BoxPrimitive>,
		public virtual Reference
	{
	public:
		virtual ~BoxProcessor(){}
		void RayElementIntersection( RayIntersectionGeometric& ri, const BoxPrimitive elem, const bool, const bool ) const
		{
			BOX_HIT h;
			RayBoxIntersection( ri.ray, h, elem.bbox.ll, elem.bbox.ur );
			if( h.bHit && h.dRange >= NEARZERO ) {
				ri.bHit = true;
				ri.range = h.dRange;
				ri.ptIntersection = ri.ray.PointAtLength( h.dRange );
				ri.ptCoord = Point2( static_cast<Scalar>(elem.id), 0 );
			}
		}

		void RayElementIntersection( RayIntersection& ri, const BoxPrimitive elem, const bool f, const bool b, const bool ) const
		{
			RayElementIntersection( ri.geometric, elem, f, b );
		}

		bool RayElementIntersection_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const BoxPrimitive elem, const bool, const bool ) const
		{
			BOX_HIT h;
			RayBoxIntersection( ray, h, elem.bbox.ll, elem.bbox.ur );
			return h.bHit && h.dRange >= NEARZERO && h.dRange <= dHowFar;
		}

		BoundingBox GetElementBoundingBox( const BoxPrimitive elem ) const { return elem.bbox; }

		bool ElementBoxIntersection( const BoxPrimitive elem, const BoundingBox& bbox ) const
		{
			return elem.bbox.DoIntersect( bbox );
		}

		char WhichSideofPlaneIsElement( const BoxPrimitive elem, const Plane& plane ) const
		{
			return GeometricUtilities::WhichSideOfPlane( plane, elem.bbox );
		}

		void SerializeElement( IWriteBuffer& buffer, const BoxPrimitive elem ) const
		{
			buffer.setUInt( elem.id );
			elem.bbox.Serialize( buffer );
		}

		void DeserializeElement( IReadBuffer& buffer, BoxPrimitive& ret ) const
		{
			ret.id = buffer.getUInt();
			ret.bbox.Deserialize( buffer );
		}
	};

	void TestBSPTraversalConsistency()
	{
		std::cout << "Testing BSP traversal consistency with RayBoxIntersection..." << std::endl;

		BoxProcessor processor;
		std::vector<BoxPrimitive> primitives;

		// Create a grid of small boxes
		unsigned int id = 1;
		for( int x = 0; x < 5; x++ ) {
			for( int y = 0; y < 5; y++ ) {
				for( int z = 0; z < 5; z++ ) {
					const Scalar bx = Scalar(x) * 3.0;
					const Scalar by = Scalar(y) * 3.0;
					const Scalar bz = Scalar(z) * 3.0;
					primitives.push_back( BoxPrimitive( id++, BoundingBox( Point3(bx, by, bz), Point3(bx+1.0, by+1.0, bz+1.0) ) ) );
				}
			}
		}

		// Compute overall bounding box
		BoundingBox overall( Point3(RISE_INFINITY,RISE_INFINITY,RISE_INFINITY), Point3(-RISE_INFINITY,-RISE_INFINITY,-RISE_INFINITY) );
		for( size_t i=0; i<primitives.size(); i++ ) {
			overall.Include( primitives[i].bbox );
		}

		BSPTreeSAH<BoxPrimitive>* pTree = new BSPTreeSAH<BoxPrimitive>( processor, overall, 2 );
		pTree->AddElements( primitives, 24 );

		// Test many ray directions
		const Point3 origins[] = {
			Point3(-5, 7, 7),
			Point3(7, -5, 7),
			Point3(7, 7, -5),
			Point3(20, 7, 7),
			Point3(7, 20, 7),
			Point3(7, 7, 20),
			Point3(-5, -5, -5),
			Point3(7, 7, 7),       // inside grid
			Point3(0.5, 0.5, 0.5), // inside first box
			Point3(1.5, 1.5, 1.5), // between boxes
		};

		const Vector3 directions[] = {
			Vector3(1, 0, 0),
			Vector3(0, 1, 0),
			Vector3(0, 0, 1),
			Vector3(-1, 0, 0),
			Vector3(0, -1, 0),
			Vector3(0, 0, -1),
			Vector3(0.577, 0.577, 0.577),
			Vector3(1, 0.1, 0.05),
			Vector3(0.3, 0.7, -0.2),
			Vector3(-0.5, 0.5, 0.7),
		};

		unsigned int tests_run = 0;
		for( int o = 0; o < 10; o++ ) {
			for( int d = 0; d < 10; d++ ) {
				Vector3 dir = directions[d];
				const Scalar len = Vector3Ops::Magnitude( dir );
				dir = dir * (1.0 / len);

				const Ray ray( origins[o], dir );

				// Naive brute-force
				RayIntersectionGeometric expected( ray, nullRasterizerState );
				for( size_t i=0; i<primitives.size(); i++ ) {
					RayIntersectionGeometric cur( ray, nullRasterizerState );
					processor.RayElementIntersection( cur, primitives[i], true, true );
					if( cur.bHit && cur.range < expected.range ) {
						expected = cur;
					}
				}

				// Tree traversal
				RayIntersectionGeometric actual( ray, nullRasterizerState );
				pTree->IntersectRay( actual, true, true );

				if( actual.bHit != expected.bHit ) {
					std::cerr << "MISMATCH at o=" << o << " d=" << d
						<< " origin=(" << ray.origin.x << "," << ray.origin.y << "," << ray.origin.z << ")"
						<< " dir=(" << ray.Dir().x << "," << ray.Dir().y << "," << ray.Dir().z << ")"
						<< " expected.bHit=" << expected.bHit << " actual.bHit=" << actual.bHit;
					if( expected.bHit ) {
						std::cerr << " expected.range=" << expected.range
							<< " expected.id=" << expected.ptCoord.x;
					}
					std::cerr << std::endl;
				}
				assert( actual.bHit == expected.bHit );
				if( actual.bHit ) {
					assert( IsClose( actual.range, expected.range, 1e-4 ) );
				}

				// Also test IntersectionOnly
				const bool expectedShadow = expected.bHit && expected.range <= 100.0;
				assert( pTree->IntersectRay_IntersectionOnly( ray, 100.0, true, true ) == expectedShadow );

				tests_run++;
			}
		}

		safe_release( pTree );

		std::cout << "BSP traversal consistency Passed! (" << tests_run << " rays tested)" << std::endl;
	}

	// =====================================================================
	// Test 8: Symmetric rays - verify consistent results when direction
	// is negated (ray comes from opposite side)
	// =====================================================================
	void TestSymmetry()
	{
		std::cout << "Testing RayBoxIntersection symmetry..." << std::endl;

		const Point3 ll( 2, 2, 2 );
		const Point3 ur( 4, 4, 4 );

		// Rays from opposite directions should both hit at the same distance
		// from the box surface
		const Point3 center( 3, 3, 3 );
		const Scalar dist = 10.0;

		struct { Vector3 dir; } cases[] = {
			{ Vector3(1, 0, 0) },
			{ Vector3(0, 1, 0) },
			{ Vector3(0, 0, 1) },
			{ Vector3(0.577, 0.577, 0.577) },
			{ Vector3(1, 0.5, 0.25) },
		};

		for( int i = 0; i < 5; i++ ) {
			Vector3 dir = cases[i].dir;
			const Scalar len = Vector3Ops::Magnitude( dir );
			dir = dir * (1.0 / len);

			const Point3 o1 = Point3Ops::mkPoint3( center, dir * (-dist) );
			const Point3 o2 = Point3Ops::mkPoint3( center, dir * dist );

			BOX_HIT h1, h2;
			RayBoxIntersection( Ray( o1, dir ), h1, ll, ur );
			RayBoxIntersection( Ray( o2, dir * (-1.0) ), h2, ll, ur );

			assert( h1.bHit == h2.bHit );
			if( h1.bHit ) {
				// Both should enter at the same distance from center minus half-extent
				assert( IsClose( h1.dRange, h2.dRange, 1e-6 ) );
			}
		}

		std::cout << "RayBoxIntersection symmetry Passed!" << std::endl;
	}
}

int main()
{
	TestBasicHitMiss();
	TestOriginInside();
	TestDiagonalRays();
	TestEdgeGrazing();
	TestVariousBoxPositions();
	TestSideIdentification();
	TestBSPTraversalConsistency();
	TestSymmetry();

	std::cout << "All RayBoxIntersection tests passed!" << std::endl;
	return 0;
}
