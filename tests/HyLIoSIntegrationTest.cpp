//////////////////////////////////////////////////////////////////////
//
//  HyLIoSIntegrationTest.cpp - P5 integration tests for HyLIoSSkinMaterial:
//    1. Analytic Lambertian + mirror standalone asset: BSDF::valueNM,
//       SPF::ScatterNM, SPF::PdfNM agreement, horizon & band gating,
//       mirrored-transform chirality invariance.
//    2. Immutable asset cache sharing, release on material destruction,
//       and aggregate memory budget enforcement.
//    3. Shared D65 visible guide equality across ComputeHyLIoSVisibleGuideRGB,
//       BSDF::albedo, EvaluateMaterialAOVGuides, and MixMaterial.
//    4. Capability census (ObjectManager), AutoRasterizer -> PT routing,
//       BDPT/VCM refusal, and PathTracingSpectralRasterizer band preflight.
//    5. End-to-end baked_local scene chunk parse (`hylios_skin_material`),
//       semantic mismatch rejection, CST round-trip, and incremental fallback.
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "../src/Library/RISE_API.h"
#include "../src/Library/Job.h"
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/Materials/HyLIoS/HyLIoSSkinMaterial.h"
#include "../src/Library/Materials/HyLIoS/HyLIoSLocalBake.h"
#include "../src/Library/Materials/HyLIoS/HyLIoSLocalResponseAsset.h"
#include "../src/Library/Materials/MixMaterial.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Managers/ObjectManager.h"
#include "../src/Library/Rendering/AutoRasterizer.h"
#include "../src/Library/Rendering/BDPTPelRasterizer.h"
#include "../src/Library/Rendering/VCMPelRasterizer.h"
#include "../src/Library/Rendering/PathTracingSpectralRasterizer.h"
#include "../src/Library/Rendering/RayCaster.h"
#include "../src/Library/Shaders/StandardShader.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/IORStack.h"
#include "../src/Library/Utilities/RISECBOR64.h"
#include "../src/Library/SceneEditor/MaterialIntrospection.h"
#include "CstRenderEquivalence.h"

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

