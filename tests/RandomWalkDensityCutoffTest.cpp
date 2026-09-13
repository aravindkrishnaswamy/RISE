//////////////////////////////////////////////////////////////////////
//
//  RandomWalkDensityCutoffTest.cpp - DL-57 dimensional density cutoff.
//
//  A collision density is dimensionful.  For neutral sigma_s=sigma_t,
//  its conditional importance weight is exactly one even when its numeric
//  value is below 1e-20.  These real closed-sphere fixtures make that
//  condition reachable, then retain a geometrically scaled counterpart
//  with the same optical depth and the same inverse-CDF variate.
//
//  Author: GPT-5.6 Terra <noreply@anthropic.com>
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <iomanip>
#include <iostream>
#include <vector>

#include "../src/Library/Utilities/BSSRDFSampling.h"
#include "../src/Library/Utilities/RandomWalkSSS.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Intersection/RayIntersection.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Objects/Object.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{

int gFailures = 0;

class SequenceSampler : public ISampler
{
public:
	explicit SequenceSampler( const std::vector<Scalar>& values ) :
		m_values(values), m_next(0), m_exhausted(false)
	{}

	Scalar Get1D() override
	{
		if( m_next == m_values.size() ) {
			m_exhausted = true;
			return 0.5;
		}
		const Scalar value = m_values[m_next++];
		if( !( value >= 0.0 && value < 1.0 ) ) {
			m_exhausted = true;
		}
		return value;
	}

	Point2 Get2D() override
	{
		const Scalar first = Get1D();
		const Scalar second = Get1D();
		return Point2( first, second );
	}

	bool Exhausted() const { return m_exhausted; }
	std::size_t Consumed() const { return m_next; }

private:
	const std::vector<Scalar> m_values;
	std::size_t m_next;
	bool m_exhausted;
};

bool Require( const bool condition, const char* message )
{
	if( !condition ) {
		std::cerr << "FAIL: " << message << std::endl;
		++gFailures;
	}
	return condition;
}

bool Finite( const Scalar value )
{
	return std::isfinite( value );
}

bool Close( const Scalar actual, const Scalar expected,
	const Scalar tolerance = 2e-12 )
{
	return Finite(actual) && Finite(expected) &&
		std::fabs(actual - expected) <= tolerance *
		std::fmax(Scalar(1.0), std::fabs(expected));
}

// A new Reference starts with one ownership reference. Object retains the
// geometry, so releasing the local geometry reference here is sufficient.
Object* MakeClosedSphere( const Scalar radius )
{
	SphereGeometry* geometry = new SphereGeometry( radius );
	Object* object = new Object( geometry );
	geometry->release();
	return object;
}

RayIntersectionGeometric MakeSouthPoleEntry( const Scalar radius )
{
	const Point3 point( 0, -radius, 0 );
	const Vector3 normal( 0, -1, 0 );
	const Vector3 incoming( 0, 1, 0 );
	RayIntersectionGeometric entry(
		Ray(Point3Ops::mkPoint3(point, -incoming * 2.0), incoming),
		nullRasterizerState );
	entry.bHit = true;
	entry.ptIntersection = point;
	entry.vNormal = normal;
	entry.vGeomNormal = normal;
	entry.onb.CreateFromW( normal );
	return entry;
}

bool MeasureBackFaceDistance( const Object& object, const Point3& origin,
	const Vector3& direction, Scalar& distance )
{
	RayIntersection hit( Ray(origin,direction), nullRasterizerState );
	object.IntersectRay( hit, RISE_INFINITY, false, true, false );
	if( !Require(hit.geometric.bHit,
		"closed sphere must have a real back-face continuation hit") ) {
		return false;
	}
	distance = hit.geometric.range;
	return Require(Finite(distance) && distance > 0.0,
		"measured closed-sphere continuation range must be finite and positive");
}

