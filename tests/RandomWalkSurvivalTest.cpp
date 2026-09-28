//////////////////////////////////////////////////////////////////////
//
//  RandomWalkSurvivalTest.cpp - DL-50 spectral free-flight survival.
//
//  The random-walk distance sampler already conditions an exit on no
//  volume collision.  For neutral coefficients that conditional spatial
//  weight is one; Beer attenuation belongs in the unconditional estimate
//  through the frequency of surviving free-flight samples.
//
//  Author: GPT-5.6 Terra <noreply@anthropic.com>
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <iomanip>
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
	explicit SequenceSampler( const std::vector<Scalar>& values ) :
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
		const Scalar u = Get1D();
		const Scalar v = Get1D();
		return Point2( u, v );
	}

	bool Exhausted() const { return m_exhausted; }
	std::size_t Consumed() const { return m_next; }

private:
	const std::vector<Scalar>& m_values;
	std::size_t m_next;
	bool m_exhausted;
};

void Require( const bool condition, const char* message )
{
	if( !condition ) {
		std::cerr << "FAIL: " << message << std::endl;
		++gFailures;
	}
}

bool Close( const Scalar actual, const Scalar expected,
	const Scalar tolerance = 1e-11 )
{
	return std::fabs( actual - expected ) <= tolerance *
		std::fmax( Scalar( 1 ), std::fabs( expected ) );
}

bool Finite( const Scalar value )
{
	return std::isfinite( value );
}

Object* MakeClosedUnitSphere( const Scalar radius = 1.0 )
{
	SphereGeometry* geometry = new SphereGeometry( radius );
	Object* object = new Object( geometry );
	geometry->release();
	return object;
}

RayIntersectionGeometric MakeSurfaceHit( const Scalar radius = 1.0 )
{
	const Point3 point( 0, -radius, 0 );
	const Vector3 normal( 0, -1, 0 );
	const Vector3 incoming( 0, 1, 0 );
	RayIntersectionGeometric ri(
		Ray( Point3Ops::mkPoint3( point, -incoming * 2.0 ), incoming ),
		nullRasterizerState );
	ri.bHit = true;
	ri.ptIntersection = point;
	ri.vNormal = normal;
	ri.vGeomNormal = normal;
	ri.onb.CreateFromW( normal );
	return ri;
}

// Measure the actual chord selected by the same closed object after the
// documented walk-origin offset.  This keeps the Beer oracle independent of
// a guessed diameter or of an assumed epsilon-to-surface distance.
Scalar MeasureWalkExitDistance( const Object& object,
	const RayIntersectionGeometric& entry )
{
	const Vector3 direction = Vector3Ops::Normalize( entry.ray.Dir() );
	const Point3 origin = Point3Ops::mkPoint3( entry.ptIntersection,
		direction * BSSRDFSampling::BSSRDF_RAY_EPSILON );
	RayIntersection exitRI( Ray( origin, direction ), nullRasterizerState );
	object.IntersectRay( exitRI, RISE_INFINITY, false, true, false );
	Require( exitRI.geometric.bHit && Finite(exitRI.geometric.range) &&
		exitRI.geometric.range > 0,
		"real closed object must provide a finite back-face exit distance" );
	return exitRI.geometric.range;
}

Scalar IndependentAngularWeight( const BSSRDFSampling::SampleResult& sample )
{
	const Scalar cosine = Vector3Ops::Dot(
		sample.scatteredRay.Dir(), sample.entryNormal );
	// At IOR 1 there is no interface: the exact dielectric transmission the
	// SSS boundary uses since DL-306 is 1 at every cosine and its
	// cosine-hemisphere normalization is 1, so the angular weight is exactly
	// 1.  (Under DL-48's Schlick law it was (1-(1-mu)^5)/(20/21) -- Schlick
	// still "reflects" (1-mu)^5 at a matched index.)  The cosine is still
	// required to be a real exit direction.
	Require( Finite( cosine ) && cosine > 0, "angular oracle needs an outward exit direction" );
	return 1.0;
}

void RequireFiniteSample( const BSSRDFSampling::SampleResult& sample,
	const char* label )
{
	Require( sample.valid, label );
	Require( Finite(sample.weightSpatialNM) && Finite(sample.weightNM) &&
		Finite(sample.cosinePdf), "NM result must stay finite" );
	for( int channel = 0; channel < 3; ++channel ) {
		Require( Finite(sample.weightSpatial[channel]) && Finite(sample.weight[channel]),
			"RGB result must stay finite" );
	}
}

