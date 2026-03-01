#include <iostream>
#include <vector>
#include <cassert>
#include <cmath>
#include "../src/Library/PhotonMapping/IrradianceCache.h"

using namespace RISE;
using namespace RISE::Implementation;

bool IsClose(Scalar a, Scalar b, Scalar epsilon = 1e-8) {
	return std::fabs(a - b) <= epsilon;
}

bool IsPelClose(const RISEPel& a, const RISEPel& b, Scalar epsilon = 1e-8) {
	return IsClose(a[0], b[0], epsilon) &&
		IsClose(a[1], b[1], epsilon) &&
		IsClose(a[2], b[2], epsilon);
}

void TestGradientDataRoundTripsThroughQuery() {
	std::cout << "Testing IrradianceCache gradient round-trip..." << std::endl;

	IIrradianceCache* pCache = new IrradianceCache(16.0, 0.2, 0.01, 100.0);

	const Point3 p(1.0, 1.0, 1.0);
	const Vector3 n(0.0, 1.0, 0.0);
	const RISEPel c(0.2, 0.4, 0.6);

	RISEPel rot[3] = {
		RISEPel(0.11, 0.12, 0.13),
		RISEPel(0.21, 0.22, 0.23),
		RISEPel(0.31, 0.32, 0.33)
	};

	RISEPel trans[3] = {
		RISEPel(0.41, 0.42, 0.43),
		RISEPel(0.51, 0.52, 0.53),
		RISEPel(0.61, 0.62, 0.63)
	};

	pCache->InsertElement(p, n, c, 1.0, rot, trans);
	pCache->FinishedPrecomputation();

	std::vector<IIrradianceCache::CacheElement> results;
	const Scalar weights = pCache->Query(p, n, results);

	assert(weights > 0);
	assert(results.size() == 1);

	for (int i = 0; i < 3; i++) {
		assert(IsPelClose(results[0].rotationalGradient[i], rot[i]));
		assert(IsPelClose(results[0].translationalGradient[i], trans[i]));
	}

	safe_release(pCache);
	std::cout << "IrradianceCache gradient round-trip Passed!" << std::endl;
}

void TestNullGradientsDefaultToZero() {
	std::cout << "Testing IrradianceCache null gradient default..." << std::endl;

	IIrradianceCache* pCache = new IrradianceCache(16.0, 0.2, 0.01, 100.0);

	const Point3 p(2.0, 2.0, 2.0);
	const Vector3 n(0.0, 1.0, 0.0);
	const RISEPel c(0.7, 0.8, 0.9);

	pCache->InsertElement(p, n, c, 1.0, 0, 0);
	pCache->FinishedPrecomputation();

	std::vector<IIrradianceCache::CacheElement> results;
	const Scalar weights = pCache->Query(p, n, results);

	assert(weights > 0);
	assert(results.size() == 1);

	const RISEPel zero(0.0, 0.0, 0.0);
	for (int i = 0; i < 3; i++) {
		assert(IsPelClose(results[0].rotationalGradient[i], zero));
		assert(IsPelClose(results[0].translationalGradient[i], zero));
	}

	safe_release(pCache);
	std::cout << "IrradianceCache null gradient default Passed!" << std::endl;
}

void TestQueryAccruesAcrossTreeLevels() {
	std::cout << "Testing IrradianceCache hierarchical query accrual..." << std::endl;

	IIrradianceCache* pCache = new IrradianceCache(16.0, 0.2, 0.01, 100.0);

	const Vector3 n(0.0, 1.0, 0.0);
	const Point3 query(1.0, 1.0, 1.0);

	// Large radius record should be kept high in the tree.
	pCache->InsertElement(Point3(0.0, 0.0, 0.0), n, RISEPel(1.0, 0.0, 0.0), 10.0, 0, 0);
	// Small radius record should be inserted deeper in the tree.
	pCache->InsertElement(query, n, RISEPel(0.0, 1.0, 0.0), 0.05, 0, 0);
	pCache->FinishedPrecomputation();

	std::vector<IIrradianceCache::CacheElement> results;
	const Scalar weights = pCache->Query(query, n, results);

	assert(weights > 0);
	assert(results.size() == 2);

	bool sawRed = false;
	bool sawGreen = false;
	for (size_t i = 0; i < results.size(); i++) {
		if (IsPelClose(results[i].cIRad, RISEPel(1.0, 0.0, 0.0))) {
			sawRed = true;
		}
		if (IsPelClose(results[i].cIRad, RISEPel(0.0, 1.0, 0.0))) {
			sawGreen = true;
		}
	}

	assert(sawRed);
	assert(sawGreen);

	safe_release(pCache);
	std::cout << "IrradianceCache hierarchical query accrual Passed!" << std::endl;
}

