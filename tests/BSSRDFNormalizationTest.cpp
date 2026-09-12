//////////////////////////////////////////////////////////////////////
//
//  BSSRDFNormalizationTest.cpp - DL-48 directional Sw normalization.
//
//  The reference integrates the Schlick transmission law directly.  It
//  intentionally does not copy a production normalization coefficient.
//  The profile-backed adapter uses Burley; constructing Donner-Jensen's
//  multipole fit here would add unrelated test cost to its shared Fresnel law.
//
//////////////////////////////////////////////////////////////////////

#include <cassert>
#include <cmath>
#include <iomanip>
#include <iostream>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Math3D/Constants.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/BSSRDFSampling.h"
#include "../src/Library/Utilities/RandomWalkSSS.h"
#include "../src/Library/Utilities/SSSCoefficients.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Painters/RGBScalarPainter.h"
#include "../src/Library/Materials/BurleyNormalizedDiffusionProfile.h"
#include "../src/Library/Materials/SubSurfaceScatteringMaterial.h"
#include "../src/Library/Shaders/BSSRDFEntryAdapters.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{

static int gFailures = 0;
// The normalized cosine integrand has |second derivative| <= 21.
// Composite midpoint error is therefore <= 21/(24*N*N), below 3.3e-9
// at 16K bins. These tolerances cover that bound, the 32K reference
// quadrature, and floating-point direction/ratio evaluation.
static const Scalar kQuadratureTolerance = 2e-8;
static const Scalar kRatioTolerance = 5e-8;

class TestSampler : public ISampler
{
	RandomNumberGenerator rng;
public:
	explicit TestSampler( const unsigned int seed ) : rng( seed ) {}
	Scalar Get1D() { return rng.CanonicalRandom(); }
	Point2 Get2D() { return Point2( Get1D(), Get1D() ); }
};

static bool Close( const Scalar actual, const Scalar expected,
	const Scalar tolerance = kQuadratureTolerance )
{
	return std::fabs( actual - expected ) <= tolerance *
		std::fmax( Scalar(1), std::fabs(expected) );
}

static void RequireFinitePositive( const Scalar value, const char* label )
{
	if( !std::isfinite(value) || value <= 0 ) {
		std::cerr << "FAIL: " << label << " must be finite and positive, got "
			<< value << std::endl;
		++gFailures;
	}
}

static Scalar IndependentTransmission( const Scalar cosine, const Scalar eta )
{
	const Scalar f0 = (eta - 1.0) * (eta - 1.0) /
		((eta + 1.0) * (eta + 1.0));
	return 1.0 - (f0 + (1.0 - f0) * pow(1.0 - cosine, 5.0));
}

// c = integral(Ft(w) cos(theta) dw) / PI. This midpoint quadrature is the
// oracle for the normalized directional law and has no production constant.
static Scalar IndependentCosineNormalization( const Scalar eta )
{
	const int count = 32768;
	Scalar sum = 0;
	for( int i = 0; i < count; ++i ) {
		const Scalar cosine = (Scalar(i) + 0.5) / count;
		sum += 2.0 * cosine * IndependentTransmission( cosine, eta );
	}
	return sum / count;
}

static RayIntersectionGeometric MakeSurfaceRI()
{
	const Point3 point( 0, -1, 0 );
	const Vector3 normal( 0, -1, 0 );
	const Vector3 incoming( 0, 1, 0 );
	RayIntersectionGeometric ri(
		Ray( Point3Ops::mkPoint3(point, -incoming * 2.0), incoming ),
		nullRasterizerState );
	ri.bHit = true;
	ri.ptIntersection = point;
	ri.vNormal = normal;
	ri.vGeomNormal = normal;
	ri.onb.CreateFromW( normal );
	return ri;
}

static Object* MakeUnitSphere()
{
	SphereGeometry* geometry = new SphereGeometry( 1.0 );
	geometry->addref();
	Object* object = new Object( geometry );
	object->addref();
	geometry->release();
	return object;
}

template<typename Evaluate>
static Scalar IntegrateActual( const Evaluate& evaluate, const char* label )
{
	const int count = 16384;
	Scalar sum = 0;
	for( int i = 0; i < count; ++i ) {
		const Scalar cosine = (Scalar(i) + 0.5) / count;
		const Scalar value = evaluate( cosine );
		RequireFinitePositive( value, label );
		sum += TWO_PI * cosine * value;
	}
	return sum / count;
}

static void RequireUnitIntegral( const char* label, const Scalar eta,
	const Scalar integral )
{
	std::cout << "  " << label << " eta=" << eta
		<< " integral=" << std::setprecision(10) << integral << std::endl;
	if( !Close(integral, 1.0) ) {
		std::cerr << "FAIL: " << label << " eta=" << eta
			<< " has cosine-hemisphere integral " << integral
			<< ", expected 1" << std::endl;
		++gFailures;
	}
}

