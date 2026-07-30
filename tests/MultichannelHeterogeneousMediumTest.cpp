//////////////////////////////////////////////////////////////////////
//
//  MultichannelHeterogeneousMediumTest.cpp
//
//  Phase-A gate for the painter-baked carbon + temperature medium:
//  shared trilinear lattice, chromatic phi(T) optics, the 10^-3 g/m^3
//  conversion, spectral tracking, scene-unit invariance, and §9 requiredness.
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IScalarPainterManager.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Materials/HenyeyGreensteinPhaseFunction.h"
#include "../src/Library/Materials/HeterogeneousMedium.h"
#include "../src/Library/Materials/HomogeneousMedium.h"
#include "../src/Library/Parsers/ChunkParserRegistry.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Utilities/MediumTransport.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/Reference.h"

using namespace RISE;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

namespace
{
	int passed = 0;
	int failed = 0;

	void Check( const bool condition, const char* label )
	{
		if( condition ) {
			++passed;
		} else {
			++failed;
			std::cout << "FAIL: " << label << std::endl;
		}
	}

	bool Near( const Scalar actual, const Scalar expected, const Scalar tolerance )
	{
		return std::fabs( actual - expected ) <= tolerance;
	}

	bool NearRelative( const Scalar actual, const Scalar expected, const Scalar tolerance )
	{
		const Scalar scale = std::fmax( std::fabs( expected ), Scalar( 1e-300 ) );
		return std::fabs( actual - expected ) <= tolerance * scale;
	}

	Scalar ScalarFromBits( const std::uint64_t bits )
	{
		static_assert( sizeof( Scalar ) == sizeof( bits ),
			"non-finite regression inputs require binary64 Scalar" );
		volatile std::uint64_t barrier = bits;
		const std::uint64_t materialised = barrier;
		Scalar value = 0.0;
		std::memcpy( &value, &materialised, sizeof( value ) );
		return value;
	}

	class AffineWorldScalarPainter :
		public virtual IScalarPainter,
		public virtual Implementation::Reference
	{
	public:
		Scalar bias;
		Scalar x;
		Scalar y;
		Scalar z;

		AffineWorldScalarPainter( Scalar bias_, Scalar x_, Scalar y_, Scalar z_ ) :
		  bias( bias_ ), x( x_ ), y( y_ ), z( z_ )
		{
		}

		ScalarTriple GetValuesAt( const RayIntersectionGeometric& ri ) const override
		{
			return ScalarTriple(
				bias + x * ri.ptIntersection.x + y * ri.ptIntersection.y + z * ri.ptIntersection.z );
		}

		bool HasPerChannelVariation() const override { return false; }

	protected:
		~AffineWorldScalarPainter() override = default;
	};

	class DerivedMultichannelHeterogeneousMedium final :
		public MultichannelHeterogeneousMedium
	{
	public:
		DerivedMultichannelHeterogeneousMedium(
			const IScalarPainter& carbonPainter,
			const IScalarPainter& temperaturePainter,
			const IPhaseFunction& phase
			) :
		  MultichannelHeterogeneousMedium(
			carbonPainter, temperaturePainter, 2, 2, 2,
			Point3( 0, 0, 0 ), Point3( 1, 1, 1 ), 1.0,
			0.26, 1800.0, 0.40, 0.85, 8.7, 1.2, 0.70, -0.35,
			phase )
		{
		}

		~DerivedMultichannelHeterogeneousMedium() override = default;
	};

	class PluginPhase final :
		public virtual IPhaseFunction,
		public virtual Implementation::Reference
	{
	public:
		Scalar Evaluate( const Vector3&, const Vector3& ) const override
		{
			return 1.0 / FOUR_PI;
		}
		Vector3 Sample( const Vector3& wi, ISampler& ) const override { return wi; }
		Scalar Pdf( const Vector3&, const Vector3& ) const override
		{
			return 1.0 / FOUR_PI;
		}

	protected:
		~PluginPhase() override = default;
	};

	class DerivedHomogeneousMedium final : public HomogeneousMedium
	{
	public:
		explicit DerivedHomogeneousMedium( const IPhaseFunction& phase ) :
		  HomogeneousMedium( RISEPel( 0, 0, 0 ), RISEPel( 1, 1, 1 ), phase )
		{
		}

		~DerivedHomogeneousMedium() override = default;
	};

	class PluginMedium final :
		public virtual IMedium,
		public virtual Implementation::Reference
	{
	public:
		explicit PluginMedium( const IPhaseFunction& phase ) : m_phase( phase )
		{
			m_phase.addref();
		}

		MediumCoefficients GetCoefficients( const Point3& ) const override
		{
			MediumCoefficients c;
			c.sigma_t = RISEPel( 1, 1, 1 );
			c.sigma_s = RISEPel( 1, 1, 1 );
			c.emission = RISEPel( 0, 0, 0 );
			return c;
		}
		MediumCoefficientsNM GetCoefficientsNM( const Point3&, const Scalar ) const override
		{
			MediumCoefficientsNM c;
			c.sigma_t = 1;
			c.sigma_s = 1;
			c.emission = 0;
			return c;
		}
		const IPhaseFunction* GetPhaseFunction() const override { return &m_phase; }
		Scalar SampleDistance(
			const Ray&, const Scalar maxDist, ISampler&, bool& scattered ) const override
		{
			scattered = false;
			return maxDist;
		}
		Scalar SampleDistanceNM(
			const Ray&, const Scalar maxDist, const Scalar,
			ISampler&, bool& scattered ) const override
		{
			scattered = false;
			return maxDist;
		}
		RISEPel EvalTransmittance( const Ray&, const Scalar ) const override
		{
			return RISEPel( 1, 1, 1 );
		}
		Scalar EvalTransmittanceNM(
			const Ray&, const Scalar, const Scalar ) const override { return 1; }
		bool IsHomogeneous() const override { return true; }

	protected:
		~PluginMedium() override { m_phase.release(); }

	private:
		const IPhaseFunction& m_phase;
	};

