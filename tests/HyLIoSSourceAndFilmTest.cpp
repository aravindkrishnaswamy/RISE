//////////////////////////////////////////////////////////////////////
//
//  HyLIoSSourceAndFilmTest.cpp - P6b preflight & radiometric accuracy
//    tests for HyperspectralRadiometricRasterizer:
//    1. Mechanical refusal of all bias controls (direct_clamp,
//       indirect_clamp, filter_glossy, oidn_denoise, tone_map,
//       negative-lobe pixel filters).
//    2. Refusal of non-physical RGB delta lights in strict mode and
//       uncovered HyLIoS wavelength bands.
//    3. Exact physical radiometric units L_lambda [W m^-2 sr^-1 nm^-1]
//       under a calibrated physical_directional_light illuminating a
//       known Lambertian HyLIoS target: L_lambda == (rho / pi) * E_lambda.
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "../src/Library/RISE_API.h"
#include "../src/Library/Job.h"
#include "../src/Library/Materials/HyLIoS/HyLIoSSkinMaterial.h"
#include "../src/Library/Materials/HyLIoS/HyLIoSLocalResponseAsset.h"
#include "../src/Library/Rendering/HyperspectralRadiometricRasterizer.h"
#include "../src/Library/Rendering/RayCaster.h"
#include "../src/Library/Shaders/StandardShader.h"
#include "../src/Library/Utilities/RISECBOR64.h"

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

void RelNear( const double actual, const double expected, const double relTol, const std::string& msg )
{
	const double tol = std::max( 1e-6, relTol * std::fabs( expected ) );
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
		dir = std::filesystem::temp_directory_path() / ( std::string( "rise_hylios_sf_" ) + tag + "_" + std::to_string( std::rand() ) );
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
	const V m = V::MapValue( { { "actual", V::Float( val ) }, { "revision", V::String( tag ) } } );
	if( !RISE::RISECBOR64::Encode( m, out, &err ) ) throw std::runtime_error( err );
	return out;
}

std::string WriteLambertianAsset(
	const std::filesystem::path& path,
	const std::vector<double>& wavelengths,
	const std::vector<double>& rhoPerWl,
	LocalAssetSemanticIdentity& outSem
	)
{
	const LocalAssetRecord rec = EncodeMap( 1.0, "radiometric_fixture_v1" );
	outSem = { rec, rec, rec, rec, rec, rec, rec, rec, rec, wavelengths.front(), wavelengths.back() };

	LocalAssetDefinition def;
	def.semantic = outSem;
	def.production = {
		"analytic_oracle_v1", "hylios_angular_lineage_v1", "analog_v1",
		"index_mapping_v1", "analytic_stats_v1",
		11ULL, 16ULL, 2ULL, 32ULL, 512ULL, rec, rec
	};
	def.response.incident = { { 0.0, 0.5, 1.0 }, { 0.0, LocalResponsePi, LocalResponseTwoPi } };
	def.response.outgoing = { { 0.0, 0.5, 1.0 }, { 0.0, LocalResponsePi, LocalResponseTwoPi } };
	def.response.wavelengthNm = wavelengths;

	const std::size_t nIn = 4, nOut = 4, nWl = wavelengths.size();
	def.response.continuous.assign( nIn * nOut * nWl, 0.0 );
	def.response.mirror.assign( nIn * nWl, 0.0 );
	for( std::size_t k = 0; k < nWl; ++k ) {
		for( std::size_t wi = 0; wi < nIn; ++wi ) {
			for( std::size_t wo = 0; wo < nOut; ++wo ) {
				def.response.continuous[( k * nIn + wi ) * nOut + wo] = rhoPerWl[k] / LocalResponsePi;
			}
		}
	}
	def.normalIncidenceGuide = rhoPerWl;

	SaveLocalResponseAsset( path.string(), def );
	return path.string();
}

