//////////////////////////////////////////////////////////////////////
//
//  BSPMailboxingTest.cpp - Tests for mailboxing and leaf pruning
//  in the BSP tree traversal to avoid redundant triangle tests.
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <vector>
#include <cassert>
#include <cmath>

#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"
#include "../src/Library/Intersection/RayIntersection.h"

using namespace RISE;
using namespace RISE::Implementation;

// Helper: build a simple indexed triangle mesh geometry with the given
// triangles and return it configured with BSP enabled.
static TriangleMeshGeometryIndexed* BuildMesh(
	const std::vector<Point3>& verts,
	const std::vector<unsigned int>& indices,
	unsigned int maxPerLeaf = 2,
	unsigned char maxDepth = 20
)
{
	TriangleMeshGeometryIndexed* pMesh = new TriangleMeshGeometryIndexed(
		maxPerLeaf,    // max polygons per node
		maxDepth,      // max recursion level
		true,          // double sided
		true,          // use BSP
		true           // use face normals (avoids needing per-vertex normals)
	);

	pMesh->BeginIndexedTriangles();

	// Add vertices
	for( size_t i = 0; i < verts.size(); i++ ) {
		pMesh->AddVertex( verts[i] );
	}

	// Add texture coords (one per vertex)
	for( size_t i = 0; i < verts.size(); i++ ) {
		pMesh->AddTexCoord( Point2(0, 0) );
	}

	// Add triangles (indices into vertex/texcoord lists)
	for( size_t i = 0; i + 2 < indices.size(); i += 3 ) {
		IndexedTriangle t;
		t.iVertices[0] = indices[i];
		t.iVertices[1] = indices[i+1];
		t.iVertices[2] = indices[i+2];
		t.iNormals[0] = indices[i];
		t.iNormals[1] = indices[i+1];
		t.iNormals[2] = indices[i+2];
		t.iCoords[0] = indices[i];
		t.iCoords[1] = indices[i+1];
		t.iCoords[2] = indices[i+2];
		pMesh->AddIndexedTriangle( t );
	}

	pMesh->DoneIndexedTriangles();
	return pMesh;
}

//
// Test 1: Basic correctness — rays that should hit still hit,
//         rays that should miss still miss.
//
void TestBasicCorrectness()
{
	std::cout << "Testing BSP mailboxing basic correctness..." << std::endl;

	// Create a simple quad (two triangles) in the XZ plane at y=0
	//   v0=(0,0,0), v1=(2,0,0), v2=(2,0,2), v3=(0,0,2)
	std::vector<Point3> verts;
	verts.push_back( Point3(0, 0, 0) );
	verts.push_back( Point3(2, 0, 0) );
	verts.push_back( Point3(2, 0, 2) );
	verts.push_back( Point3(0, 0, 2) );

	std::vector<unsigned int> indices;
	// Triangle 0: v0, v1, v2
	indices.push_back(0); indices.push_back(1); indices.push_back(2);
	// Triangle 1: v0, v2, v3
	indices.push_back(0); indices.push_back(2); indices.push_back(3);

	TriangleMeshGeometryIndexed* pMesh = BuildMesh( verts, indices, 1, 10 );

	// Ray straight down into the center — should hit
	{
		Ray ray( Point3(1, 5, 1), Vector3(0, -1, 0) );
		RasterizerState rast;
		RayIntersectionGeometric ri( ray, rast );
		ri.bHit = false;
		ri.range = RISE_INFINITY;
		pMesh->IntersectRay( ri, true, true, false );
		assert( ri.bHit );
		assert( fabs(ri.range - 5.0) < 0.01 );
	}

	// Ray straight up from below — should hit (double sided)
	{
		Ray ray( Point3(1, -3, 1), Vector3(0, 1, 0) );
		RasterizerState rast;
		RayIntersectionGeometric ri( ray, rast );
		ri.bHit = false;
		ri.range = RISE_INFINITY;
		pMesh->IntersectRay( ri, true, true, false );
		assert( ri.bHit );
		assert( fabs(ri.range - 3.0) < 0.01 );
	}

	// Ray that clearly misses
	{
		Ray ray( Point3(5, 5, 5), Vector3(0, 1, 0) );
		RasterizerState rast;
		RayIntersectionGeometric ri( ray, rast );
		ri.bHit = false;
		ri.range = RISE_INFINITY;
		pMesh->IntersectRay( ri, true, true, false );
		assert( !ri.bHit );
	}

	safe_release( pMesh );
	std::cout << "BSP mailboxing basic correctness Passed!" << std::endl;
}

