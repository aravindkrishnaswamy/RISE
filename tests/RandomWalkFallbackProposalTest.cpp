//////////////////////////////////////////////////////////////////////
//
//  RandomWalkFallbackProposalTest.cpp - DL-55 fallback proposal density.
//
//  The RGB distance sampler substitutes sigma_t_max for a channel whose
//  physical extinction is tiny.  These checks use the *substituted* rates
//  in an independent mixture-density oracle; the original physical rates
//  remain only in the numerator.  The NM rows do the analogous scalar
//  physical/proposal calculation for a collision.
//
//  Author: GPT-5.6 Terra <noreply@anthropic.com>
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <vector>

#include "../src/Library/Utilities/BSSRDFSampling.h"
#include "../src/Library/Utilities/Math3D/Constants.h"
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
	explicit SequenceSampler( std::vector<Scalar> values ) :
		m_values( values ), m_next( 0 ), m_exhausted( false )
	{}

	Scalar Get1D() override
	{
		if( m_next >= m_values.size() ) {
			m_exhausted = true;
			return 0.5;
		}
		return m_values[m_next++];
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
	const Scalar tolerance = 1e-11 )
{
	return Finite(actual) && Finite(expected) &&
		std::fabs( actual - expected ) <= tolerance *
		std::fmax( Scalar(1.0), std::fabs(expected) );
}

// New Reference objects begin with one reference. Object's constructor retains
// its geometry, so the immediately following geometry release leaves exactly
// the owning Object reference and does not need an addref() before it.
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
		Ray( Point3Ops::mkPoint3(point, -incoming * 2.0), incoming ),
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
	RayIntersection hit( Ray(origin, direction), nullRasterizerState );
	object.IntersectRay( hit, RISE_INFINITY, false, true, false );
	if( !Require(hit.geometric.bHit,
		"closed sphere must provide the requested back-face hit") ) {
		return false;
	}
	distance = hit.geometric.range;
	return Require( Finite(distance) && distance > 0,
		"closed sphere back-face distance must be finite and positive" );
}

bool RequireActiveSample( const BSSRDFSampling::SampleResult& sample,
	const SequenceSampler& sampler, const std::size_t expectedDraws,
	const char* label )
{
	if( !Require(sample.valid, label) ||
		!Require(!sampler.Exhausted() && sampler.Consumed() == expectedDraws,
			"active sample must consume exactly its explicit draw sequence") ) {
		return false;
	}
	if( !Require(Finite(sample.weightSpatialNM) && Finite(sample.weightNM) &&
		Finite(sample.cosinePdf), "active NM fields must be finite") ) {
		return false;
	}
	for( int channel = 0; channel < 3; ++channel ) {
		if( !Require(Finite(sample.weightSpatial[channel]) && Finite(sample.weight[channel]),
			"active RGB fields must be finite") ) {
			return false;
		}
	}
	return true;
}

Scalar IndependentAngularWeight( const BSSRDFSampling::SampleResult& sample )
{
	const Scalar cosine = Vector3Ops::Dot(
		sample.scatteredRay.Dir(), sample.entryNormal );
	return ( 1.0 - std::pow(1.0 - cosine,5.0) ) / (20.0 / 21.0);
}

Scalar EffectiveRGBRate( const RISEPel& sigmaT, const int channel )
{
	const Scalar sigmaTMax = ColorMath::MaxValue( sigmaT );
	return sigmaT[channel] < 1e-20 ? sigmaTMax : sigmaT[channel];
}

Scalar RGBProposalCollision( const RISEPel& sigmaT, const Scalar distance )
{
	Scalar sum = 0;
	for( int channel = 0; channel < 3; ++channel ) {
		const Scalar rate = EffectiveRGBRate( sigmaT, channel );
		sum += rate * std::exp( -rate * distance );
	}
	return sum / 3.0;
}

Scalar RGBProposalSurvival( const RISEPel& sigmaT, const Scalar distance )
{
	Scalar sum = 0;
	for( int channel = 0; channel < 3; ++channel ) {
		sum += std::exp( -EffectiveRGBRate(sigmaT,channel) * distance );
	}
	return sum / 3.0;
}

Scalar SampledDistance( const Scalar proposalRate, const Scalar draw )
{
	return -std::log( std::fmax(Scalar(1e-20),1.0-draw) ) / proposalRate;
}

