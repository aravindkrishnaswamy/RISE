//////////////////////////////////////////////////////////////////////
//
//  HyLIoSSpectralTest.cpp - P5 spectral transport & HWSS per-lane
//    delegation tests for HyLIoSSkinMaterial:
//    1. Direct camera hit on a wavelength-varying HyLIoSSkinMaterial:
//       IntegrateRayHWSS per-lane delegation agrees with IntegrateRayNM
//       at every wavelength in the bundle (450, 550, 650, 720 nm).
//    2. Mid-path bounce hit (camera -> perfect mirror -> HyLIoS ->
//       constant spectral environment): IntegrateRayHWSS mid-path
//       delegation agrees with IntegrateRayNM and preserves prior
//       throughput across all lanes.
//    3. Out-of-band bundles return finite zero radiance without NaN/Inf.
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "../src/Library/RISE_API.h"
#include "../src/Library/Job.h"
#include "../src/Library/Materials/HyLIoS/HyLIoSSkinMaterial.h"
#include "../src/Library/Materials/HyLIoS/HyLIoSLocalResponseAsset.h"
#include "../src/Library/Shaders/PathTracingIntegrator.h"
#include "../src/Library/Shaders/StandardShader.h"
#include "../src/Library/Rendering/RayCaster.h"
#include "../src/Library/Rendering/RadianceMap.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/RISECBOR64.h"
#include "../src/Library/Utilities/Color/SampledWavelengths.h"

using namespace RISE;
using namespace RISE::Implementation;
using namespace RISE::HyLIoS;