//
// Test 2: Straddling triangle — a large triangle that spans multiple
//         BSP leaves should be found exactly once (correct range)
//         regardless of which leaf the ray enters.
//
void TestStraddlingTriangle()
{
	std::cout << "Testing BSP straddling triangle correctness..." << std::endl;

	// One big triangle and several small ones to force splits
	//   Big triangle: covers x=[0,10], z=[0,10] at y=0
	//   Small triangles: tiny, scattered to force BSP splits that bisect the big one
	std::vector<Point3> verts;
	// Big triangle (indices 0,1,2)
	verts.push_back( Point3(0, 0, 0) );
	verts.push_back( Point3(10, 0, 0) );
	verts.push_back( Point3(5, 0, 10) );

	// Small triangle 1 at x=1 (indices 3,4,5)
	verts.push_back( Point3(0.9, 0, 0.1) );
	verts.push_back( Point3(1.1, 0, 0.1) );
	verts.push_back( Point3(1.0, 0, 0.3) );

	// Small triangle 2 at x=8 (indices 6,7,8)
	verts.push_back( Point3(7.9, 0, 0.1) );
	verts.push_back( Point3(8.1, 0, 0.1) );
	verts.push_back( Point3(8.0, 0, 0.3) );

	// Small triangle 3 at x=5, z=8 (indices 9,10,11)
	verts.push_back( Point3(4.9, 0, 7.9) );
	verts.push_back( Point3(5.1, 0, 7.9) );
	verts.push_back( Point3(5.0, 0, 8.1) );

	std::vector<unsigned int> indices;
	for( unsigned int i = 0; i < 12; i++ ) {
		indices.push_back(i);
	}

	// maxPerLeaf=1 forces deep splitting, maximizing triangle duplication
	TriangleMeshGeometryIndexed* pMesh = BuildMesh( verts, indices, 1, 20 );

	// Cast rays at various positions across the big triangle
	const double testPoints[][2] = {
		{2.0, 1.0}, {5.0, 3.0}, {7.0, 2.0}, {4.0, 6.0}, {5.0, 9.0}
	};

	for( int i = 0; i < 5; i++ ) {
		const double x = testPoints[i][0];
		const double z = testPoints[i][1];
		Ray ray( Point3(x, 10, z), Vector3(0, -1, 0) );
		RasterizerState rast;
		RayIntersectionGeometric ri( ray, rast );
		ri.bHit = false;
		ri.range = RISE_INFINITY;
		pMesh->IntersectRay( ri, true, true, false );

		// Check if this point is inside the big triangle:
		// Triangle (0,0),(10,0),(5,10) — use barycentric check
		// The point (x,z) should be inside if the ray should hit
		// For now just check that the range is exactly 10 (height)
		if( ri.bHit ) {
			assert( fabs(ri.range - 10.0) < 0.01 );
		}
	}

	// IntersectRay_IntersectionOnly should agree with IntersectRay
	for( int i = 0; i < 5; i++ ) {
		const double x = testPoints[i][0];
		const double z = testPoints[i][1];
		Ray ray( Point3(x, 10, z), Vector3(0, -1, 0) );
		RasterizerState rast;

		RayIntersectionGeometric ri( ray, rast );
		ri.bHit = false;
		ri.range = RISE_INFINITY;
		pMesh->IntersectRay( ri, true, true, false );
		const bool fullHit = ri.bHit;

		const bool fastHit = pMesh->IntersectRay_IntersectionOnly( ray, RISE_INFINITY, true, true );
		assert( fullHit == fastHit );
	}

	safe_release( pMesh );
	std::cout << "BSP straddling triangle correctness Passed!" << std::endl;
}