bool RequireRGBFields( const BSSRDFSampling::SampleResult& sample,
	const Scalar expectedSpatial[3], const Scalar tolerance,
	const char* label )
{
	const Scalar angular = IndependentAngularWeight( sample );
	if( !Require(Finite(angular), "independent angular continuation factor must be finite") ) {
		return false;
	}
	bool matches = true;
	for( int channel = 0; channel < 3; ++channel ) {
		std::cout << "  RGB channel=" << channel
			<< " spatial=" << sample.weightSpatial[channel]
			<< " expected=" << expectedSpatial[channel]
			<< " full=" << sample.weight[channel]
			<< " expected-full=" << expectedSpatial[channel] * angular << std::endl;
		const bool spatialOK = Require(Close(sample.weightSpatial[channel],
			expectedSpatial[channel],tolerance), label);
		const bool fullOK = Require(Close(sample.weight[channel],
			expectedSpatial[channel] * angular,tolerance),
			"RGB full field must equal independent spatial proposal weight times Sw");
		matches = spatialOK && fullOK && matches;
	}
	return matches;
}

bool RequireNMFields( const BSSRDFSampling::SampleResult& sample,
	const Scalar expectedSpatial, const Scalar tolerance, const char* label )
{
	const Scalar angular = IndependentAngularWeight( sample );
	if( !Require(Finite(angular), "independent angular continuation factor must be finite") ) {
		return false;
	}
	std::cout << "  NM spatial=" << sample.weightSpatialNM
		<< " expected=" << expectedSpatial << " full=" << sample.weightNM
		<< " expected-full=" << expectedSpatial * angular << std::endl;
	const bool spatialOK = Require(Close(sample.weightSpatialNM,expectedSpatial,tolerance), label);
	const bool fullOK = Require(Close(sample.weightNM,expectedSpatial * angular,tolerance),
		"NM full field must equal independent spatial proposal weight times Sw");
	return spatialOK && fullOK;
}

// The zero red channel is sampled for the boundary and falls back to rate 1.
// Green retains rate .4. Thus the actual survival proposal is the nonuniform
// mixture (exp(-d) + exp(-.4d) + exp(-d))/3, not a selected-channel density
// and not the mixture formed from the physical zero channel.
void TestRGBFallbackBoundaryMixture()
{
	std::cout << "Test A: RGB fallback boundary uses actual nonuniform mixture" << std::endl;
	Object* sphere = MakeClosedSphere( 1.0 );
	const RayIntersectionGeometric entry = MakeSouthPoleEntry( 1.0 );
	const Vector3 direction = Vector3Ops::Normalize( entry.ray.Dir() );
	Scalar distance = 0;
	const Point3 origin = Point3Ops::mkPoint3(entry.ptIntersection,
		direction * BSSRDFSampling::BSSRDF_RAY_EPSILON);
	if( !MeasureBackFaceDistance(*sphere,origin,direction,distance) ) {
		sphere->release();
		return;
	}

	const RISEPel sigmaA( 0.0, 0.2, 0.2 );
	const RISEPel sigmaS( 0.0, 0.2, 0.8 );
	const RISEPel sigmaT( 0.0, 0.4, 1.0 );
	const std::vector<Scalar> draws = { 0.0, 0.99, 0.5, 0.25, 0.75 };
	SequenceSampler sampler( draws );
	const BSSRDFSampling::SampleResult sample = RandomWalkSSS::SampleExit(
		entry, sphere, sigmaA, sigmaS, sigmaT, 0.0, 1.0, 1, sampler, 0.0 );
	if( RequireActiveSample(sample,sampler,draws.size(),
		"zero-channel RGB fallback boundary must exit") ) {
		const Scalar proposal = RGBProposalSurvival( sigmaT, distance );
		Scalar expected[3];
		for( int channel = 0; channel < 3; ++channel ) {
			expected[channel] = std::exp(-sigmaT[channel] * distance) / proposal;
		}
		if( Require(Finite(proposal) && proposal > 0,
			"actual RGB fallback survival mixture must be finite and positive") ) {
			RequireRGBFields(sample,expected,1e-12,
				"RGB boundary spatial field must divide by the actual fallback mixture");
		}
	}
	sphere->release();
}