	IMedium* CreateMedium(
		const IScalarPainter& carbon,
		const IScalarPainter& temperature,
		const Point3& bboxMin,
		const Point3& bboxMax,
		const Scalar sceneUnitMeters,
		const unsigned int resolution = 8
		)
	{
		IMedium* medium = nullptr;
		const bool ok = RISE_API_CreateMultichannelHeterogeneousMedium(
			&medium, carbon, temperature,
			resolution, resolution, resolution,
			bboxMin, bboxMax, sceneUnitMeters,
			0.26, 1800.0, 0.10, 0.5,
			8.7, 1.2, 0.6, 0.6 );
		return ok ? medium : nullptr;
	}

	Scalar SampleMeanCosine(
		const IPhaseFunction& phase,
		const unsigned int seed,
		const unsigned int sampleCount = 40000
		)
	{
		RandomNumberGenerator rng( seed );
		Implementation::IndependentSampler sampler( rng );
		const Vector3 wi( 0, 0, 1 );
		Scalar sum = 0.0;
		for( unsigned int i = 0; i < sampleCount; ++i ) {
			sum += Vector3Ops::Dot( wi, phase.Sample( wi, sampler ) );
		}
		return sum / Scalar( sampleCount );
	}

	struct FactoryInputs
	{
		Point3 bboxMin;
		Point3 bboxMax;
		Scalar sceneUnitMeters;
		Scalar sootEm;
		Scalar sootDensity;
		Scalar sootAlbedoHot;
		Scalar sootGHot;
		Scalar smokeKmCarbon;
		Scalar smokeNCarbon;
		Scalar smokeAlbedoCarbon;
		Scalar smokeGCarbon;

		FactoryInputs() :
		  bboxMin( 0, 0, 0 ), bboxMax( 1, 1, 1 ), sceneUnitMeters( 1.0 ),
		  sootEm( 0.26 ), sootDensity( 1800.0 ), sootAlbedoHot( 0.10 ), sootGHot( 0.5 ),
		  smokeKmCarbon( 8.7 ), smokeNCarbon( 1.2 ),
		  smokeAlbedoCarbon( 0.6 ), smokeGCarbon( 0.6 )
		{
		}
	};

	bool FactoryRejects(
		const IScalarPainter& carbon,
		const IScalarPainter& temperature,
		const FactoryInputs& inputs
		)
	{
		IMedium* medium = nullptr;
		const bool created = RISE_API_CreateMultichannelHeterogeneousMedium(
			&medium, carbon, temperature, 2, 2, 2,
			inputs.bboxMin, inputs.bboxMax, inputs.sceneUnitMeters,
			inputs.sootEm, inputs.sootDensity, inputs.sootAlbedoHot, inputs.sootGHot,
			inputs.smokeKmCarbon, inputs.smokeNCarbon,
			inputs.smokeAlbedoCarbon, inputs.smokeGCarbon );
		safe_release( medium );
		return !created && !medium;
	}

	void TestBakedTrilinearChannelsAndOptics()
	{
		std::cout << "TestBakedTrilinearChannelsAndOptics" << std::endl;
		AffineWorldScalarPainter* carbon = new AffineWorldScalarPainter( 2.0, 1.0, 2.0, 3.0 );
		AffineWorldScalarPainter* temperature = new AffineWorldScalarPainter( 600.0, 400.0, 0.0, 0.0 );
		IMedium* medium = CreateMedium(
			*carbon, *temperature, Point3( 0, 0, 0 ), Point3( 1, 1, 1 ), 1.0 );
		Check( medium != nullptr, "factory creates the two-channel medium" );
		MultichannelHeterogeneousMedium* fire =
			dynamic_cast<MultichannelHeterogeneousMedium*>( medium );
		Check( fire != nullptr && fire->IsValid(), "factory returns the multichannel concrete type" );
		if( !fire ) {
			safe_release( medium );
			safe_release( carbon );
			safe_release( temperature );
			return;
		}

		const Point3 p( 0.4375, 0.4375, 0.4375 );
		const Scalar expectedCarbon = 4.625;
		const Scalar expectedTemperature = 775.0;
		const Scalar smoothArg = (expectedTemperature - 700.0) / 200.0;
		const Scalar expectedPhi = smoothArg * smoothArg * (3.0 - 2.0 * smoothArg);
		Check( Near( fire->LookupCarbon( p ), expectedCarbon, 1e-12 ),
			"trilinear bake reproduces an affine carbon field between voxels" );
		Check( Near( fire->LookupTemperature( p ), expectedTemperature, 1e-12 ),
			"temperature shares the same trilinear lattice" );
		Check( Near( fire->HotOpticsFraction( p ), expectedPhi, 1e-12 ),
			"phi(T) is the pinned 700-900 K smoothstep" );

		const Scalar hotAbsorptionMass =
			6.0 * PI * 0.26 * 1.0e-3 / (633.0e-9 * 1800.0);
		const Scalar hotAbsorption = expectedCarbon * expectedPhi * hotAbsorptionMass;
		const Scalar hotScattering = hotAbsorption * 0.10 / 0.90;
		const Scalar coolExtinction = expectedCarbon * (1.0 - expectedPhi) * 8.7;
		const Scalar coolScattering = coolExtinction * 0.6;
		const Scalar expectedSigmaS = hotScattering + coolScattering;
		const Scalar expectedSigmaT = hotAbsorption + hotScattering + coolExtinction;
		const MediumCoefficients c = fire->GetCoefficients( p );
		Check( Near( c.sigma_t[0], expectedSigmaT, 1e-11 ) &&
			Near( c.sigma_t[1], expectedSigmaT, 1e-11 ) &&
			Near( c.sigma_t[2], expectedSigmaT, 1e-11 ),
			"grey 633-nm extinction follows the hot/cool carbon partition" );
		Check( Near( c.sigma_s[0], expectedSigmaS, 1e-11 ),
			"grey scattering applies both authored single-scattering albedos" );
		const Scalar hotScale500 = 633.0 / 500.0;
		const Scalar coolScale500 = std::pow( hotScale500, 1.2 );
		const Scalar expectedSigmaS500 =
			hotScattering * hotScale500 + coolScattering * coolScale500;
		const Scalar expectedSigmaT500 =
			(hotAbsorption + hotScattering) * hotScale500 +
			coolExtinction * coolScale500;
		const MediumCoefficientsNM cnm = fire->GetCoefficientsNM( p, 500.0 );
		Check( Near( cnm.sigma_t, expectedSigmaT500, 1e-11 ) &&
			Near( cnm.sigma_s, expectedSigmaS500, 1e-11 ),
			"NM coefficients apply the pinned hot 1/lambda and cool lambda^-n laws" );

		carbon->bias = 200.0;
		temperature->bias = 2000.0;
		Check( Near( fire->LookupCarbon( p ), expectedCarbon, 1e-12 ) &&
			Near( fire->LookupTemperature( p ), expectedTemperature, 1e-12 ),
			"channel painters are baked once rather than sampled during rendering" );

		safe_release( medium );
		safe_release( carbon );
		safe_release( temperature );
	}