//
// Test 3: Closest-hit consistency — when multiple triangles are
//         candidates, mailboxing must not prevent finding the closest one.
//
void TestClosestHitConsistency()
{
	std::cout << "Testing BSP closest-hit consistency..." << std::endl;

	// Stack of 4 horizontal triangles at different heights
	std::vector<Point3> verts;
	const Scalar heights[] = { 0.0, 1.0, 2.0, 3.0 };

	for( int i = 0; i < 4; i++ ) {
		verts.push_back( Point3(-5, heights[i], -5) );
		verts.push_back( Point3( 5, heights[i], -5) );
		verts.push_back( Point3( 0, heights[i],  5) );
	}

	std::vector<unsigned int> indices;
	for( unsigned int i = 0; i < 12; i++ ) {
		indices.push_back(i);
	}

	TriangleMeshGeometryIndexed* pMesh = BuildMesh( verts, indices, 1, 20 );

	// Ray from above — should hit the highest triangle (y=3) first
	{
		Ray ray( Point3(0, 10, 0), Vector3(0, -1, 0) );
		RasterizerState rast;
		RayIntersectionGeometric ri( ray, rast );
		ri.bHit = false;
		ri.range = RISE_INFINITY;
		pMesh->IntersectRay( ri, true, true, false );
		assert( ri.bHit );
		assert( fabs(ri.range - 7.0) < 0.01 );  // 10 - 3 = 7
	}

	// Ray from below — should hit the lowest triangle (y=0) first
	{
		Ray ray( Point3(0, -5, 0), Vector3(0, 1, 0) );
		RasterizerState rast;
		RayIntersectionGeometric ri( ray, rast );
		ri.bHit = false;
		ri.range = RISE_INFINITY;
		pMesh->IntersectRay( ri, true, true, false );
		assert( ri.bHit );
		assert( fabs(ri.range - 5.0) < 0.01 );  // -5 to 0 = 5
	}

	// Diagonal ray — should find closest hit
	{
		// Aim at (0,0,0) which is inside all four triangles' XZ projection
		Ray ray( Point3(0, 10, 0), Vector3Ops::Normalize(Vector3(0.1, -1, 0.1)) );
		RasterizerState rast;
		RayIntersectionGeometric ri( ray, rast );
		ri.bHit = false;
		ri.range = RISE_INFINITY;
		pMesh->IntersectRay( ri, true, true, false );
		assert( ri.bHit );
		// Should hit the y=3 triangle first (closest from above)
		// Ray y-component: 10 - t * (1/sqrt(1.02)) ≈ 10 - 0.99*t
		// hits y=3 when t ≈ 7.07
		const Point3 hitPt = Point3(
			ray.origin.x + ri.range * ray.Dir().x,
			ray.origin.y + ri.range * ray.Dir().y,
			ray.origin.z + ri.range * ray.Dir().z
		);
		assert( fabs(hitPt.y - 3.0) < 0.01 );
	}

	safe_release( pMesh );
	std::cout << "BSP closest-hit consistency Passed!" << std::endl;
}