void TestBehindPlaneRecordIsRejected() {
	std::cout << "Testing IrradianceCache behind-plane rejection..." << std::endl;

	IIrradianceCache* pCache = new IrradianceCache(16.0, 0.2, 0.01, 100.0);
	const Vector3 n(0.0, 1.0, 0.0);
	const Point3 recordPos(0.0, 0.0, 0.0);

	pCache->InsertElement(recordPos, n, RISEPel(0.2, 0.2, 0.2), 1.0, 0, 0);
	pCache->FinishedPrecomputation();

	// In front of the record's tangent plane: should reuse the sample.
	{
		std::vector<IIrradianceCache::CacheElement> results;
		const Scalar weights = pCache->Query(Point3(0.0, 0.01, 0.0), n, results);
		assert(weights > 0);
		assert(results.size() == 1);
		assert(pCache->IsSampleNeeded(Point3(0.0, 0.01, 0.0), n) == false);
	}

	// Behind the record's tangent plane with similarly-oriented normals:
	// should still be reusable (we intentionally avoid rejecting these to
	// prevent tiny offset holes on the same surface).
	{
		std::vector<IIrradianceCache::CacheElement> results;
		const Scalar weights = pCache->Query(Point3(0.0, -0.01, 0.0), n, results);
		assert(weights > 0);
		assert(results.size() == 1);
		assert(pCache->IsSampleNeeded(Point3(0.0, -0.01, 0.0), n) == false);
	}

	// Behind the tangent plane with opposing normals should be rejected.
	{
		std::vector<IIrradianceCache::CacheElement> results;
		const Vector3 opposite(0.0, -1.0, 0.0);
		const Scalar weights = pCache->Query(Point3(0.0, -0.01, 0.0), opposite, results);
		assert(weights == 0);
		assert(results.empty());
		assert(pCache->IsSampleNeeded(Point3(0.0, -0.01, 0.0), opposite) == true);
	}

	safe_release(pCache);
	std::cout << "IrradianceCache behind-plane rejection Passed!" << std::endl;
}

void TestNeighborCellReuseAcrossBoundary() {
	std::cout << "Testing IrradianceCache neighbor-cell reuse..." << std::endl;

	IIrradianceCache* pCache = new IrradianceCache(16.0, 0.2, 0.01, 100.0);
	const Vector3 n(0.0, 1.0, 0.0);

	// This r0 is small enough to force insertion into a child node.
	const Scalar r0 = 0.5;
	const Point3 samplePos(0.001, 0.0, 0.0);      // Right side of the root split plane (x > 0)
	const Point3 queryPos(-0.001, 0.0, 0.0);      // Left side of the root split plane (x < 0)

	pCache->InsertElement(samplePos, n, RISEPel(0.3, 0.3, 0.3), r0, 0, 0);
	pCache->FinishedPrecomputation();

	std::vector<IIrradianceCache::CacheElement> results;
	const Scalar weights = pCache->Query(queryPos, n, results);

	assert(weights > 0);
	assert(results.size() == 1);
	assert(pCache->IsSampleNeeded(queryPos, n) == false);

	safe_release(pCache);
	std::cout << "IrradianceCache neighbor-cell reuse Passed!" << std::endl;
}

void TestWeightFallsOffWithDistanceAndAngle() {
	std::cout << "Testing IrradianceCache weight monotonicity..." << std::endl;

	IIrradianceCache::CacheElement elem(
		Point3(0.0, 0.0, 0.0),
		Vector3(0.0, 1.0, 0.0),
		RISEPel(0.2, 0.2, 0.2),
		1.0,
		0.0,
		0,
		0
		);

	const Scalar nearWeight = elem.ComputeWeight(Point3(0.1, 0.0, 0.0), Vector3(0.0, 1.0, 0.0));
	const Scalar farWeight = elem.ComputeWeight(Point3(0.5, 0.0, 0.0), Vector3(0.0, 1.0, 0.0));
	assert(nearWeight > farWeight);

	const Vector3 tiltedNormal(0.0, 0.70710678118, 0.70710678118);
	const Scalar alignedWeight = elem.ComputeWeight(Point3(0.1, 0.0, 0.0), Vector3(0.0, 1.0, 0.0));
	const Scalar tiltedWeight = elem.ComputeWeight(Point3(0.1, 0.0, 0.0), tiltedNormal);
	assert(alignedWeight > tiltedWeight);

	std::cout << "IrradianceCache weight monotonicity Passed!" << std::endl;
}

void TestQueryUsesSofterThresholdThanSampleNeeded() {
	std::cout << "Testing IrradianceCache query/sample threshold split..." << std::endl;

	IIrradianceCache* pCache = new IrradianceCache(16.0, 0.2, 0.01, 100.0);
	const Vector3 n(0.0, 1.0, 0.0);

	// invTolerance = 1/0.2 = 5.0, query threshold = 2.5
	// Same normal => weight is r0 / distance. Choose distance so 2.5 < w < 5.0.
	pCache->InsertElement(Point3(0.0, 0.0, 0.0), n, RISEPel(0.4, 0.4, 0.4), 1.0, 0, 0);
	pCache->FinishedPrecomputation();

	const Point3 queryPt(0.3, 0.0, 0.0); // Weight ~= 3.33
	std::vector<IIrradianceCache::CacheElement> results;
	const Scalar weights = pCache->Query(queryPt, n, results);

	assert(weights > 0);
	assert(results.size() == 1);
	assert(results[0].dWeight > 2.5);
	assert(results[0].dWeight < 5.0);
	assert(pCache->IsSampleNeeded(queryPt, n) == true);

	safe_release(pCache);
	std::cout << "IrradianceCache query/sample threshold split Passed!" << std::endl;
}

int main() {
	TestGradientDataRoundTripsThroughQuery();
	TestNullGradientsDefaultToZero();
	TestQueryAccruesAcrossTreeLevels();
	TestBehindPlaneRecordIsRejected();
	TestNeighborCellReuseAcrossBoundary();
	TestWeightFallsOffWithDistanceAndAngle();
	TestQueryUsesSofterThresholdThanSampleNeeded();
	return 0;
}
