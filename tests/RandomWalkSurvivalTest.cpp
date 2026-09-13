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

Object* MakeClosedUnitSphere()
{
	SphereGeometry* geometry = new SphereGeometry( 1.0 );
	Object* object = new Object( geometry );
	geometry->release();
	return object;
}

RayIntersectionGeometric MakeSurfaceHit()
{
	const Point3 point( 0, -1, 0 );
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
	// At IOR 1, Schlick transmission is 1-(1-mu)^5.  Its cosine-hemisphere
	// normalization is 2*integral_0^1 mu*(1-(1-mu)^5)dmu = 20/21.
	return ( 1.0 - pow( 1.0 - cosine, 5.0 ) ) / ( 20.0 / 21.0 );
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

} // namespace

int main()
{
	std::cout << "=== DL-50 random-walk survival tests ===" << std::endl;
	TestNeutralConditionalSurvival();
	TestStratifiedUnconditionalBeerAttenuation();
	if( gFailures ) {
		std::cerr << "=== DL-50 survival failures: " << gFailures << " ===" << std::endl;
		return 1;
	}
	std::cout << "=== All DL-50 survival tests passed ===" << std::endl;
	return 0;
}