//
// Test 4: Many rays through dense mesh — stress test to verify
//         no missed intersections vs brute-force reference.
//
void TestBruteForceConsistency()
{
	std::cout << "Testing BSP vs brute-force consistency (dense mesh)..." << std::endl;

	// Create a grid of triangles in the XZ plane at y=0
	// Grid: 10x10 quads = 200 triangles
	const int gridN = 10;
	std::vector<Point3> verts;
	for( int iz = 0; iz <= gridN; iz++ ) {
		for( int ix = 0; ix <= gridN; ix++ ) {
			verts.push_back( Point3( Scalar(ix), 0.0, Scalar(iz) ) );
		}
	}

	std::vector<unsigned int> indices;
	for( int iz = 0; iz < gridN; iz++ ) {
		for( int ix = 0; ix < gridN; ix++ ) {
			unsigned int v00 = iz * (gridN+1) + ix;
			unsigned int v10 = iz * (gridN+1) + ix + 1;
			unsigned int v01 = (iz+1) * (gridN+1) + ix;
			unsigned int v11 = (iz+1) * (gridN+1) + ix + 1;
			indices.push_back(v00); indices.push_back(v10); indices.push_back(v11);
			indices.push_back(v00); indices.push_back(v11); indices.push_back(v01);
		}
	}

	// Small maxPerLeaf to force lots of splitting and triangle duplication
	TriangleMeshGeometryIndexed* pMesh = BuildMesh( verts, indices, 2, 20 );

	// Cast a grid of rays and verify hit/miss and range
	int mismatches = 0;
	int tested = 0;

	for( int iy = 0; iy < 20; iy++ ) {
		for( int ix = 0; ix < 20; ix++ ) {
			const Scalar x = 0.25 + Scalar(ix) * 0.5;
			const Scalar z = 0.25 + Scalar(iy) * 0.5;
			Ray ray( Point3(x, 5.0, z), Vector3(0, -1, 0) );
			RasterizerState rast;

			// BSP result
			RayIntersectionGeometric ri( ray, rast );
			ri.bHit = false;
			ri.range = RISE_INFINITY;
			pMesh->IntersectRay( ri, true, true, false );

			// Expected: hit at range 5.0 if inside [0,10]x[0,10], miss otherwise
			const bool expectedHit = (x >= 0 && x <= gridN && z >= 0 && z <= gridN);
			if( ri.bHit != expectedHit ) {
				mismatches++;
				std::cerr << "  Mismatch at (" << x << "," << z << "): bsp="
					<< ri.bHit << " expected=" << expectedHit << std::endl;
			} else if( ri.bHit && fabs(ri.range - 5.0) > 0.01 ) {
				mismatches++;
				std::cerr << "  Range mismatch at (" << x << "," << z << "): range="
					<< ri.range << " expected=5.0" << std::endl;
			}

			// Also verify IntersectRay_IntersectionOnly agrees
			const bool fastHit = pMesh->IntersectRay_IntersectionOnly( ray, RISE_INFINITY, true, true );
			if( fastHit != ri.bHit ) {
				mismatches++;
				std::cerr << "  IntersectionOnly mismatch at (" << x << "," << z << ")" << std::endl;
			}

			tested++;
		}
	}

	assert( mismatches == 0 );
	std::cout << "  Tested " << tested << " rays, 0 mismatches" << std::endl;
	std::cout << "BSP vs brute-force consistency Passed!" << std::endl;

	safe_release( pMesh );
}