void TestPreflightRefusals()
{
	TempDirGuard tmp( "preflight" );
	const std::vector<double> wls = { 450.0, 550.0, 650.0 };
	const std::vector<double> rho = { 0.3, 0.5, 0.7 };
	LocalAssetSemanticIdentity sem;
	const std::string assetPath = WriteLambertianAsset( tmp.dir / "lam.hylios", wls, rho, sem );

	IJob* pJobI = 0;
	RISE_CreateJob( &pJobI );
	Job* pJob = dynamic_cast<Job*>( pJobI );
	const double loc[3] = { 0, 0, 2 }, look[3] = { 0, 0, 0 }, up[3] = { 0, 1, 0 };
	const double oriZero[3] = { 0, 0, 0 }, targetZero[2] = { 0, 0 };
	pJob->SetFilm( 4, 4, 1.0 );
	pJob->AddPinholeCamera( "cam", loc, look, up, 0.8, 0.0, 0.0, 0.0, oriZero, targetZero );
	pJob->AddBoxGeometry( "slab", 20.0, 20.0, 0.2 );

	HyLIoSSkinMaterialConfig matCfg;
	matCfg.responseAssetPath = assetPath.c_str();
	matCfg.useStandaloneAssetCodec = true;
	matCfg.explicitSemanticIdentity = &sem;
	pJob->AddHyLIoSSkinMaterial( "skin", matCfg );
	const double pos[3] = { 0, 0, -0.1 }, sca[3] = { 1, 1, 1 };
	pJob->AddObject( "skin_obj", "slab", "skin", 0, 0, RadianceMapConfig(), pos, oriZero, sca, true, true );

	// Add a physical directional light
	const double dir[3] = { 0, 0, 1 };
	const double irr[3] = { 10.0, 20.0, 30.0 };
	Check( pJob->AddPhysicalDirectionalLight( "sun", 3, wls.data(), irr, "zero", 1.0, dir ),
		"AddPhysicalDirectionalLight succeeds" );

	IScenePriv* pScene = pJob->GetScene();
	std::vector<IShaderOp*> emptyOps;
	StandardShader* pDefShader = new StandardShader( emptyOps );
	RayCaster* pCaster = new RayCaster( true, 8, *pDefShader, true );

	HyperspectralRadiometricConfig base;
	base.wavelengthsNm = wls;
	base.samplesPerPixel = 4;
	base.outputPrefix = ( tmp.dir / "out" ).string();

	auto expectFail = [&]( const HyperspectralRadiometricConfig& c, const char* label ) {
		HyperspectralRadiometricRasterizer* r = new HyperspectralRadiometricRasterizer( pCaster, c );
		std::vector<double> resolved;
		std::string err;
		Check( !r->ValidateSceneAndConfig( *pScene, resolved, err ) && !err.empty(),
			std::string( "Preflight rejects: " ) + label );
		safe_release( r );
	};

	// Baseline must pass
	{
		HyperspectralRadiometricRasterizer* r = new HyperspectralRadiometricRasterizer( pCaster, base );
		std::vector<double> resolved;
		std::string err;
		Check( r->ValidateSceneAndConfig( *pScene, resolved, err ), "Baseline valid config passes: " + err );
		safe_release( r );
	}

	// Bias controls rejected
	{ auto c = base; c.stabilityConfig.directClamp = 10.0;   expectFail( c, "directClamp > 0" ); }
	{ auto c = base; c.stabilityConfig.indirectClamp = 10.0; expectFail( c, "indirectClamp > 0" ); }
	{ auto c = base; c.stabilityConfig.filterGlossy = 0.5;   expectFail( c, "filterGlossy > 0" ); }
	{ auto c = base; c.oidnDenoise = true;                   expectFail( c, "oidnDenoise" ); }
	{ auto c = base; c.toneMap = true;                       expectFail( c, "toneMap" ); }
	{ auto c = base; c.pixelFilterName = "mitchell";         expectFail( c, "mitchell negative-lobe filter" ); }
	{ auto c = base; c.pixelFilterName = "lanczos";          expectFail( c, "lanczos negative-lobe filter" ); }
	{ auto c = base; c.wavelengthsNm = { 300.0, 550.0 };     expectFail( c, "uncovered HyLIoS band" ); }

	// Non-physical RGB point light refused when allowRGBUpliftSources == false
	{
		const double lpos[3] = { 0, 0, 5 }, col[3] = { 1, 1, 1 };
		pJob->AddPointOmniLight( "legacy_rgb", 10.0, col, "Rec709RGB_Linear", lpos, false );
		expectFail( base, "legacy RGB point light in strict mode" );

		auto allowCfg = base;
		allowCfg.allowRGBUpliftSources = true;
		HyperspectralRadiometricRasterizer* r = new HyperspectralRadiometricRasterizer( pCaster, allowCfg );
		std::vector<double> resolved;
		std::string err;
		Check( r->ValidateSceneAndConfig( *pScene, resolved, err ), "allowRGBUpliftSources opt-in passes" );
		safe_release( r );
	}

	safe_release( pCaster );
	safe_release( pDefShader );
	safe_release( pJobI );
}