// The collision selects the nonzero green channel, then deliberately keeps
// its phase direction forward so a real, independently retraced sphere
// distance supplies the second (exit) proposal factor.
void TestRGBFallbackCollisionMixture()
{
	std::cout << "Test B: RGB fallback collision uses actual nonuniform mixture" << std::endl;
	Object* sphere = MakeClosedSphere( 1.0 );
	const RayIntersectionGeometric entry = MakeSouthPoleEntry( 1.0 );
	const Vector3 direction = Vector3Ops::Normalize( entry.ray.Dir() );
	const Point3 initial = Point3Ops::mkPoint3(entry.ptIntersection,
		direction * BSSRDFSampling::BSSRDF_RAY_EPSILON);
	const RISEPel sigmaA( 0.0, 0.2, 0.2 );
	const RISEPel sigmaS( 0.0, 0.2, 0.8 );
	const RISEPel sigmaT( 0.0, 0.4, 1.0 );
	const Scalar collisionDraw = 1.0 - std::exp( -0.4 * 0.25 );
	const Scalar collisionDistance = SampledDistance( 0.4, collisionDraw );
	const Point3 collisionPoint = Point3Ops::mkPoint3(initial,
		direction * collisionDistance);
	Scalar remainingDistance = 0;
	if( !Require(Finite(collisionDistance) && collisionDistance > 0 &&
		collisionDistance < 1.0,
		"nonzero-channel RGB collision distance must be finite and inside sphere") ||
		!MeasureBackFaceDistance(*sphere,collisionPoint,direction,remainingDistance) ) {
		sphere->release();
		return;
	}

	// channel, collision distance, HG polar/azimuth (forward), then
	// channel, exit distance, Fresnel, cosine polar/azimuth.
	const std::vector<Scalar> draws = {
		0.5, collisionDraw, 0.0, 0.0, 0.0, 0.99, 0.5, 0.25, 0.75
	};
	SequenceSampler sampler( draws );
	const BSSRDFSampling::SampleResult sample = RandomWalkSSS::SampleExit(
		entry, sphere, sigmaA, sigmaS, sigmaT, 0.0, 1.0, 2, sampler, 0.0 );
	if( RequireActiveSample(sample,sampler,draws.size(),
		"nonzero-channel RGB collision followed by exit must be active") ) {
		const Scalar collisionProposal = RGBProposalCollision(sigmaT,collisionDistance);
		const Scalar exitProposal = RGBProposalSurvival(sigmaT,remainingDistance);
		if( Require(Finite(collisionProposal) && collisionProposal > 0 &&
			Finite(exitProposal) && exitProposal > 0,
			"actual RGB collision and survival mixtures must be finite and positive") ) {
			Scalar expected[3];
			for( int channel = 0; channel < 3; ++channel ) {
				expected[channel] = sigmaS[channel] *
					std::exp(-sigmaT[channel] * collisionDistance) / collisionProposal *
					std::exp(-sigmaT[channel] * remainingDistance) / exitProposal;
			}
			RequireRGBFields(sample,expected,1e-12,
				"RGB collision spatial field must use actual fallback mixture densities");
		}
	}
	sphere->release();
}