static Vector3 DirectionForCosine( const Scalar cosine )
{
	return Vector3( sqrt(1.0 - cosine*cosine), -cosine, 0 );
}

static void TestDirectionalEvaluators()
{
	std::cout << "Test A: helper and RGB/NM entry adapters integrate to one" << std::endl;
	const Scalar iors[] = { 1.00, 1.10, 1.30, 1.50, 2.00 };
	const RayIntersectionGeometric ri = MakeSurfaceRI();

	for( const Scalar eta : iors )
	{
		UniformScalarPainter* ior = new UniformScalarPainter( eta ); ior->addref();
		RGBScalarPainter* absorption =
			new RGBScalarPainter( 0.05, 0.10, 0.20 ); absorption->addref();
		RGBScalarPainter* scattering =
			new RGBScalarPainter( 1.0, 1.0, 1.0 ); scattering->addref();
		BurleyNormalizedDiffusionProfile* profile =
			new BurleyNormalizedDiffusionProfile( *ior, *absorption, *scattering, 0.0 );
		profile->addref();

		const Scalar independentNormalization =
			IndependentCosineNormalization( eta );
		RequireFinitePositive( independentNormalization, "independent normalization" );
		RequireUnitIntegral( "independent normalized law", eta, IntegrateActual(
			[&]( const Scalar cosine ) {
				return IndependentTransmission(cosine, eta) /
					(independentNormalization * PI);
			}, "independent normalized law" ) );

		RequireUnitIntegral( "BSSRDFSampling::EvaluateSwWithFresnel", eta,
			IntegrateActual( [&]( const Scalar cosine ) {
				return BSSRDFSampling::EvaluateSwWithFresnel(
					profile->FresnelTransmission(cosine, ri), eta );
			}, "BSSRDFSampling::EvaluateSwWithFresnel" ) );

		BSSRDFAdapters::BSSRDFEntryBSDF diffusionEntry( profile, eta );
		RequireUnitIntegral( "BSSRDFEntryBSDF RGB", eta,
			IntegrateActual( [&]( const Scalar cosine ) {
				return diffusionEntry.value(DirectionForCosine(cosine), ri)[0];
			}, "BSSRDFEntryBSDF RGB" ) );
		RequireUnitIntegral( "BSSRDFEntryBSDF NM", eta,
			IntegrateActual( [&]( const Scalar cosine ) {
				return diffusionEntry.valueNM(DirectionForCosine(cosine), ri, 550.0);
			}, "BSSRDFEntryBSDF NM" ) );

		BSSRDFAdapters::RandomWalkEntryBSDF randomWalkEntry( eta );
		RequireUnitIntegral( "RandomWalkEntryBSDF RGB", eta,
			IntegrateActual( [&]( const Scalar cosine ) {
				return randomWalkEntry.value(DirectionForCosine(cosine), ri)[0];
			}, "RandomWalkEntryBSDF RGB" ) );
		RequireUnitIntegral( "RandomWalkEntryBSDF NM", eta,
			IntegrateActual( [&]( const Scalar cosine ) {
				return randomWalkEntry.valueNM(DirectionForCosine(cosine), ri, 550.0);
			}, "RandomWalkEntryBSDF NM" ) );

		profile->release();
		ior->release();
		absorption->release();
		scattering->release();
	}
}

static void RequireRatio( const char* label, const Scalar actual,
	const Scalar expected, const Scalar eta, bool& reported )
{
	if( !std::isfinite(actual) || !Close(actual, expected, kRatioTolerance) ) {
		if( reported ) return;
		std::cerr << "FAIL: " << label << " eta=" << eta
			<< " actual=" << actual << " expected=" << expected << std::endl;
		reported = true;
		++gFailures;
	}
}