	void TestChromaticNMTrackingAndTransmittance()
	{
		std::cout << "TestChromaticNMTrackingAndTransmittance" << std::endl;
		IScalarPainter* carbon = nullptr;
		IScalarPainter* hotTemperature = nullptr;
		IScalarPainter* coolTemperature = nullptr;
		RISE_API_CreateUniformScalarPainter( &carbon, 0.2 );
		RISE_API_CreateUniformScalarPainter( &hotTemperature, 1000.0 );
		RISE_API_CreateUniformScalarPainter( &coolTemperature, 600.0 );
		IMedium* hot = CreateMedium( *carbon, *hotTemperature,
			Point3( 0, 0, 0 ), Point3( 1, 1, 1 ), 1.0 );
		IMedium* cool = CreateMedium( *carbon, *coolTemperature,
			Point3( 0, 0, 0 ), Point3( 1, 1, 1 ), 1.0 );
		Check( hot && cool, "uniform hot and cool chromatic fixtures construct" );
		if( hot && cool ) {
			const Point3 midpoint( 0.5, 0.5, 0.5 );
			const MultichannelHeterogeneousMedium* hotFire =
				dynamic_cast<const MultichannelHeterogeneousMedium*>( hot );
			const MediumCoefficientsNM hot500 = hot->GetCoefficientsNM( midpoint, 500.0 );
			const MediumCoefficientsNM hot700 = hot->GetCoefficientsNM( midpoint, 700.0 );
			Check( NearRelative( hot500.sigma_t / hot700.sigma_t, 1.4, 1e-13 ) &&
				NearRelative( hot500.sigma_s / hot700.sigma_s, 1.4, 1e-13 ),
				"hot-soot absorption and scattering both follow 1/lambda" );

			const MediumCoefficientsNM cool500 = cool->GetCoefficientsNM( midpoint, 500.0 );
			const MediumCoefficientsNM cool700 = cool->GetCoefficientsNM( midpoint, 700.0 );
			const Scalar coolRatio = std::pow( 700.0 / 500.0, 1.2 );
			Check( NearRelative( cool500.sigma_t / cool700.sigma_t, coolRatio, 1e-13 ) &&
				NearRelative( cool500.sigma_s / cool500.sigma_t, 0.6, 1e-13 ),
				"cool-smoke extinction follows lambda^-n with its authored albedo split" );
			if( hotFire ) {
				const Scalar majorant380 = hotFire->TrackingMajorantAtNM( midpoint, 380.0 );
				const Scalar majorant500 = hotFire->TrackingMajorantAtNM( midpoint, 500.0 );
				const Scalar majorant780 = hotFire->TrackingMajorantAtNM( midpoint, 780.0 );
				Check( majorant380 >= hot->GetCoefficientsNM( midpoint, 380.0 ).sigma_t &&
					majorant500 >= hot500.sigma_t &&
					majorant780 >= hot->GetCoefficientsNM( midpoint, 780.0 ).sigma_t,
					"spectral tracking majorant bounds extinction across the visible interval" );
				Check( majorant380 == majorant500 && majorant500 == majorant780,
					"Phase-A NM tracking retains the locked max-over-lambda majorant" );
			}

			const Ray ray( Point3( 0.5, 0.5, 0.125 ), Vector3( 0, 0, 1 ) );
			const Scalar length = 0.5;
			const Scalar expectedT500 = std::exp( -hot500.sigma_t * length );
			const Scalar expectedT700 = std::exp( -hot700.sigma_t * length );
			Check( NearRelative( hot->EvalDistancePdfNM(
				ray, length, false, length, 500.0 ), expectedT500, 1e-12 ) &&
				NearRelative( hot->EvalDistancePdfNM(
				ray, length, false, length, 700.0 ), expectedT700, 1e-12 ),
				"deterministic NM distance survival uses chromatic optical depth" );

			const unsigned int samples = 30000;
			RandomNumberGenerator rng500( 0x500u );
			RandomNumberGenerator rng700( 0x700u );
			Implementation::IndependentSampler sampler500( rng500 );
			Implementation::IndependentSampler sampler700( rng700 );
			unsigned int events500 = 0;
			unsigned int events700 = 0;
			Scalar transmittance500 = 0.0;
			Scalar transmittance700 = 0.0;
			for( unsigned int i = 0; i < samples; ++i ) {
				bool scattered500 = false;
				bool scattered700 = false;
				hot->SampleDistanceNM( ray, length, 500.0, sampler500, scattered500 );
				hot->SampleDistanceNM( ray, length, 700.0, sampler700, scattered700 );
				if( scattered500 ) ++events500;
				if( scattered700 ) ++events700;
				transmittance500 += hot->EvalTransmittanceNM( ray, length, 500.0 );
				transmittance700 += hot->EvalTransmittanceNM( ray, length, 700.0 );
			}
			const Scalar eventRate500 = Scalar(events500) / Scalar(samples);
			const Scalar eventRate700 = Scalar(events700) / Scalar(samples);
			Check( Near( eventRate500, 1.0 - expectedT500, 0.012 ) &&
				Near( eventRate700, 1.0 - expectedT700, 0.012 ),
				"delta tracking samples each wavelength's extinction law" );
			Check( Near( transmittance500 / Scalar(samples), expectedT500, 0.012 ) &&
				Near( transmittance700 / Scalar(samples), expectedT700, 0.012 ),
				"ratio tracking estimates chromatic NM transmittance" );
		}

		safe_release( hot );
		safe_release( cool );
		safe_release( carbon );
		safe_release( hotTemperature );
		safe_release( coolTemperature );
	}