// Exact conditional contract.  All deterministic free-flight draws are in
// the exit branch, so the event probability has already been paid by the
// sampler.  The angular term is checked separately through full/spatial.
void TestNeutralConditionalSurvival()
{
	std::cout << "Test A: neutral conditional survival is one in RGB and NM" << std::endl;
	Object* sphere = MakeClosedUnitSphere();
	const RayIntersectionGeometric entry = MakeSurfaceHit();
	const Scalar exitDistance = MeasureWalkExitDistance( *sphere, entry );
	const RISEPel sigmaA( 1.0, 1.0, 1.0 );
	const RISEPel sigmaS( 0.0, 0.0, 0.0 );
	const RISEPel sigmaT( 1.0, 1.0, 1.0 );

	// exp(-t) < 0.01, whereas the measured exit event requires no collision
	// before the real chord.  The following three draws transmit at IOR 1
	// and select a fixed outgoing cosine direction.
	Require( 0.99 > 1.0 - std::exp( -exitDistance ),
		"deterministic free-flight draw must select the exit event" );
	const std::vector<Scalar> nmDraws = { 0.99, 0.5, 0.25, 0.75 };
	const std::vector<Scalar> rgbDraws = { 0.5, 0.99, 0.5, 0.25, 0.75 };

	SequenceSampler rgbSampler( rgbDraws );
	const BSSRDFSampling::SampleResult rgb = RandomWalkSSS::SampleExit(
		entry, sphere, sigmaA, sigmaS, sigmaT, 0.0, 1.0, 1, rgbSampler, 0.0 );
	RequireFiniteSample( rgb, "neutral RGB exit must be active" );
	Require( !rgbSampler.Exhausted() && rgbSampler.Consumed() == rgbDraws.size(),
		"RGB exit must consume its deterministic sampler sequence" );
	const Scalar rgbAngular = IndependentAngularWeight( rgb );
	for( int channel = 0; channel < 3; ++channel ) {
		Require( Close(rgb.weightSpatial[channel],1.0),
			"neutral RGB conditional spatial survival must be one" );
		Require( Close(rgb.weight[channel] / rgb.weightSpatial[channel],rgbAngular),
			"RGB full/spatial must contain only the independently checked angular term" );
	}

	const Scalar wavelengths[] = { 450.0, 550.0, 650.0 };
	for( const Scalar wavelength : wavelengths ) {
		SequenceSampler nmSampler( nmDraws );
		const BSSRDFSampling::SampleResult nm = RandomWalkSSS::SampleExit(
			entry, sphere, sigmaA, sigmaS, sigmaT, 0.0, 1.0, 1, nmSampler, wavelength );
		RequireFiniteSample( nm, "neutral NM exit must be active" );
		Require( !nmSampler.Exhausted() && nmSampler.Consumed() == nmDraws.size(),
			"NM exit must consume its deterministic sampler sequence" );
		Require( Close(nm.weightSpatialNM,1.0),
			"neutral NM conditional spatial survival must be one" );
		const Scalar nmAngular = IndependentAngularWeight( nm );
		Require( Close(nm.weightNM / nm.weightSpatialNM,nmAngular),
			"NM full/spatial must contain only the independently checked angular term" );
		Require( Close(nm.weightNM / nm.weightSpatialNM,
			rgb.weight[0] / rgb.weightSpatial[0]),
			"neutral RGB and NM must agree on the actual angular continuation weight" );
		std::cout << "  wavelength=" << wavelength
			<< " spatial=" << nm.weightSpatialNM
			<< " angular=" << (nm.weightNM / nm.weightSpatialNM) << std::endl;
	}

	sphere->release();
}