// Midpoint strata deterministically integrate the discrete RGB channel choice
// and free-flight draw. This is quadrature, explicitly not a confidence
// interval or a stochastic-render variance claim.
void TestRGBFallbackUnconditionalBeer()
{
	std::cout << "Test C: RGB fallback unconditional Beer quadrature" << std::endl;
	Object* sphere = MakeClosedSphere( 1.0 );
	const RayIntersectionGeometric entry = MakeSouthPoleEntry( 1.0 );
	const Vector3 direction = Vector3Ops::Normalize( entry.ray.Dir() );
	const Point3 origin = Point3Ops::mkPoint3(entry.ptIntersection,
		direction * BSSRDFSampling::BSSRDF_RAY_EPSILON);
	Scalar distance = 0;
	if( !MeasureBackFaceDistance(*sphere,origin,direction,distance) ) {
		sphere->release();
		return;
	}

	const RISEPel sigmaA( 0.0, 0.4, 1.0 );
	const RISEPel sigmaS( 0.0 );
	const RISEPel sigmaT( 0.0, 0.4, 1.0 );
	const Scalar channelDraws[] = { 0.0, 0.5, 0.9 };
	const int strata = 4096;
	double sum[3] = { 0, 0, 0 };
	int active = 0;
	for( int index = 0; index < strata; ++index ) {
		const Scalar flightDraw = (Scalar(index) + 0.5) / strata;
		for( int choice = 0; choice < 3; ++choice ) {
			const std::vector<Scalar> draws = {
				channelDraws[choice], flightDraw, 0.5, 0.25, 0.75
			};
			SequenceSampler sampler( draws );
			const BSSRDFSampling::SampleResult sample = RandomWalkSSS::SampleExit(
				entry, sphere, sigmaA, sigmaS, sigmaT, 0.0, 1.0, 1, sampler, 0.0 );
			if( !sample.valid ) {
				Require(!sampler.Exhausted() && sampler.Consumed() == 2,
					"zero-scatter collision stratum must stop after its explicit collision draws");
				continue;
			}
			if( !RequireActiveSample(sample,sampler,draws.size(),
				"pure-absorption RGB survival stratum must exit") ) {
				continue;
			}
			++active;
			for( int channel = 0; channel < 3; ++channel ) {
				sum[channel] += sample.weightSpatial[channel];
			}
		}
	}

	const int total = strata * 3;
	std::cout << "  Beer active=" << active << "/" << total << std::endl;
	Require(active > 0 && active < total,
		"pure-absorption quadrature must exercise both collision and survival events");
	const Scalar tolerance = 2.0 / strata;
	for( int channel = 0; channel < 3; ++channel ) {
		const Scalar expectedBeer = std::exp( -sigmaT[channel] * distance );
		const Scalar observed = sum[channel] / total;
		std::cout << "  Beer channel=" << channel << " observed=" << observed
			<< " expected=" << expectedBeer << " tolerance=" << tolerance << std::endl;
		Require(Finite(expectedBeer) && Finite(observed),
			"independent RGB Beer values must be finite before comparison");
		Require(Close(observed,expectedBeer,tolerance),
			"unconditional RGB fallback estimate must equal one physical Beer attenuation");
	}
	sphere->release();
}

void TestZeroScatterZeroChannelCollision()
{
	std::cout << "Test D: zero-scatter zero-channel collisions remain inactive" << std::endl;
	Object* sphere = MakeClosedSphere( 1.0 );
	const RayIntersectionGeometric entry = MakeSouthPoleEntry( 1.0 );
	const RISEPel sigmaA( 0.0, 0.4, 1.0 );
	const RISEPel sigmaS( 0.0 );
	const RISEPel sigmaT( 0.0, 0.4, 1.0 );
	const Scalar rgbCollisionDraw = 1.0 - std::exp(-0.25);
	const std::vector<Scalar> rgbDraws = { 0.0, rgbCollisionDraw };
	SequenceSampler rgbSampler( rgbDraws );
	const BSSRDFSampling::SampleResult rgb = RandomWalkSSS::SampleExit(
		entry, sphere, sigmaA, sigmaS, sigmaT, 0.0, 1.0, 1, rgbSampler, 0.0 );
	Require(!rgb.valid && !rgbSampler.Exhausted() &&
		rgbSampler.Consumed() == rgbDraws.size(),
		"zero-scatter RGB fallback collision must not fabricate an active exit");

	const Scalar nmCollisionDraw = 1.0 - std::exp(-0.25);
	const std::vector<Scalar> nmDraws = { nmCollisionDraw };
	SequenceSampler nmSampler( nmDraws );
	const BSSRDFSampling::SampleResult nm = RandomWalkSSS::SampleExit(
		entry, sphere, sigmaA, sigmaS, sigmaT, 0.0, 1.0, 1, nmSampler, 550.0 );
	Require(!nm.valid && !nmSampler.Exhausted() &&
		nmSampler.Consumed() == nmDraws.size(),
		"zero-scatter NM collision must not fabricate an active exit");
	sphere->release();
}