	void TestWavelengthBoundConstituentPhaseClosure()
	{
		std::cout << "TestWavelengthBoundConstituentPhaseClosure" << std::endl;
		const Scalar hotG = 0.85;
		const Scalar coolG = -0.35;
		const Scalar hotAlbedo = 0.40;
		const Scalar coolAlbedo = 0.70;
		AffineWorldScalarPainter* carbon =
			new AffineWorldScalarPainter( 1.0, 0.0, 0.0, 0.0 );
		AffineWorldScalarPainter* temperature =
			new AffineWorldScalarPainter( 600.0, 400.0, 0.0, 0.0 );
		IMedium* medium = nullptr;
		const bool created = RISE_API_CreateMultichannelHeterogeneousMedium(
			&medium, *carbon, *temperature, 8, 8, 8,
			Point3( 0, 0, 0 ), Point3( 1, 1, 1 ), 1.0,
			0.26, 1800.0, hotAlbedo, hotG,
			8.7, 1.2, coolAlbedo, coolG );
		Check( created && medium, "phase-closure fixture constructs" );
		if( !medium ) {
			safe_release( carbon );
			safe_release( temperature );
			return;
		}

		Check( medium->GetPhaseFunction() == nullptr,
			"fire medium exposes no legacy fixed phase function" );
		Check( MediumTransport::IsContinuationPhaseClosureNMPreflightAllowlisted( *medium ),
			"exact multichannel fire medium occupies the continuation-closure allowlist row" );
		{
			IPhaseFunction* legacyPhase = nullptr;
			RISE_API_CreateHenyeyGreensteinPhaseFunction( &legacyPhase, 0.0 );
			DerivedMultichannelHeterogeneousMedium derived(
				*carbon, *temperature, *legacyPhase );
			Check( !MediumTransport::IsContinuationPhaseClosureNMPreflightAllowlisted( derived ),
				"derived multichannel fire medium is default-denied by the exact-type table" );
			safe_release( legacyPhase );
		}

		const Point3 coolPoint( 0.125, 0.5, 0.5 );
		const Point3 hotPoint( 0.875, 0.5, 0.5 );
		const Scalar wavelengths[] = { 450.0, 750.0 };
		for( const Scalar nm : wavelengths )
		{
			const IPhaseFunction* coolClosure = medium->MakePhaseClosure( coolPoint, nm );
			const IPhaseFunction* hotClosure = medium->MakePhaseClosure( hotPoint, nm );
			Check( coolClosure && hotClosure,
				"two-position/two-wavelength closures construct" );
			if( coolClosure && hotClosure )
			{
				Check( Near( coolClosure->GetMeanCosine(), coolG, 1e-12 ) &&
					Near( hotClosure->GetMeanCosine(), hotG, 1e-12 ),
					"opposite constituent positions bind their authored g values" );
				const Vector3 wi( 0, 0, 1 );
				const Vector3 wo = Vector3Ops::Normalize( Vector3( 0.6, 0, 0.8 ) );
				const Scalar expectedCool =
					HenyeyGreensteinPhaseFunction::EvaluateWithG( 0.8, coolG );
				const Scalar expectedHot =
					HenyeyGreensteinPhaseFunction::EvaluateWithG( 0.8, hotG );
				Check( NearRelative( coolClosure->Evaluate( wi, wo ), expectedCool, 1e-13 ) &&
					NearRelative( hotClosure->Evaluate( wi, wo ), expectedHot, 1e-13 ) &&
					NearRelative( coolClosure->Pdf( wi, wo ), expectedCool, 1e-13 ) &&
					NearRelative( hotClosure->Pdf( wi, wo ), expectedHot, 1e-13 ),
					"closure Evaluate/Pdf are the sigma_s-weighted HG law" );
			}
			safe_release( coolClosure );
			safe_release( hotClosure );
		}

		const Point3 mixedPoint( 0.5, 0.5, 0.5 );
		const Scalar mixedWavelengths[] = { 450.0, 750.0 };
		Scalar measuredMeans[2] = { 0.0, 0.0 };
		for( unsigned int i = 0; i < 2; ++i )
		{
			const Scalar nm = mixedWavelengths[i];
			const IPhaseFunction* closure = medium->MakePhaseClosure( mixedPoint, nm );
			Check( closure != nullptr, "mixed-constituent closure constructs" );
			if( closure )
			{
				const Scalar scale = 633.0 / nm;
				const Scalar hotAbsorptionMass =
					6.0 * PI * 0.26 * 1.0e-3 / (633.0e-9 * 1800.0);
				const Scalar hotS = 0.5 * hotAbsorptionMass * scale *
					hotAlbedo / (1.0 - hotAlbedo);
				const Scalar coolS = 0.5 * 8.7 * std::pow( scale, 1.2 ) * coolAlbedo;
				const Scalar expectedMean = (hotS * hotG + coolS * coolG) / (hotS + coolS);
				measuredMeans[i] = closure->GetMeanCosine();
				Check( NearRelative( measuredMeans[i], expectedMean, 1e-13 ),
					"mixed closure mean cosine uses local wavelength-dependent sigma_s weights" );

				const Vector3 wi( 0, 0, 1 );
				const Vector3 wo = Vector3Ops::Normalize( Vector3( 0.6, 0, 0.8 ) );
				RayIntersectionGeometric ri( Ray( mixedPoint, wi ), RasterizerState{ 0, 0 } );
				MediumTransport::MediumScatterBSDF adapterBSDF( closure, wi );
				MediumTransport::MediumScatterMaterial adapterMaterial( closure, wi );
				const Scalar expected = closure->Evaluate( wo, wi );
				const IORStack ior( 1.0 );
				Check( NearRelative( adapterBSDF.valueNM( wo, ri, nm ), expected, 1e-13 ) &&
					NearRelative( adapterMaterial.PdfNM( wo, ri, nm, ior ),
						closure->Pdf( wo, wi ), 1e-13 ),
					"NEE evaluation and Pdf adapters consume the retained closure instance" );
			}
			safe_release( closure );
		}
		Check( std::fabs( measuredMeans[0] - measuredMeans[1] ) > 1e-4,
			"one position binds distinct phase closures at distinct wavelengths" );

		const IPhaseFunction* coolClosure = medium->MakePhaseClosure( coolPoint, 500.0 );
		const IPhaseFunction* hotClosure = medium->MakePhaseClosure( hotPoint, 500.0 );
		if( coolClosure && hotClosure )
		{
			const Scalar sampledCool = SampleMeanCosine( *coolClosure, 0xc001u );
			const Scalar sampledHot = SampleMeanCosine( *hotClosure, 0x807u );
			Check( std::fabs( sampledCool - coolG ) < 0.015 &&
				std::fabs( sampledHot - hotG ) < 0.015 && sampledHot - sampledCool > 1.0,
				"stored constituent g values materially change sampled directions" );
		}
		safe_release( coolClosure );
		safe_release( hotClosure );

		safe_release( medium );
		safe_release( carbon );
		safe_release( temperature );
	}

