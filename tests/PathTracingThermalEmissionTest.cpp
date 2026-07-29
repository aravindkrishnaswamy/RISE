//////////////////////////////////////////////////////////////////////
//
//  PathTracingThermalEmissionTest.cpp - Phase-A step-6 spectral gates.
//
//  Verifies that the standalone PathTracingIntegrator scores fire thermal
//  emission before the pure-absorber and max-volume-bounce continuation
//  exits, agrees with RayCaster's already-gated estimator, preserves scene-
//  unit invariance, and routes HWSS-requested fire transport through the
//  unbiased per-wavelength NM fallback.
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#ifdef _WIN32
	#include <process.h>
	#define getpid _getpid
#else
	#include <unistd.h>
#endif

#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IRayCaster.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Interfaces/IShader.h"
#include "../src/Library/Interfaces/IShaderManager.h"
#include "../src/Library/Intersection/RayIntersection.h"
#include "../src/Library/Job.h"
#include "../src/Library/Lights/LightSampler.h"
#include "../src/Library/Materials/HeterogeneousMedium.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/SceneEditor/SceneEdit.h"
#include "../src/Library/SceneEditor/SceneEditor.h"
#include "../src/Library/Shaders/PathTracingIntegrator.h"
#include "../src/Library/Utilities/EquiangularSampler.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Utilities/PlanckRadiance.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/RuntimeContext.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

namespace
{
	int passed = 0;
	int failed = 0;
	const Scalar kTemperatureK = 1800.0;
	const Scalar kTargetSigmaSI = 0.8;
	const unsigned int kSamples = 80000;

	void Check( const bool condition, const char* label )
	{
		if( condition ) {
			++passed;
		} else {
			++failed;
			std::cout << "FAIL: " << label << std::endl;
		}
	}

	bool NearRelative( const Scalar actual, const Scalar expected, const Scalar tolerance )
	{
		const Scalar scale = std::fmax( std::fabs( expected ), Scalar( 1e-300 ) );
		return std::fabs( actual - expected ) <= tolerance * scale;
	}

	Scalar HotAbsorptionMass633()
	{
		return 6.0 * PI * 0.26 * 1.0e-3 / (633.0e-9 * 1800.0);
	}

	std::string SceneText(
		const Scalar sceneUnitMeters,
		const Scalar slabLength,
		const bool positionalLight,
		const Scalar sootAlbedoHot = 0.0 )
	{
		const Scalar scale = 1.0 / sceneUnitMeters;
		const Scalar carbon = kTargetSigmaSI / HotAbsorptionMass633();
		std::ostringstream ss;
		ss << std::setprecision( 17 ) <<
			"RISE ASCII SCENE 7\n"
			"scene_options\n{\nscene_unit " << sceneUnitMeters << "\n}\n"
			"standard_shader\n{\nname global\n}\n"
			"scalar_painter\n{\nname carbon\nvalue " << carbon << "\n}\n"
			"scalar_painter\n{\nname temperature\nvalue " << kTemperatureK << "\n}\n"
			"multichannel_heterogeneous_medium\n{\n"
			"name fire\n"
			"channel_carbon painter carbon\n"
			"channel_temperature painter temperature\n"
			"bake_resolution 4 4 4\n"
			"bbox_min " << -0.5 * scale << " " << -0.5 * scale << " " << -slabLength << "\n"
			"bbox_max " << 0.5 * scale << " " << 0.5 * scale << " " << 2.0 * slabLength << "\n"
			"soot_em 0.26\n"
			"soot_density 1800\n"
			"soot_albedo_hot " << sootAlbedoHot << "\n"
			"soot_g_hot 0.5\n"
			"smoke_km_carbon 8.7\n"
			"smoke_n_carbon 1.2\n"
			"smoke_albedo_carbon 0.6\n"
			"smoke_g_carbon 0.6\n"
			"}\n"
			"global_medium\n{\nmedium fire\n}\n"
			"box_geometry\n{\nname wall_geometry\nwidth " << 10.0 * scale <<
			"\nheight " << 10.0 * scale << "\ndepth " << 0.2 * scale << "\n}\n"
			"standard_object\n{\nname black_wall\ngeometry wall_geometry\nmaterial none\nposition 0 0 " <<
			(slabLength + 0.1 * scale) << "\n}\n";
		if( positionalLight ) {
			ss << "omni_light\n{\nname sampling_pivot\npower 1\ncolor 1 1 1\nposition "
				<< 0.75 * scale << " 0 " << 0.5 * slabLength << "\n}\n";
		}
		return ss.str();
	}

	struct Fixture
	{
		IJobPriv* job;
		IRayCaster* caster;
		PathTracingIntegrator* integrator;
		std::filesystem::path scenePath;
		Scalar slabLength;

		Fixture() : job( nullptr ), caster( nullptr ), integrator( nullptr ), slabLength( 0 ) {}

		~Fixture()
		{
			safe_release( integrator );
			safe_release( caster );
			safe_release( job );
			if( !scenePath.empty() ) std::filesystem::remove( scenePath );
		}

		bool Initialize(
			const char* tag,
			const Scalar sceneUnitMeters,
			const Scalar length,
			const bool positionalLight = false,
			const Scalar sootAlbedoHot = 0.0 )
		{
			slabLength = length;
			char filename[176];
			std::snprintf( filename, sizeof(filename),
				"rise_pathtracing_thermal_%s_%d.RISEscene", tag, static_cast<int>( ::getpid() ) );
			scenePath = std::filesystem::temp_directory_path() / filename;
			{
				std::ofstream output( scenePath );
				if( !output.is_open() ) return false;
				output << SceneText(
					sceneUnitMeters, length, positionalLight, sootAlbedoHot );
			}

			if( !RISE_CreateJobPriv( &job ) || !job ||
				!job->LoadAsciiSceneViaCst( scenePath.string().c_str() ) ) return false;
			IShader* shader = job->GetShaders()->GetItem( "global" );
			if( !shader || !RISE_API_CreateRayCaster(
				&caster, false, 0, *shader, true ) || !caster ) return false;
			caster->AttachScene( job->GetScene() );

			StabilityConfig stability;
			stability.maxVolumeBounce = 0;
			integrator = new PathTracingIntegrator( ManifoldSolverConfig(), stability );
			return true;
		}