// A 1e8-radius closed sphere keeps BSSRDF_RAY_EPSILON representable while
// making (q-s)*t observable. The requested collision is near 5e7; its real
// continuation distance is retraced before any weight comparison. q=1.3e-19
// exposes the missing ratio by about 4e-13 in the final weight. q=1e-20 pins
// the source's strict `> 1e-20` collision gate.
void TestNMFallbackCollision( const Scalar proposalRate, const char* label )
{
	std::cout << "  " << label << std::endl;
	const Scalar radius = 1e8;
	const Scalar wantedCollisionDistance = 5e7;
	Object* sphere = MakeClosedSphere( radius );
	const RayIntersectionGeometric entry = MakeSouthPoleEntry( radius );
	const Vector3 direction = Vector3Ops::Normalize( entry.ray.Dir() );
	const Point3 initial = Point3Ops::mkPoint3(entry.ptIntersection,
		direction * BSSRDFSampling::BSSRDF_RAY_EPSILON);
	const Scalar collisionDraw = 1.0 - std::exp(-proposalRate * wantedCollisionDistance);
	const Scalar collisionDistance = SampledDistance(proposalRate,collisionDraw);
	const Point3 collisionPoint = Point3Ops::mkPoint3(initial,
		direction * collisionDistance);
	Scalar initialDistance = 0;
	Scalar remainingDistance = 0;
	if( !MeasureBackFaceDistance(*sphere,initial,direction,initialDistance) ||
		!Require(Finite(collisionDistance) && collisionDistance > 4e7 &&
			collisionDistance < 6e7,
			"NM fallback collision must be active at its intended real scale") ||
		!MeasureBackFaceDistance(*sphere,collisionPoint,direction,remainingDistance) ) {
		sphere->release();
		return;
	}
	if( !Require(initialDistance > 1e8 && remainingDistance > 1e8,
		"large closed sphere must provide real finite activity distances") ) {
		sphere->release();
		return;
	}

	std::cout << "  NM q=" << proposalRate << " collision-distance=" << collisionDistance
		<< " initial-distance=" << initialDistance << " remaining-distance=" << remainingDistance
		<< " missing-collision-factor=" << std::exp((proposalRate - 0.0722 * proposalRate) * collisionDistance) << std::endl;
	const RISEPel sigmaA( 0.0 );
	const RISEPel sigmaS( 0.0, 0.0, proposalRate );
	const RISEPel sigmaT( 0.0, 0.0, proposalRate );
	const Scalar sigmaSNM = 0.0722 * proposalRate;
	const Scalar sigmaTNM = sigmaSNM;
	// collision distance, HG polar/azimuth (forward), exit distance,
	// Fresnel, cosine polar/azimuth. SequenceSampler owns this exact order.
	const std::vector<Scalar> draws = {
		collisionDraw, 0.0, 0.0, 0.99, 0.5, 0.25, 0.75
	};
	SequenceSampler sampler( draws );
	const BSSRDFSampling::SampleResult sample = RandomWalkSSS::SampleExit(
		entry, sphere, sigmaA, sigmaS, sigmaT, 0.0, 1.0, 2, sampler, 550.0 );
	if( RequireActiveSample(sample,sampler,draws.size(),
		"NM fallback collision followed by a real exit must be active") ) {
		const Scalar physicalCollision = sigmaSNM *
			std::exp(-sigmaTNM * collisionDistance);
		const Scalar proposalCollision = proposalRate *
			std::exp(-proposalRate * collisionDistance);
		const Scalar physicalSurvival = std::exp(-sigmaTNM * remainingDistance);
		const Scalar proposalSurvival = std::exp(-proposalRate * remainingDistance);
		if( Require(Finite(physicalCollision) && Finite(proposalCollision) &&
			Finite(physicalSurvival) && Finite(proposalSurvival) &&
			proposalCollision > 0 && proposalSurvival > 0,
			"independent NM physical and proposal factors must be finite and positive") ) {
			const Scalar expected = physicalCollision / proposalCollision *
				physicalSurvival / proposalSurvival;
			// 1e-13 is well below the ~4e-13 above-threshold missing-collision
			// signal, yet accommodates independent elementary-function rounding.
			RequireNMFields(sample,expected,1e-13,
				"NM collision must include its physical-to-fallback-proposal ratio");
		}
	}
	sphere->release();
}

void TestNMFallbackCollisions()
{
	std::cout << "Test E: NM fallback collision proposal ratios" << std::endl;
	TestNMFallbackCollision( 1.3e-19,
		"above threshold: physical NM rate is 9.386e-21 and q is 1.3e-19" );
	TestNMFallbackCollision( 1.0e-20,
		"exact threshold: fallback q=1e-20 must not skip collision weighting" );
}

} // namespace

int main()
{
	std::cout << std::setprecision(17);
	std::cout << "=== DL-55 random-walk fallback proposal tests ===" << std::endl;
	TestRGBFallbackBoundaryMixture();
	TestRGBFallbackCollisionMixture();
	TestRGBFallbackUnconditionalBeer();
	TestZeroScatterZeroChannelCollision();
	TestNMFallbackCollisions();
	if( gFailures ) {
		std::cerr << "=== DL-55 fallback proposal failures: " << gFailures << " ===" << std::endl;
		return 1;
	}
	std::cout << "=== All DL-55 fallback proposal tests passed ===" << std::endl;
	return 0;
}