void TestCalibratedPhysicalDirectionalRadiance()
{
	TempDirGuard tmp( "calibrated" );
	const std::vector<double> wls = { 450.0, 550.0, 650.0 };
	const std::vector<double> rho = { 0.30,  0.50,  0.70  };
	const std::vector<double> E   = { 10.0,  25.0,  40.0  }; // W m^-2 nm^-1

	LocalAssetSemanticIdentity sem;
	const std::string assetPath = WriteLambertianAsset( tmp.dir / "cal.hylios", wls, rho, sem );

	IJob* pJobI = 0;
	RISE_CreateJob( &pJobI );
	Job* pJob = dynamic_cast<Job*>( pJobI );
	const double loc[3] = { 0, 0, 2 }, look[3] = { 0, 0, 0 }, up[3] = { 0, 1, 0 };
	const double oriZero[3] = { 0, 0, 0 }, targetZero[2] = { 0, 0 };
	pJob->SetFilm( 4, 4, 1.0 );
	pJob->AddPinholeCamera( "cam", loc, look, up, 0.5, 0.0, 0.0, 0.0, oriZero, targetZero );
	pJob->AddBoxGeometry( "slab", 20.0, 20.0, 0.2 );

	HyLIoSSkinMaterialConfig matCfg;
	matCfg.responseAssetPath = assetPath.c_str();
	matCfg.useStandaloneAssetCodec = true;
	matCfg.explicitSemanticIdentity = &sem;
	pJob->AddHyLIoSSkinMaterial( "skin", matCfg );
	const double pos[3] = { 0, 0, -0.1 }, sca[3] = { 1, 1, 1 };
	pJob->AddObject( "skin_obj", "slab", "skin", 0, 0, RadianceMapConfig(), pos, oriZero, sca, true, true );

	// Directional light arriving straight down along -Z (towards +Z surface normal => cos(theta) = 1)
	const double dirToLight[3] = { 0, 0, 1 };
	Check( pJob->AddPhysicalDirectionalLight( "sun", 3, wls.data(), E.data(), "zero", 1.0, dirToLight ),
		"AddPhysicalDirectionalLight calibrated" );

	IScenePriv* pScene = pJob->GetScene();
	std::vector<IShaderOp*> emptyOps;
	StandardShader* pDefShader = new StandardShader( emptyOps );
	RayCaster* pCaster = new RayCaster( true, 8, *pDefShader, true );

	HyperspectralRadiometricConfig cfg;
	cfg.wavelengthsNm = wls;
	cfg.samplesPerPixel = 8;
	cfg.seed = 42;
	cfg.outputPrefix = ( tmp.dir / "cal" ).string();

	HyperspectralRadiometricRasterizer* pRast = new HyperspectralRadiometricRasterizer( pCaster, cfg );
	pRast->RasterizeScene( *pScene, 0, 0 );
	Check( pRast->LastRenderCompleted(), "Calibrated sweep completed: " + pRast->LastError() );
	Check( pRast->LastSnapshots().size() == 3, "3 wavelength snapshots produced" );

	// Center pixel (2, 2): L_lambda == (rho / pi) * E_lambda (no environment, single direct bounce)
	for( std::size_t k = 0; k < 3 && k < pRast->LastSnapshots().size(); ++k ) {
		const RadiometricSnapshot& snap = pRast->LastSnapshots()[k];
		const RadiometricPixel& px = snap.pixels[2 * 4 + 2];
		const double expectedL = ( rho[k] / LocalResponsePi ) * E[k];
		RelNear( px.mean, expectedL, 1e-4,
			"Calibrated L_lambda at " + std::to_string( int( wls[k] ) ) + " nm == (rho/pi)*E" );
	}

	safe_release( pRast );
	safe_release( pCaster );
	safe_release( pDefShader );
	safe_release( pJobI );
}

} // namespace

int main()
{
	TestPreflightRefusals();
	TestCalibratedPhysicalDirectionalRadiance();

	if( gFailures == 0 ) {
		std::cout << "HyLIoSSourceAndFilmTest: ALL PASSED\n";
		return 0;
	}
	std::cerr << "HyLIoSSourceAndFilmTest: " << gFailures << " FAILURES\n";
	return 1;
}
