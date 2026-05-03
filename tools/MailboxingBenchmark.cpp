//////////////////////////////////////////////////////////////////////
//
//  MailboxingBenchmark.cpp - Performance micro-benchmark for
//  thread_local mailboxing. This is an informational benchmark,
//  not an assertion-style correctness test.
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <vector>
#include <cassert>
#include <cmath>
#include <chrono>
#include <algorithm>

#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"
#include "../src/Library/Intersection/RayIntersection.h"

using namespace RISE;
using namespace RISE::Implementation;

static TriangleMeshGeometryIndexed* BuildDenseMesh(int gridN)
{
	TriangleMeshGeometryIndexed* pMesh = new TriangleMeshGeometryIndexed(
		true, true);
	pMesh->BeginIndexedTriangles();

	for(int iz = 0; iz <= gridN; iz++) {
		for(int ix = 0; ix <= gridN; ix++) {
			pMesh->AddVertex(Point3(Scalar(ix), 0.0, Scalar(iz)));
		}
	}
	for(int i = 0; i < (gridN+1)*(gridN+1); i++) {
		pMesh->AddTexCoord(Point2(0,0));
	}
	for(int iz = 0; iz < gridN; iz++) {
		for(int ix = 0; ix < gridN; ix++) {
			unsigned int v00 = iz*(gridN+1)+ix;
			unsigned int v10 = iz*(gridN+1)+ix+1;
			unsigned int v01 = (iz+1)*(gridN+1)+ix;
			unsigned int v11 = (iz+1)*(gridN+1)+ix+1;
			IndexedTriangle t1, t2;
			t1.iVertices[0]=v00; t1.iVertices[1]=v10; t1.iVertices[2]=v11;
			t1.iNormals[0]=v00; t1.iNormals[1]=v10; t1.iNormals[2]=v11;
			t1.iCoords[0]=v00; t1.iCoords[1]=v10; t1.iCoords[2]=v11;
			t2.iVertices[0]=v00; t2.iVertices[1]=v11; t2.iVertices[2]=v01;
			t2.iNormals[0]=v00; t2.iNormals[1]=v11; t2.iNormals[2]=v01;
			t2.iCoords[0]=v00; t2.iCoords[1]=v11; t2.iCoords[2]=v01;
			pMesh->AddIndexedTriangle(t1);
			pMesh->AddIndexedTriangle(t2);
		}
	}
	pMesh->DoneIndexedTriangles();
	return pMesh;
}

int main()
{
	std::cout << "Mailboxing Benchmark" << std::endl;
	std::cout << "====================" << std::endl;

	const int gridN = 50;  // 50x50 grid = 5000 triangles
	TriangleMeshGeometryIndexed* pMesh = BuildDenseMesh(gridN);

	const int numRays = 100000;
	const int numIterations = 7;
	std::vector<double> timings;

	// Build ray list (mix of vertical and oblique)
	struct RayData { Point3 origin; Vector3 dir; };
	std::vector<RayData> rays(numRays);
	for(int i = 0; i < numRays; i++) {
		double fx = double(i % 200) * 0.25 + 0.125;
		double fz = double((i / 200) % 200) * 0.25 + 0.125;
		if(i % 3 == 0) {
			rays[i] = {Point3(fx, 10.0, fz), Vector3(0, -1, 0)};
		} else if(i % 3 == 1) {
			rays[i] = {Point3(fx, 10.0, fz),
				Vector3Ops::Normalize(Vector3(0.3, -1.0, 0.2))};
		} else {
			rays[i] = {Point3(fx, 10.0, fz),
				Vector3Ops::Normalize(Vector3(-0.2, -1.0, 0.4))};
		}
	}

	int totalHits = 0;
	for(int iter = 0; iter < numIterations; iter++) {
		int hits = 0;
		auto start = std::chrono::high_resolution_clock::now();

		for(int i = 0; i < numRays; i++) {
			RasterizerState rast;
			RayIntersectionGeometric ri(Ray(rays[i].origin, rays[i].dir), rast);
			ri.bHit = false;
			ri.range = RISE_INFINITY;
			pMesh->IntersectRay(ri, true, true, false);
			if(ri.bHit) hits++;
		}

		auto end = std::chrono::high_resolution_clock::now();
		double elapsed_ns = std::chrono::duration<double, std::nano>(end - start).count();
		double per_ray_ns = elapsed_ns / numRays;
		timings.push_back(per_ray_ns);

		if(iter == 0) totalHits = hits;
		std::cout << "  Iteration " << iter << ": " << per_ray_ns << " ns/ray (" << hits << " hits)" << std::endl;
	}

	std::sort(timings.begin(), timings.end());
	double median_ns = timings[numIterations / 2];
	std::cout << std::endl;
	std::cout << "Median: " << median_ns << " ns/ray" << std::endl;
	std::cout << "Hits: " << totalHits << " / " << numRays << std::endl;

	// Keep the basic intersection sanity check so benchmark runs still catch obvious breakage.
	assert(totalHits > numRays / 2);

	safe_release(pMesh);

	std::cout << std::endl;
	std::cout << "Mailboxing benchmark complete." << std::endl;
	std::cout << "Use the median ns/ray for before/after comparison." << std::endl;

	return 0;
}
