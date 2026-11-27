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

} // namespace

int main()
{
	TestEXRManifestAndCheckpointResume();
	TestCSTChunkParseAndIncrementalFallback();

	if( gFailures == 0 ) {
		std::cout << "HyperspectralOutputTest: ALL PASSED\n";
		return 0;
	}
	std::cerr << "HyperspectralOutputTest: " << gFailures << " FAILURES\n";
	return 1;
}
