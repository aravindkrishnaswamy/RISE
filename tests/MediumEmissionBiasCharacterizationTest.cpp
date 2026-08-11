//////////////////////////////////////////////////////////////////////
//
//  MediumEmissionBiasCharacterizationTest.cpp - Phase-A step-0 numeric
//    characterization of the EXISTING RayCaster additive-emission path.
//
//  This test records the pre-fire-estimator Pel baseline and the repaired NM
//  additive-source contract.  The
//  current homogeneous-medium pickup integrates an analytic prefix at a
//  sampled collision and the full segment on the no-collision atom.  Because
//  branch selection already carries the transmittance probability, its RGB
//  expectation is
//
//      E[L_RGB] = epsilon * (1 - exp(-2 sigma_t L)) / (2 sigma_t),
//
//  rather than the physical source integral
//
//      L = epsilon * (1 - exp(-sigma_t L)) / sigma_t.
//
//  CastRayNM now treats this absorption-independent source separately from
//  thermal emission and integrates a homogeneous full segment exactly, so its
//  expectation is the physical source integral on every collision outcome.
//
//  The final case characterizes the separate equiangular early return.  In a
//  zero-extinction additive emitter, the DT half of the mixture reaches the
//  no-scatter block and is compensated by 1/0.5, while the equiangular half
//  returns zero before that block.  The distribution is therefore {0, 2 e L}
//  with equal probability even though its mean remains e L.
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
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
#include "../src/Library/Interfaces/IMedium.h"
#include "../src/Library/Interfaces/IRayCaster.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Interfaces/IShader.h"
#include "../src/Library/Interfaces/IShaderManager.h"
#include "../src/Library/RISE_API.h"
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
	int passCount = 0;
	int failCount = 0;

	const double kSlabLength = 1.0;
	const double kEmission = 2.0;
	const unsigned int kSamples = 50000;

	void Check( const bool condition, const char* name )
	{
		if( condition ) {
			++passCount;
		} else {
			++failCount;
			std::cout << "  FAIL: " << name << std::endl;
		}
	}

	bool NearRelative( const double measured, const double expected, const double tolerance )
	{
		const double scale = std::fmax( std::fabs( expected ), 1e-12 );
		return std::fabs( measured - expected ) / scale <= tolerance;
	}

	std::string SceneText( const bool positionalLight )
	{
		std::ostringstream ss;
		ss <<
			"RISE ASCII SCENE 7\n"
			"standard_shader\n"
			"{\n"
			"  name global\n"
			"}\n"
			"box_geometry\n"
			"{\n"
			"  name wall_geometry\n"
			"  width 10\n"
			"  height 10\n"
			"  depth 0.2\n"
			"}\n"
			"standard_object\n"
			"{\n"
			"  name black_wall\n"
			"  geometry wall_geometry\n"
			"  material none\n"
			"  position 0 0 1.1\n"
			"}\n";

		if( positionalLight ) {
			ss <<
				"omni_light\n"
				"{\n"
				"  name equiangular_pivot\n"
				"  power 1\n"
				"  color 1 1 1\n"
				"  position 1 0 0.5\n"
				"}\n";
		}

		return ss.str();
	}

	std::string WriteScene( const bool positionalLight )
	{
		char filename[160];
		std::snprintf( filename, sizeof(filename),
			"rise_medium_emission_step0_%s_%d.RISEscene",
			positionalLight ? "equiangular" : "dt",
			static_cast<int>( ::getpid() ) );
		const std::string path =
			(std::filesystem::temp_directory_path() / filename).string();

		std::ofstream output( path );
		if( !output.is_open() ) {
			return std::string();
		}
		output << SceneText( positionalLight );
		return path;
	}

	struct Fixture
	{
		IJobPriv* job;
		IRayCaster* caster;
		std::string scenePath;

		Fixture() : job( 0 ), caster( 0 ) {}

		~Fixture()
		{
			safe_release( caster );
			safe_release( job );
			if( !scenePath.empty() ) {
				std::remove( scenePath.c_str() );
			}
		}

		bool Initialize(
			const bool positionalLight,
			const double sigmaA,
			const double sigmaS,
			const double emission )
		{
			scenePath = WriteScene( positionalLight );
			if( scenePath.empty() || !RISE_CreateJobPriv( &job ) || !job ) {
				return false;
			}
			if( !job->LoadAsciiSceneViaCst( scenePath.c_str() ) ) {
				return false;
			}

			const double sigmaA_RGB[3] = { sigmaA, sigmaA, sigmaA };
			const double sigmaS_RGB[3] = { sigmaS, sigmaS, sigmaS };
			const double emissionRGB[3] = { emission, emission, emission };
			if( !job->AddHomogeneousMediumSpectral(
				"emissive_slab", sigmaA_RGB, sigmaS_RGB, emissionRGB,
				0, 0, "isotropic", 0.0 ) ||
				!job->SetGlobalMedium( "emissive_slab" ) ) {
				return false;
			}
			const IMedium* medium = job->GetScene()->GetGlobalMedium();
			if( !medium ||
				!NearRelative( medium->GetCoefficients( Point3( 0, 0, 0 ) ).emission.r,
					emission, 1e-12 ) ) {
				return false;
			}

			IShaderManager* shaders = job->GetShaders();
			IShader* shader = shaders ? shaders->GetItem( "global" ) : 0;
			if( !shader || !RISE_API_CreateRayCaster(
				&caster, false, 0, *shader, true ) || !caster ) {
				return false;
			}

			caster->AttachScene( job->GetScene() );
			return true;
		}
	};

	double MeanPel( IRayCaster& caster, const unsigned int samples, const unsigned int seed )
	{
		RandomNumberGenerator rng( seed );
		RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
		const RasterizerState rast = { 0, 0 };
		const Ray ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );
		IRayCaster::RAY_STATE rayState;
		rayState.depth = 0;
		double sum = 0;

		for( unsigned int i = 0; i < samples; ++i ) {
			RISEPel value( 0, 0, 0 );
			Scalar distance = 0;
			caster.CastRay( rc, rast, ray, value, rayState, &distance, 0 );
			sum += ( value.r + value.g + value.b ) / 3.0;
		}

		return sum / static_cast<double>( samples );
	}

	double MeanNM( IRayCaster& caster, const unsigned int samples, const unsigned int seed )
	{
		RandomNumberGenerator rng( seed );
		RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
		const RasterizerState rast = { 0, 0 };
		const Ray ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );
		IRayCaster::RAY_STATE rayState;
		rayState.depth = 0;
		double sum = 0;

		for( unsigned int i = 0; i < samples; ++i ) {
			Scalar value = 0;
			Scalar distance = 0;
			caster.CastRayNM( rc, rast, ray, value, rayState, 500.0, &distance, 0 );
			sum += value;
		}

		return sum / static_cast<double>( samples );
	}

	void TestHomogeneousBias()
	{
		std::cout << "[1] homogeneous emissive slab, sigma_s > 0, RGB and NM" << std::endl;
		const double sigmaA = 0.4;
		const double sigmaS = 0.6;
		const double sigmaT = sigmaA + sigmaS;
		const double tau = sigmaT * kSlabLength;

		Fixture fixture;
		Check( fixture.Initialize( false, sigmaA, sigmaS, kEmission ),
			"homogeneous slab fixture initializes" );
		if( !fixture.caster ) {
			return;
		}

		const double measuredPel = MeanPel( *fixture.caster, kSamples, 0x5a17u );
		const double measuredNM = MeanNM( *fixture.caster, kSamples, 0xc041u );

		const double physical = kEmission * (1.0 - std::exp( -tau )) / sigmaT;
		const double currentPel = kEmission * (1.0 - std::exp( -2.0 * tau )) /
			(2.0 * sigmaT);
		const double oldNM = (kEmission / sigmaT) *
			((1.0 - std::exp( -tau )) - 0.5 * (1.0 - std::exp( -2.0 * tau )));

		std::cout << "    RGB measured=" << measuredPel
			<< "  current-closed-form=" << currentPel
			<< "  physical=" << physical << std::endl;
		std::cout << "    NM  measured=" << measuredNM
			<< "  old-closed-form=" << oldNM
			<< "  physical=" << physical << std::endl;

		Check( NearRelative( measuredPel, currentPel, 0.01 ),
			"RGB matches the current double-transmittance closed form" );
		Check( NearRelative( measuredNM, physical, 1e-12 ),
			"NM additive source matches the exact homogeneous full-segment integral" );
		Check( !NearRelative( measuredPel, physical, 0.10 ),
			"RGB baseline is numerically distinct from physical source integration" );
		Check( !NearRelative( measuredNM, oldNM, 0.10 ),
			"NM no longer matches the collision-prefix-only baseline" );
	}

	void TestEquiangularZeroContributionEarlyOut()
	{
		std::cout << "[2] equiangular zero-density early-out distribution" << std::endl;

		Fixture fixture;
		Check( fixture.Initialize( true, 0.0, 0.0, kEmission ),
			"zero-extinction equiangular fixture initializes" );
		if( !fixture.caster ) {
			return;
		}

		RandomNumberGenerator rng( 0xe911u );
		RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
		const RasterizerState rast = { 0, 0 };
		const Ray ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );
		IRayCaster::RAY_STATE rayState;
		rayState.depth = 0;
		unsigned int zeroCount = 0;
		unsigned int compensatedCount = 0;
		double sum = 0;

		for( unsigned int i = 0; i < kSamples; ++i ) {
			RISEPel value( 0, 0, 0 );
			Scalar distance = 0;
			fixture.caster->CastRay( rc, rast, ray, value, rayState, &distance, 0 );
			const double sample = ( value.r + value.g + value.b ) / 3.0;
			sum += sample;
			if( std::fabs( sample ) < 1e-12 ) {
				++zeroCount;
			} else if( NearRelative( sample, 2.0 * kEmission * kSlabLength, 1e-10 ) ) {
				++compensatedCount;
			}
		}

		const double mean = sum / static_cast<double>( kSamples );
		const double zeroFraction = static_cast<double>( zeroCount ) / kSamples;
		const double compensatedFraction = static_cast<double>( compensatedCount ) / kSamples;
		std::cout << "    zero fraction=" << zeroFraction
			<< "  compensated fraction=" << compensatedFraction
			<< "  mean=" << mean << std::endl;

		Check( zeroCount + compensatedCount == kSamples,
			"every sample is either early-out zero or compensated DT no-scatter" );
		Check( std::fabs( zeroFraction - 0.5 ) < 0.01,
			"equiangular strategy half returns zero before additive-emission pickup" );
		Check( std::fabs( compensatedFraction - 0.5 ) < 0.01,
			"delta-tracking half reaches the no-scatter pickup with 1/0.5 compensation" );
		Check( NearRelative( mean, kEmission * kSlabLength, 0.01 ),
			"compensated mixture mean remains the vacuum additive-emission integral" );
	}
}

int main()
{
	std::cout << "MediumEmissionBiasCharacterizationTest" << std::endl;
	TestHomogeneousBias();
	TestEquiangularZeroContributionEarlyOut();

	std::cout << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount == 0 ? 0 : 1;
}