		const IMedium* Medium() const
		{
			return job ? job->GetScene()->GetGlobalMedium() : nullptr;
		}

		Scalar MeanRayCasterNM( const unsigned int seed, const Scalar nm ) const
		{
			RandomNumberGenerator rng( seed );
			RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
			const RasterizerState rast = { 0, 0 };
			const Ray ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );
			IRayCaster::RAY_STATE state;
			state.volumeBounces = 64;
			Scalar sum = 0;
			for( unsigned int i = 0; i < kSamples; ++i ) {
				Scalar value = 0;
				caster->CastRayNM( rc, rast, ray, value, state, nm, nullptr, nullptr );
				sum += value;
			}
			return sum / Scalar( kSamples );
		}

		Scalar MeanPathTracingNM( const unsigned int seed, const Scalar nm ) const
		{
			RandomNumberGenerator rng( seed );
			RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
			IndependentSampler sampler( rng );
			const RasterizerState rast = { 0, 0 };
			const Ray ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );
			Scalar sum = 0;
			for( unsigned int i = 0; i < kSamples; ++i ) {
				sum += integrator->IntegrateRayNM( rc, rast, ray, nm,
					*job->GetScene(), *caster, sampler, nullptr, nullptr );
			}
			return sum / Scalar( kSamples );
		}

		void MeanPathTracingHWSS(
			const unsigned int seed,
			const SampledWavelengths& wavelengths,
			Scalar means[SampledWavelengths::N] ) const
		{
			RandomNumberGenerator rng( seed );
			RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
			IndependentSampler sampler( rng );
			const RasterizerState rast = { 0, 0 };
			const Ray ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );
			for( unsigned int w = 0; w < SampledWavelengths::N; ++w ) means[w] = 0;
			for( unsigned int i = 0; i < kSamples; ++i ) {
				SampledWavelengths swl = wavelengths;
				Scalar values[SampledWavelengths::N];
				integrator->IntegrateRayHWSS( rc, rast, ray, swl,
					*job->GetScene(), *caster, sampler, nullptr, values, nullptr );
				for( unsigned int w = 0; w < SampledWavelengths::N; ++w ) means[w] += values[w];
			}
			for( unsigned int w = 0; w < SampledWavelengths::N; ++w ) means[w] /= Scalar( kSamples );
		}
	};

	Scalar ExpectedSlab( const Fixture& fixture, const Scalar nm )
	{
		const IMedium* medium = fixture.Medium();
		const MediumCoefficientsNM coeff = medium->GetCoefficientsNM(
			Point3( 0, 0, 0.5 * fixture.slabLength ), nm );
		const Scalar tau = coeff.sigma_t * fixture.slabLength;
		return PlanckSpectralRadianceNM( nm, kTemperatureK ) * ( -std::expm1( -tau ) );
	}

	class SpatialAdditiveMedium :
		public virtual IMedium,
		public virtual Reference
	{
	public:
		MediumCoefficients GetCoefficients( const Point3& pt ) const override
		{
			MediumCoefficients c;
			c.sigma_t = RISEPel( 0, 0, 0 );
			c.sigma_s = RISEPel( 0, 0, 0 );
			c.emission = RISEPel( SourceAt( pt ), SourceAt( pt ), SourceAt( pt ) );
			return c;
		}
		MediumCoefficientsNM GetCoefficientsNM(
			const Point3& pt, const Scalar ) const override
		{
			MediumCoefficientsNM c;
			c.sigma_t = 0;
			c.sigma_s = 0;
			c.emission = SourceAt( pt );
			return c;
		}
		const IPhaseFunction* GetPhaseFunction() const override { return nullptr; }
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
		bool IsHomogeneous() const override { return false; }
		bool GetBoundingBox( Point3& bbMin, Point3& bbMax ) const override
		{
			bbMin = Point3( -0.5, -0.5, 0 );
			bbMax = Point3( 0.5, 0.5, 1 );
			return true;
		}

	protected:
		~SpatialAdditiveMedium() override = default;

	private:
		static Scalar SourceAt( const Point3& pt )
		{
			return pt.z >= 0.5 && pt.z <= 1.0 ? 2.0 : 0.0;
		}
	};

	class ForwardPhase :
		public virtual IPhaseFunction,
		public virtual Reference
	{
	public:
		Scalar Evaluate( const Vector3&, const Vector3& ) const override { return 1.0; }
		Vector3 Sample( const Vector3& wi, ISampler& ) const override { return wi; }
		Scalar Pdf( const Vector3&, const Vector3& ) const override { return 1.0; }
	protected:
		~ForwardPhase() override = default;
	};

	class TwoEventFireMedium :
		public virtual IMedium,
		public virtual Reference
	{
	public:
		explicit TwoEventFireMedium( IPhaseFunction& phase ) : pPhase( &phase )
		{
			pPhase->addref();
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
			c.sigma_t = 1.0;
			c.sigma_s = 1.0;
			c.emission = 0.0;
			return c;
		}
		const IPhaseFunction* GetPhaseFunction() const override { return pPhase; }
		Scalar SampleDistance(
			const Ray&, const Scalar maxDist, ISampler&, bool& scattered ) const override
		{
			scattered = false;
			return maxDist;
		}
		Scalar SampleDistanceNM(
			const Ray& ray, const Scalar maxDist, const Scalar,
			ISampler&, bool& scattered ) const override
		{
			if( ray.origin.z < 0.1 ) {
				scattered = true;
				return 0.25;
			}
			if( ray.origin.z < 0.5 ) {
				scattered = true;
				return 0.5;
			}
			scattered = false;
			return maxDist;
		}
		RISEPel EvalTransmittance( const Ray&, const Scalar dist ) const override
		{
			const Scalar tr = std::exp( -dist );
			return RISEPel( tr, tr, tr );
		}
		Scalar EvalTransmittanceNM(
			const Ray&, const Scalar dist, const Scalar ) const override
		{
			return std::exp( -dist );
		}
		bool IsHomogeneous() const override { return false; }
		bool GetBoundingBox( Point3& bbMin, Point3& bbMax ) const override
		{
			bbMin = Point3( -1, -1, 0 );
			bbMax = Point3( 1, 1, 1 );
			return true;
		}
		bool IsFireMedium() const override { return true; }
		Scalar GetThermalEmissionNM( const Point3& pt, const Scalar ) const override
		{
			return pt.z > 0.5 ? 1.0 : 0.0;
		}

	protected:
		~TwoEventFireMedium() override { safe_release( pPhase ); }

	private:
		IPhaseFunction* pPhase;
	};

	class DelayedDensityAccessor :
		public virtual IVolumeAccessor,
		public virtual Reference
	{
	public:
		bool tracking;
		mutable unsigned int zeroDensityQueries;

		DelayedDensityAccessor() : tracking( false ), zeroDensityQueries( 0 ) {}

		Scalar GetValue( Scalar, Scalar, Scalar z ) const override
		{
			if( tracking && z < 0.6 ) {
				++zeroDensityQueries;
				return 0.0;
			}
			return 1.0;
		}
		Scalar GetValue( int, int, int ) const override { return 1.0; }
		void BindVolume( const IVolume* ) override {}

	protected:
		~DelayedDensityAccessor() override = default;
	};

	class DelayedThermalHeterogeneousMedium : public HeterogeneousMedium
	{
	public:
		DelayedThermalHeterogeneousMedium(
			const IPhaseFunction& phase,
			IVolumeAccessor& accessor
			) : HeterogeneousMedium(
				RISEPel( 1000, 1000, 1000 ), RISEPel( 0, 0, 0 ),
				phase, accessor, 2, 2, 2,
				Point3( -1, -1, 0 ), Point3( 1, 1, 2 ) )
		{}

		bool IsFireMedium() const override { return true; }
		Scalar GetThermalEmissionNM( const Point3& pt, const Scalar nm ) const override
		{
			return 3.0 * HeterogeneousMedium::GetCoefficientsNM( pt, nm ).sigma_t;
		}

	protected:
		~DelayedThermalHeterogeneousMedium() override = default;
	};

	class LogTailFireMedium :
		public virtual IMedium,
		public virtual Reference
	{
	public:
		mutable Scalar lastLogEventDistance;
		LogTailFireMedium() : lastLogEventDistance( -1.0 ) {}
		static Scalar EventDistance() { return 1.0e31; }
		static Scalar SigmaT() { return 99.0 / EventDistance(); }

		MediumCoefficients GetCoefficients( const Point3& ) const override
		{
			MediumCoefficients c;
			c.sigma_t = RISEPel( SigmaT(), SigmaT(), SigmaT() );
			c.sigma_s = RISEPel( 0, 0, 0 );
			c.emission = RISEPel( 0, 0, 0 );
			return c;
		}
		MediumCoefficientsNM GetCoefficientsNM( const Point3&, const Scalar ) const override
		{
			MediumCoefficientsNM c;
			c.sigma_t = SigmaT();
			c.sigma_s = 0.0;
			c.emission = 0.0;
			return c;
		}
		const IPhaseFunction* GetPhaseFunction() const override { return nullptr; }
		Scalar SampleDistance(
			const Ray&, const Scalar maxDist, ISampler&, bool& scattered ) const override
		{
			scattered = false;
			return maxDist;
		}
		Scalar SampleDistanceNM(
			const Ray&, const Scalar, const Scalar,
			ISampler&, bool& scattered ) const override
		{
			scattered = true;
			return EventDistance();
		}
		RISEPel EvalTransmittance( const Ray&, const Scalar dist ) const override
		{
			const Scalar tr = std::exp( -SigmaT() * dist );
			return RISEPel( tr, tr, tr );
		}
		Scalar EvalTransmittanceNM(
			const Ray&, const Scalar dist, const Scalar ) const override
		{
			return std::exp( -SigmaT() * dist );
		}
		bool IsHomogeneous() const override { return false; }
		bool GetBoundingBox( Point3& bbMin, Point3& bbMax ) const override
		{
			bbMin = Point3( -1, -1, 0 );
			bbMax = Point3( 1, 1, 1.1 * EventDistance() );
			return true;
		}
		bool IsFireMedium() const override { return true; }
		Scalar GetThermalEmissionNM( const Point3&, const Scalar ) const override
		{
			return SigmaT();
		}
		Scalar EvalDistancePdfNM(
			const Ray&, const Scalar, const bool,
			const Scalar, const Scalar ) const override
		{
			// Deliberately model the historical linear-domain floor.  Correct
			// PT consumes the separate log-density contract below, so this stale
			// compatibility value cannot suppress the measurable tail.
			return 1.0e-30;
		}
		Scalar EvalLogDistancePdfNM(
			const Ray&, const Scalar t, const bool scattered,
			const Scalar maxDist, const Scalar ) const override
		{
			if( scattered ) lastLogEventDistance = t;
			return scattered ? std::log( SigmaT() ) - SigmaT() * t :
				-SigmaT() * maxDist;
		}
	protected:
		~LogTailFireMedium() override = default;
	};

	void TestNMAbsoluteParityAndSceneUnits()
	{
		std::cout << "TestNMAbsoluteParityAndSceneUnits" << std::endl;
		Fixture metres;
		Fixture centimetres;
		Check( metres.Initialize( "metres", 1.0, 1.0 ), "metre PT fire slab initializes" );
		Check( centimetres.Initialize( "centimetres", 0.01, 100.0 ),
			"centimetre PT fire slab initializes" );
		if( !metres.integrator || !centimetres.integrator ) return;

		const Scalar wavelengths[] = { 500.0, 700.0 };
		for( const Scalar nm : wavelengths ) {
			const Scalar expected = ExpectedSlab( metres, nm );
			const Scalar rayCaster = metres.MeanRayCasterNM( 0x51ab00u + static_cast<unsigned int>( nm ), nm );
			const Scalar pathTracing = metres.MeanPathTracingNM( 0x51ab00u + static_cast<unsigned int>( nm ), nm );
			const Scalar pathTracingCM = centimetres.MeanPathTracingNM(
				0x51ab00u + static_cast<unsigned int>( nm ), nm );
			Check( NearRelative( pathTracing, expected, 0.012 ),
				"PT NM pure-absorber slab matches absolute spectral target at maxVolumeBounce=0" );
			Check( NearRelative( pathTracing, rayCaster, 0.012 ),
				"PT NM and RayCaster entry routes produce the same spectral radiance" );
			Check( NearRelative( pathTracingCM, pathTracing, 0.012 ),
				"PT NM fire radiance is invariant between metre and centimetre scenes" );
		}
	}

	void TestHWSSRequestedUsesPerWavelengthFallback()
	{
		std::cout << "TestHWSSRequestedUsesPerWavelengthFallback" << std::endl;
		Fixture fixture;
		Check( fixture.Initialize( "hwss", 1.0, 1.0 ), "HWSS-requested PT fire slab initializes" );
		if( !fixture.integrator ) return;

		const SampledWavelengths wavelengths = SampledWavelengths::SampleEquidistant(
			0.30, 380.0, 780.0 );
		Scalar means[SampledWavelengths::N];
		fixture.MeanPathTracingHWSS( 0x48575353u, wavelengths, means );
		for( unsigned int w = 0; w < SampledWavelengths::N; ++w ) {
			const Scalar expected = ExpectedSlab( fixture, wavelengths.lambda[w] );
			Check( NearRelative( means[w], expected, 0.016 ),
				"HWSS-requested fire path matches its per-wavelength NM slab target" );
		}
	}

	void TestEquiangularMixtureUsesItsActualDistanceDensity()
	{
		std::cout << "TestEquiangularMixtureUsesItsActualDistanceDensity" << std::endl;
		Fixture fixture;
		Check( fixture.Initialize( "equiangular", 1.0, 1.0, true ),
			"positional-light PT fire slab initializes" );
		if( !fixture.integrator ) return;
		const Scalar nm = 500.0;
		const Scalar expected = ExpectedSlab( fixture, nm );
		const Scalar measured = fixture.MeanPathTracingNM( 0xe9a117u, nm );
		Check( NearRelative( measured, expected, 0.018 ),
			"PT thermal score uses the active DT/equiangular mixture density" );
	}

	void TestPrimaryScatteringEventHonorsVolumeCapAfterEmission()
	{
		std::cout << "TestPrimaryScatteringEventHonorsVolumeCapAfterEmission" << std::endl;
		Fixture fixture;
		const Scalar albedo = 0.5;
		Check( fixture.Initialize( "scattering_cap", 1.0, 1.0, false, albedo ),
			"scattering fire slab initializes at maxVolumeBounce=0" );
		if( !fixture.integrator || !fixture.Medium() ) return;

		const Scalar nm = 500.0;
		const MediumCoefficientsNM coeff = fixture.Medium()->GetCoefficientsNM(
			Point3( 0, 0, 0.5 ), nm );
		const Scalar sigmaA = coeff.sigma_t - coeff.sigma_s;
		const Scalar expected = ( sigmaA / coeff.sigma_t ) *
			PlanckSpectralRadianceNM( nm, kTemperatureK ) *
			( -std::expm1( -coeff.sigma_t * fixture.slabLength ) );
		const Scalar measured = fixture.MeanPathTracingNM( 0x5ca77e2u, nm );
		Check( NearRelative( measured, expected, 0.012 ),
			"primary scattering collision emits, then maxVolumeBounce=0 suppresses continuation" );
	}

	void TestDirectPelEntryRejectsFire()
	{
		std::cout << "TestDirectPelEntryRejectsFire" << std::endl;
		Fixture fixture;
		Check( fixture.Initialize( "pel_reject", 1.0, 1.0 ),
			"direct Pel rejection fixture initializes" );
		if( !fixture.integrator || !fixture.caster || !fixture.job ) return;

		RandomNumberGenerator rng( 0x9e1f17eu );
		RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
		IndependentSampler sampler( rng );
		const RasterizerState rast = { 0, 0 };
		const RISEPel value = fixture.integrator->IntegrateRay(
			rc, rast, Ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) ),
			*fixture.job->GetScene(), *fixture.caster, sampler, nullptr, nullptr );
		Check( value.r == 0.0 && value.g == 0.0 && value.b == 0.0,
			"pure PathTracingPel entry rejects fire until Phase-A step 7" );
	}

	void TestBoundedObjectFireRejectsShaderDispatchPel()
	{
		std::cout << "TestBoundedObjectFireRejectsShaderDispatchPel" << std::endl;
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() /
			( "rise_pt_bounded_pel_reject_" +
				std::to_string( static_cast<int>( ::getpid() ) ) + ".RISEscene" );
		{
			std::ofstream output( scenePath );
			output <<
				"RISE ASCII SCENE 7\n"
				"pathtracing_shaderop\n{\nname pathtracer\n}\n"
				"standard_shader\n{\nname global\nshaderop pathtracer\n}\n"
				"standard_shader\n{\nname emission_only\nshaderop DefaultEmission\n}\n"
				"uniformcolor_painter\n{\nname emitter_color\ncolor 1 1 1\n}\n"
				"lambertian_luminaire_material\n{\nname emitter\nexitance emitter_color\n"
				"scale 10\nmaterial none\n}\n"
				"scalar_painter\n{\nname carbon\nvalue 0.2\n}\n"
				"scalar_painter\n{\nname temperature\nvalue 1800\n}\n"
				"multichannel_heterogeneous_medium\n{\nname fire\n"
				"channel_carbon painter carbon\nchannel_temperature painter temperature\n"
				"bake_resolution 4 4 4\nbbox_min -1 -1 0\nbbox_max 1 1 4\n"
				"soot_em 0.26\nsoot_density 1800\nsoot_albedo_hot 0.1\nsoot_g_hot 0.5\n"
				"smoke_km_carbon 8.7\nsmoke_n_carbon 1.2\n"
				"smoke_albedo_carbon 0.6\nsmoke_g_carbon 0.6\n}\n"
				"box_geometry\n{\nname container_geometry\nwidth 2\nheight 2\ndepth 2\n}\n"
				"standard_object\n{\nname container\ngeometry container_geometry\n"
				"material emitter\ninterior_medium fire\nposition 0 0 2\n}\n";
		}

		IJobPriv* job = nullptr;
		IRayCaster* caster = nullptr;
		IRayCaster* emissionCaster = nullptr;
		PathTracingIntegrator* integrator = nullptr;
		Check( RISE_CreateJobPriv( &job ) && job &&
			job->LoadAsciiSceneViaCst( scenePath.string().c_str() ),
			"bounded-object Pel rejection fixture initializes" );
		if( job ) {
			IShader* shader = job->GetShaders()->GetItem( "global" );
			IShader* emissionShader = job->GetShaders()->GetItem( "emission_only" );
			Check( shader && emissionShader && RISE_API_CreateRayCaster(
				&caster, false, 10, *shader, true ) && caster,
				"bounded-object PT Pel rejection caster initializes" );
			Check( emissionShader && RISE_API_CreateRayCaster(
				&emissionCaster, false, 10, *emissionShader, true ) && emissionCaster,
				"bounded-object non-PT emission caster initializes" );
			if( caster ) caster->AttachScene( job->GetScene() );
			if( emissionCaster ) emissionCaster->AttachScene( job->GetScene() );
			integrator = new PathTracingIntegrator(
				ManifoldSolverConfig(), StabilityConfig() );
		}

		if( integrator && caster && emissionCaster && job ) {
			RandomNumberGenerator rng( 0xb0a0dedu );
			RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
			IndependentSampler sampler( rng );
			const RasterizerState rast = { 0, 0 };
			const Ray ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );
			RayIntersection firstHit( ray, rast );
			job->GetScene()->GetObjects()->IntersectRay(
				firstHit, true, true, false );
			Check( firstHit.geometric.bHit,
				"camera outside the bounded fire reaches its container boundary" );
			IORStack iorStack( 1.0 );
			const RISEPel direct = integrator->IntegrateFromHit(
				rc, rast, firstHit, *job->GetScene(), *caster, sampler,
				nullptr, 0, iorStack, 0.0, RISEPel( 0.0 ), true, 1.0,
				IRayCaster::RAY_STATE::eRayView,
				0, 0, 0, 0, 0, 0.0, false, false, nullptr );
			Check( direct.r == 0.0 && direct.g == 0.0 && direct.b == 0.0,
				"public IntegrateFromHit rejects scene-wide bounded fire" );

			IRayCaster::RAY_STATE state;
			RISEPel dispatched( 1.0 );
			const bool hit = emissionCaster->CastRay(
				rc, rast, ray, dispatched, state, nullptr, nullptr );
			Check( !hit && dispatched.r == 0.0 && dispatched.g == 0.0 && dispatched.b == 0.0,
				"RayCaster rejects bounded fire before a non-PT emission shader" );

			// Matched no-fire control: clear only the interior binding.  The
			// same visible emitter must then be nonzero through both entry routes,
			// proving the black/false results above came from rejection rather
			// than an ordinarily black boundary material.
			SceneEditor editor( *job->GetScene() );
			editor.SetJob( job );
			SceneEdit clear;
			clear.op = SceneEdit::SetObjectInteriorMedium;
			clear.objectName = "container";
			clear.propertyValue = "none";
			Check( editor.Apply( clear ),
				"bounded-fire control clears only the interior medium" );
			caster->AttachScene( job->GetScene() );
			emissionCaster->AttachScene( job->GetScene() );
			const LightSampler* lights = caster->GetLightSampler();
			Check( lights && !lights->SceneHasFireMedia(),
				"bounded-fire control contains no active fire medium" );

			RayIntersection controlHit( ray, rast );
			job->GetScene()->GetObjects()->IntersectRay(
				controlHit, true, true, false );
			const RISEPel directControl = integrator->IntegrateFromHit(
				rc, rast, controlHit, *job->GetScene(), *caster, sampler,
				nullptr, 0, iorStack, 0.0, RISEPel( 0.0 ), true, 1.0,
				IRayCaster::RAY_STATE::eRayView,
				0, 0, 0, 0, 0, 0.0, false, false, nullptr );
			Check( ColorMath::MaxValue( directControl ) > 1.0,
				"no-fire IntegrateFromHit control sees the emissive boundary" );
			RISEPel dispatchedControl( 0.0 );
			const bool controlHitResult = emissionCaster->CastRay(
				rc, rast, ray, dispatchedControl, state, nullptr, nullptr );
			if( !controlHitResult || ColorMath::MaxValue( dispatchedControl ) <= 1.0 ) {
				std::cout << "  shader-dispatch control hit=" << controlHitResult <<
					" value=(" << dispatchedControl.r << "," << dispatchedControl.g <<
					"," << dispatchedControl.b << ")" << std::endl;
			}
			Check( controlHitResult && ColorMath::MaxValue( dispatchedControl ) > 1.0,
				"no-fire non-PT RayCaster control sees the emissive boundary" );
		}

		safe_release( integrator );
		safe_release( emissionCaster );
		safe_release( caster );
		safe_release( job );
		std::filesystem::remove( scenePath );
	}

	void TestPostScatterSegmentRunsMediumTransport()
	{
		std::cout << "TestPostScatterSegmentRunsMediumTransport" << std::endl;
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() /
			( "rise_pt_post_scatter_" + std::to_string( static_cast<int>( ::getpid() ) ) + ".RISEscene" );
		{
			std::ofstream output( scenePath );
			output << "RISE ASCII SCENE 7\nstandard_shader\n{\nname global\n}\n";
		}

		IJobPriv* job = nullptr;
		IRayCaster* caster = nullptr;
		PathTracingIntegrator* integrator = nullptr;
		ForwardPhase* phase = new ForwardPhase();
		TwoEventFireMedium* medium = new TwoEventFireMedium( *phase );
		Check( RISE_CreateJobPriv( &job ) && job &&
			job->LoadAsciiSceneViaCst( scenePath.string().c_str() ),
			"two-event continuation fixture initializes" );
		if( job ) {
			job->GetScene()->SetGlobalMedium( medium );
			IShader* shader = job->GetShaders()->GetItem( "global" );
			Check( shader && RISE_API_CreateRayCaster(
				&caster, false, 0, *shader, true ) && caster,
				"two-event continuation caster initializes" );
			if( caster ) caster->AttachScene( job->GetScene() );
			StabilityConfig stability;
			stability.maxVolumeBounce = 2;
			integrator = new PathTracingIntegrator( ManifoldSolverConfig(), stability );
			integrator->SetMaxPathDepth( 1 );
		}

		if( integrator && caster && job ) {
			RandomNumberGenerator rng( 0x2e7e17u );
			RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
			IndependentSampler sampler( rng );
			const RasterizerState rast = { 0, 0 };
			const Scalar value = integrator->IntegrateRayNM(
				rc, rast, Ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) ), 500.0,
				*job->GetScene(), *caster, sampler, nullptr, nullptr );
			Check( NearRelative( value, 1.0, 1e-13 ),
				"path-depth-capped post-scatter segment still scores its thermal event" );
		}

		safe_release( integrator );
		safe_release( caster );
		safe_release( job );
		safe_release( medium );
		safe_release( phase );
		std::filesystem::remove( scenePath );
	}

	void TestForcedNullProposalsThroughPathTracing()
	{
		std::cout << "TestForcedNullProposalsThroughPathTracing" << std::endl;
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() /
			( "rise_pt_forced_null_" + std::to_string( static_cast<int>( ::getpid() ) ) + ".RISEscene" );
		{
			std::ofstream output( scenePath );
			output << "RISE ASCII SCENE 7\nstandard_shader\n{\nname global\n}\n";
		}

		IJobPriv* job = nullptr;
		IRayCaster* caster = nullptr;
		PathTracingIntegrator* integrator = nullptr;
		DelayedDensityAccessor* accessor = new DelayedDensityAccessor();
		IPhaseFunction* phase = nullptr;
		Check( RISE_API_CreateIsotropicPhaseFunction( &phase ) && phase,
			"forced-null production phase initializes" );
		DelayedThermalHeterogeneousMedium* medium = phase ?
			new DelayedThermalHeterogeneousMedium( *phase, *accessor ) : nullptr;
		accessor->tracking = true;
		Check( RISE_CreateJobPriv( &job ) && job &&
			job->LoadAsciiSceneViaCst( scenePath.string().c_str() ),
			"forced-null PT fixture initializes" );
		if( job && medium ) {
			job->GetScene()->SetGlobalMedium( medium );
			IShader* shader = job->GetShaders()->GetItem( "global" );
			Check( shader && RISE_API_CreateRayCaster(
				&caster, false, 0, *shader, true ) && caster,
				"forced-null PT caster initializes" );
			if( caster ) caster->AttachScene( job->GetScene() );
			StabilityConfig stability;
			stability.maxVolumeBounce = 0;
			integrator = new PathTracingIntegrator( ManifoldSolverConfig(), stability );
		}

		if( integrator && caster && job ) {
			RandomNumberGenerator rng( 0x1024u );
			RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
			IndependentSampler sampler( rng );
			const RasterizerState rast = { 0, 0 };
			const Scalar value = integrator->IntegrateRayNM(
				rc, rast, Ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) ), 500.0,
				*job->GetScene(), *caster, sampler, nullptr, nullptr );
			Check( accessor->zeroDensityQueries > 1024,
				"PT runs the production heterogeneous tracker past 1024 null proposals" );
			Check( NearRelative( value, 3.0, 1e-13 ),
				"PT matches the uncapped thermal-event reference after the null tail" );
		}

		safe_release( integrator );
		safe_release( caster );
		safe_release( job );
		safe_release( medium );
		safe_release( phase );
		safe_release( accessor );
		std::filesystem::remove( scenePath );
	}

	void TestOpticallyThickMixedDensityUsesLogDomain()
	{
		std::cout << "TestOpticallyThickMixedDensityUsesLogDomain" << std::endl;
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() /
			( "rise_pt_log_tail_" + std::to_string( static_cast<int>( ::getpid() ) ) + ".RISEscene" );
		{
			std::ofstream output( scenePath );
			output <<
				"RISE ASCII SCENE 7\nstandard_shader\n{\nname global\n}\n"
				"omni_light\n{\nname pivot\npower 1\ncolor 1 1 1\nposition 1e-10 0 0\n}\n";
		}

		IJobPriv* job = nullptr;
		IRayCaster* caster = nullptr;
		PathTracingIntegrator* integrator = nullptr;
		LogTailFireMedium* medium = new LogTailFireMedium();
		Check( RISE_CreateJobPriv( &job ) && job &&
			job->LoadAsciiSceneViaCst( scenePath.string().c_str() ),
			"tau=99 PT log-tail fixture initializes" );
		if( job ) {
			job->GetScene()->SetGlobalMedium( medium );
			IShader* shader = job->GetShaders()->GetItem( "global" );
			Check( shader && RISE_API_CreateRayCaster(
				&caster, false, 0, *shader, true ) && caster,
				"tau=99 PT log-tail caster initializes" );
			if( caster ) caster->AttachScene( job->GetScene() );
			StabilityConfig stability;
			stability.maxVolumeBounce = 0;
			integrator = new PathTracingIntegrator( ManifoldSolverConfig(), stability );
		}

		Scalar measured = 0.0;
		bool sampledTail = false;
		if( integrator && caster && job ) {
			const RasterizerState rast = { 0, 0 };
			for( unsigned int seed = 1; seed <= 1000 && !sampledTail; ++seed ) {
				medium->lastLogEventDistance = -1.0;
				RandomNumberGenerator rng( seed );
				RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
				IndependentSampler sampler( rng );
				measured = integrator->IntegrateRayNM(
					rc, rast, Ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) ), 500.0,
					*job->GetScene(), *caster, sampler, nullptr, nullptr );
				sampledTail = medium->lastLogEventDistance == LogTailFireMedium::EventDistance();
			}
		}
		Check( sampledTail, "PT mixed distance proposal exercises a sub-1e-30 tau=99 event" );
		if( sampledTail ) {
			const Ray ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );
			const Scalar eventDistance = LogTailFireMedium::EventDistance();
			const Scalar pdfDt = LogTailFireMedium::SigmaT() * std::exp( -99.0 );
			const Scalar pdfEq = EquiangularSampling::Pdf(
				ray, Point3( 1e-10, 0, 0 ), 0.0,
				1.1 * eventDistance, eventDistance );
			const Scalar expected = pdfDt / ( 0.5 * pdfDt + 0.5 * pdfEq );
			Check( pdfDt < 1e-30 && pdfEq < 1e-30,
				"both tau-tail proposal densities are materially below the old floor" );
			Check( RISE::IsFiniteDouble( measured ) &&
				NearRelative( measured, expected, 1e-12 ),
				"tau=99 PT tail matches the unfloored log-sum-exp reference" );
		}

		safe_release( integrator );
		safe_release( caster );
		safe_release( job );
		safe_release( medium );
		std::filesystem::remove( scenePath );
	}

	void TestObjectFireRoutingCacheRefreshesAfterBinding()
	{
		std::cout << "TestObjectFireRoutingCacheRefreshesAfterBinding" << std::endl;
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() /
			( "rise_pt_object_fire_cache_" + std::to_string( static_cast<int>( ::getpid() ) ) + ".RISEscene" );
		{
			std::ofstream output( scenePath );
			output <<
				"RISE ASCII SCENE 7\nstandard_shader\n{\nname global\n}\n"
				"scalar_painter\n{\nname carbon\nvalue 0.2\n}\n"
				"scalar_painter\n{\nname temperature\nvalue 1800\n}\n"
				"multichannel_heterogeneous_medium\n{\nname fire\n"
				"channel_carbon painter carbon\nchannel_temperature painter temperature\n"
				"bake_resolution 4 4 4\nbbox_min -1 -1 -1\nbbox_max 1 1 1\n"
				"soot_em 0.26\nsoot_density 1800\nsoot_albedo_hot 0.1\nsoot_g_hot 0.5\n"
				"smoke_km_carbon 8.7\nsmoke_n_carbon 1.2\n"
				"smoke_albedo_carbon 0.6\nsmoke_g_carbon 0.6\n}\n"
				"box_geometry\n{\nname container_geometry\nwidth 2\nheight 2\ndepth 2\n}\n"
				"standard_object\n{\nname container\ngeometry container_geometry\nmaterial none\n}\n";
		}

		IJobPriv* job = nullptr;
		IRayCaster* caster = nullptr;
		Check( RISE_CreateJobPriv( &job ) && job &&
			job->LoadAsciiSceneViaCst( scenePath.string().c_str() ),
			"object-fire cache fixture initializes" );
		if( job ) {
			IShader* shader = job->GetShaders()->GetItem( "global" );
			Check( shader && RISE_API_CreateRayCaster(
				&caster, false, 0, *shader, true ) && caster,
				"object-fire cache caster initializes" );
			if( caster ) {
				caster->AttachScene( job->GetScene() );
				const LightSampler* lights = caster->GetLightSampler();
				Check( lights && !lights->SceneHasFireMedia(),
					"prepared sampler starts without an object fire medium" );
				Check( job->SetObjectInteriorMedium( "container", "fire" ),
					"runtime object fire binding succeeds" );
				caster->AttachScene( job->GetScene() );
				lights = caster->GetLightSampler();
				Check( lights && lights->SceneHasFireMedia(),
					"same-scene caster refreshes fire routing after interior-medium binding" );

				SceneEditor editor( *job->GetScene() );
				editor.SetJob( job );
				SceneEdit clear;
				clear.op = SceneEdit::SetObjectInteriorMedium;
				clear.objectName = "container";
				clear.propertyValue = "none";
				Check( editor.Apply( clear ),
					"CST object interior-medium clear succeeds" );
				caster->AttachScene( job->GetScene() );
				lights = caster->GetLightSampler();
				Check( lights && !lights->SceneHasFireMedia(),
					"CST optional-slot clear invalidates the object-fire cache" );

				Check( editor.Undo(), "CST object interior-medium clear undo succeeds" );
				caster->AttachScene( job->GetScene() );
				lights = caster->GetLightSampler();
				Check( lights && lights->SceneHasFireMedia(),
					"CST undo refreshes the object-fire cache" );
				Check( editor.Redo(), "CST object interior-medium clear redo succeeds" );
				caster->AttachScene( job->GetScene() );
				lights = caster->GetLightSampler();
				Check( lights && !lights->SceneHasFireMedia(),
					"CST redo refreshes the cleared object-fire cache" );

				Check( editor.Undo(), "CST fire binding restored before object removal" );
				caster->AttachScene( job->GetScene() );
				Check( job->RemoveObject( "container" ),
					"last bounded fire object removal succeeds" );
				caster->AttachScene( job->GetScene() );
				lights = caster->GetLightSampler();
				Check( lights && !lights->SceneHasFireMedia(),
					"removing the last bounded fire object invalidates the cache" );
			}
		}

		safe_release( caster );
		safe_release( job );
		std::filesystem::remove( scenePath );
	}

	void TestLegacySceneEditorMediumUndoRefreshesCache()
	{
		std::cout << "TestLegacySceneEditorMediumUndoRefreshesCache" << std::endl;
		Job* job = new Job();
		const char* shaderOps[] = { "DefaultDirectLighting" };
		const double sigmaA[3] = { 0.1, 0.1, 0.1 };
		const double sigmaS[3] = { 0.0, 0.0, 0.0 };
		const double zero[3] = { 0.0, 0.0, 0.0 };
		const double one[3] = { 1.0, 1.0, 1.0 };
		RadianceMapConfig noRadianceMap;
		Check( job->AddStandardShader( "global", 1, shaderOps ) &&
			job->AddBoxGeometry( "container_geometry", 2.0, 2.0, 2.0 ) &&
			job->AddObject(
				"container", "container_geometry", "none", nullptr, nullptr,
				noRadianceMap, zero, zero, one, true, true ) &&
			job->AddHomogeneousMedium(
				"fog", sigmaA, sigmaS, "isotropic", 0.0 ),
			"programmatic legacy medium-edit fixture initializes" );

		IRayCaster* caster = nullptr;
		IShader* shader = job->GetShaders()->GetItem( "global" );
		Check( shader && RISE_API_CreateRayCaster(
			&caster, false, 0, *shader, true ) && caster,
			"programmatic legacy medium-edit caster initializes" );
		if( caster ) caster->AttachScene( job->GetScene() );

		if( caster ) {
			const LightSampler* lights = caster->GetLightSampler();
			Check( lights && !lights->SceneHasMedia(),
				"legacy fixture starts without object media" );
			SceneEditor editor( *job->GetScene() );
			editor.SetJob( job );
			SceneEdit bind;
			bind.op = SceneEdit::SetObjectInteriorMedium;
			bind.objectName = "container";
			bind.propertyValue = "fog";
			Check( editor.Apply( bind ), "legacy SceneEditor medium bind succeeds" );
			caster->AttachScene( job->GetScene() );
			lights = caster->GetLightSampler();
			Check( lights && lights->SceneHasMedia(),
				"legacy forward bind refreshes the object-medium cache" );

			Check( editor.Undo(), "legacy SceneEditor medium-bind undo succeeds" );
			caster->AttachScene( job->GetScene() );
			lights = caster->GetLightSampler();
			Check( lights && !lights->SceneHasMedia(),
				"legacy undo clears and refreshes the object-medium cache" );
			Check( editor.Redo(), "legacy SceneEditor medium-bind redo succeeds" );
			caster->AttachScene( job->GetScene() );
			lights = caster->GetLightSampler();
			Check( lights && lights->SceneHasMedia(),
				"legacy redo restores and refreshes the object-medium cache" );
		}

		safe_release( caster );
		safe_release( job );
	}

	void TestSpatialAdditiveSourceIsAnIndependentFullSegmentEstimator()
	{
		std::cout << "TestSpatialAdditiveSourceIsAnIndependentFullSegmentEstimator" << std::endl;
		char filename[176];
		std::snprintf( filename, sizeof(filename),
			"rise_pathtracing_spatial_additive_%d.RISEscene", static_cast<int>( ::getpid() ) );
		const std::filesystem::path scenePath =
			std::filesystem::temp_directory_path() / filename;
		{
			std::ofstream output( scenePath );
			output << "RISE ASCII SCENE 7\nstandard_shader\n{\nname global\n}\n";
		}

		IJobPriv* job = nullptr;
		IRayCaster* caster = nullptr;
		PathTracingIntegrator* integrator = nullptr;
		Check( RISE_CreateJobPriv( &job ) && job &&
			job->LoadAsciiSceneViaCst( scenePath.string().c_str() ),
			"spatial additive PT fixture initializes" );
		if( job ) {
			SpatialAdditiveMedium* medium = new SpatialAdditiveMedium();
			job->GetScene()->SetGlobalMedium( medium );
			safe_release( medium );
			IShader* shader = job->GetShaders()->GetItem( "global" );
			Check( shader && RISE_API_CreateRayCaster(
				&caster, false, 0, *shader, true ) && caster,
				"spatial additive PT caster initializes" );
			if( caster ) caster->AttachScene( job->GetScene() );
			StabilityConfig stability;
			stability.maxVolumeBounce = 0;
			integrator = new PathTracingIntegrator( ManifoldSolverConfig(), stability );
		}

		if( integrator && caster ) {
			RandomNumberGenerator rng( 0xadd1715u );
			RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
			IndependentSampler sampler( rng );
			const RasterizerState rast = { 0, 0 };
			const Ray ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );
			Scalar sum = 0;
			for( unsigned int i = 0; i < kSamples; ++i ) {
				sum += integrator->IntegrateRayNM( rc, rast, ray, 500.0,
					*job->GetScene(), *caster, sampler, nullptr, nullptr );
			}
			Check( NearRelative( sum / Scalar( kSamples ), 1.0, 0.012 ),
				"PT scores spatial additive emission on the full escaped segment" );

			RandomNumberGenerator hwssRng( 0xadd4855u );
			RuntimeContext hwssRc( hwssRng, RuntimeContext::PASS_NORMAL, false );
			IndependentSampler hwssSampler( hwssRng );
			const SampledWavelengths wavelengths = SampledWavelengths::SampleEquidistant(
				0.25, 380.0, 780.0 );
			Scalar hwssSums[SampledWavelengths::N] = { 0, 0, 0, 0 };
			for( unsigned int i = 0; i < kSamples; ++i ) {
				SampledWavelengths swl = wavelengths;
				Scalar values[SampledWavelengths::N];
				integrator->IntegrateRayHWSS( hwssRc, rast, ray, swl,
					*job->GetScene(), *caster, hwssSampler, nullptr, values, nullptr );
				for( unsigned int w = 0; w < SampledWavelengths::N; ++w ) {
					hwssSums[w] += values[w];
				}
			}
			for( unsigned int w = 0; w < SampledWavelengths::N; ++w ) {
				Check( NearRelative( hwssSums[w] / Scalar( kSamples ), 1.0, 0.012 ),
					"HWSS PT scores spatial additive emission on the full escaped segment" );
			}
		}

		safe_release( integrator );
		safe_release( caster );
		safe_release( job );
		std::filesystem::remove( scenePath );
	}
}

int main()
{
	TestNMAbsoluteParityAndSceneUnits();
	TestHWSSRequestedUsesPerWavelengthFallback();
	TestEquiangularMixtureUsesItsActualDistanceDensity();
	TestPrimaryScatteringEventHonorsVolumeCapAfterEmission();
	TestDirectPelEntryRejectsFire();
	TestBoundedObjectFireRejectsShaderDispatchPel();
	TestPostScatterSegmentRunsMediumTransport();
	TestForcedNullProposalsThroughPathTracing();
	TestOpticallyThickMixedDensityUsesLogDomain();
	TestObjectFireRoutingCacheRefreshesAfterBinding();
	TestLegacySceneEditorMediumUndoRefreshesCache();
	TestSpatialAdditiveSourceIsAnIndependentFullSegmentEstimator();
	std::cout << passed << " passed, " << failed << " failed" << std::endl;
	return failed == 0 ? 0 : 1;
}