// Unconditional contract.  A pure absorber returns a valid walk exactly for
// the no-collision strata; averaging the conditional weights over every
// stratum must therefore recover Beer-Lambert attenuation once, not twice.
void TestStratifiedUnconditionalBeerAttenuation()
{
	std::cout << "Test B: stratified unconditional pure-absorption Beer attenuation" << std::endl;
	Object* sphere = MakeClosedUnitSphere();
	const RayIntersectionGeometric entry = MakeSurfaceHit();
	const Scalar exitDistance = MeasureWalkExitDistance( *sphere, entry );
	const Scalar sigma = 0.7;
	const Scalar expectedBeer = std::exp( -sigma * exitDistance );
	const RISEPel sigmaA( sigma, sigma, sigma );
	const RISEPel sigmaS( 0.0, 0.0, 0.0 );
	const RISEPel sigmaT( sigma, sigma, sigma );
	const int strata = 4096;
	int active = 0;
	double unconditional = 0;

	for( int index = 0; index < strata; ++index ) {
		const Scalar freeFlight = ( Scalar(index) + 0.5 ) / strata;
		const std::vector<Scalar> draws = { freeFlight, 0.5, 0.25, 0.75 };
		SequenceSampler sampler( draws );
		const BSSRDFSampling::SampleResult sample = RandomWalkSSS::SampleExit(
			entry, sphere, sigmaA, sigmaS, sigmaT, 0.0, 1.0, 1, sampler, 550.0 );
		Require( !sampler.Exhausted(), "stratified sampler must not overrun" );
		if( !sample.valid ) continue;
		++active;
		Require( sampler.Consumed() == draws.size(),
			"each surviving stratum must reach the exit continuation draws" );
		Require( Finite(sample.weightSpatialNM) && Finite(sample.weightNM),
			"surviving pure-absorption result must stay finite" );
		unconditional += sample.weightSpatialNM;
	}

	unconditional /= strata;
	const Scalar stratificationTolerance = 2.0 / strata;
	std::cout << "  distance=" << exitDistance << " expected Beer=" << expectedBeer
		<< " observed=" << unconditional << " active=" << active << "/" << strata
		<< std::endl;
	Require( active > 0 && active < strata,
		"stratified free flights must exercise both collision and exit branches" );
	Require( std::fabs( unconditional - expectedBeer ) <= stratificationTolerance,
		"unconditional pure-absorption estimator must match one Beer attenuation" );

	sphere->release();
}

// A collision pays albedo once; an internally reflected boundary pays no
// additional conditional survival factor. Draw counts and exit hemisphere
// distinguish these histories from an accidental direct exit.
void TestPriorCollisionAndReflection()
{
	std::cout << "Test C: survival after a collision and internal reflection" << std::endl;
	Object* sphere = MakeClosedUnitSphere();
	const RayIntersectionGeometric entry = MakeSurfaceHit();
	const Scalar wavelengths[] = { 0.0, 450.0, 550.0, 650.0 };
	for( int reflected = 0; reflected < 2; ++reflected ) {
		for( const Scalar wavelength : wavelengths ) {
			std::vector<Scalar> draws;
			if( wavelength == 0 ) draws.push_back( 0.5 );
			if( reflected ) {
				draws.push_back( 0.99 ); // first boundary
				draws.push_back( 0.0 );  // Fresnel reflection, F0 = 0.04
			} else {
				draws.push_back( 1.0 - std::exp( -0.25 ) );
				draws.push_back( 0.0 );  // forward isotropic phase direction
				draws.push_back( 0.25 ); // azimuth
			}
			if( wavelength == 0 ) draws.push_back( 0.5 );
			draws.insert( draws.end(), { 0.99, 0.9, 0.25, 0.75 } );
			SequenceSampler sampler( draws );
			const Scalar scattering = reflected ? 0.0 : 0.8;
			const BSSRDFSampling::SampleResult sample = RandomWalkSSS::SampleExit(
				entry, sphere, RISEPel(1.0-scattering), RISEPel(scattering),
				RISEPel(1.0), 0.0, reflected ? 1.5 : 1.0, 2, sampler, wavelength );
			RequireFiniteSample( sample, "two-segment history must exit" );
			Require( !sampler.Exhausted() && sampler.Consumed() == draws.size(),
				"two-segment history must consume every prescribed draw exactly" );
			Require( reflected ? sample.entryPoint.y < -0.99 : sample.entryPoint.y > 0.99,
				"exit hemisphere must identify the intended history" );
			const Scalar expected = reflected ? 1.0 : scattering;
			const Scalar actual = wavelength > 0 ? sample.weightSpatialNM : sample.weightSpatial[0];
			Require( Close(actual,expected),
				"survival after collision/reflection must retain only prior albedo" );
			if( wavelength == 0 ) {
				Require( Close(sample.weightSpatial[1],expected) &&
					Close(sample.weightSpatial[2],expected), "all neutral RGB channels must agree" );
			}
			std::cout << "  reflected=" << reflected << " wavelength=" << wavelength
				<< " spatial=" << actual << " expected=" << expected << std::endl;
		}
	}
	sphere->release();
}