static void TestDiffusionSampleRatios()
{
	std::cout << "Test B: BSSRDF samples full/spatial ratio is normalized Sw" << std::endl;
	const Scalar iors[] = { 1.10, 1.30, 1.50 };
	const RayIntersectionGeometric ri = MakeSurfaceRI();
	Object* sphere = MakeUnitSphere();

	for( const Scalar eta : iors )
	{
		UniformScalarPainter* ior = new UniformScalarPainter( eta ); ior->addref();
		RGBScalarPainter* absorption =
			new RGBScalarPainter( 0.05, 0.10, 0.20 ); absorption->addref();
		RGBScalarPainter* scattering =
			new RGBScalarPainter( 1.0, 1.0, 1.0 ); scattering->addref();
		SubSurfaceScatteringMaterial* material = new SubSurfaceScatteringMaterial(
			*ior, *absorption, *scattering, 0.0, 0.2 );
		material->addref();
		const Scalar normalization = IndependentCosineNormalization( eta );

		for( int spectral = 0; spectral < 2; ++spectral )
		{
			TestSampler sampler( unsigned(1000 + eta*100 + spectral) );
			int active = 0;
			bool reportedMismatch = false;
			for( int attempt = 0; attempt < 512; ++attempt )
			{
				const Scalar nm = spectral ? 550.0 : 0.0;
				const BSSRDFSampling::SampleResult sample =
					BSSRDFSampling::SampleEntryPoint(ri, sphere, material, sampler, nm);
				if( !sample.valid ) continue;
				++active;
				const Scalar cosine = Vector3Ops::Dot(
					sample.scatteredRay.Dir(), sample.entryNormal );
				RequireFinitePositive( cosine, "BSSRDF sampled cosine" );
				const Scalar expected = IndependentTransmission(cosine, eta) / normalization;
				if( spectral ) {
					RequireFinitePositive( sample.weightSpatialNM, "BSSRDF NM spatial weight" );
					RequireRatio( "BSSRDF NM full/spatial", sample.weightNM /
						sample.weightSpatialNM, expected, eta, reportedMismatch );
				} else {
					for( int channel = 0; channel < 3; ++channel ) {
						RequireFinitePositive( sample.weightSpatial[channel],
							"BSSRDF RGB spatial weight" );
						RequireRatio( "BSSRDF RGB full/spatial", sample.weight[channel] /
							sample.weightSpatial[channel], expected, eta, reportedMismatch );
					}
				}
			}
			std::cout << "  BSSRDF " << (spectral ? "NM" : "RGB")
				<< " eta=" << eta << " active=" << active << "/512" << std::endl;
			if( active < 16 ) {
				std::cerr << "FAIL: BSSRDF " << (spectral ? "NM" : "RGB")
					<< " eta=" << eta << " did not produce enough valid samples"
					<< std::endl;
				++gFailures;
			}
		}

		material->release();
		ior->release();
		absorption->release();
		scattering->release();
	}
	sphere->release();
}

static void TestRandomWalkSampleRatios()
{
	std::cout << "Test C: random-walk samples full/spatial ratio isolates Sw" << std::endl;
	const Scalar iors[] = { 1.10, 1.30, 1.50 };
	const RayIntersectionGeometric ri = MakeSurfaceRI();
	Object* sphere = MakeUnitSphere();
	const RISEPel sigmaA( 0.0, 0.0, 0.0 );
	const RISEPel sigmaS( 0.05, 0.05, 0.05 );
	RISEPel sigmaT;
	SSSCoefficients::FromCoefficients( sigmaA, sigmaS, sigmaT );

	for( const Scalar eta : iors )
	{
		const Scalar normalization = IndependentCosineNormalization( eta );
		for( int spectral = 0; spectral < 2; ++spectral )
		{
			TestSampler sampler( unsigned(2000 + eta*100 + spectral) );
			int active = 0;
			bool reportedMismatch = false;
			for( int attempt = 0; attempt < 512; ++attempt )
			{
				const Scalar nm = spectral ? 550.0 : 0.0;
				const BSSRDFSampling::SampleResult sample = RandomWalkSSS::SampleExit(
					ri, sphere, sigmaA, sigmaS, sigmaT, 0.0, eta, 16, sampler, nm );
				if( !sample.valid ) continue;
				++active;
				const Scalar cosine = Vector3Ops::Dot(
					sample.scatteredRay.Dir(), sample.entryNormal );
				RequireFinitePositive( cosine, "random-walk sampled cosine" );
				const Scalar expected = IndependentTransmission(cosine, eta) / normalization;

				// The division deliberately cancels the spatial survival weight.
				// DL-50 owns that estimator; this test covers only directional Sw.
				if( spectral ) {
					RequireFinitePositive( sample.weightSpatialNM,
						"random-walk NM spatial weight" );
					RequireRatio( "random-walk NM full/spatial", sample.weightNM /
						sample.weightSpatialNM, expected, eta, reportedMismatch );
				} else {
					for( int channel = 0; channel < 3; ++channel ) {
						RequireFinitePositive( sample.weightSpatial[channel],
							"random-walk RGB spatial weight" );
						RequireRatio( "random-walk RGB full/spatial", sample.weight[channel] /
							sample.weightSpatial[channel], expected, eta, reportedMismatch );
					}
				}
			}
			std::cout << "  random-walk " << (spectral ? "NM" : "RGB")
				<< " eta=" << eta << " active=" << active << "/512" << std::endl;
			if( active < 16 ) {
				std::cerr << "FAIL: random-walk " << (spectral ? "NM" : "RGB")
					<< " eta=" << eta << " did not produce enough valid samples"
					<< std::endl;
				++gFailures;
			}
		}
	}
	sphere->release();
}

} // namespace

int main()
{
	std::cout << "=== DL-48 BSSRDF Sw normalization ===" << std::endl;
	TestDirectionalEvaluators();
	TestDiffusionSampleRatios();
	TestRandomWalkSampleRatios();
	if( gFailures ) {
		std::cerr << "=== DL-48 normalization failures: " << gFailures << " ==="
			<< std::endl;
		return 1;
	}
	std::cout << "=== All DL-48 normalization tests passed ===" << std::endl;
	return 0;
}