void Near( const double actual, const double expected, const double tol, const std::string& msg )
{
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
		dir = std::filesystem::temp_directory_path() / ( std::string( "rise_hylios_int_" ) + tag + "_" + std::to_string( std::rand() ) );
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

LocalAssetSemanticIdentity MakeAnalyticSemantic(
	const std::vector<double>& wavelengths,
	const double exteriorIOR = 1.0
	)
{
	const LocalAssetRecord base = EncodeMap( 1.0, "analytic_fixture_v1" );
	const LocalAssetRecord ext = EncodeMap( exteriorIOR, "exterior_ior_v1" );
	LocalAssetSemanticIdentity s;
	s.model = base;
	s.placement = base;
	s.postscatter = base;
	s.boundary = base;
	s.lineage = base;
	s.data = base;
	s.specimen = base;
	s.exterior = ext;
	s.approximation = base;
	s.coveredFirstNm = wavelengths.front();
	s.coveredLastNm = wavelengths.back();
	return s;
}

std::string WriteAnalyticAsset(
	const std::filesystem::path& path,
	const std::vector<double>& wavelengths,
	const std::vector<double>& diffusePerWl,
	const std::vector<double>& mirrorPerWl,
	const LocalAssetSemanticIdentity& semantic
	)
{
	const LocalAssetRecord rec = EncodeMap( 1.0, "analytic_prod_v1" );
	LocalAssetDefinition def;
	def.semantic = semantic;
	def.production = {
		"analytic_oracle_v1", "hylios_angular_lineage_v1", "analog_v1",
		"index_mapping_v1", "analytic_stats_v1",
		42ULL, 16ULL, 2ULL, 32ULL, 512ULL, rec, rec
	};
	def.response.incident = { { 0.0, 0.5, 1.0 }, { 0.0, LocalResponsePi, LocalResponseTwoPi } };
	def.response.outgoing = { { 0.0, 0.5, 1.0 }, { 0.0, LocalResponsePi, LocalResponseTwoPi } };
	def.response.wavelengthNm = wavelengths;

	const std::size_t nIn = 4;
	const std::size_t nOut = 4;
	const std::size_t nWl = wavelengths.size();
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

void TestAnalyticBSDFSPFAndChirality()
{
	TempDirGuard tmp( "bsdf_spf" );
	const std::vector<double> wls = { 380.0, 500.0, 620.0, 780.0 };
	const std::vector<double> diff = { 0.30, 0.40, 0.50, 0.45 };
	const std::vector<double> mirr = { 0.04, 0.04, 0.04, 0.04 };
	const LocalAssetSemanticIdentity sem = MakeAnalyticSemantic( wls );
	const std::string assetPath = WriteAnalyticAsset( tmp.dir / "analytic.hylios", wls, diff, mirr, sem );

	HyLIoSSkinMaterialConfig cfg;
	cfg.responseAssetPath = assetPath.c_str();
	cfg.useStandaloneAssetCodec = true;
	cfg.explicitSemanticIdentity = &sem;

	IMaterial* pMat = 0;
	std::string err;
	Check( RISE_API_CreateHyLIoSSkinMaterial( &pMat, cfg, &err ) && pMat != 0,
		"CreateHyLIoSSkinMaterial analytic succeeded: " + err );
	if( !pMat ) return;

	Check( pMat->IsHyperspectralSkin(), "IsHyperspectralSkin() == true" );
	Check( pMat->GetPathDependence() == 0u, "GetPathDependence() == 0 (no pollution)" );
	Check( pMat->CoversWavelengthRange( 400.0, 700.0 ), "CoversWavelengthRange [400,700]" );
	Check( !pMat->CoversWavelengthRange( 300.0, 700.0 ), "Rejects uncovered [300,700]" );
	Check( !pMat->CoversWavelengthRange( 400.0, 900.0 ), "Rejects uncovered [400,900]" );

	const IBSDF* pBSDF = pMat->GetBSDF();
	const ISPF* pSPF = pMat->GetSPF();
	Check( pBSDF != 0 && pSPF != 0, "BSDF and SPF non-null" );

	const Vector3 woDir = Vector3Ops::Normalize( Vector3( 0.3, 0.2, 0.9 ) );
	const Vector3 wiDir = Vector3Ops::Normalize( Vector3( -0.4, 0.1, 0.85 ) );
	RayIntersectionGeometric ri( Ray( Point3( 0, 0, 1 ), -woDir ), nullRasterizerState );
	ri.ptIntersection = Point3( 0, 0, 0 );
	ri.vNormal = Vector3( 0, 0, 1 );
	ri.vGeomNormal = Vector3( 0, 0, 1 );
	ri.onb.CreateFromW( ri.vNormal );

	// 1. BSDF evaluation at 500 nm and out-of-band (float32 downward quantization tol 1e-6)
	Near( pBSDF->valueNM( wiDir, ri, 500.0 ), 0.40 * INV_PI, 1e-6, "valueNM at 500 nm == 0.40/pi" );
	Near( pBSDF->valueNM( wiDir, ri, 250.0 ), 0.0, 1e-12, "valueNM out-of-band == 0" );
	Near( pBSDF->valueNM( Vector3( 0, 0, -1 ), ri, 500.0 ), 0.0, 1e-12, "valueNM below horizon == 0" );

	// 2. Chirality invariance under mirrored ONB (FlipV)
	RayIntersectionGeometric riMirrored = ri;
	riMirrored.onb.FlipV();
	Near( pBSDF->valueNM( wiDir, riMirrored, 500.0 ), pBSDF->valueNM( wiDir, ri, 500.0 ), 1e-12,
		"BSDF chirality invariant under FlipV" );
	const IORStack iorStack( 1.0 );
	Near( pSPF->PdfNM( riMirrored, wiDir, 500.0, iorStack ), pSPF->PdfNM( ri, wiDir, 500.0, iorStack ), 1e-12,
		"SPF PdfNM chirality invariant under FlipV" );

	// 3. ScatterNM <-> valueNM / PdfNM consistency over 200 draws
	RandomNumberGenerator rng( 12345u );
	IndependentSampler sampler( rng );
	int nDiffuse = 0;
	int nMirror = 0;
	for( int s = 0; s < 200; ++s ) {
		ScatteredRayContainer sc;
		pSPF->ScatterNM( ri, sampler, 500.0, sc, iorStack );
		if( sc.Count() == 0 ) continue;
		const ScatteredRay& sr = sc[0];
		if( sr.isDelta ) {
			++nMirror;
			Near( sr.krayNM, 2.0 * 0.04, 1e-6, "Mirror weight == 2 * d (p_event = 0.5)" );
			Near( sr.ray.Dir().x, -woDir.x, 1e-6, "Mirror dir.x" );
			Near( sr.ray.Dir().y, -woDir.y, 1e-6, "Mirror dir.y" );
			Near( sr.ray.Dir().z,  woDir.z, 1e-6, "Mirror dir.z" );
		} else {
			++nDiffuse;
			const Scalar pdf = pSPF->PdfNM( ri, sr.ray.Dir(), 500.0, iorStack );
			const Scalar f = pBSDF->valueNM( sr.ray.Dir(), ri, 500.0 );
			const Scalar cosI = Vector3Ops::Dot( sr.ray.Dir(), ri.vNormal );
			Check( pdf > 0 && cosI > 0, "Diffuse sample has positive pdf and cosI" );
			Near( sr.pdf, pdf, 1e-10, "ScatterNM pdf matches PdfNM" );
			Near( sr.krayNM, f * cosI / pdf, 1e-9, "ScatterNM weight == f * cosI / pdf" );
		}
	}
	Check( nDiffuse > 20 && nMirror > 0, "Both diffuse and mirror events sampled" );

	// 4. Semantic mismatch rejected
	const LocalAssetSemanticIdentity badSem = MakeAnalyticSemantic( wls, 1.33 );
	HyLIoSSkinMaterialConfig badCfg = cfg;
	badCfg.exteriorIOR = 1.33;
	badCfg.explicitSemanticIdentity = &badSem;
	IMaterial* pBad = 0;
	std::string badErr;
	Check( !RISE_API_CreateHyLIoSSkinMaterial( &pBad, badCfg, &badErr ) && pBad == 0,
		"Mismatched exterior_ior rejected" );
	Check( !badErr.empty(), "Mismatch populates diagnostic" );

	safe_release( pMat );
}

void TestCacheSharingAndBudgets()
{
	TempDirGuard tmp( "cache" );
	const std::vector<double> wls = { 400.0, 550.0, 700.0 };
	const std::vector<double> diff = { 0.5, 0.5, 0.5 };
	const std::vector<double> mirr = { 0.02, 0.02, 0.02 };
	const LocalAssetSemanticIdentity sem = MakeAnalyticSemantic( wls );
	const std::string assetPath = WriteAnalyticAsset( tmp.dir / "shared.hylios", wls, diff, mirr, sem );

	Check( HyLIoSSkinMaterial::ActiveCachedAssetCount() == 0, "Cache initially empty" );

	HyLIoSSkinMaterialConfig cfg;
	cfg.responseAssetPath = assetPath.c_str();
	cfg.useStandaloneAssetCodec = true;
	cfg.explicitSemanticIdentity = &sem;

	IMaterial* m1 = 0;
	IMaterial* m2 = 0;
	Check( RISE_API_CreateHyLIoSSkinMaterial( &m1, cfg ) && m1 != 0, "Create m1" );
	Check( HyLIoSSkinMaterial::ActiveCachedAssetCount() == 1, "Cache count == 1 after m1" );
	Check( RISE_API_CreateHyLIoSSkinMaterial( &m2, cfg ) && m2 != 0, "Create m2" );
	Check( HyLIoSSkinMaterial::ActiveCachedAssetCount() == 1, "Cache shared (count still 1)" );

	// Tight aggregate cap rejects a distinct asset
	const std::string assetPath2 = WriteAnalyticAsset( tmp.dir / "other.hylios", wls, diff, mirr, sem );
	HyLIoSSkinMaterialConfig tightCfg = cfg;
	tightCfg.responseAssetPath = assetPath2.c_str();
	tightCfg.aggregateResidentCap = 1;
	IMaterial* m3 = 0;
	std::string tightErr;
	Check( !RISE_API_CreateHyLIoSSkinMaterial( &m3, tightCfg, &tightErr ) && m3 == 0,
		"Aggregate resident budget cap enforced" );

	safe_release( m1 );
	Check( HyLIoSSkinMaterial::ActiveCachedAssetCount() == 1, "Cache alive while m2 holds token" );
	safe_release( m2 );
	Check( HyLIoSSkinMaterial::ActiveCachedAssetCount() == 0, "Cache evicted when all materials released" );
}

void TestSharedVisibleGuideEquality()
{
	std::vector<double> wls;
	std::vector<double> refl;
	for( int nm = 380; nm <= 780; nm += 10 ) {
		wls.push_back( static_cast<double>( nm ) );
		refl.push_back( 0.5 );
	}
	RISEPel guide( 0, 0, 0 );
	Check( ComputeHyLIoSVisibleGuideRGB( wls, refl, guide ), "ComputeHyLIoSVisibleGuideRGB succeeds" );
	Near( guide.r, 0.5, 5e-3, "White-normalized flat 0.5 guide R" );
	Near( guide.g, 0.5, 5e-3, "White-normalized flat 0.5 guide G" );
	Near( guide.b, 0.5, 5e-3, "White-normalized flat 0.5 guide B" );

	TempDirGuard tmp( "guide" );
	const std::vector<double> diff = { 0.25, 0.55, 0.35 };
	const std::vector<double> mirr = { 0.04, 0.04, 0.04 };
	const std::vector<double> wls3 = { 420.0, 550.0, 680.0 };
	const LocalAssetSemanticIdentity sem = MakeAnalyticSemantic( wls3 );
	const std::string assetPath = WriteAnalyticAsset( tmp.dir / "guide.hylios", wls3, diff, mirr, sem );

	HyLIoSSkinMaterialConfig cfg;
	cfg.responseAssetPath = assetPath.c_str();
	cfg.useStandaloneAssetCodec = true;
	cfg.explicitSemanticIdentity = &sem;

	IMaterial* pMat = 0;
	Check( RISE_API_CreateHyLIoSSkinMaterial( &pMat, cfg ) && pMat != 0, "Create guide material" );
	if( !pMat ) return;

	RayIntersection ri( Ray( Point3( 0, 0, 1 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
	ri.geometric.vNormal = Vector3( 0, 0, 1 );
	ri.geometric.vGeomNormal = Vector3( 0, 0, 1 );
	ri.pMaterial = pMat;

	const RISEPel bsdfAlbedo = pMat->GetBSDF()->albedo( ri.geometric );
	RISEPel aovAlbedo( 0, 0, 0 );
	Vector3 aovNormal;
	EvaluateMaterialAOVGuides( ri, ri.geometric, false, 0, aovAlbedo, aovNormal );

	Near( aovAlbedo.r, bsdfAlbedo.r, 1e-12, "AOV guide R == BSDF albedo R" );
	Near( aovAlbedo.g, bsdfAlbedo.g, 1e-12, "AOV guide G == BSDF albedo G" );
	Near( aovAlbedo.b, bsdfAlbedo.b, 1e-12, "AOV guide B == BSDF albedo B" );

	UniformScalarPainter* pW = new UniformScalarPainter( 1.0 );
	const IMaterial* children[1] = { pMat };
	const IScalarPainter* weights[1] = { pW };
	const IRayIntersectionModifier* mods[1] = { 0 };
	MixMaterial* pMix = new MixMaterial( children, weights, mods, 1, 0 );

	Check( pMix->IsHyperspectralSkin(), "MixMaterial forwards IsHyperspectralSkin()" );
	Check( pMix->CoversWavelengthRange( 450.0, 650.0 ), "MixMaterial forwards CoversWavelengthRange (covered)" );
	Check( !pMix->CoversWavelengthRange( 300.0, 650.0 ), "MixMaterial forwards CoversWavelengthRange (uncovered)" );

	RayIntersection riMix = ri;
	riMix.pUnresolvedMix = pMix;
	RISEPel mixAlbedo( 0, 0, 0 );
	Vector3 mixNormal;
	EvaluateMaterialAOVGuides( riMix, riMix.geometric, false, 0, mixAlbedo, mixNormal );

	Near( mixAlbedo.r, aovAlbedo.r, 1e-12, "MixMaterial guide R == AOV guide R" );
	Near( mixAlbedo.g, aovAlbedo.g, 1e-12, "MixMaterial guide G == AOV guide G" );
	Near( mixAlbedo.b, aovAlbedo.b, 1e-12, "MixMaterial guide B == AOV guide B" );

	safe_release( pMix );
	safe_release( pW );
	safe_release( pMat );
}

void TestCapabilityCensusAndRasterizerGuards()
{
	TempDirGuard tmp( "guards" );
	const std::vector<double> wls = { 400.0, 550.0, 700.0 };
	const std::vector<double> diff = { 0.4, 0.4, 0.4 };
	const std::vector<double> mirr = { 0.04, 0.04, 0.04 };
	const LocalAssetSemanticIdentity sem = MakeAnalyticSemantic( wls );
	const std::string assetPath = WriteAnalyticAsset( tmp.dir / "guard.hylios", wls, diff, mirr, sem );

	IJob* pJobI = 0;
	Check( RISE_CreateJob( &pJobI ) && pJobI != 0, "Create job for guard tests" );
	Job* pJob = dynamic_cast<Job*>( pJobI );
	Check( pJob != 0, "Downcast Job" );
	if( !pJob ) { safe_release( pJobI ); return; }

	const double loc[3] = { 0, 0, 5 }, look[3] = { 0, 0, 0 }, up[3] = { 0, 1, 0 };
	const double oriZero[3] = { 0, 0, 0 }, targetZero[2] = { 0, 0 };
	pJob->SetFilm( 16, 16, 1.0 );
	pJob->AddPinholeCamera( "cam", loc, look, up, 0.8, 0.0, 0.0, 0.0, oriZero, targetZero );
	pJob->AddSphereGeometry( "sph", 1.0 );

	HyLIoSSkinMaterialConfig cfg;
	cfg.responseAssetPath = assetPath.c_str();
	cfg.useStandaloneAssetCodec = true;
	cfg.explicitSemanticIdentity = &sem;
	Check( pJob->AddHyLIoSSkinMaterial( "skin_mat", cfg ), "Job::AddHyLIoSSkinMaterial succeeds" );
	const double pos[3] = { 0, 0, 0 }, ori[3] = { 0, 0, 0 }, sca[3] = { 1, 1, 1 };
	pJob->AddObject( "skin_obj", "sph", "skin_mat", 0, 0, RadianceMapConfig(), pos, ori, sca, true, true );

	IScenePriv* pScene = pJob->GetScene();
	const ObjectManager* pObjMgr = dynamic_cast<const ObjectManager*>( pScene->GetObjects() );
	Check( pObjMgr != 0, "ObjectManager present" );
	if( pObjMgr ) {
		std::string offender;
		Check( pObjMgr->SceneHasHyLIoSMaterial( &offender ) && offender == "skin_obj",
			"ObjectManager::SceneHasHyLIoSMaterial detects skin_obj" );
		Check( pObjMgr->ComputeScenePathDependence() == 0u,
			"HyLIoS does not set PathDependence flags" );
		Check( pObjMgr->SceneCoversHyLIoSWavelengths( 420.0, 680.0 ),
			"SceneCoversHyLIoSWavelengths [420,680] true" );
		Check( !pObjMgr->SceneCoversHyLIoSWavelengths( 350.0, 700.0, &offender ) && offender == "skin_obj",
			"SceneCoversHyLIoSWavelengths [350,700] false" );
	}

	IMaterial* pRegisteredMat = pJob->GetMaterials()->GetItem( "skin_mat" );
	Check( pRegisteredMat != 0, "skin_mat registered" );
	if( pRegisteredMat ) {
		const String typeName = MaterialIntrospection::GetTypeName( *pRegisteredMat );
		Check( std::string( typeName.c_str() ) == "HyLIoS Skin", "MaterialIntrospection::GetTypeName == HyLIoS Skin" );
	}

	std::vector<IShaderOp*> emptyOps;
	StandardShader* pDefShader = new StandardShader( emptyOps );
	RayCaster* pCaster = new RayCaster( true, 8, *pDefShader, true );

	// AutoRasterizer routes to PT
	{
		AutoRasterizer* pAuto = new AutoRasterizer(
			pCaster, 0, 0, AutoIntegratorChoice::Auto,
			false, OidnQuality::Balanced, OidnDevice::Auto, OidnPrefilter(),
			PathGuidingConfig(), AdaptiveSamplingConfig(), StabilityConfig(),
			false, ProgressiveConfig(), false, false, SpectralConfig(), nullptr );
		pAuto->AttachToScene( pScene );
		pAuto->ResolveForFirePreflight( *pScene );
		Check( pAuto->ResolvedIntegrator() == AutoIntegratorChoice::PT,
			"AutoRasterizer routes HyLIoS scene to PT" );
		safe_release( pAuto );
	}

	// BDPT and VCM refuse HyLIoS scene; PathTracingSpectralRasterizer checks band coverage
	{
		BDPTPelRasterizer* pBDPT = new BDPTPelRasterizer( pCaster, 4, 4, PathGuidingConfig(), AdaptiveSamplingConfig(), StabilityConfig(), false );
		pBDPT->RasterizeScene( *pScene, 0, 0 );
		Check( !pBDPT->LastRenderCompleted(), "BDPT refuses HyLIoS scene" );
		safe_release( pBDPT );

		VCMPelRasterizer* pVCM = new VCMPelRasterizer( pCaster, 4, 4, 0.0, true, true, PathGuidingConfig(), AdaptiveSamplingConfig(), StabilityConfig(), false );
		pVCM->RasterizeScene( *pScene, 0, 0 );
		Check( !pVCM->LastRenderCompleted(), "VCM refuses HyLIoS scene" );
		safe_release( pVCM );

		PathTracingSpectralRasterizer* pPTSpecOk = new PathTracingSpectralRasterizer(
			pCaster, 420.0, 680.0, 64, 1, ManifoldSolverConfig(), AdaptiveSamplingConfig(), StabilityConfig(), false, true );
		Check( pPTSpecOk->ValidateSceneForRasterizer( *pScene ), "PathTracingSpectralRasterizer accepts covered band" );
		safe_release( pPTSpecOk );

		PathTracingSpectralRasterizer* pPTSpecBad = new PathTracingSpectralRasterizer(
			pCaster, 300.0, 700.0, 64, 1, ManifoldSolverConfig(), AdaptiveSamplingConfig(), StabilityConfig(), false, true );
		Check( !pPTSpecBad->ValidateSceneForRasterizer( *pScene ), "PathTracingSpectralRasterizer refuses uncovered band" );
		safe_release( pPTSpecBad );
	}

	safe_release( pCaster );
	safe_release( pDefShader );
	safe_release( pJobI );
}

void TestBakedLocalSceneChunkAndCST( const char* exePath )
{
	TempDirGuard tmp( "cst_bake" );

	// Write a self-contained resolved specimen JSON fixture matching HyLIoSLocalBake's schema.
	const std::string sourceBytes =
		"{\"schema_version\":1,\"approximate\":true,\"composition_revision\":\"synthetic-v1\","
		"\"coverage_profile_id\":\"explicit-synthetic-approximation\","
		"\"spectral_bundle_sha256\":\"untrusted_bundle_label\","
		"\"resolved_specimen_sha256\":\"untrusted_specimen_label\","
		"\"preset_id\":\"synthetic\",\"interpretation\":\"synthetic controls\","
		"\"units\":{\"length\":\"m\",\"coefficient\":\"m^-1\",\"wavelength\":\"nm\",\"mass_concentration\":\"kg/m^3\"},"
		"\"wavelength_nm\":[450,550,650],\"surface_fold_aspect_ratio\":0.75,\"provenance\":{\"retained\":\"yes\"},"
		"\"layers\":["
		"{\"layer_id\":\"stratum_corneum\",\"thickness_m\":0.0001,\"host_absorption_m_inv\":[10,10,10],\"real_refractive_index\":[1.45,1.45,1.45],\"constituent_absorption_m_inv\":{\"synthetic\":[10,10,10]},\"particles\":null},"
		"{\"layer_id\":\"stratum_granulosum\",\"thickness_m\":0.0001,\"host_absorption_m_inv\":[10,10,10],\"real_refractive_index\":[1.4,1.4,1.4],\"constituent_absorption_m_inv\":{\"synthetic\":[10,10,10]},\"particles\":null},"
		"{\"layer_id\":\"stratum_spinosum\",\"thickness_m\":0.0001,\"host_absorption_m_inv\":[10,10,10],\"real_refractive_index\":[1.4,1.4,1.4],\"constituent_absorption_m_inv\":{\"synthetic\":[10,10,10]},\"particles\":null},"
		"{\"layer_id\":\"stratum_basale\",\"thickness_m\":0.0001,\"host_absorption_m_inv\":[10,10,10],\"real_refractive_index\":[1.4,1.4,1.4],\"constituent_absorption_m_inv\":{\"synthetic\":[10,10,10]},\"particles\":null},"
		"{\"layer_id\":\"papillary_dermis\",\"thickness_m\":0.0002,\"host_absorption_m_inv\":[10,10,10],\"real_refractive_index\":[1.38,1.38,1.38],\"constituent_absorption_m_inv\":{\"synthetic\":[10,10,10]},\"particles\":null},"
		"{\"layer_id\":\"reticular_dermis\",\"thickness_m\":0.001,\"host_absorption_m_inv\":[10,10,10],\"real_refractive_index\":[1.38,1.38,1.38],\"constituent_absorption_m_inv\":{\"synthetic\":[10,10,10]},\"particles\":null}"
		"]}\n";
	const std::filesystem::path specPath = tmp.dir / "specimen.json";
	{
		std::ofstream ofs( specPath, std::ios::binary );
		ofs.write( sourceBytes.data(), static_cast<std::streamsize>( sourceBytes.size() ) );
	}

	LocalBakeConfig bakeCfg;
	bakeCfg.wavelengthsNm = { 450.0, 550.0, 650.0 };
	bakeCfg.historiesPerCellPerReplicate = 2;
	bakeCfg.replicates = 2;
	bakeCfg.normalGuideHistoriesPerReplicate = 4;
	bakeCfg.seed = 7u;
	bakeCfg.boundary = ExperimentBoundary::Smooth;
	bakeCfg.exteriorIOR = 1.0;
	bakeCfg.buildArtifactPath = exePath;

	const LocalBakeResult baked = BakeLocalResponse( sourceBytes, bakeCfg );
	const std::filesystem::path bakePath = tmp.dir / "s3_tiny.hylios";
	PublishLocalBake( bakePath.string(), baked );

	const std::string sceneText =
		"RISE ASCII SCENE 7\n"
		"hylios_skin_material\n"
		"{\n"
		"  name skin_s3\n"
		"  preset S3\n"
		"  transport_mode baked_local\n"
		"  boundary_mode smooth\n"
		"  exterior_ior 1.0\n"
		"  response_asset " + bakePath.string() + "\n"
		"  specimen_source " + specPath.string() + "\n"
		"}\n";

	const Cst::Document doc = Cst::ParseToCst( sceneText );
	Check( Cst::DocItemCount( doc ) > 0, "ParseToCst hylios_skin_material" );

	IJob* pJobI = 0;
	RISE_CreateJob( &pJobI );
	Job* pJob = dynamic_cast<Job*>( pJobI );
	std::vector<std::string> deriveDiags;
	Check( Cst::DeriveToJob( doc, *pJobI, &deriveDiags ), "DeriveToJob hylios_skin_material" );

	IMaterial* pMat = pJob->GetMaterials()->GetItem( "skin_s3" );
	Check( pMat != 0 && pMat->IsHyperspectralSkin(), "Parsed hylios_skin_material registered" );
	const std::string dump = risequiv::DumpJob( *pJob );
	Check( dump.find( "hylios_skin(mode=baked_local" ) != std::string::npos, "DumpJob includes hylios_skin" );

	// Incremental derive cleanly falls back (returns 0)
	std::vector<Cst::NodeId> chunkIds;
	for( int i = 0; i < Cst::DocItemCount( doc ); ++i ) {
		const Cst::NodeId nid = Cst::DocNodeIdAt( doc, i );
		const Cst::NodeRef n = Cst::DocResolveNodeId( doc, nid );
		if( n && n->kind == Cst::NodeKind::Chunk && n->role == "hylios_skin_material" ) {
			chunkIds.push_back( nid );
		}
	}
	Check( !chunkIds.empty(), "Found hylios_skin_material CST node" );
	std::vector<std::string> incDiags;
	Check( Cst::DeriveToJobIncremental( doc, *pJobI, chunkIds, &incDiags ) == 0,
		"DeriveToJobIncremental cleanly falls back (returns 0) for hylios_skin_material" );
	safe_release( pJobI );

	// Mismatched exterior_ior in scene file must fail DeriveToJob
	const std::string badSceneText =
		"RISE ASCII SCENE 7\n"
		"hylios_skin_material\n"
		"{\n"
		"  name skin_s3_bad\n"
		"  preset S3\n"
		"  transport_mode baked_local\n"
		"  boundary_mode smooth\n"
		"  exterior_ior 1.33\n"
		"  response_asset " + bakePath.string() + "\n"
		"  specimen_source " + specPath.string() + "\n"
		"}\n";
	const Cst::Document badDoc = Cst::ParseToCst( badSceneText );
	IJob* pBadJob = 0;
	RISE_CreateJob( &pBadJob );
	std::vector<std::string> badDeriveDiags;
	Check( !Cst::DeriveToJob( badDoc, *pBadJob, &badDeriveDiags ),
		"DeriveToJob rejects mismatched exterior_ior against baked asset" );
	safe_release( pBadJob );
}

} // namespace

int main( int argc, char** argv )
{
	TestAnalyticBSDFSPFAndChirality();
	TestCacheSharingAndBudgets();
	TestSharedVisibleGuideEquality();
	TestCapabilityCensusAndRasterizerGuards();
	TestBakedLocalSceneChunkAndCST( ( argc > 0 && argv && argv[0] ) ? argv[0] : "bin/tests/HyLIoSIntegrationTest" );

	if( gFailures == 0 ) {
		std::cout << "HyLIoSIntegrationTest: ALL PASSED\n";
		return 0;
	}
	std::cerr << "HyLIoSIntegrationTest: " << gFailures << " FAILURES\n";
	return 1;
}