// The tiny-extinction proposal uses the largest RGB rate, even in NM.
// A large physical chord makes that otherwise invisible difference testable.
// Keep the inward epsilon representable at this scale, and verify the real
// sphere's far hit before accepting any estimator observation. The expected
// deviation from one is still many thousands of double-precision ULPs.
void TestFallbackSurvivalProbability()
{
	std::cout << "Test D: NM fallback uses its actual survival probability" << std::endl;
	const Scalar radius = 1e8;
	Object* sphere = MakeClosedUnitSphere( radius );
	const RayIntersectionGeometric entry = MakeSurfaceHit( radius );
	const Ray ray( Point3Ops::mkPoint3( entry.ptIntersection,
		Vector3(0,BSSRDFSampling::BSSRDF_RAY_EPSILON,0) ), Vector3(0,1,0) );
	RayIntersection measured( ray, nullRasterizerState );
	sphere->IntersectRay( measured, RISE_INFINITY, false, true, false );
	if( !measured.geometric.bHit ) {
		RayIntersection fallback( ray, nullRasterizerState );
		sphere->IntersectRay( fallback, RISE_INFINITY, true, false, false );
		measured = fallback;
	}
	const Scalar distance = measured.geometric.range;
	Require( measured.geometric.bHit && Finite(distance) && Close(distance/(2*radius),1.0),
		"large sphere must yield its finite far boundary, not a self-hit" );
	const Scalar proposalRate = 1e-19;
	const Scalar physicalRate = 0.0722 * proposalRate;
	const Scalar physicalTr = std::exp( -physicalRate * distance );
	const Scalar survivalProbability = std::exp( -proposalRate * distance );
	const Scalar expected = physicalTr / survivalProbability;
	Require( physicalRate < 1e-20 && proposalRate > 1e-20 &&
		0.99 > 1.0-survivalProbability && expected > 1.0 + 1e-11,
		"fallback fixture must exercise a distinct surviving proposal" );
	const Scalar wavelengths[] = { 450.0, 550.0, 650.0 };
	for( const Scalar wavelength : wavelengths ) {
		const std::vector<Scalar> draws = {0.99,0.5,0.25,0.75};
		SequenceSampler sampler( draws );
		const BSSRDFSampling::SampleResult sample = RandomWalkSSS::SampleExit(
			entry, sphere, RISEPel(0,0,proposalRate), RISEPel(0.0),
			RISEPel(0,0,proposalRate), 0.0, 1.0, 1, sampler, wavelength );
		RequireFiniteSample( sample, "fallback survival must produce an actual exit" );
		Require( !sampler.Exhausted() && sampler.Consumed() == 4 &&
			Close(sample.entryPoint.y/radius,1.0),
			"fallback survival must reach the far boundary with the intended draws" );
		Require( Close(sample.weightSpatialNM,expected,1e-12),
			"NM fallback survival weight must divide by actual proposal probability" );
		std::cout << "  wavelength=" << wavelength << " spatial=" << sample.weightSpatialNM
			<< " expected=" << expected << " distance=" << distance
			<< " consumed=" << sampler.Consumed() << " valid=" << sample.valid
			<< " measuredNormalY=" << measured.geometric.vNormal.y << std::endl;
	}
	sphere->release();
}

} // namespace

int main()
{
	std::cout << std::setprecision(17);
	std::cout << "=== DL-50 random-walk survival tests ===" << std::endl;
	TestNeutralConditionalSurvival();
	TestStratifiedUnconditionalBeerAttenuation();
	TestPriorCollisionAndReflection();
	TestFallbackSurvivalProbability();
	if( gFailures ) {
		std::cerr << "=== DL-50 survival failures: " << gFailures << " ===" << std::endl;
		return 1;
	}
	std::cout << "=== All DL-50 survival tests passed ===" << std::endl;
	return 0;
}