Scalar CollisionDistance( const Scalar rate, const Scalar xi )
{
	return -std::log( 1.0 - xi ) / rate;
}

Scalar IndependentAngularWeight( const BSSRDFSampling::SampleResult& sample )
{
	const Scalar cosine = Vector3Ops::Dot(sample.scatteredRay.Dir(),sample.entryNormal);
	return ( 1.0 - std::pow(1.0-cosine,5.0) ) / (20.0/21.0);
}

struct DensityCase
{
	Scalar radius;
	Scalar rate;
	Scalar wantedCollisionDistance;
	Scalar xi;
	Scalar initialDistance;
	Scalar collisionDistance;
	Scalar remainingDistance;
};

bool PrepareCase( const Object& sphere, const RayIntersectionGeometric& entry,
	DensityCase& testCase )
{
	const Vector3 direction = Vector3Ops::Normalize( entry.ray.Dir() );
	const Point3 initial = Point3Ops::mkPoint3(entry.ptIntersection,
		direction * BSSRDFSampling::BSSRDF_RAY_EPSILON);
	// Subtracting xi from one quantizes this tiny optical depth. Verify
	// the resulting physical scale, and use the actual sampled distance
	// (not the requested distance) in every density and geometry oracle.
	testCase.collisionDistance = CollisionDistance(testCase.rate,testCase.xi);
	const Point3 collision = Point3Ops::mkPoint3(initial,
		direction * testCase.collisionDistance);
	return MeasureBackFaceDistance(sphere,initial,direction,testCase.initialDistance) &&
		Require(Finite(testCase.collisionDistance) &&
			testCase.collisionDistance > 0.0 &&
			testCase.collisionDistance < testCase.initialDistance,
			"sampled collision must lie strictly inside the measured closed sphere") &&
		MeasureBackFaceDistance(sphere,collision,direction,testCase.remainingDistance) &&
		Require(std::fabs(testCase.collisionDistance / testCase.wantedCollisionDistance - 1.0) < 2e-4,
			"quantized inverse-CDF collision must reach the intended physical scale");
}

bool RequireActiveAndFinite( const BSSRDFSampling::SampleResult& sample,
	const SequenceSampler& sampler, const std::size_t expectedDraws, const char* label )
{
	if( !Require(sample.valid,label) ||
		!Require(!sampler.Exhausted() && sampler.Consumed() == expectedDraws,
			"active sample must consume exactly the specified finite draw sequence") ) {
		return false;
	}
	if( !Require(Finite(sample.weightNM) && Finite(sample.weightSpatialNM) &&
		Finite(sample.cosinePdf), "active scalar fields must be finite") ) {
		return false;
	}
	for( int channel = 0; channel < 3; ++channel ) {
		if( !Require(Finite(sample.weight[channel]) && Finite(sample.weightSpatial[channel]),
			"active RGB fields must be finite") ) {
			return false;
		}
	}
	return true;
}

bool RequireNeutralRGBWeights( const BSSRDFSampling::SampleResult& sample,
	const char* label )
{
	const Scalar angular = IndependentAngularWeight( sample );
	if( !Require(Finite(angular) && angular > 0.0,
		"independent angular continuation factor must be finite and positive") ) {
		return false;
	}
	bool correct = true;
	for( int channel = 0; channel < 3; ++channel ) {
		std::cout << "  RGB channel=" << channel << " spatial="
			<< sample.weightSpatial[channel] << " full=" << sample.weight[channel]
			<< " Sw=" << angular << std::endl;
		correct = Require(Close(sample.weightSpatial[channel],1.0),label) && correct;
		correct = Require(Close(sample.weight[channel],angular),
			"neutral RGB full weight must equal the independently derived Sw") && correct;
	}
	return correct;
}