	void TestContinuationPhaseClosurePreflightTable()
	{
		std::cout << "TestContinuationPhaseClosurePreflightTable" << std::endl;
		IPhaseFunction* isotropic = nullptr;
		RISE_API_CreateIsotropicPhaseFunction( &isotropic );
		HomogeneousMedium* exactHomogeneous = new HomogeneousMedium(
			RISEPel( 0, 0, 0 ), RISEPel( 1, 1, 1 ), *isotropic );
		Check( MediumTransport::IsContinuationPhaseClosureNMPreflightAllowlisted(
			*exactHomogeneous ),
			"exact homogeneous medium with exact isotropic phase is allowlisted" );

		DerivedHomogeneousMedium* derived = new DerivedHomogeneousMedium( *isotropic );
		Check( !MediumTransport::IsContinuationPhaseClosureNMPreflightAllowlisted( *derived ),
			"derived built-in medium is default-denied" );

		PluginMedium* pluginMedium = new PluginMedium( *isotropic );
		Check( !MediumTransport::IsContinuationPhaseClosureNMPreflightAllowlisted(
			*pluginMedium ),
			"plugin medium is default-denied" );

		PluginPhase* pluginPhase = new PluginPhase();
		HomogeneousMedium* pluginPhaseMedium = new HomogeneousMedium(
			RISEPel( 0, 0, 0 ), RISEPel( 1, 1, 1 ), *pluginPhase );
		Check( !MediumTransport::IsContinuationPhaseClosureNMPreflightAllowlisted(
			*pluginPhaseMedium ),
			"plugin phase on an exact built-in medium is default-denied" );

		HenyeyGreensteinPhaseFunction* invalidHG =
			new HenyeyGreensteinPhaseFunction( 1.0 );
		HomogeneousMedium* invalidHGMedium = new HomogeneousMedium(
			RISEPel( 0, 0, 0 ), RISEPel( 1, 1, 1 ), *invalidHG );
		Check( !MediumTransport::IsContinuationPhaseClosureNMPreflightAllowlisted(
			*invalidHGMedium ),
			"finite-density HG qualification rejects g at the singular endpoint" );

		AffineWorldScalarPainter* carbon =
			new AffineWorldScalarPainter( 1.0, 0.0, 0.0, 0.0 );
		AffineWorldScalarPainter* temperature =
			new AffineWorldScalarPainter( 1000.0, 0.0, 0.0, 0.0 );
		MultichannelHeterogeneousMedium* invalidFire =
			new MultichannelHeterogeneousMedium(
				*carbon, *temperature, 2, 2, 2,
				Point3( 0, 0, 0 ), Point3( 1, 1, 1 ), 1.0,
				0.26, 1800.0, 0.4, 1.0, 8.7, 1.2, 0.7, -0.35,
				*isotropic );
		Check( !invalidFire->IsValid() &&
			!MediumTransport::IsContinuationPhaseClosureNMPreflightAllowlisted(
				*invalidFire ),
			"invalid exact fire instance cannot pass continuation preflight" );

		safe_release( invalidFire );
		safe_release( temperature );
		safe_release( carbon );
		safe_release( invalidHGMedium );
		safe_release( invalidHG );
		safe_release( pluginPhaseMedium );
		safe_release( pluginPhase );
		safe_release( pluginMedium );
		safe_release( derived );
		safe_release( exactHomogeneous );
		safe_release( isotropic );
	}

