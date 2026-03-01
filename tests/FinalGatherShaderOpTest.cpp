#include <iostream>
#include <vector>
#include <cassert>
#include <cmath>
#include "../src/Library/Shaders/FinalGatherInterpolation.h"

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

IIrradianceCache::CacheElement MakeElement(
	const Point3& p,
	const Vector3& n,
	const RISEPel& c,
	const Scalar weight,
	const RISEPel* rot,
	const RISEPel* trans
	)
{
	return IIrradianceCache::CacheElement(p, n, c, 1.0, weight, rot, trans);
}

void TestEffectiveContributorsTracksWeightBalance() {
	std::cout << "Testing FinalGather effective contributor metric..." << std::endl;

	std::vector<IIrradianceCache::CacheElement> one;
	one.push_back(MakeElement(Point3(0, 0, 0), Vector3(0, 1, 0), RISEPel(1, 0, 0), 10.0, 0, 0));
	const Scalar oneEff = FinalGatherInterpolation::ComputeEffectiveContributors(one, 10.0);
	assert(IsClose(oneEff, 1.0));

	std::vector<IIrradianceCache::CacheElement> twoBalanced;
	twoBalanced.push_back(MakeElement(Point3(0, 0, 0), Vector3(0, 1, 0), RISEPel(1, 0, 0), 5.0, 0, 0));
	twoBalanced.push_back(MakeElement(Point3(0, 0, 0), Vector3(0, 1, 0), RISEPel(0, 1, 0), 5.0, 0, 0));
	const Scalar twoEff = FinalGatherInterpolation::ComputeEffectiveContributors(twoBalanced, 10.0);
	assert(IsClose(twoEff, 2.0));

	std::vector<IIrradianceCache::CacheElement> twoDominated;
	twoDominated.push_back(MakeElement(Point3(0, 0, 0), Vector3(0, 1, 0), RISEPel(1, 0, 0), 9.0, 0, 0));
	twoDominated.push_back(MakeElement(Point3(0, 0, 0), Vector3(0, 1, 0), RISEPel(0, 1, 0), 1.0, 0, 0));
	const Scalar dominatedEff = FinalGatherInterpolation::ComputeEffectiveContributors(twoDominated, 10.0);
	assert(dominatedEff < 2.0);
	assert(dominatedEff > 1.0);

	std::cout << "FinalGather effective contributor metric Passed!" << std::endl;
}

void TestInterpolationRequiresEnoughEffectiveContributors() {
	std::cout << "Testing FinalGather interpolation stability gate..." << std::endl;

	std::vector<IIrradianceCache::CacheElement> results;
	results.push_back(MakeElement(Point3(0, 0, 0), Vector3(0, 1, 0), RISEPel(0.4, 0.2, 0.1), 4.0, 0, 0));

	RISEPel c(1, 1, 1);
	const bool ok = FinalGatherInterpolation::TryInterpolate(
		Point3(0, 0, 0),
		Vector3(0, 1, 0),
		results,
		4.0,
		false,
		2.0,
		c,
		0
		);

	assert(ok == false);
	assert(IsPelClose(c, RISEPel(0, 0, 0)));

	std::cout << "FinalGather interpolation stability gate Passed!" << std::endl;
}

void TestInterpolationProducesWeightedAverage() {
	std::cout << "Testing FinalGather weighted interpolation..." << std::endl;

	std::vector<IIrradianceCache::CacheElement> results;
	results.push_back(MakeElement(Point3(0, 0, 0), Vector3(0, 1, 0), RISEPel(1, 0, 0), 2.0, 0, 0));
	results.push_back(MakeElement(Point3(0, 0, 0), Vector3(0, 1, 0), RISEPel(0, 1, 0), 1.0, 0, 0));

	RISEPel c(0, 0, 0);
	unsigned int fallbackCount = 99;
	const bool ok = FinalGatherInterpolation::TryInterpolate(
		Point3(0, 0, 0),
		Vector3(0, 1, 0),
		results,
		3.0,
		false,
		1.5,
		c,
		&fallbackCount
		);

	assert(ok == true);
	assert(fallbackCount == 0);
	assert(IsPelClose(c, RISEPel(2.0/3.0, 1.0/3.0, 0.0), 1e-7));

	std::cout << "FinalGather weighted interpolation Passed!" << std::endl;
}

void TestGradientFallbackRestoresBaseIrradiance() {
	std::cout << "Testing FinalGather gradient fallback..." << std::endl;

	RISEPel rot[3] = {
		RISEPel(0, 0, 0),
		RISEPel(0, 0, 0),
		RISEPel(0, 0, 0)
	};

	RISEPel trans[3] = {
		RISEPel(-1.0, 0, 0),
		RISEPel(0, 0, 0),
		RISEPel(0, 0, 0)
	};

	std::vector<IIrradianceCache::CacheElement> results;
	results.push_back(MakeElement(Point3(0, 0, 0), Vector3(0, 1, 0), RISEPel(0.4, 0.5, 0.6), 2.0, rot, trans));

	RISEPel c(0, 0, 0);
	unsigned int fallbackCount = 0;
	const bool ok = FinalGatherInterpolation::TryInterpolate(
		Point3(1, 0, 0),
		Vector3(0, 1, 0),
		results,
		2.0,
		true,
		1.0,
		c,
		&fallbackCount
		);

	assert(ok == true);
	assert(fallbackCount == 1);
	assert(IsPelClose(c, RISEPel(0.4, 0.5, 0.6)));

	std::cout << "FinalGather gradient fallback Passed!" << std::endl;
}

void TestInterpolationClampsNegativeInputs() {
	std::cout << "Testing FinalGather interpolation positivity clamp..." << std::endl;

	std::vector<IIrradianceCache::CacheElement> results;
	results.push_back(MakeElement(Point3(0, 0, 0), Vector3(0, 1, 0), RISEPel(-0.2, 0.2, -0.1), 1.0, 0, 0));

	RISEPel c(0, 0, 0);
	const bool ok = FinalGatherInterpolation::TryInterpolate(
		Point3(0, 0, 0),
		Vector3(0, 1, 0),
		results,
		1.0,
		false,
		1.0,
		c,
		0
		);

	assert(ok == true);
	assert(IsPelClose(c, RISEPel(0.0, 0.2, 0.0)));

	std::cout << "FinalGather interpolation positivity clamp Passed!" << std::endl;
}

int main() {
	TestEffectiveContributorsTracksWeightBalance();
	TestInterpolationRequiresEnoughEffectiveContributors();
	TestInterpolationProducesWeightedAverage();
	TestGradientFallbackRestoresBaseIrradiance();
	TestInterpolationClampsNegativeInputs();
	return 0;
}