void TestRGBDensityCutoffAndScaleInvariance()
{
	std::cout << "Test A: RGB collision density cutoff has a unit conditional weight" << std::endl;
	const Scalar xi = -std::expm1( -5e-13 );
	DensityCase large = { 1e8, 1e-20, 5e7, xi, 0.0, 0.0, 0.0 };
	DensityCase scaled = { 1.0, 1e-12, 0.5, xi, 0.0, 0.0, 0.0 };
	Object* largeSphere = MakeClosedSphere( large.radius );
	Object* scaledSphere = MakeClosedSphere( scaled.radius );
	const RayIntersectionGeometric largeEntry = MakeSouthPoleEntry(large.radius);
	const RayIntersectionGeometric scaledEntry = MakeSouthPoleEntry(scaled.radius);
	if( !PrepareCase(*largeSphere,largeEntry,large) ||
		!PrepareCase(*scaledSphere,scaledEntry,scaled) ||
		!Require(large.xi == scaled.xi,
			"scale-paired cases must use the same inverse-CDF variate") ||
		!Require(std::fabs((large.rate * large.collisionDistance) /
			(scaled.rate * scaled.collisionDistance) - 1.0) < 1e-12,
			"scale-paired sampled collisions must have the same optical depth") ||
		!Require(large.initialDistance > 1e8 && large.remainingDistance > 1e8,
			"large case must prove real initial and post-collision continuation ranges") ||
		!Require(scaled.initialDistance > 1.0 && scaled.remainingDistance > 1.0,
			"scaled case must prove real initial and post-collision continuation ranges") ) {
		largeSphere->release();
		scaledSphere->release();
		return;
	}

	BSSRDFSampling::SampleResult samples[2];
	bool active[2] = { false, false };
	for( int caseIndex = 0; caseIndex < 2; ++caseIndex ) {
		const DensityCase& testCase = caseIndex ? scaled : large;
		const Object* sphere = caseIndex ? scaledSphere : largeSphere;
		const RayIntersectionGeometric& entry = caseIndex ? scaledEntry : largeEntry;
		const Scalar density = testCase.rate * std::exp(-testCase.rate * testCase.collisionDistance);
		Require(Finite(density) && density > 0.0 &&
			(caseIndex ? density > 1e-20 : density < 1e-20),
			"measured collision density must straddle the old cutoff across scales");
		std::cout << "  radius=" << testCase.radius << " rate=" << testCase.rate
			<< " collision=" << testCase.collisionDistance << " initial=" << testCase.initialDistance
			<< " remaining=" << testCase.remainingDistance << " density=" << density << std::endl;
		const RISEPel zero( 0.0 );
		const RISEPel neutral( testCase.rate );
		// RGB channel, collision distance, HG forward polar/azimuth, next
		// channel, exit distance (.99), Fresnel (.5), cosine polar/azimuth.
		const std::vector<Scalar> draws = {
			0.0, testCase.xi, 0.0, 0.0, 0.0, 0.99, 0.5, 0.25, 0.75
		};
		SequenceSampler sampler( draws );
		const BSSRDFSampling::SampleResult sample = RandomWalkSSS::SampleExit(
			entry, sphere, zero, neutral, neutral, 0.0, 1.0, 2, sampler, 0.0 );
		const char* activeLabel = caseIndex ?
			"scaled RGB collision followed by real exit must be active" :
			"sub-cutoff RGB collision followed by real exit must be active";
		active[caseIndex] = RequireActiveAndFinite(sample,sampler,draws.size(),activeLabel);
		if( active[caseIndex] ) {
			samples[caseIndex] = sample;
			RequireNeutralRGBWeights(sample,
				"neutral RGB collision spatial weight must equal one in every channel");
		}
	}
	if( active[0] && active[1] ) {
		for( int channel = 0; channel < 3; ++channel ) {
			Require(Close(samples[0].weightSpatial[channel],samples[1].weightSpatial[channel]),
				"same-optical-depth scale pair must retain equal RGB spatial weights");
			Require(Close(samples[0].weight[channel],samples[1].weight[channel]),
				"same-optical-depth scale pair must retain equal RGB full weights");
		}
	}
	largeSphere->release();
	scaledSphere->release();
}