	void TestPhysicalUnitsAndSceneScale()
	{
		std::cout << "TestPhysicalUnitsAndSceneScale" << std::endl;
		IScalarPainter* carbon = nullptr;
		IScalarPainter* temperature = nullptr;
		RISE_API_CreateUniformScalarPainter( &carbon, 1.0 );
		RISE_API_CreateUniformScalarPainter( &temperature, 1000.0 );
		IMedium* metres = CreateMedium(
			*carbon, *temperature, Point3( 0, 0, 0 ), Point3( 1, 1, 1 ), 1.0 );
		IMedium* centimetres = CreateMedium(
			*carbon, *temperature, Point3( 0, 0, 0 ), Point3( 100, 100, 100 ), 0.01 );
		MultichannelHeterogeneousMedium* m =
			dynamic_cast<MultichannelHeterogeneousMedium*>( metres );
		MultichannelHeterogeneousMedium* cm =
			dynamic_cast<MultichannelHeterogeneousMedium*>( centimetres );
		Check( m != nullptr && cm != nullptr, "metre and centimetre fixtures construct" );
		if( m && cm ) {
			Check( Near( m->HotSootVolumeFraction( Point3( 0.5, 0.5, 0.5 ) ),
				5.555555555555556e-7, 1e-18 ),
				"1 g/m^3 at phi=1 gives f_v=5.5556e-7" );
			const MediumCoefficients cM = m->GetCoefficients( Point3( 0.5, 0.5, 0.5 ) );
			const MediumCoefficients cCM = cm->GetCoefficients( Point3( 50, 50, 50 ) );
			Check( Near( cCM.sigma_t[0], 0.01 * cM.sigma_t[0], 1e-13 ),
				"inverse-length coefficients convert from SI to scene units" );

			const Ray rayM( Point3( 0.5, 0.5, 0.125 ), Vector3( 0, 0, 1 ) );
			const Ray rayCM( Point3( 50, 50, 12.5 ), Vector3( 0, 0, 1 ) );
			const Scalar transM = m->EvalDistancePdf( rayM, 0.5, false, 0.5 );
			const Scalar transCM = cm->EvalDistancePdf( rayCM, 50.0, false, 50.0 );
			const Scalar expectedTau = cM.sigma_t[0] * 0.5;
			Check( Near( -std::log( transM ), expectedTau, 1e-10 ),
				"deterministic optical depth matches the uniform slab" );
			Check( Near( transCM, transM, 1e-12 ),
				"the same physical slab is invariant between metre and centimetre scenes" );

			const MediumCoefficientsNM cM500 = m->GetCoefficientsNM(
				Point3( 0.5, 0.5, 0.5 ), 500.0 );
			const MediumCoefficientsNM cCM500 = cm->GetCoefficientsNM(
				Point3( 50, 50, 50 ), 500.0 );
			const Scalar transM500 = m->EvalDistancePdfNM(
				rayM, 0.5, false, 0.5, 500.0 );
			const Scalar transCM500 = cm->EvalDistancePdfNM(
				rayCM, 50.0, false, 50.0, 500.0 );
			Check( NearRelative( cCM500.sigma_t, 0.01 * cM500.sigma_t, 1e-13 ) &&
				NearRelative( transCM500, transM500, 1e-12 ),
				"chromatic NM coefficients and optical depth are scene-unit invariant" );
		}

		safe_release( metres );
		safe_release( centimetres );
		safe_release( carbon );
		safe_release( temperature );
	}

	void TestPhiSupMajorant()
	{
		std::cout << "TestPhiSupMajorant" << std::endl;
		// Opposing gradients make every x-cell endpoint product small while
		// carbon*k(phi(T)) is large in the interior.  A grid built from the
		// nonlinear product's knots is non-conservative; the required bound
		// is max_phi(k) times the carbon-only lattice majorant.
		AffineWorldScalarPainter* carbon = new AffineWorldScalarPainter( 1.5, -2.0, 0.0, 0.0 );
		AffineWorldScalarPainter* temperature = new AffineWorldScalarPainter( 600.0, 400.0, 0.0, 0.0 );
		const Scalar targetHotMassExtinction = 100.0;
		const Scalar sootEm = targetHotMassExtinction * 633.0e-9 * 1800.0 /
			(6.0 * PI * 1.0e-3);
		IMedium* medium = nullptr;
		const bool created = RISE_API_CreateMultichannelHeterogeneousMedium(
			&medium, *carbon, *temperature,
			2, 2, 2, Point3( 0, 0, 0 ), Point3( 1, 1, 1 ), 1.0,
			sootEm, 1800.0, 0.0, 0.5,
			1.0, 1.2, 0.0, 0.6 );
		MultichannelHeterogeneousMedium* fire =
			dynamic_cast<MultichannelHeterogeneousMedium*>( medium );
		Check( created && fire, "adversarial majorant fixture constructs" );
		if( fire ) {
			const Point3 interior( 0.5, 0.5, 0.5 );
			const Scalar sigmaT = fire->GetCoefficients( interior ).sigma_t[0];
			const Scalar majorant = fire->TrackingMajorantAt( interior );
			Check( Near( sigmaT, 25.25, 1e-11 ),
				"opposing carbon/temperature gradients create the intended interior maximum" );
			Check( majorant >= sigmaT,
				"phi-sup carbon majorant bounds the nonlinear interior extinction" );
			const Scalar spectralMajorant = fire->TrackingMajorantAtNM( interior, 500.0 );
			Check( spectralMajorant >= fire->GetCoefficientsNM( interior, 380.0 ).sigma_t &&
				spectralMajorant >= fire->GetCoefficientsNM( interior, 500.0 ).sigma_t &&
				spectralMajorant >= fire->GetCoefficientsNM( interior, 780.0 ).sigma_t,
				"max-over-lambda majorant bounds an adversarial nonlinear phi mixture" );
		}
		safe_release( medium );
		safe_release( carbon );
		safe_release( temperature );
	}