namespace {

int gFailures = 0;

void Check( const bool cond, const std::string& msg )
{
	if( !cond ) {
		++gFailures;
		std::cerr << "FAIL: " << msg << "\n";
	}
}

void RelNear( const double actual, const double expected, const double relTol, const double absTol, const std::string& msg )
{
	const double tol = std::max( absTol, relTol * std::fabs( expected ) );
	if( !std::isfinite( actual ) || std::fabs( actual - expected ) > tol ) {
		++gFailures;
		std::cerr << "FAIL: " << msg << " (actual=" << actual << ", expected=" << expected
				  << ", tol=" << tol << ")\n";
	}
}

struct TempDirGuard
{
	std::filesystem::path dir;
	explicit TempDirGuard( const char* tag )
	{
		dir = std::filesystem::temp_directory_path() / ( std::string( "rise_hylios_spec_" ) + tag + "_" + std::to_string( std::rand() ) );
		std::filesystem::create_directories( dir );
	}
	~TempDirGuard()
	{
		std::error_code ec;
		std::filesystem::remove_all( dir, ec );
	}
};

LocalAssetRecord EncodeMap( const double val, const char* tag )
{
	using V = RISE::RISECBOR64::Value;
	LocalAssetRecord out;
	std::string err;
	const V m = V::MapValue( {
		{ "actual", V::Float( val ) },
		{ "revision", V::String( tag ) }
	} );
	if( !RISE::RISECBOR64::Encode( m, out, &err ) ) {
		throw std::runtime_error( err );
	}
	return out;
}

std::string WriteDispersiveAnalyticAsset(
	const std::filesystem::path& path,
	const std::vector<double>& wavelengths,
	const std::vector<double>& diffusePerWl,
	const std::vector<double>& mirrorPerWl,
	LocalAssetSemanticIdentity& outSemantic
	)
{
	const LocalAssetRecord rec = EncodeMap( 1.0, "dispersive_fixture_v1" );
	outSemantic = {
		rec, rec, rec, rec, rec, rec, rec, rec, rec,
		wavelengths.front(), wavelengths.back()
	};

	LocalAssetDefinition def;
	def.semantic = outSemantic;
	def.production = {
		"analytic_oracle_v1", "hylios_angular_lineage_v1", "analog_v1",
		"index_mapping_v1", "analytic_stats_v1",
		9ULL, 16ULL, 2ULL, 32ULL, 512ULL, rec, rec
	};
	def.response.incident = { { 0.0, 0.5, 1.0 }, { 0.0, LocalResponsePi, LocalResponseTwoPi } };
	def.response.outgoing = { { 0.0, 0.5, 1.0 }, { 0.0, LocalResponsePi, LocalResponseTwoPi } };
	def.response.wavelengthNm = wavelengths;

	const std::size_t nIn = 4, nOut = 4, nWl = wavelengths.size();
	def.response.continuous.assign( nIn * nOut * nWl, 0.0 );
	def.response.mirror.assign( nIn * nWl, 0.0 );
	for( std::size_t k = 0; k < nWl; ++k ) {
		for( std::size_t wi = 0; wi < nIn; ++wi ) {
			def.response.mirror[k * nIn + wi] = mirrorPerWl[k];
			for( std::size_t wo = 0; wo < nOut; ++wo ) {
				def.response.continuous[( k * nIn + wi ) * nOut + wo] = diffusePerWl[k] / LocalResponsePi;
			}
		}
	}

	def.normalIncidenceGuide.resize( nWl, 0.0 );
	for( std::size_t k = 0; k < nWl; ++k ) {
		def.normalIncidenceGuide[k] = diffusePerWl[k] + mirrorPerWl[k];
	}

	SaveLocalResponseAsset( path.string(), def );
	return path.string();
}

void TestFirstHitAndMidPathHWSSDelegation()
{
	TempDirGuard tmp( "hwss" );
	const std::vector<double> wls  = { 450.0, 550.0, 650.0, 720.0 };
	const std::vector<double> diff = { 0.15,  0.40,  0.75,  0.55  };
	const std::vector<double> mirr = { 0.05,  0.04,  0.03,  0.02  };
	LocalAssetSemanticIdentity sem;
	const std::string assetPath = WriteDispersiveAnalyticAsset( tmp.dir / "disp.hylios", wls, diff, mirr, sem );

	IJob* pJobI = 0;
	RISE_CreateJob( &pJobI );
	Job* pJob = dynamic_cast<Job*>( pJobI );

	const double loc[3] = { 0, 0, 2 }, look[3] = { 0, 0, 0 }, up[3] = { 0, 1, 0 };
	const double oriZero[3] = { 0, 0, 0 }, targetZero[2] = { 0, 0 };
	pJob->SetFilm( 16, 16, 1.0 );
	pJob->AddPinholeCamera( "cam", loc, look, up, 0.8, 0.0, 0.0, 0.0, oriZero, targetZero );

	pJob->AddBoxGeometry( "skin_slab", 20.0, 20.0, 0.2 );
	HyLIoSSkinMaterialConfig cfg;
	cfg.responseAssetPath = assetPath.c_str();
	cfg.useStandaloneAssetCodec = true;
	cfg.explicitSemanticIdentity = &sem;
	Check( pJob->AddHyLIoSSkinMaterial( "skin_disp", cfg ), "AddHyLIoSSkinMaterial dispersive" );
	const double posSkin[3] = { 0, 0, -0.1 }, scaOne[3] = { 1, 1, 1 };
	pJob->AddObject( "skin_obj", "skin_slab", "skin_disp", 0, 0, RadianceMapConfig(), posSkin, oriZero, scaOne, true, true );

	// Constant white environment map
	UniformColorPainter* pEnvPainter = new UniformColorPainter( RISEPel( 1.0, 1.0, 1.0 ) );
	RadianceMap* pEnvMap = new RadianceMap( *pEnvPainter, 1.0 );
	pJob->SetGlobalRadianceMap( pEnvMap );

	IScenePriv* pScene = pJob->GetScene();
	pScene->GetObjects()->PrepareForRendering();

	std::vector<IShaderOp*> emptyOps;
	StandardShader* pDefShader = new StandardShader( emptyOps );
	RayCaster* pCaster = new RayCaster( true, 8, *pDefShader, true );
	pCaster->AttachScene( pScene );

	PathTracingIntegrator* pIntegrator = new PathTracingIntegrator(
		ManifoldSolverConfig(), StabilityConfig() );

	const IRadianceMap* pEnv = pScene->GetGlobalRadianceMap();
	Check( pEnv != 0, "Global radiance map present" );

	// 1. Direct camera hit on HyLIoS
	const Ray camRay( Point3( 0, 0, 2 ), Vector3( 0, 0, -1 ) );
	const int N = 400;
	double sumNM[4] = { 0, 0, 0, 0 };
	double sumHWSS[4] = { 0, 0, 0, 0 };

	SampledWavelengths swl;
	for( unsigned int i = 0; i < SampledWavelengths::N; ++i ) {
		swl.lambda[i] = wls[i];
		swl.pdf[i] = 1.0;
		swl.terminated[i] = false;
	}

	for( int s = 0; s < N; ++s ) {
		RandomNumberGenerator rngNM( 1000u + static_cast<unsigned int>( s ) );
		IndependentSampler smpNM( rngNM );
		RuntimeContext rcNM( rngNM, RuntimeContext::PASS_NORMAL, false );
		for( unsigned int i = 0; i < SampledWavelengths::N; ++i ) {
			sumNM[i] += pIntegrator->IntegrateRayNM( rcNM, nullRasterizerState, camRay, wls[i], *pScene, *pCaster, smpNM, pEnv );
		}

		RandomNumberGenerator rngHW( 20000u + static_cast<unsigned int>( s ) );
		IndependentSampler smpHW( rngHW );
		RuntimeContext rcHW( rngHW, RuntimeContext::PASS_NORMAL, false );
		SampledWavelengths swlCopy = swl;
		Scalar res[SampledWavelengths::N] = { 0 };
		pIntegrator->IntegrateRayHWSS( rcHW, nullRasterizerState, camRay, swlCopy, *pScene, *pCaster, smpHW, pEnv, res );
		for( unsigned int i = 0; i < SampledWavelengths::N; ++i ) {
			Check( std::isfinite( res[i] ), "First-hit HWSS lane finite" );
			sumHWSS[i] += res[i];
		}
	}

	const Ray envProbeRay( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );
	for( unsigned int i = 0; i < SampledWavelengths::N; ++i ) {
		const double meanNM = sumNM[i] / N;
		const double meanHW = sumHWSS[i] / N;
		const double envL = pEnv->GetRadianceNM( envProbeRay, nullRasterizerState, wls[i] );
		const double expected = ( diff[i] + mirr[i] ) * envL;
		RelNear( meanNM, expected, 0.12, 0.02, "First-hit NM matches expected R(" + std::to_string( int( wls[i] ) ) + ")" );
		RelNear( meanHW, expected, 0.12, 0.02, "First-hit HWSS matches expected R(" + std::to_string( int( wls[i] ) ) + ")" );
		RelNear( meanHW, meanNM,   0.12, 0.02, "First-hit HWSS agrees with NM at " + std::to_string( int( wls[i] ) ) + " nm" );
	}

	// 2. Out-of-band bundle returns finite zero
	{
		SampledWavelengths oobSwl;
		const double oobWls[4] = { 200.0, 220.0, 240.0, 260.0 };
		for( unsigned int i = 0; i < SampledWavelengths::N; ++i ) {
			oobSwl.lambda[i] = oobWls[i];
			oobSwl.pdf[i] = 1.0;
			oobSwl.terminated[i] = false;
		}
		RandomNumberGenerator rng( 777u );
		IndependentSampler smp( rng );
		RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
		Scalar res[SampledWavelengths::N] = { 0 };
		pIntegrator->IntegrateRayHWSS( rc, nullRasterizerState, camRay, oobSwl, *pScene, *pCaster, smp, pEnv, res );
		for( unsigned int i = 0; i < SampledWavelengths::N; ++i ) {
			Check( std::isfinite( res[i] ) && res[i] == 0.0, "Out-of-band HWSS lane == 0" );
		}
	}

	// 3. Bounce-2 mid-path delegation (mirror -> HyLIoS -> env)
	const double mirrorRefl[3] = { 0.6, 0.6, 0.6 };
	pJob->AddUniformColorPainter( "mirror_p", mirrorRefl, "Rec709RGB_Linear" );
	pJob->AddPerfectReflectorMaterial( "mirror_m", "mirror_p" );
	pJob->AddBoxGeometry( "mirror_box", 4.0, 4.0, 0.2 );
	const double posMirror[3] = { 0, 0, 4.1 };
	pJob->AddObject( "mirror_obj", "mirror_box", "mirror_m", 0, 0, RadianceMapConfig(), posMirror, oriZero, scaOne, true, true );
	pScene->GetObjects()->InvalidateSpatialStructure();
	pScene->GetObjects()->PrepareForRendering();
	pCaster->AttachScene( pScene );

	const Ray bounceRay( Point3( 0, 0, 2 ), Vector3( 0, 0, 1 ) );
	double sumB2NM[4] = { 0, 0, 0, 0 };
	double sumB2HW[4] = { 0, 0, 0, 0 };
	for( int s = 0; s < N; ++s ) {
		RandomNumberGenerator rngNM( 50000u + static_cast<unsigned int>( s ) );
		IndependentSampler smpNM( rngNM );
		RuntimeContext rcNM( rngNM, RuntimeContext::PASS_NORMAL, false );
		for( unsigned int i = 0; i < SampledWavelengths::N; ++i ) {
			sumB2NM[i] += pIntegrator->IntegrateRayNM( rcNM, nullRasterizerState, bounceRay, wls[i], *pScene, *pCaster, smpNM, pEnv );
		}

		RandomNumberGenerator rngHW( 90000u + static_cast<unsigned int>( s ) );
		IndependentSampler smpHW( rngHW );
		RuntimeContext rcHW( rngHW, RuntimeContext::PASS_NORMAL, false );
		SampledWavelengths swlCopy = swl;
		Scalar res[SampledWavelengths::N] = { 0 };
		pIntegrator->IntegrateRayHWSS( rcHW, nullRasterizerState, bounceRay, swlCopy, *pScene, *pCaster, smpHW, pEnv, res );
		for( unsigned int i = 0; i < SampledWavelengths::N; ++i ) {
			Check( std::isfinite( res[i] ), "Bounce-2 HWSS lane finite" );
			sumB2HW[i] += res[i];
		}
	}

	for( unsigned int i = 0; i < SampledWavelengths::N; ++i ) {
		const double meanNM = sumB2NM[i] / N;
		const double meanHW = sumB2HW[i] / N;
		Check( meanNM > 0.05 && meanHW > 0.05, "Bounce-2 positive radiance at " + std::to_string( int( wls[i] ) ) + " nm" );
		RelNear( meanHW, meanNM, 0.14, 0.02, "Bounce-2 HWSS agrees with NM at " + std::to_string( int( wls[i] ) ) + " nm" );
	}
	Check( ( sumB2HW[2] / N ) > 2.5 * ( sumB2HW[0] / N ), "Bounce-2 HWSS preserves strong spectral dispersion (650nm >> 450nm)" );

	safe_release( pIntegrator );
	safe_release( pCaster );
	safe_release( pDefShader );
	safe_release( pEnvMap );
	safe_release( pEnvPainter );
	safe_release( pJobI );
}

} // namespace

int main()
{
	TestFirstHitAndMidPathHWSSDelegation();

	if( gFailures == 0 ) {
		std::cout << "HyLIoSSpectralTest: ALL PASSED\n";
		return 0;
	}
	std::cerr << "HyLIoSSpectralTest: " << gFailures << " FAILURES\n";
	return 1;
}