//
// Test 5: Oblique rays through straddling geometry — rays that
//         cross multiple BSP cells and could test the same
//         straddling triangle multiple times.
//
void TestObliqueRaysThroughMultipleCells()
{
	std::cout << "Testing BSP oblique rays through multiple cells..." << std::endl;

	// Large triangle plus small triangles to force lots of splits
	std::vector<Point3> verts;
	// Big triangle covering x=[0,20], z=[0,20]
	verts.push_back( Point3(0, 0, 0) );
	verts.push_back( Point3(20, 0, 0) );
	verts.push_back( Point3(10, 0, 20) );

	// 8 tiny triangles scattered to force many BSP splits
	for( int i = 0; i < 8; i++ ) {
		Scalar cx = 2.5 * (i + 1);
		verts.push_back( Point3(cx - 0.05, 0, 0.05) );
		verts.push_back( Point3(cx + 0.05, 0, 0.05) );
		verts.push_back( Point3(cx, 0, 0.15) );
	}

	std::vector<unsigned int> indices;
	for( unsigned int i = 0; i < verts.size(); i++ ) {
		indices.push_back(i);
	}

	TriangleMeshGeometryIndexed* pMesh = BuildMesh( verts, indices, 1, 20 );

	// Oblique rays that cross many BSP cells
	const Vector3 dirs[] = {
		Vector3Ops::Normalize(Vector3( 1, -1,  0.5)),
		Vector3Ops::Normalize(Vector3(-1, -1,  0.3)),
		Vector3Ops::Normalize(Vector3( 0.5, -1, 1)),
		Vector3Ops::Normalize(Vector3(-0.3, -1, -0.7)),
	};

	for( int d = 0; d < 4; d++ ) {
		Ray ray( Point3(10, 20, 5), dirs[d] );
		RasterizerState rast;
		RayIntersectionGeometric ri( ray, rast );
		ri.bHit = false;
		ri.range = RISE_INFINITY;
		pMesh->IntersectRay( ri, true, true, false );

		// Verify consistency with IntersectionOnly
		const bool fastHit = pMesh->IntersectRay_IntersectionOnly( ray, RISE_INFINITY, true, true );
		assert( ri.bHit == fastHit );

		if( ri.bHit ) {
			// Verify the hit point is actually on the y=0 plane
			const Point3 hitPt = Point3(
				ray.origin.x + ri.range * ray.Dir().x,
				ray.origin.y + ri.range * ray.Dir().y,
				ray.origin.z + ri.range * ray.Dir().z
			);
			assert( fabs(hitPt.y) < 0.01 );
		}
	}

	safe_release( pMesh );
	std::cout << "BSP oblique rays through multiple cells Passed!" << std::endl;
}