	void TestNonFiniteRejection()
	{
		std::cout << "TestNonFiniteRejection" << std::endl;
		const Scalar infinity = ScalarFromBits( UINT64_C(0x7FF0000000000000) );
		const Scalar nan = ScalarFromBits( UINT64_C(0x7FF8000000000000) );
		AffineWorldScalarPainter* carbon = new AffineWorldScalarPainter( 1.0, 0.0, 0.0, 0.0 );
		AffineWorldScalarPainter* temperature = new AffineWorldScalarPainter( 1000.0, 0.0, 0.0, 0.0 );

		AffineWorldScalarPainter* infiniteCarbon =
			new AffineWorldScalarPainter( infinity, 0.0, 0.0, 0.0 );
		Check( FactoryRejects( *infiniteCarbon, *temperature, FactoryInputs() ),
			"+Inf carbon painter is rejected before majorant construction" );
		safe_release( infiniteCarbon );

		AffineWorldScalarPainter* nanTemperature =
			new AffineWorldScalarPainter( nan, 0.0, 0.0, 0.0 );
		Check( FactoryRejects( *carbon, *nanTemperature, FactoryInputs() ),
			"NaN temperature painter is rejected under fast-math" );
		safe_release( nanTemperature );

		AffineWorldScalarPainter* zeroTemperature =
			new AffineWorldScalarPainter( 0.0, 0.0, 0.0, 0.0 );
		Check( FactoryRejects( *carbon, *zeroTemperature, FactoryInputs() ),
			"non-positive temperature is rejected" );
		safe_release( zeroTemperature );

		FactoryInputs invalid;
		invalid.bboxMin.x = nan;
		Check( FactoryRejects( *carbon, *temperature, invalid ),
			"NaN bbox component is rejected by the direct factory" );
		invalid = FactoryInputs();
		invalid.bboxMax.z = infinity;
		Check( FactoryRejects( *carbon, *temperature, invalid ),
			"+Inf bbox component is rejected by the direct factory" );

		struct ScalarInput
		{
			const char* label;
			Scalar FactoryInputs::* member;
		};
		const ScalarInput scalarInputs[] = {
			{ "scene_unit", &FactoryInputs::sceneUnitMeters },
			{ "soot_em", &FactoryInputs::sootEm },
			{ "soot_density", &FactoryInputs::sootDensity },
			{ "soot_albedo_hot", &FactoryInputs::sootAlbedoHot },
			{ "soot_g_hot", &FactoryInputs::sootGHot },
			{ "smoke_km_carbon", &FactoryInputs::smokeKmCarbon },
			{ "smoke_n_carbon", &FactoryInputs::smokeNCarbon },
			{ "smoke_albedo_carbon", &FactoryInputs::smokeAlbedoCarbon },
			{ "smoke_g_carbon", &FactoryInputs::smokeGCarbon }
		};
		for( const ScalarInput& input : scalarInputs ) {
			invalid = FactoryInputs();
			invalid.*(input.member) = infinity;
			Check( FactoryRejects( *carbon, *temperature, invalid ),
				(std::string( "+Inf " ) + input.label + " is rejected by the direct factory").c_str() );
		}

		invalid = FactoryInputs();
		invalid.sootGHot = 1.0;
		Check( FactoryRejects( *carbon, *temperature, invalid ),
			"g_hot=1 is rejected because the HG continuation density must remain finite" );
		invalid = FactoryInputs();
		invalid.smokeGCarbon = -1.0;
		Check( FactoryRejects( *carbon, *temperature, invalid ),
			"g_carbon=-1 is rejected because the HG continuation density must remain finite" );

		safe_release( carbon );
		safe_release( temperature );
	}

	const IAsciiChunkParser* FindMultichannelParser(
		const std::vector<ChunkParserEntry>& entries
		)
	{
		for( const ChunkParserEntry& entry : entries ) {
			if( entry.keyword == "multichannel_heterogeneous_medium" ) {
				return entry.parser.get();
			}
		}
		return nullptr;
	}

	void FillValidBag( ParseStateBag& bag, const char* omit )
	{
		struct Pair { const char* key; const char* value; };
		static const Pair values[] = {
			{ "name", "fire" },
			{ "channel_carbon", "painter carbon" },
			{ "channel_temperature", "painter temperature" },
			{ "bake_resolution", "4 4 4" },
			{ "bbox_min", "0 0 0" },
			{ "bbox_max", "1 1 1" },
			{ "soot_em", "0.26" },
			{ "soot_density", "1800" },
			{ "soot_albedo_hot", "0.10" },
			{ "soot_g_hot", "0.5" },
			{ "smoke_km_carbon", "8.7" },
			{ "smoke_n_carbon", "1.2" },
			{ "smoke_albedo_carbon", "0.6" },
			{ "smoke_g_carbon", "0.6" }
		};
		for( const Pair& value : values ) {
			if( !omit || std::string( value.key ) != omit ) {
				bag.SetSingle( value.key, value.value );
			}
		}
	}

