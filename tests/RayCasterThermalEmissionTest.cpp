//////////////////////////////////////////////////////////////////////
//
//  RayCasterThermalEmissionTest.cpp - Phase-A step-4 spectral gates.
//
//  Gates the collision estimator against the absolute isothermal-slab
//  solution, proves metre/centimetre invariance, keeps thermal scoring ahead
//  of continuation roulette and the volume-bounce cap, verifies optically
//  thick densities are not floored, and forces both distance sampling and
//  ratio tracking past more than 1024 null proposals.  Pel is deliberately
//  rejected until the ordered preview step.
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
#include "../src/Library/Materials/HeterogeneousMedium.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Utilities/ISampler.h"
#include "../src/Library/Utilities/IORStack.h"
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
	const Scalar kWavelengthNM = 500.0;
	const Scalar kRedWavelengthNM = 700.0;
	const Scalar kTemperatureK = 1800.0;
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

	unsigned int SeedForFirstRouletteDraw( const bool survives )
	{
		for( unsigned int seed = 1; seed < 100000; ++seed ) {
			RandomNumberGenerator probe( seed );
			if( ( probe.CanonicalRandom() < 0.5 ) == survives ) return seed;
		}
		return 0;
	}

	std::string SceneText(
		const Scalar sceneUnitMeters,
		const Scalar slabLength,
		const Scalar targetSigmaSI )
	{
		const Scalar scale = 1.0 / sceneUnitMeters;
		const Scalar carbon = targetSigmaSI / HotAbsorptionMass633();
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
			"soot_albedo_hot 0\n"
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
		return ss.str();
	}

	struct Fixture
	{
		IJobPriv* job;
		IRayCaster* caster;
		std::filesystem::path scenePath;
		Scalar slabLength;

		Fixture() : job( nullptr ), caster( nullptr ), slabLength( 0.0 ) {}

		~Fixture()
		{
			safe_release( caster );
			safe_release( job );
			if( !scenePath.empty() ) std::filesystem::remove( scenePath );
		}

		bool Initialize(
			const char* tag,
			const Scalar sceneUnitMeters,
			const Scalar length,
			const Scalar targetSigmaSI )
		{
			slabLength = length;
			char filename[160];
			std::snprintf( filename, sizeof(filename),
				"rise_raycaster_thermal_%s_%d.RISEscene", tag, static_cast<int>( ::getpid() ) );
			scenePath = std::filesystem::temp_directory_path() / filename;
			{
				std::ofstream output( scenePath );
				if( !output.is_open() ) return false;
				output << SceneText( sceneUnitMeters, length, targetSigmaSI );
			}

			if( !RISE_CreateJobPriv( &job ) || !job ||
				!job->LoadAsciiSceneViaCst( scenePath.string().c_str() ) ) return false;
			IShader* shader = job->GetShaders()->GetItem( "global" );
			if( !shader || !RISE_API_CreateRayCaster(
				&caster, false, 0, *shader, true ) || !caster ) return false;
			caster->AttachScene( job->GetScene() );
			return true;
		}

		const IMedium* Medium() const
		{
			return job ? job->GetScene()->GetGlobalMedium() : nullptr;
		}

		Scalar MeanNM( const unsigned int seed, const Scalar nm ) const
		{
			RandomNumberGenerator rng( seed );
			RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
			const RasterizerState rast = { 0, 0 };
			const Ray ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );
			IRayCaster::RAY_STATE state;
			state.volumeBounces = 64;
			Scalar sum = 0.0;
			for( unsigned int i = 0; i < kSamples; ++i ) {
				Scalar value = 0.0;
				Scalar distance = 0.0;
				caster->CastRayNM( rc, rast, ray, value, state,
					nm, &distance, nullptr );
				sum += value;
			}
			return sum / Scalar( kSamples );
		}
	};

	void TestAbsoluteSlabAndSceneUnits()
	{
		std::cout << "TestAbsoluteSlabAndSceneUnits" << std::endl;
		const Scalar targetSigmaSI = 0.8;
		Fixture metres;
		Fixture centimetres;
		Check( metres.Initialize( "metres", 1.0, 1.0, targetSigmaSI ),
			"metre fire slab initializes" );
		Check( centimetres.Initialize( "centimetres", 0.01, 100.0, targetSigmaSI ),
			"centimetre fire slab initializes" );
		if( !metres.caster || !centimetres.caster ) return;

		const IMedium* mediumM = metres.Medium();
		const IMedium* mediumCM = centimetres.Medium();
		Check( mediumM && mediumCM && mediumM->IsFireMedium() && mediumCM->IsFireMedium(),
			"fixtures install fire media" );
		if( !mediumM || !mediumCM ) return;

		const Point3 midpointM( 0, 0, 0.5 );
		const Point3 midpointCM( 0, 0, 50.0 );
		const MediumCoefficientsNM coeffM = mediumM->GetCoefficientsNM(
			midpointM, kWavelengthNM );
		const MediumCoefficientsNM coeffMRed = mediumM->GetCoefficientsNM(
			midpointM, kRedWavelengthNM );
		const MediumCoefficientsNM coeffCM = mediumCM->GetCoefficientsNM(
			midpointCM, kWavelengthNM );
		const MultichannelHeterogeneousMedium* concreteM =
			dynamic_cast<const MultichannelHeterogeneousMedium*>( mediumM );
		Check( concreteM && concreteM->TrackingMajorantAt( midpointM ) >= coeffM.sigma_t,
			"fire medium installs a valid delta-tracking majorant" );
		const Scalar planck = PlanckSpectralRadianceNM( kWavelengthNM, kTemperatureK );
		Check( NearRelative(
			mediumM->GetThermalEmissionNM( midpointM, kWavelengthNM ) / coeffM.sigma_t,
			planck, 1e-13 ), "thermal source is sigma_a times the pinned Planck kernel" );
		Check( NearRelative( coeffCM.sigma_t * centimetres.slabLength,
			coeffM.sigma_t * metres.slabLength, 1e-13 ),
			"metre and centimetre slabs have equal optical depth" );
		Check( NearRelative( coeffM.sigma_t / coeffMRed.sigma_t, 1.4, 1e-13 ),
			"hot-soot slab extinction has the required tau(500)/tau(700)=1.4" );

		const Scalar tau = coeffM.sigma_t * metres.slabLength;
		const Scalar expected = planck * ( -std::expm1( -tau ) );
		const Scalar tauRed = coeffMRed.sigma_t * metres.slabLength;
		const Scalar expectedRed = PlanckSpectralRadianceNM(
			kRedWavelengthNM, kTemperatureK ) * ( -std::expm1( -tauRed ) );
		const Scalar measuredM = metres.MeanNM( 0x5a174u, kWavelengthNM );
		const Scalar measuredCM = centimetres.MeanNM( 0x5a174u, kWavelengthNM );
		const Scalar measuredMRed = metres.MeanNM( 0x7ed174u, kRedWavelengthNM );
		const Scalar measuredCMRed = centimetres.MeanNM( 0x7ed174u, kRedWavelengthNM );
		std::cout << std::setprecision( 12 ) << "  expected=" << expected <<
			" metres=" << measuredM << " centimetres=" << measuredCM <<
			" red_expected=" << expectedRed << " red_metres=" << measuredMRed <<
			" red_centimetres=" << measuredCMRed << std::endl;
		Check( NearRelative( measuredM, expected, 0.012 ),
			"RayCaster NM matches the absolute isothermal-slab target" );
		Check( NearRelative( measuredCM, expected, 0.012 ),
			"centimetre RayCaster NM matches the same absolute target" );
		Check( NearRelative( measuredCM, measuredM, 1e-13 ),
			"RayCaster NM radiance is scene-unit invariant for identical random samples" );
		Check( NearRelative( measuredMRed, expectedRed, 0.012 ),
			"RayCaster NM matches the chromatic 700-nm slab target" );
		Check( NearRelative( measuredCMRed, expectedRed, 0.012 ) &&
			NearRelative( measuredCMRed, measuredMRed, 1e-13 ),
			"700-nm RayCaster radiance is scene-unit invariant" );

		RandomNumberGenerator rng( 7u );
		RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
		const RasterizerState rast = { 0, 0 };
		IRayCaster::RAY_STATE state;
		RISEPel pel( 1, 1, 1 );
		Scalar distance = -1.0;
		const bool pelHit = metres.caster->CastRay( rc, rast,
			Ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) ), pel, state, &distance, nullptr );
		Check( !pelHit && pel.r == 0.0 && pel.g == 0.0 && pel.b == 0.0,
			"Pel RayCaster rejects fire media until the preview step" );
	}

	void TestOpticallyThickDensityIsNotFloored()
	{
		std::cout << "TestOpticallyThickDensityIsNotFloored" << std::endl;
		Fixture thick;
		Check( thick.Initialize( "thick", 1.0, 100.0, 1.0 ),
			"optically thick fire slab initializes" );
		const IMedium* medium = thick.Medium();
		if( !medium ) return;
		const Ray ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );
		const Scalar sigmaT = medium->GetCoefficientsNM(
			Point3( 0, 0, 50.0 ), kWavelengthNM ).sigma_t;
		const Scalar eventDistance = 99.0 / sigmaT;
		const Scalar pdf = medium->EvalDistancePdfNM(
			ray, eventDistance, true, 100.0, kWavelengthNM );
		const Scalar expected = sigmaT * std::exp( -99.0 );
		Check( pdf > 0.0 && NearRelative( pdf, expected, 1e-10 ),
			"tau=99 distance density retains its true sub-1e-30 value" );
		Check( NearRelative( medium->EvalLogDistancePdfNM(
			ray, eventDistance, true, 100.0, kWavelengthNM ),
			std::log( sigmaT ) - 99.0, 1e-12 ),
			"tau=99 log density remains representable" );
	}

	void TestThermalScorePrecedesContinuationRoulette()
	{
		std::cout << "TestThermalScorePrecedesContinuationRoulette" << std::endl;
		Fixture thick;
		Check( thick.Initialize( "roulette", 1.0, 1.0, 1000.0 ),
			"roulette fire slab initializes" );
		if( !thick.caster ) return;

		const unsigned int rejectedSeed = SeedForFirstRouletteDraw( false );
		const unsigned int survivedSeed = SeedForFirstRouletteDraw( true );
		Check( rejectedSeed != 0 && survivedSeed != 0,
			"deterministic seeds cover both continuation-roulette outcomes" );
		if( rejectedSeed == 0 || survivedSeed == 0 ) return;

		const unsigned int seeds[] = { rejectedSeed, survivedSeed };
		const char* labels[] = {
			"rejected continuation retains the unweighted thermal collision score",
			"surviving continuation does not roulette-weight the thermal collision score"
		};
		for( unsigned int i = 0; i < 2; ++i ) {
			RandomNumberGenerator rng( seeds[i] );
			RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
			const RasterizerState rast = { 0, 0 };
			IRayCaster::RAY_STATE state;
			state.importance = 0.005;
			state.volumeBounces = 64;
			Scalar value = 0.0;
			Scalar distance = 0.0;
			const bool hasSource = thick.caster->CastRayNM(
				rc, rast, Ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) ),
				value, state, kWavelengthNM, &distance, nullptr );
			Check( hasSource && distance > 0.0,
				"thermal collision is sampled before continuation roulette terminates" );
			Check( NearRelative( value,
				PlanckSpectralRadianceNM( kWavelengthNM, kTemperatureK ), 1e-13 ),
				labels[i] );
		}
	}

	void TestHWSSFallbackPrecedesDepthGate()
	{
		std::cout << "TestHWSSFallbackPrecedesDepthGate" << std::endl;
		Fixture thick;
		Check( thick.Initialize( "hwss_depth", 1.0, 1.0, 1000.0 ),
			"HWSS fallback fire slab initializes" );
		if( !thick.caster ) return;

		RandomNumberGenerator rng( 0x4a775u );
		RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
		const RasterizerState rast = { 0, 0 };
		IRayCaster::RAY_STATE state;
		state.volumeBounces = 64;
		SampledWavelengths wavelengths = SampledWavelengths::SampleEquidistant(
			0.2, 400.0, 700.0 );
		IORStack iorStack( 1.0 );
		Scalar values[SampledWavelengths::N] = { 0.0, 0.0, 0.0, 0.0 };
		Scalar distance = 0.0;
		const bool hasSource = thick.caster->CastRayHWSS(
			rc, rast, Ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) ),
			values, state, wavelengths, &distance, nullptr, iorStack );
		Check( hasSource,
			"HWSS-requested fire uses the per-NM fallback before the depth gate" );
		for( unsigned int i = 0; i < SampledWavelengths::N; ++i ) {
			Check( NearRelative( values[i], PlanckSpectralRadianceNM(
				wavelengths.lambda[i], kTemperatureK ), 1e-13 ),
				"HWSS fallback retains the per-wavelength thermal collision score" );
		}
	}

	class DelayedDensityAccessor :
		public virtual IVolumeAccessor,
		public virtual Reference
	{
	public:
		bool tracking;
		DelayedDensityAccessor() : tracking( false ) {}
		Scalar GetValue( Scalar, Scalar, Scalar z ) const override
		{
			return tracking && z < 0.2 ? 0.0 : 1.0;
		}
		Scalar GetValue( int, int, int ) const override { return 1.0; }
		void BindVolume( const IVolume* ) override {}
	protected:
		~DelayedDensityAccessor() override = default;
	};

	class FixedSampler : public ISampler
	{
	public:
		unsigned int draws;
		FixedSampler() : draws( 0 ) {}
		Scalar Get1D() override
		{
			++draws;
			return 1.0 - std::exp( -1.0 );
		}
		Point2 Get2D() override { return Point2( Get1D(), Get1D() ); }
	};

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
			c.sigma_t = 0.0;
			c.sigma_s = 0.0;
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
			const Ray&, const Scalar, const Scalar ) const override { return 1.0; }
		bool IsHomogeneous() const override { return false; }
		bool GetBoundingBox( Point3& bbMin, Point3& bbMax ) const override
		{
			bbMin = Point3( -0.5, -0.5, 0.0 );
			bbMax = Point3( 0.5, 0.5, 1.0 );
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

	void TestSpatialAdditiveSourceSurvivesEscape()
	{
		std::cout << "TestSpatialAdditiveSourceSurvivesEscape" << std::endl;
		char filename[160];
		std::snprintf( filename, sizeof(filename),
			"rise_raycaster_spatial_additive_%d.RISEscene", static_cast<int>( ::getpid() ) );
		const std::filesystem::path scenePath =
			std::filesystem::temp_directory_path() / filename;
		{
			std::ofstream output( scenePath );
			output << "RISE ASCII SCENE 7\nstandard_shader\n{\nname global\n}\n";
		}

		IJobPriv* job = nullptr;
		IRayCaster* caster = nullptr;
		Check( RISE_CreateJobPriv( &job ) && job &&
			job->LoadAsciiSceneViaCst( scenePath.string().c_str() ),
			"spatial additive escape fixture initializes" );
		if( job ) {
			SpatialAdditiveMedium* medium = new SpatialAdditiveMedium();
			Check( medium->GetCoefficientsNM( Point3( 0, 0, 0 ), kWavelengthNM ).emission == 0.0,
				"spatial additive fixture has zero source at the ray origin" );
			job->GetScene()->SetGlobalMedium( medium );
			safe_release( medium );
			IShader* shader = job->GetShaders()->GetItem( "global" );
			Check( shader && RISE_API_CreateRayCaster(
				&caster, false, 10, *shader, true ) && caster,
				"spatial additive escape caster initializes" );
			if( caster ) caster->AttachScene( job->GetScene() );
		}

		if( caster ) {
			RandomNumberGenerator rng( 0xadd1715u );
			RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
			const RasterizerState rast = { 0, 0 };
			const Ray ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );
			IRayCaster::RAY_STATE state;
			Scalar acceptedSum = 0.0;
			for( unsigned int i = 0; i < kSamples; ++i ) {
				Scalar value = 0.0;
				if( caster->CastRayNM( rc, rast, ray, value, state,
					kWavelengthNM, nullptr, nullptr ) ) {
					acceptedSum += value;
				}
			}
			const Scalar measured = acceptedSum / Scalar( kSamples );
			Check( NearRelative( measured, 1.0, 0.012 ),
				"spatial additive source survives an empty-background escape entry route" );
		}

		safe_release( caster );
		safe_release( job );
		std::filesystem::remove( scenePath );
	}

	void TestDistanceSamplingContinuesPast1024Nulls()
	{
		std::cout << "TestDistanceSamplingContinuesPast1024Nulls" << std::endl;
		DelayedDensityAccessor* accessor = new DelayedDensityAccessor();
		IPhaseFunction* phase = nullptr;
		RISE_API_CreateIsotropicPhaseFunction( &phase );
		HeterogeneousMedium* medium = new HeterogeneousMedium(
			RISEPel( 1000, 1000, 1000 ), RISEPel( 0, 0, 0 ), *phase, *accessor,
			2, 2, 2, Point3( -1, -1, 0 ), Point3( 1, 1, 2 ) );
		accessor->tracking = true;
		FixedSampler sampler;
		bool scattered = false;
		const Scalar t = medium->SampleDistanceNM(
			Ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) ),
			2.0, kWavelengthNM, sampler, scattered );
		Check( scattered && t > 1.2 && t < 2.0,
			"real event after the historical proposal cap is retained" );
		Check( sampler.draws > 2048,
			"fixture forced more than 1024 null proposals before the event" );
		safe_release( medium );
		safe_release( phase );
		safe_release( accessor );
	}

	void TestTransmittanceContinuesPast1024Nulls()
	{
		std::cout << "TestTransmittanceContinuesPast1024Nulls" << std::endl;
		DelayedDensityAccessor* accessor = new DelayedDensityAccessor();
		IPhaseFunction* phase = nullptr;
		RISE_API_CreateIsotropicPhaseFunction( &phase );
		HeterogeneousMedium* medium = new HeterogeneousMedium(
			RISEPel( 1000, 1000, 1000 ), RISEPel( 0, 0, 0 ), *phase, *accessor,
			2, 2, 2, Point3( -1, -1, 0 ), Point3( 1, 1, 2 ) );
		accessor->tracking = true;
		const Scalar transmittance = medium->EvalTransmittanceNM(
			Ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) ),
			2.0, kWavelengthNM );
		Check( transmittance < 1e-12,
			"ratio tracking traverses the dense tail after more than 1024 null proposals" );
		safe_release( medium );
		safe_release( phase );
		safe_release( accessor );
	}
}

int main()
{
	TestAbsoluteSlabAndSceneUnits();
	TestOpticallyThickDensityIsNotFloored();
	TestThermalScorePrecedesContinuationRoulette();
	TestHWSSFallbackPrecedesDepthGate();
	TestDistanceSamplingContinuesPast1024Nulls();
	TestTransmittanceContinuesPast1024Nulls();
	TestSpatialAdditiveSourceSurvivesEscape();
	std::cout << passed << " passed, " << failed << " failed" << std::endl;
	return failed == 0 ? 0 : 1;
}