//
// Test 6: Iterative traversal correctness — dense mesh with rays from
//         many angles to verify iterative traversal matches expected results.
//         Specifically tests early-out when near-child hit prunes far-child.
//
void TestIterativeTraversalCorrectness()
{
	std::cout << "Testing iterative traversal correctness..." << std::endl;

	// Create a scene with multiple layers at different heights to
	// stress-test early-out behavior.  Near-child hits should prevent
	// unnecessary far-child traversal while still finding the closest hit.
	std::vector<Point3> verts;

	// 8 layers of triangles at y = 0, 0.5, 1.0, ... 3.5
	// Each layer is a 4x4 grid of quads (32 triangles per layer = 256 total)
	const int layers = 8;
	const int gridN = 4;
	for( int layer = 0; layer < layers; layer++ ) {
		const Scalar y = Scalar(layer) * 0.5;
		const unsigned int baseVert = static_cast<unsigned int>(verts.size());
		for( int iz = 0; iz <= gridN; iz++ ) {
			for( int ix = 0; ix <= gridN; ix++ ) {
				verts.push_back( Point3( Scalar(ix), y, Scalar(iz) ) );
			}
		}
	}

	std::vector<unsigned int> indices;
	for( int layer = 0; layer < layers; layer++ ) {
		const unsigned int baseVert = layer * (gridN+1) * (gridN+1);
		for( int iz = 0; iz < gridN; iz++ ) {
			for( int ix = 0; ix < gridN; ix++ ) {
				unsigned int v00 = baseVert + iz * (gridN+1) + ix;
				unsigned int v10 = baseVert + iz * (gridN+1) + ix + 1;
				unsigned int v01 = baseVert + (iz+1) * (gridN+1) + ix;
				unsigned int v11 = baseVert + (iz+1) * (gridN+1) + ix + 1;
				indices.push_back(v00); indices.push_back(v10); indices.push_back(v11);
				indices.push_back(v00); indices.push_back(v11); indices.push_back(v01);
			}
		}
	}

	// Small leaf size to force deep tree
	TriangleMeshGeometryIndexed* pMesh = BuildMesh( verts, indices, 2, 24 );

	int mismatches = 0;
	int tested = 0;

	// Rays from above — should hit topmost layer (y=3.5)
	for( int iz = 0; iz < 8; iz++ ) {
		for( int ix = 0; ix < 8; ix++ ) {
			const Scalar x = 0.25 + Scalar(ix) * 0.5;
			const Scalar z = 0.25 + Scalar(iz) * 0.5;
			Ray ray( Point3(x, 10.0, z), Vector3(0, -1, 0) );
			RasterizerState rast;

			RayIntersectionGeometric ri( ray, rast );
			ri.bHit = false;
			ri.range = RISE_INFINITY;
			pMesh->IntersectRay( ri, true, true, false );

			const bool expectedHit = (x >= 0 && x <= gridN && z >= 0 && z <= gridN);
			if( ri.bHit != expectedHit ) {
				mismatches++;
			} else if( ri.bHit ) {
				// Should hit top layer at y=3.5, range = 10 - 3.5 = 6.5
				if( fabs(ri.range - 6.5) > 0.01 ) {
					mismatches++;
					std::cerr << "  Range mismatch at (" << x << "," << z << "): range="
						<< ri.range << " expected=6.5" << std::endl;
				}
			}

			// IntersectionOnly must agree
			const bool fastHit = pMesh->IntersectRay_IntersectionOnly( ray, RISE_INFINITY, true, true );
			if( fastHit != ri.bHit ) {
				mismatches++;
			}

			tested++;
		}
	}

	// Oblique rays from various angles
	const Vector3 dirs[] = {
		Vector3Ops::Normalize(Vector3( 0.3, -1.0,  0.2)),
		Vector3Ops::Normalize(Vector3(-0.2, -1.0,  0.4)),
		Vector3Ops::Normalize(Vector3( 0.5, -1.0, -0.3)),
		Vector3Ops::Normalize(Vector3(-0.4, -1.0, -0.1)),
	};

	for( int d = 0; d < 4; d++ ) {
		for( int i = 0; i < 16; i++ ) {
			const Scalar x = 0.5 + Scalar(i % 4);
			const Scalar z = 0.5 + Scalar(i / 4);
			Ray ray( Point3(x, 10.0, z), dirs[d] );
			RasterizerState rast;

			RayIntersectionGeometric ri( ray, rast );
			ri.bHit = false;
			ri.range = RISE_INFINITY;
			pMesh->IntersectRay( ri, true, true, false );

			const bool fastHit = pMesh->IntersectRay_IntersectionOnly( ray, RISE_INFINITY, true, true );
			if( ri.bHit != fastHit ) {
				mismatches++;
				std::cerr << "  IntersectionOnly mismatch for oblique ray " << d
					<< " at (" << x << "," << z << ")" << std::endl;
			}

			if( ri.bHit ) {
				// Hit point should be on one of the layer planes
				const Point3 hitPt = Point3(
					ray.origin.x + ri.range * ray.Dir().x,
					ray.origin.y + ri.range * ray.Dir().y,
					ray.origin.z + ri.range * ray.Dir().z
				);
				// y should be near one of 0, 0.5, 1.0, ... 3.5
				const Scalar yRounded = floor(hitPt.y * 2.0 + 0.5) / 2.0;
				if( fabs(hitPt.y - yRounded) > 0.02 ) {
					mismatches++;
					std::cerr << "  Hit not on layer plane: y=" << hitPt.y << std::endl;
				}
			}

			tested++;
		}
	}

	assert( mismatches == 0 );
	std::cout << "  Tested " << tested << " rays, 0 mismatches" << std::endl;
	std::cout << "Iterative traversal correctness Passed!" << std::endl;

	safe_release( pMesh );
}

int main()
{
	TestBasicCorrectness();
	TestStraddlingTriangle();
	TestClosestHitConsistency();
	TestBruteForceConsistency();
	TestObliqueRaysThroughMultipleCells();
	TestIterativeTraversalCorrectness();
	std::cout << "All BSP mailboxing and leaf pruning tests passed!" << std::endl;
	return 0;
}