	void TestDescriptorAndRequiredness()
	{
		std::cout << "TestDescriptorAndRequiredness" << std::endl;
		std::vector<ChunkParserEntry> entries = CreateAllChunkParsers();
		const IAsciiChunkParser* parser = FindMultichannelParser( entries );
		Check( parser != nullptr, "chunk is registered" );
		if( !parser ) return;

		const ChunkDescriptor& descriptor = parser->Describe();
		std::vector<std::string> required;
		for( const ParameterDescriptor& parameter : descriptor.parameters ) {
			if( parameter.required ) required.push_back( parameter.name );
		}
		Check( required.size() == 14, "all 14 Phase-A fields are descriptor-required" );

		IJobPriv* job = nullptr;
		RISE_CreateJobPriv( &job );
		Check( job != nullptr, "requiredness fixture job constructs" );
		if( !job ) return;

		for( const std::string& omitted : required ) {
			ParseStateBag bag( &descriptor );
			FillValidBag( bag, omitted.c_str() );
			Check( !parser->Finalize( bag, *job ),
				("omitting required parameter " + omitted + " fails").c_str() );
		}
		safe_release( job );
	}

	std::string SceneText()
	{
		return
			"RISE ASCII SCENE 7\n\n"
			"scene_options\n{\nscene_unit 0.01\n}\n\n"
			"scalar_painter\n{\nname carbon\nvalue 1\n}\n\n"
			"scalar_painter\n{\nname temperature\nvalue 800\n}\n\n"
			"multichannel_heterogeneous_medium\n{\n"
			"name fire\n"
			"channel_carbon painter carbon\n"
			"channel_temperature painter temperature\n"
			"bake_resolution 4 4 4\n"
			"bbox_min 0 0 0\n"
			"bbox_max 100 100 100\n"
			"soot_em 0.26\n"
			"soot_density 1800\n"
			"soot_albedo_hot 0.10\n"
			"soot_g_hot 0.8\n"
			"smoke_km_carbon 8.7\n"
			"smoke_n_carbon 1.2\n"
			"smoke_albedo_carbon 0.6\n"
			"smoke_g_carbon -0.4\n"
			"}\n";
	}

	void TestSceneLanguageAndSceneUnitPropagation()
	{
		std::cout << "TestSceneLanguageAndSceneUnitPropagation" << std::endl;
		char filename[128];
		std::snprintf( filename, sizeof(filename),
			"rise_multichannel_medium_%d.RISEscene", static_cast<int>( ::getpid() ) );
		const std::filesystem::path path =
			std::filesystem::temp_directory_path() / filename;
		{
			std::ofstream output( path );
			output << SceneText();
		}

		IJobPriv* job = nullptr;
		RISE_CreateJobPriv( &job );
		const bool loaded = job && job->LoadAsciiSceneViaCst( path.string().c_str() );
		Check( loaded, "complete multichannel scene chunk loads" );
		if( loaded ) {
			const IMedium* medium = job->GetMedium( "fire" );
			const MultichannelHeterogeneousMedium* fire =
				dynamic_cast<const MultichannelHeterogeneousMedium*>( medium );
			Check( fire != nullptr, "scene chunk registers the concrete medium" );
			if( fire ) {
				const Scalar hotAbsorptionMass =
					6.0 * PI * 0.26 * 1.0e-3 / (633.0e-9 * 1800.0);
				const Scalar expectedSigmaT = 0.01 * 0.5 *
					(hotAbsorptionMass / 0.90 + 8.7);
				Check( Near( fire->GetCoefficients( Point3( 50, 50, 50 ) ).sigma_t[0],
					expectedSigmaT, 1e-12 ),
					"scene_options.scene_unit reaches medium construction" );

				const Scalar wavelengths[] = { 450.0, 750.0 };
				for( const Scalar nm : wavelengths ) {
					const Scalar wavelengthScale = 633.0 / nm;
					const Scalar hotScattering = 0.5 * hotAbsorptionMass * wavelengthScale *
						0.10 / 0.90;
					const Scalar coolScattering = 0.5 * 8.7 *
						pow( wavelengthScale, 1.2 ) * 0.60;
					const Scalar expectedMean =
						(hotScattering * 0.8 + coolScattering * -0.4) /
						(hotScattering + coolScattering);
					const IPhaseFunction* closure = fire->MakePhaseClosure(
						Point3( 50, 50, 50 ), nm );
					Check( closure && Near( closure->GetMeanCosine(), expectedMean, 1e-12 ),
						"CST wiring preserves both authored g values in wavelength-bound mixtures" );
					if( closure ) closure->release();
				}
			}
		}

		safe_release( job );
		std::filesystem::remove( path );
	}
}

int main()
{
	TestBakedTrilinearChannelsAndOptics();
	TestChromaticNMTrackingAndTransmittance();
	TestWavelengthBoundConstituentPhaseClosure();
	TestContinuationPhaseClosurePreflightTable();
	TestPhysicalUnitsAndSceneScale();
	TestPhiSupMajorant();
	TestNonFiniteRejection();
	TestDescriptorAndRequiredness();
	TestSceneLanguageAndSceneUnitPropagation();

	std::cout << passed << " passed, " << failed << " failed" << std::endl;
	return failed == 0 ? 0 : 1;
}