void TestNMNeutralControlAndPureAbsorption()
{
	std::cout << "Test B: NM neutral control and pure-absorption collision control" << std::endl;
	const Scalar radius = 1e8;
	const Scalar rate = 1e-20;
	const Scalar xi = -std::expm1( -5e-13 );
	DensityCase testCase = { radius, rate, 5e7, xi, 0.0, 0.0, 0.0 };
	Object* sphere = MakeClosedSphere( radius );
	const RayIntersectionGeometric entry = MakeSouthPoleEntry(radius);
	if( !PrepareCase(*sphere,entry,testCase) ||
		!Require(testCase.initialDistance > 1e8 && testCase.remainingDistance > 1e8,
			"NM control must prove real initial and post-collision ranges before weights") ) {
		sphere->release();
		return;
	}

	const RISEPel zero( 0.0 );
	const RISEPel neutral( rate );
	const std::vector<Scalar> nmDraws = { xi, 0.0, 0.0, 0.99, 0.5, 0.25, 0.75 };
	SequenceSampler nmSampler( nmDraws );
	const BSSRDFSampling::SampleResult nm = RandomWalkSSS::SampleExit(
		entry, sphere, zero, neutral, neutral, 0.0, 1.0, 2, nmSampler, 550.0 );
	if( RequireActiveAndFinite(nm,nmSampler,nmDraws.size(),
		"neutral NM collision followed by real exit must be active") ) {
		const Scalar angular = IndependentAngularWeight( nm );
		std::cout << "  NM spatial=" << nm.weightSpatialNM << " full=" << nm.weightNM
			<< " Sw=" << angular << std::endl;
		Require(Close(nm.weightSpatialNM,1.0),
			"neutral NM collision spatial weight must equal one");
		Require(Close(nm.weightNM,angular),
			"neutral NM full weight must equal the independently derived Sw");
	}

	const std::vector<Scalar> rgbAbsorptionDraws = { 0.0, xi };
	SequenceSampler rgbAbsorptionSampler( rgbAbsorptionDraws );
	const BSSRDFSampling::SampleResult rgbAbsorption = RandomWalkSSS::SampleExit(
		entry, sphere, neutral, zero, neutral, 0.0, 1.0, 1,
		rgbAbsorptionSampler, 0.0 );
	Require(!rgbAbsorption.valid && !rgbAbsorptionSampler.Exhausted() &&
		rgbAbsorptionSampler.Consumed() == rgbAbsorptionDraws.size(),
		"pure-absorption RGB collision must remain inactive after its collision draws");

	const std::vector<Scalar> nmAbsorptionDraws = { xi };
	SequenceSampler nmAbsorptionSampler( nmAbsorptionDraws );
	const BSSRDFSampling::SampleResult nmAbsorption = RandomWalkSSS::SampleExit(
		entry, sphere, neutral, zero, neutral, 0.0, 1.0, 1,
		nmAbsorptionSampler, 550.0 );
	Require(!nmAbsorption.valid && !nmAbsorptionSampler.Exhausted() &&
		nmAbsorptionSampler.Consumed() == nmAbsorptionDraws.size(),
		"pure-absorption NM collision must remain inactive after its collision draw");
	sphere->release();
}

} // namespace

int main()
{
	std::cout << std::setprecision(17);
	std::cout << "=== DL-57 random-walk density cutoff tests ===" << std::endl;
	TestRGBDensityCutoffAndScaleInvariance();
	TestNMNeutralControlAndPureAbsorption();
	if( gFailures ) {
		std::cerr << "=== DL-57 density cutoff failures: " << gFailures << " ===" << std::endl;
		return 1;
	}
	std::cout << "=== All DL-57 density cutoff tests passed ===" << std::endl;
	return 0;
}
