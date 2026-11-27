//////////////////////////////////////////////////////////////////////
//
//  HyperspectralOutputTest.cpp - P6b end-to-end output, manifest,
//    checkpoint resume bit-exactness, and CST scene-chunk tests for
//    HyperspectralRadiometricRasterizer.
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "../src/Library/RISE_API.h"
#include "../src/Library/Job.h"
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/Materials/HyLIoS/HyLIoSSkinMaterial.h"
#include "../src/Library/Materials/HyLIoS/HyLIoSLocalResponseAsset.h"
#include "../src/Library/Rendering/HyperspectralRadiometricRasterizer.h"
#include "../src/Library/Rendering/RayCaster.h"
#include "../src/Library/Rendering/RadianceMap.h"
#include "../src/Library/Rendering/HosekWilkieSpectralRadianceMap.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/BlackBodyPainter.h"
#include "../src/Library/Shaders/StandardShader.h"
#include "../src/Library/Interfaces/IProgressCallback.h"
#include "../src/Library/Utilities/RISECBOR64.h"
#include "../src/Library/Agent/Json.h"

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

struct TempDirGuard
{
	std::filesystem::path dir;
	explicit TempDirGuard( const char* tag )
	{
		dir = std::filesystem::temp_directory_path() / ( std::string( "rise_hylios_out_" ) + tag + "_" + std::to_string( std::rand() ) );
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

std::string WriteFixtureAsset(
	const std::filesystem::path& path,
	const std::vector<double>& wavelengths,
	LocalAssetSemanticIdentity& outSem
	)
{
	const LocalAssetRecord rec = EncodeMap( 1.0, "output_fixture_v1" );
	outSem = { rec, rec, rec, rec, rec, rec, rec, rec, rec, wavelengths.front(), wavelengths.back() };

	LocalAssetDefinition def;
	def.semantic = outSem;
	def.production = {
		"analytic_oracle_v1", "hylios_angular_lineage_v1", "analog_v1",
		"index_mapping_v1", "analytic_stats_v1",
		19ULL, 16ULL, 2ULL, 32ULL, 512ULL, rec, rec
	};
	def.response.incident = { { 0.0, 0.5, 1.0 }, { 0.0, LocalResponsePi, LocalResponseTwoPi } };
	def.response.outgoing = { { 0.0, 0.5, 1.0 }, { 0.0, LocalResponsePi, LocalResponseTwoPi } };
	def.response.wavelengthNm = wavelengths;

	const std::size_t nIn = 4, nOut = 4, nWl = wavelengths.size();
	def.response.continuous.assign( nIn * nOut * nWl, 0.0 );
	def.response.mirror.assign( nIn * nWl, 0.0 );
	for( std::size_t k = 0; k < nWl; ++k ) {
		const double r = 0.2 + 0.2 * static_cast<double>( k );
		for( std::size_t wi = 0; wi < nIn; ++wi ) {
			def.response.mirror[k * nIn + wi] = 0.04;
			for( std::size_t wo = 0; wo < nOut; ++wo ) {
				def.response.continuous[( k * nIn + wi ) * nOut + wo] = r / LocalResponsePi;
			}
		}
	}
	def.normalIncidenceGuide.assign( nWl, 0.4 );

	SaveLocalResponseAsset( path.string(), def );
	return path.string();
}

class CancelAfterCallsProgress : public IProgressCallback
{
	int maxCalls;
	int calls;
public:
	explicit CancelAfterCallsProgress( int maxC ) : maxCalls( maxC ), calls( 0 ) {}
	bool Progress( const double, const double ) override { return ( ++calls <= maxCalls ); }
	void SetTitle( const char* ) override {}
};

void TestEXRManifestAndCheckpointResume()
{
	TempDirGuard tmp( "exr_ckpt" );
	const std::vector<double> wls = { 450.0, 550.0, 650.0 };
	const std::vector<double> E   = { 12.0,  18.0,  24.0  };

	LocalAssetSemanticIdentity sem;
	const std::string assetPath = WriteFixtureAsset( tmp.dir / "skin.hylios", wls, sem );

	IJob* pJobI = 0;
	RISE_CreateJob( &pJobI );
	Job* pJob = dynamic_cast<Job*>( pJobI );
	const double loc[3] = { 0, 0, 2 }, look[3] = { 0, 0, 0 }, up[3] = { 0, 1, 0 };
	const double oriZero[3] = { 0, 0, 0 }, targetZero[2] = { 0, 0 };
	pJob->SetFilm( 4, 4, 1.0 );
	pJob->AddPinholeCamera( "cam", loc, look, up, 0.6, 0.0, 0.0, 0.0, oriZero, targetZero );
	pJob->AddBoxGeometry( "slab", 20.0, 20.0, 0.2 );

	HyLIoSSkinMaterialConfig matCfg;
	matCfg.responseAssetPath = assetPath.c_str();
	matCfg.useStandaloneAssetCodec = true;
	matCfg.explicitSemanticIdentity = &sem;
	pJob->AddHyLIoSSkinMaterial( "skin", matCfg );
	const double pos[3] = { 0, 0, -0.1 }, sca[3] = { 1, 1, 1 };
	pJob->AddObject( "skin_obj", "slab", "skin", 0, 0, RadianceMapConfig(), pos, oriZero, sca, true, true );
	const double dir[3] = { 0, 0, 1 };
	Check( pJob->AddPhysicalDirectionalLight( "sun", 3, wls.data(), E.data(), "zero", 1.0, dir ),
		"AddPhysicalDirectionalLight succeeds" );

	IScenePriv* pScene = pJob->GetScene();
	std::vector<IShaderOp*> emptyOps;
	StandardShader* pDefShader = new StandardShader( emptyOps );
	RayCaster* pCaster = new RayCaster( true, 8, *pDefShader, true );

	// 1. Uninterrupted reference sweep
	HyperspectralRadiometricConfig refCfg;
	refCfg.wavelengthsNm = wls;
	refCfg.samplesPerPixel = 6;
	refCfg.seed = 99;
	refCfg.outputPrefix = ( tmp.dir / "ref" ).string();

	HyperspectralRadiometricRasterizer* pRef = new HyperspectralRadiometricRasterizer( pCaster, refCfg );
	pRef->RasterizeScene( *pScene, 0, 0 );
	Check( pRef->LastRenderCompleted(), "Reference sweep completed: " + pRef->LastError() );
	Check( pRef->LastSnapshots().size() == 3, "Reference has 3 snapshots" );

	// Verify EXR files and JSON manifest exist and parse cleanly
	Check( std::filesystem::exists( tmp.dir / "ref_450nm.exr" ), "ref_450nm.exr written" );
	Check( std::filesystem::exists( tmp.dir / "ref_550nm.exr" ), "ref_550nm.exr written" );
	Check( std::filesystem::exists( tmp.dir / "ref_650nm.exr" ), "ref_650nm.exr written" );
	Check( std::filesystem::exists( pRef->LastManifestPath() ), "JSON manifest written" );
	{
		std::ifstream ifs( pRef->LastManifestPath() );
		const std::string text( ( std::istreambuf_iterator<char>( ifs ) ), std::istreambuf_iterator<char>() );
		RISE::Agent::JsonValue j;
		std::string jerr;
		Check( RISE::Agent::JsonParse( text, j, jerr ), "Manifest parses as valid JSON: " + jerr );
		Check( j.get( "schema_version" ).asNumber() == 1.0, "Manifest schema_version == 1" );
		Check( j.get( "units" ).asString() == "W m^-2 sr^-1 nm^-1", "Manifest physical units" );
		Check( j.get( "bands" ).size() == 3, "Manifest lists 3 bands" );
	}

	// 2. Cancelled sweep with checkpoint, then resumed sweep -> bit-exact pixel means!
	HyperspectralRadiometricConfig ckptCfg = refCfg;
	ckptCfg.outputPrefix = ( tmp.dir / "resumed" ).string();
	ckptCfg.checkpointPrefix = ( tmp.dir / "ckpt" ).string();

	{
		HyperspectralRadiometricRasterizer* pPart = new HyperspectralRadiometricRasterizer( pCaster, ckptCfg );
		CancelAfterCallsProgress cancelProg( 2 ); // cancel after 2 rows of first wavelength
		pPart->SetProgressCallback( &cancelProg );
		pPart->RasterizeScene( *pScene, 0, 0 );
		Check( !pPart->LastRenderCompleted(), "Cancelled sweep does not report complete" );
		Check( std::filesystem::exists( tmp.dir / "ckpt_450nm.rcp" ), "Checkpoint saved on cancel" );
		Check( !std::filesystem::exists( tmp.dir / "resumed_450nm.exr" ), "No partial EXR written on cancel" );
		safe_release( pPart );
	}

	{
		HyperspectralRadiometricRasterizer* pRes = new HyperspectralRadiometricRasterizer( pCaster, ckptCfg );
		pRes->RasterizeScene( *pScene, 0, 0 );
		Check( pRes->LastRenderCompleted(), "Resumed sweep completed: " + pRes->LastError() );
		Check( pRes->LastSnapshots().size() == 3, "Resumed sweep has 3 snapshots" );

		for( std::size_t k = 0; k < 3; ++k ) {
			const auto& refPx = pRef->LastSnapshots()[k].pixels;
			const auto& resPx = pRes->LastSnapshots()[k].pixels;
			Check( refPx.size() == resPx.size(), "Snapshot pixel count matches" );
			for( std::size_t i = 0; i < refPx.size(); ++i ) {
				Check( refPx[i].count == resPx[i].count && refPx[i].mean == resPx[i].mean,
					"Resumed pixel bit-exact match at band " + std::to_string( k ) + " px " + std::to_string( i ) );
			}
		}
		safe_release( pRes );
	}

	safe_release( pRef );
	safe_release( pCaster );
	safe_release( pDefShader );
	safe_release( pJobI );
}

void TestCSTChunkParseAndIncrementalFallback()
{
	TempDirGuard tmp( "cst" );
	const std::string sceneText =
		"RISE ASCII SCENE 7\n"
		"hyperspectral_radiometric_rasterizer\n"
		"{\n"
		"  wavelength_nm 450\n"
		"  wavelength_nm 550\n"
		"  wavelength_nm 650\n"
		"  samples_per_pixel 8\n"
		"  seed 7\n"
		"  output_prefix " + ( tmp.dir / "cst_out" ).string() + "\n"
		"}\n";

	const Cst::Document doc = Cst::ParseToCst( sceneText );
	Check( Cst::DocItemCount( doc ) > 0, "ParseToCst hyperspectral_radiometric_rasterizer" );

	IJob* pJobI = 0;
	RISE_CreateJob( &pJobI );
	Job* pJob = dynamic_cast<Job*>( pJobI );
	std::vector<std::string> diags;
	Check( Cst::DeriveToJob( doc, *pJobI, &diags ), "DeriveToJob hyperspectral_radiometric_rasterizer" );

	const HyperspectralRadiometricRasterizer* pRast =
		dynamic_cast<const HyperspectralRadiometricRasterizer*>( pJob->GetRasterizer() );
	Check( pRast != 0, "Installed rasterizer is HyperspectralRadiometricRasterizer" );
	if( pRast ) {
		Check( pRast->GetConfig().wavelengthsNm.size() == 3, "3 explicit wavelength_nm parsed" );
		Check( pRast->GetConfig().samplesPerPixel == 8, "samples_per_pixel == 8" );
		Check( pRast->GetConfig().seed == 7, "seed == 7" );
	}

	std::vector<Cst::NodeId> chunkIds;
	for( int i = 0; i < Cst::DocItemCount( doc ); ++i ) {
		const Cst::NodeId nid = Cst::DocNodeIdAt( doc, i );
		const Cst::NodeRef n = Cst::DocResolveNodeId( doc, nid );
		if( n && n->kind == Cst::NodeKind::Chunk && n->role == "hyperspectral_radiometric_rasterizer" ) {
			chunkIds.push_back( nid );
		}
	}
	Check( !chunkIds.empty(), "Found CST node for hyperspectral_radiometric_rasterizer" );
	std::vector<std::string> incDiags;
	Check( Cst::DeriveToJobIncremental( doc, *pJobI, chunkIds, &incDiags ) == 0,
		"DeriveToJobIncremental cleanly falls back for hyperspectral_radiometric_rasterizer" );

	safe_release( pJobI );
}

void TestStrictSkyPreflightAndObservables()
{
	TempDirGuard tmp( "sky_obs" );
	const std::vector<double> wls = { 450.0, 550.0, 650.0 };
	const std::vector<double> E   = { 10.0,  15.0,  20.0  };

	LocalAssetSemanticIdentity sem;
	const std::string assetPath = WriteFixtureAsset( tmp.dir / "skin.hylios", wls, sem );

	IJob* pJobI = 0;
	RISE_CreateJob( &pJobI );
	Job* pJob = dynamic_cast<Job*>( pJobI );
	const double loc[3] = { 0, 0, 2 }, look[3] = { 0, 0, 0 }, up[3] = { 0, 1, 0 };
	const double oriZero[3] = { 0, 0, 0 }, targetZero[2] = { 0, 0 };
	pJob->SetFilm( 2, 2, 1.0 );
	pJob->AddPinholeCamera( "cam", loc, look, up, 0.6, 0.0, 0.0, 0.0, oriZero, targetZero );
	pJob->AddBoxGeometry( "slab", 20.0, 20.0, 0.2 );
	HyLIoSSkinMaterialConfig matCfg;
	matCfg.responseAssetPath = assetPath.c_str();
	matCfg.useStandaloneAssetCodec = true;
	matCfg.explicitSemanticIdentity = &sem;
	pJob->AddHyLIoSSkinMaterial( "skin", matCfg );
	const double pos[3] = { 0, 0, -0.1 }, sca[3] = { 1, 1, 1 };
	pJob->AddObject( "skin_obj", "slab", "skin", 0, 0, RadianceMapConfig(), pos, oriZero, sca, true, true );
	const double dir[3] = { 0, 0, 1 };
	pJob->AddPhysicalDirectionalLight( "sun", 3, wls.data(), E.data(), "zero", 1.0, dir );

	IScenePriv* pScene = pJob->GetScene();
	std::vector<IShaderOp*> emptyOps;
	StandardShader* pDefShader = new StandardShader( emptyOps );
	RayCaster* pCaster = new RayCaster( true, 8, *pDefShader, true );

	// 1. Strict environment / sky preflight
	{
		UniformColorPainter* pRgbP = new UniformColorPainter( RISEPel( 1, 1, 1 ) );
		RadianceMap* pRgbEnv = new RadianceMap( *pRgbP, 1.0 );
		pJob->SetGlobalRadianceMap( pRgbEnv );

		HyperspectralRadiometricConfig cfg;
		cfg.wavelengthsNm = wls;
		cfg.samplesPerPixel = 2;
		cfg.outputPrefix = ( tmp.dir / "rgb_env" ).string();
		cfg.allowRGBUpliftSources = false;
		HyperspectralRadiometricRasterizer* pR = new HyperspectralRadiometricRasterizer( pCaster, cfg );
		std::vector<double> rw;
		std::string err;
		Check( !pR->ValidateSceneAndConfig( *pScene, rw, err ) && !err.empty(),
			"Strict mode refuses RGB-uplifted RadianceMap" );
		safe_release( pR );

		cfg.allowRGBUpliftSources = true;
		pR = new HyperspectralRadiometricRasterizer( pCaster, cfg );
		Check( pR->ValidateSceneAndConfig( *pScene, rw, err ),
			"allow_rgb_uplift_sources=true accepts RGB-uplifted RadianceMap" );
		safe_release( pR );

		HosekWilkieSpectralRadianceMap* pSky = new HosekWilkieSpectralRadianceMap( 0.6, 0.0, 2.5, RISEPel( 0.2, 0.2, 0.2 ), 1.0 );
		pJob->SetGlobalRadianceMap( pSky );
		cfg.allowRGBUpliftSources = false;
		cfg.wavelengthsNm = { 450.0, 550.0, 650.0 };
		pR = new HyperspectralRadiometricRasterizer( pCaster, cfg );
		Check( pR->ValidateSceneAndConfig( *pScene, rw, err ),
			"Strict mode accepts Hosek-Wilkie sky within [380, 780] nm" );
		safe_release( pR );

		// Hosek-Wilkie outside [380, 780] nm (e.g. 900 nm) must be refused
		// (use a scene without HyLIoS band cap or just verify refusal message)
		IJob* pSkyJobI = 0;
		RISE_CreateJob( &pSkyJobI );
		Job* pSkyJob = dynamic_cast<Job*>( pSkyJobI );
		pSkyJob->SetFilm( 2, 2, 1.0 );
		pSkyJob->AddPinholeCamera( "cam", loc, look, up, 0.6, 0.0, 0.0, 0.0, oriZero, targetZero );
		pSkyJob->SetGlobalRadianceMap( pSky );
		cfg.wavelengthsNm = { 450.0, 900.0 };
		pR = new HyperspectralRadiometricRasterizer( pCaster, cfg );
		Check( !pR->ValidateSceneAndConfig( *pSkyJob->GetScene(), rw, err ) &&
			err.find( "Hosek-Wilkie" ) != std::string::npos,
			"Strict mode refuses Hosek-Wilkie sky outside [380, 780] nm" );
		safe_release( pR );
		safe_release( pSkyJobI );

		// BlackBodyPainter-backed RadianceMap is physical and accepted in strict mode when unnormalized,
		// and refused when peak-normalized (dimensionless).
		BlackBodyPainter* pBB = new BlackBodyPainter( 5800.0, 400.0, 700.0, 64, false, 1.0 );
		RadianceMap* pBBEnv = new RadianceMap( *pBB, 1.0 );
		pJob->SetGlobalRadianceMap( pBBEnv );
		cfg.wavelengthsNm = wls;
		pR = new HyperspectralRadiometricRasterizer( pCaster, cfg );
		Check( pR->ValidateSceneAndConfig( *pScene, rw, err ),
			"Strict mode accepts unnormalized BlackBodyPainter-backed RadianceMap" );
		safe_release( pR );

		BlackBodyPainter* pBBNorm = new BlackBodyPainter( 5800.0, 400.0, 700.0, 64, true, 1.0 );
		RadianceMap* pBBNormEnv = new RadianceMap( *pBBNorm, 1.0 );
		pJob->SetGlobalRadianceMap( pBBNormEnv );
		pR = new HyperspectralRadiometricRasterizer( pCaster, cfg );
		Check( !pR->ValidateSceneAndConfig( *pScene, rw, err ) && !err.empty(),
			"Strict mode refuses peak-normalized BlackBodyPainter-backed RadianceMap" );
		safe_release( pR );

		pScene->SetGlobalRadianceMap( 0 );
		safe_release( pBBNormEnv );
		safe_release( pBBNorm );
		safe_release( pBBEnv );
		safe_release( pBB );
		safe_release( pSky );
		safe_release( pRgbEnv );
		safe_release( pRgbP );
	}

	// 2. Derived observables: band_integrated, band_average, sensor_energy, sensor_photon
	{
		HyperspectralRadiometricConfig badObs;
		badObs.wavelengthsNm = wls;
		badObs.samplesPerPixel = 2;
		badObs.outputPrefix = ( tmp.dir / "bad_obs" ).string();
		badObs.outputObservable = "sensor_energy"; // missing sensorResponse
		HyperspectralRadiometricRasterizer* pBad = new HyperspectralRadiometricRasterizer( pCaster, badObs );
		std::vector<double> rw;
		std::string err;
		Check( !pBad->ValidateSceneAndConfig( *pScene, rw, err ),
			"Preflight refuses sensor_energy without sensor_response" );
		safe_release( pBad );

		// band_integrated
		HyperspectralRadiometricConfig biCfg;
		biCfg.wavelengthsNm = wls;
		biCfg.samplesPerPixel = 4;
		biCfg.seed = 42;
		biCfg.outputPrefix = ( tmp.dir / "bi" ).string();
		biCfg.outputObservable = "band_integrated";
		HyperspectralRadiometricRasterizer* pBI = new HyperspectralRadiometricRasterizer( pCaster, biCfg );
		pBI->RasterizeScene( *pScene, 0, 0 );
		Check( pBI->LastRenderCompleted(), "band_integrated render completed: " + pBI->LastError() );
		Check( std::filesystem::exists( pBI->LastDerivedObservablePath() ), "band_integrated EXR written" );
		Check( pBI->LastDerivedObservable().size() == 4, "band_integrated has 4 pixels" );
		const double l0 = pBI->LastSnapshots()[0].pixels[0].mean;
		const double l1 = pBI->LastSnapshots()[1].pixels[0].mean;
		const double l2 = pBI->LastSnapshots()[2].pixels[0].mean;
		const double expectedBI = 0.5 * ( l0 + l1 ) * 100.0 + 0.5 * ( l1 + l2 ) * 100.0;
		Check( std::fabs( pBI->LastDerivedObservable()[0] - expectedBI ) <= 1e-12 * std::max( 1.0, expectedBI ),
			"band_integrated matches exact composite trapezoidal quadrature" );

		// band_average
		HyperspectralRadiometricConfig baCfg = biCfg;
		baCfg.outputPrefix = ( tmp.dir / "ba" ).string();
		baCfg.outputObservable = "band_average";
		HyperspectralRadiometricRasterizer* pBA = new HyperspectralRadiometricRasterizer( pCaster, baCfg );
		pBA->RasterizeScene( *pScene, 0, 0 );
		Check( pBA->LastRenderCompleted(), "band_average render completed" );
		Check( std::fabs( pBA->LastDerivedObservable()[0] - expectedBI / 200.0 ) <= 1e-12 * std::max( 1.0, expectedBI ),
			"band_average equals band_integrated / bandSpan" );

		// sensor_energy vs sensor_photon (never CIE Y normalized!)
		const std::vector<std::pair<double, double>> sr = { { 450.0, 0.5 }, { 550.0, 1.0 }, { 650.0, 0.8 } };
		HyperspectralRadiometricConfig seCfg = biCfg;
		seCfg.outputPrefix = ( tmp.dir / "se" ).string();
		seCfg.outputObservable = "sensor_energy";
		seCfg.sensorResponse = sr;
		HyperspectralRadiometricRasterizer* pSE = new HyperspectralRadiometricRasterizer( pCaster, seCfg );
		pSE->RasterizeScene( *pScene, 0, 0 );
		Check( pSE->LastRenderCompleted(), "sensor_energy render completed" );
		const double expectedSE = 0.5 * ( l0 * 0.5 + l1 * 1.0 ) * 100.0 + 0.5 * ( l1 * 1.0 + l2 * 0.8 ) * 100.0;
		Check( std::fabs( pSE->LastDerivedObservable()[0] - expectedSE ) <= 1e-12 * std::max( 1.0, expectedSE ),
			"sensor_energy matches exact sensor-weighted trapezoidal integral" );

		HyperspectralRadiometricConfig spCfg = seCfg;
		spCfg.outputPrefix = ( tmp.dir / "sp" ).string();
		spCfg.outputObservable = "sensor_photon";
		HyperspectralRadiometricRasterizer* pSP = new HyperspectralRadiometricRasterizer( pCaster, spCfg );
		pSP->RasterizeScene( *pScene, 0, 0 );
		Check( pSP->LastRenderCompleted(), "sensor_photon render completed" );
		const double hc = 6.62607015e-34 * 2.99792458e8;
		const double p0 = l0 * ( 450e-9 / hc ) * 0.5;
		const double p1 = l1 * ( 550e-9 / hc ) * 1.0;
		const double p2 = l2 * ( 650e-9 / hc ) * 0.8;
		const double expectedSP = 0.5 * ( p0 + p1 ) * 100.0 + 0.5 * ( p1 + p2 ) * 100.0;
		Check( std::fabs( pSP->LastDerivedObservable()[0] - expectedSP ) <= 1e-12 * std::max( 1.0, expectedSP ),
			"sensor_photon matches exact lambda/(h*c) photon-weighted trapezoidal integral" );

		// Verify manifest records derived_observable units and h*c
		{
			std::ifstream ifs( pSP->LastManifestPath() );
			const std::string text( ( std::istreambuf_iterator<char>( ifs ) ), std::istreambuf_iterator<char>() );
			RISE::Agent::JsonValue j;
			std::string jerr;
			Check( RISE::Agent::JsonParse( text, j, jerr ), "sensor_photon manifest parses cleanly" );
			Check( j.get( "output_observable" ).asString() == "sensor_photon", "Manifest output_observable == sensor_photon" );
			const auto& drv = j.get( "derived_observable" );
			Check( drv.isObject() && drv.get( "units" ).asString() == "photons s^-1 m^-2 sr^-1 (sensor-weighted)",
				"Manifest records exact photon-weighted physical units" );
			Check( drv.get( "photon_conversion_hc_J_m" ).asNumber() > 0.0,
				"Manifest records SI h*c conversion constant" );
		}

		safe_release( pSP );
		safe_release( pSE );
		safe_release( pBA );
		safe_release( pBI );
	}

	// 3. CST parse of output_observable + repeatable sensor_response
	{
		const std::string sceneText =
			"RISE ASCII SCENE 7\n"
			"hyperspectral_radiometric_rasterizer\n"
			"{\n"
			"  wavelength_nm 450\n"
			"  wavelength_nm 650\n"
			"  samples_per_pixel 4\n"
			"  output_observable sensor_photon\n"
			"  sensor_response 450 0.5\n"
			"  sensor_response 650 1.0\n"
			"  output_prefix " + ( tmp.dir / "cst_sp" ).string() + "\n"
			"}\n";
		const Cst::Document doc = Cst::ParseToCst( sceneText );
		IJob* pCstJobI = 0;
		RISE_CreateJob( &pCstJobI );
		Job* pCstJob = dynamic_cast<Job*>( pCstJobI );
		std::vector<std::string> diags;
		Check( Cst::DeriveToJob( doc, *pCstJobI, &diags ), "CST parse sensor_photon + sensor_response" );
		const HyperspectralRadiometricRasterizer* pRast =
			dynamic_cast<const HyperspectralRadiometricRasterizer*>( pCstJob->GetRasterizer() );
		Check( pRast && pRast->GetConfig().outputObservable == "sensor_photon" &&
			pRast->GetConfig().sensorResponse.size() == 2,
			"Parsed output_observable and 2 sensor_response points" );
		safe_release( pCstJobI );
	}

	safe_release( pCaster );
	safe_release( pDefShader );
	safe_release( pJobI );
}

} // namespace

int main()
{
	TestEXRManifestAndCheckpointResume();
	TestCSTChunkParseAndIncrementalFallback();
	TestStrictSkyPreflightAndObservables();

	if( gFailures == 0 ) {
		std::cout << "HyperspectralOutputTest: ALL PASSED\n";
		return 0;
	}
	std::cerr << "HyperspectralOutputTest: " << gFailures << " FAILURES\n";
	return 1;
}
