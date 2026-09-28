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

#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Interfaces/IRasterImageAccessor.h"
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
#include "../src/Library/Materials/SubSurfaceScatteringSPF.h"
#include "../src/Library/Utilities/IORStack.h"
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

// A real two-by-two texture makes the original and entry IOR differ.
// Both UV records are explicitly supplied: missing entry UV/signals is not needed
// to trigger the original-hit denominator / entry-hit numerator mismatch.
static void TestTexturedIORAdapter()
{
	std::cout << "Test D: textured IOR uses one record for Ft and normalization" << std::endl;
	IRasterImage* image = nullptr;
	RISE_API_CreateRISEColorRasterImage( &image, 2, 2, RISEColor(RISEPel(0,0,0),1) );
	image->SetPEL( 0, 0, RISEColor(RISEPel(1,1,1),1) );
	IRasterImageAccessor* accessor = nullptr;
	RISE_API_CreateNNBRasterImageAccessor( &accessor, *image );
	IScalarPainter* ior = nullptr;
	RISE_API_CreateTextureScalarPainterAffine( &ior, accessor, 0, 0.5, 1.5 );
	accessor->release(); image->release();
	RGBScalarPainter* absorption = new RGBScalarPainter(0.05,0.10,0.20); absorption->addref();
	RGBScalarPainter* scattering = new RGBScalarPainter(1,1,1); scattering->addref();
	SubSurfaceScatteringMaterial* material = new SubSurfaceScatteringMaterial(
		*ior, *absorption, *scattering, 0.0, 0.2 ); material->addref();
	ISubSurfaceDiffusionProfile* profile = material->GetDiffusionProfile();
	Object* sphere = MakeUnitSphere();
	for( int reverse = 0; reverse < 2; ++reverse ) {
		RayIntersectionGeometric original = MakeSurfaceRI();
		RayIntersectionGeometric entry = MakeSurfaceRI();
		original.ptCoord = reverse ? Point2(0.1,0.1) : Point2(0.75,0.75);
		entry.ptCoord = reverse ? Point2(0.75,0.75) : Point2(0.1,0.1);
		const Scalar originalEta = profile->GetIOR(original);
		const Scalar entryEta = profile->GetIOR(entry);
		const Scalar expectedOriginal = reverse ? 2.0 : 1.5;
		const Scalar expectedEntry = reverse ? 1.5 : 2.0;
		if( !Close(originalEta,expectedOriginal) || !Close(entryEta,expectedEntry) ) {
			std::cerr << "FAIL: real IOR texture must select distinct known texels" << std::endl;
			++gFailures;
		}
		std::cout << "  original IOR=" << originalEta << " entry IOR=" << entryEta << std::endl;
		BSSRDFAdapters::BSSRDFEntryBSDF adapter(profile, originalEta);
		RequireUnitIntegral("textured same-record RGB control", originalEta,
			IntegrateActual([&](Scalar mu) { return adapter.value(DirectionForCosine(mu),original)[0]; }, "textured control"));
		RequireUnitIntegral("textured entry RGB", entryEta,
			IntegrateActual([&](Scalar mu) { return adapter.value(DirectionForCosine(mu),entry)[0]; }, "textured RGB"));
		RequireUnitIntegral("textured entry NM", entryEta,
			IntegrateActual([&](Scalar mu) { return adapter.valueNM(DirectionForCosine(mu),entry,550); }, "textured NM"));
		for( int spectral = 0; spectral < 2; ++spectral ) {
			TestSampler sampler(3000 + reverse*2 + spectral);
			int active = 0;
			bool reportedMismatch = false;
			for( int attempt = 0; attempt < 256; ++attempt ) {
				const auto sample = BSSRDFSampling::SampleEntryPoint(
					original,sphere,material,sampler,spectral ? 550.0 : 0.0);
				if( !sample.valid ) continue;
				++active;
				RayIntersectionGeometric evaluated = entry;
				evaluated.vNormal = sample.entryNormal;
				const Vector3 direction = sample.scatteredRay.Dir();
				if( spectral ) {
					RequireFinitePositive(sample.weightSpatialNM,"textured NM spatial weight");
					const Scalar continuation = sample.weightNM / sample.weightSpatialNM;
					RequireFinitePositive(continuation,"textured NM continuation");
					RequireRatio("textured NM adapter/continuation", adapter.valueNM(direction,evaluated,550)*PI,
						continuation,entryEta,reportedMismatch);
				} else {
					const RISEPel sw = adapter.value(direction,evaluated);
					for( int channel = 0; channel < 3; ++channel ) {
						RequireFinitePositive(sample.weightSpatial[channel],"textured RGB spatial weight");
						const Scalar continuation = sample.weight[channel] / sample.weightSpatial[channel];
						RequireFinitePositive(continuation,"textured RGB continuation");
						RequireRatio("textured RGB adapter/continuation",sw[channel]*PI,
							continuation,entryEta,reportedMismatch);
					}
				}
			}
			std::cout << "  textured " << (spectral ? "NM" : "RGB") << " original=" << originalEta
				<< " entry=" << entryEta << " active=" << active << "/256" << std::endl;
			if( active < 16 ) { ++gFailures; std::cerr << "FAIL: textured sample activity" << std::endl; }
		}
	}
	sphere->release(); material->release(); ior->release(); absorption->release(); scattering->release();
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

// ================================================================
// Test E (DL-306): reflection + transmission partition at ONE interface.
//
// SubSurfaceScatteringSPF prices the surface REFLECTION of an SSS
// boundary with the exact dielectric Fresnel law; the integrators price
// the TRANSMISSION into the subsurface event with the profile's
// FresnelTransmission (diffusion) or the Sw adapters' law (random walk).
// Both halves describe the same interface, so at every exterior cosine
// R(mu) + T(mu) must be 1, and over a cosine-weighted incident hemisphere
// <R> + <T> must be 1.  Before DL-306 T was Schlick's approximation:
// <R> + <T> = 0.966 / 0.980 / 0.9992 / 1.006 at eta 1.05 / 1.128 / 1.33 /
// 1.5 (independent quadrature, recorded in docs/DL306_SSS_FRESNEL_PARTITION.md).
//
// Every quantity comes from production code EXCEPT the oracles:
//   R      the real SubSurfaceScatteringSPF (smooth), RGB kray and NM krayNM,
//          at an IOR stack whose top is the exterior index;
//   T      the real Burley profile's FresnelTransmission at a record whose
//          ambientIOR is that exterior index;
//   shape  the RandomWalkEntryBSDF / BSSRDFEntryBSDF adapters: pi*Sw(mu)
//          must be (1 - R(mu)) / c for ONE constant c at every mu (the
//          random walk's T lives only in this shape and in the integrators'
//          entry coin, which call the same helper);
//   c      that constant must equal an independent quadrature of
//          2 * integral (1 - F_exact(mu)) mu dmu.
// eta < 1 (a denser exterior) is included: past the critical angle the
// SPF reflects totally and T must be exactly 0.
// ================================================================

static Scalar IndependentExactReflectance( const Scalar cosI, const Scalar ni, const Scalar nt )
{
	const Scalar sin2T = (ni / nt) * (ni / nt) * (1.0 - cosI * cosI);
	if( sin2T >= 1.0 ) {
		return 1.0;
	}
	const Scalar cosT = sqrt( 1.0 - sin2T );
	const Scalar rs = (ni * cosI - nt * cosT) / (ni * cosI + nt * cosT);
	const Scalar rp = (nt * cosI - ni * cosT) / (nt * cosI + ni * cosT);
	return 0.5 * (rs * rs + rp * rp);
}

// 2 * integral_0^1 (1 - F_exact(mu; 1 -> eta)) mu dmu by a 2^20-bin
// midpoint rule.  The integrand is continuous (a square-root edge at the
// critical cosine for eta < 1), so the rule's error is below 1e-9.
static Scalar IndependentExactNormalization( const Scalar eta )
{
	const int count = 1 << 20;
	Scalar sum = 0;
	for( int i = 0; i < count; ++i ) {
		const Scalar mu = (Scalar(i) + 0.5) / count;
		sum += 2.0 * mu * (1.0 - IndependentExactReflectance( mu, 1.0, eta ));
	}
	return sum / count;
}

static RayIntersectionGeometric MakeIncidentRI( const Scalar mu, const Scalar exterior )
{
	RayIntersectionGeometric ri = MakeSurfaceRI();
	const Vector3 incoming = -DirectionForCosine( mu );
	ri.ray = Ray( Point3Ops::mkPoint3( ri.ptIntersection, -incoming * 2.0 ), incoming );
	ri.ambientIOR = exterior;
	return ri;
}

static void TestReflectionTransmissionPartition()
{
	std::cout << "Test E: SPF reflection + subsurface transmission partition (DL-306)" << std::endl;
	struct Case { Scalar interior, exterior; };
	// Relative indices 1.05 / 1.128 / 1.33 / 1.5 in air; 1.128 immersed
	// (1.5 in water); and the eta < 1 direction (1.33 in 1.5 glass, 1.5 in
	// 1.575).
	const Case cases[] = {
		{ 1.05, 1.0 }, { 1.128, 1.0 }, { 1.33, 1.0 }, { 1.5, 1.0 },
		{ 1.5, 1.33 }, { 1.33, 1.5 }, { 1.5, 1.575 } };
	const int count = 4096;

	for( const Case& cs : cases )
	{
		const Scalar eta = cs.interior / cs.exterior;
		UniformScalarPainter* ior = new UniformScalarPainter( cs.interior ); ior->addref();
		RGBScalarPainter* absorption = new RGBScalarPainter( 0.05, 0.10, 0.20 ); absorption->addref();
		RGBScalarPainter* scattering = new RGBScalarPainter( 1.0, 1.0, 1.0 ); scattering->addref();
		BurleyNormalizedDiffusionProfile* profile =
			new BurleyNormalizedDiffusionProfile( *ior, *absorption, *scattering, 0.0 );
		profile->addref();
		SubSurfaceScatteringSPF* spf = new SubSurfaceScatteringSPF( *ior, 0.0, 0.0, true );
		spf->addref();
		BSSRDFAdapters::BSSRDFEntryBSDF diffusionEntry( profile, cs.interior );
		BSSRDFAdapters::RandomWalkEntryBSDF walkEntry( cs.interior );
		const IORStack stack( cs.exterior );
		TestSampler sampler( 7000 );

		Scalar worstPoint = 0, meanR = 0, meanT = 0;
		Scalar cMinW = RISE_INFINITY, cMaxW = 0, cMinD = RISE_INFINITY, cMaxD = 0;
		bool tirExact = true;
		for( int i = 0; i < count; ++i )
		{
			const Scalar mu = (Scalar(i) + 0.5) / count;
			const RayIntersectionGeometric ri = MakeIncidentRI( mu, cs.exterior );

			ScatteredRayContainer rays, raysNM;
			spf->Scatter( ri, sampler, rays, stack );
			spf->ScatterNM( ri, sampler, 550.0, raysNM, stack );
			const Scalar R = rays.Count() == 1 ? rays[0].kray[0] : -1.0;
			const Scalar RNM = raysNM.Count() == 1 ? raysNM[0].krayNM : -1.0;
			const Scalar T = profile->FresnelTransmission( mu, ri );
			if( R < 0 || RNM < 0 || !std::isfinite(T) ) {
				std::cerr << "FAIL: partition eta=" << eta << " mu=" << mu
					<< " SPF did not emit one reflection ray" << std::endl;
				++gFailures;
				break;
			}
			worstPoint = std::fmax( worstPoint, std::fabs( R + T - 1.0 ) );
			worstPoint = std::fmax( worstPoint, std::fabs( RNM + T - 1.0 ) );
			meanR += 2.0 * mu * R / count;
			meanT += 2.0 * mu * T / count;
			if( R >= 1.0 && T != 0.0 ) tirExact = false;

			// Adapter shape: (1 - R) / (pi * Sw) must be one constant c.
			const Scalar oneMinusR = 1.0 - R;
			if( oneMinusR > 1e-3 ) {
				const Vector3 wi = DirectionForCosine( mu );
				const Scalar swW = walkEntry.value( wi, ri )[0];
				const Scalar swD = diffusionEntry.value( wi, ri )[0];
				if( swW > 0 && swD > 0 ) {
					const Scalar cW = oneMinusR / (PI * swW);
					const Scalar cD = oneMinusR / (PI * swD);
					cMinW = std::fmin( cMinW, cW ); cMaxW = std::fmax( cMaxW, cW );
					cMinD = std::fmin( cMinD, cD ); cMaxD = std::fmax( cMaxD, cD );
				} else {
					tirExact = false;
				}
			}
		}

		const Scalar cIndependent = IndependentExactNormalization( eta );
		const Scalar hemi = meanR + meanT;
		std::cout << std::setprecision(9)
			<< "  n_s=" << cs.interior << " n_e=" << cs.exterior << " eta=" << eta
			<< "  <R>=" << meanR << " <T>=" << meanT << " <R>+<T>=" << hemi
			<< "  worst |R+T-1|=" << worstPoint
			<< "  c walk [" << cMinW << "," << cMaxW << "] diffusion [" << cMinD << "," << cMaxD << "]"
			<< " independent " << cIndependent << std::endl;

		if( !(worstPoint < 1e-4) ) {
			std::cerr << "FAIL: partition eta=" << eta << " pointwise |R+T-1| = " << worstPoint << std::endl;
			++gFailures;
		}
		if( !(std::fabs( hemi - 1.0 ) < 1e-4) ) {
			std::cerr << "FAIL: partition eta=" << eta << " hemispherical <R>+<T> = " << hemi << std::endl;
			++gFailures;
		}
		if( !tirExact ) {
			std::cerr << "FAIL: partition eta=" << eta << " transmission past total reflection" << std::endl;
			++gFailures;
		}
		// One constant across mu (the Sw SHAPE is 1 - R), equal to the
		// independent normalization of the exact law.
		const bool shapeW = cMaxW > 0 && (cMaxW - cMinW) <= 1e-7 * cMaxW &&
			std::fabs( cMaxW - cIndependent ) <= 1e-7;
		const bool shapeD = cMaxD > 0 && (cMaxD - cMinD) <= 1e-7 * cMaxD &&
			std::fabs( cMaxD - cIndependent ) <= 1e-7;
		if( !shapeW ) {
			std::cerr << "FAIL: partition eta=" << eta << " RandomWalkEntryBSDF Sw is not (1-R)/(c pi) with c = "
				<< cIndependent << std::endl;
			++gFailures;
		}
		if( !shapeD ) {
			std::cerr << "FAIL: partition eta=" << eta << " BSSRDFEntryBSDF Sw is not (1-R)/(c pi) with c = "
				<< cIndependent << std::endl;
			++gFailures;
		}

		spf->release();
		profile->release();
		ior->release();
		absorption->release();
		scattering->release();
	}
}

} // namespace

int main()
{
	std::cout << "=== DL-48 BSSRDF Sw normalization ===" << std::endl;
	TestDirectionalEvaluators();
	TestDiffusionSampleRatios();
	TestRandomWalkSampleRatios();
	TestTexturedIORAdapter();
	TestReflectionTransmissionPartition();
	if( gFailures ) {
		std::cerr << "=== DL-48 normalization failures: " << gFailures << " ==="
			<< std::endl;
		return 1;
	}
	std::cout << "=== All DL-48 normalization tests passed ===" << std::endl;
	return 0;
}
