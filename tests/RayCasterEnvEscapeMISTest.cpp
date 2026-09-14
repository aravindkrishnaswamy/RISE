//////////////////////////////////////////////////////////////////////
//
//  RayCasterEnvEscapeMISTest.cpp - DL-53 red-proof regression: a
//    BSDF-sampled ray that escapes to the scene's global radiance map
//    through RayCaster::CastRay{,NM,HWSS}'s EXPLICIT pRadianceMap
//    branch must carry the same complementary MIS weight as the
//    branch reached when pRadianceMap is null (the "MIS PARTNER
//    RULE" -- see docs/PT_ENV_MIS_DOUBLECOUNT.md and DL-53's row in
//    docs/DEBT_LEDGER.md).
//
//    THE BUG THIS GUARDS AGAINST:
//      RayCaster's three CastRay overloads (RGB/NM/HWSS) each resolve
//      a ray-miss escape with `if( pRadianceMap ) { <raw, unweighted> }
//      else if( global map ) { <MIS-weighted against env-NEE> }`.
//      Every production pathtracing_* rasterizer's BSSRDF / random-walk
//      SSS continuation forwards the SAME global map through the
//      explicit pRadianceMap parameter (see PathTracingIntegrator.cpp's
//      PTCastRay call sites), with a real positive cosine bsdfPdf.  That
//      continuation is env-NEE's complementary BSDF-sampled partner
//      strategy for the entry point's already-weighted env-NEE sample --
//      but the FIRST arm above always won, so the weight in the second
//      arm was unreachable and the escape summed to 1 + w_nee instead of
//      1.  This is the RayCaster analogue of the PT-integrator bug fixed
//      in docs/PT_ENV_MIS_DOUBLECOUNT.md (F1/F2); that fix did NOT touch
//      RayCaster.cpp, which is a separate, still-shared implementation
//      used by SSS continuations, the internal volume phase-scatter
//      continuation, and the legacy shader-dispatch rasterizers.
//
//    WHAT THIS TEST DOES:
//      Builds three trivial scenes (RGB / NM / HWSS variants of the
//      pixel-based rasterizers), each with a bright global radiance map
//      and ZERO objects (so every ray, from any origin/direction,
//      unconditionally misses and reaches the escape branches).  After
//      one cheap render triggers RayCaster::AttachScene (building the
//      real LightSampler + EnvironmentSampler), the test reaches into
//      the live RayCaster/Scene through the SAME accessors production
//      code uses (Job::SetActiveRasterizerRadianceScale's
//      PixelBasedRasterizerHelper::GetRayCaster() pattern) and calls
//      CastRay/CastRayNM/CastRayHWSS DIRECTLY with a manufactured
//      miss ray and a real positive RAY_STATE::bsdfPdf, comparing:
//        (a) pRadianceMap == nullptr            (the always-correct arm)
//        (b) pRadianceMap == the scene's global map, passed EXPLICITLY
//            (the SSS-continuation call shape; the buggy arm pre-fix)
//      against an INDEPENDENTLY computed closed-form power-heuristic
//      weight (reimplemented locally, not calling into production
//      PowerHeuristic/RayCasterEnvEscapeMISWeight) applied to the raw
//      map radiance queried directly.
//
//      Also covers, as non-regression invariants (never buggy, must
//      stay working):
//        * bsdfPdf == 0 (delta scatter): both arms must return the RAW,
//          UNweighted radiance -- there is no NEE partner to correct
//          against for a delta BSDF sample.
//        * A genuinely DISTINCT per-object override map (pointer NOT
//          equal to the scene's global map) passed explicitly must keep
//          FULL weight (1), never the env-NEE partner weight -- nothing
//          importance-samples a per-object override through NEE.
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <cmath>
#ifdef _WIN32
	#include <process.h>
	#define getpid _getpid
#else
	#include <unistd.h>
#endif

#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Interfaces/IRayCaster.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Interfaces/IPainter.h"
#include "../src/Library/Interfaces/IRadianceMap.h"
#include "../src/Library/Rendering/PixelBasedRasterizerHelper.h"
#include "../src/Library/Rendering/EnvironmentSampler.h"
#include "../src/Library/Lights/LightSampler.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/PathTransportUtilities.h"
#include "../src/Library/Utilities/RuntimeContext.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/IORStack.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/Color/Color_Template.h"
#include "../src/Library/Utilities/Color/SampledWavelengths.h"
#include "../src/Library/RISE_API.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

static unsigned int g_renderSeed = 4242u;

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const char* testName )
{
	if( condition ) {
		passCount++;
	} else {
		failCount++;
		std::cout << "  FAIL: " << testName << std::endl;
	}
}

static void CheckClose( double a, double b, double tol, const char* testName )
{
	const bool ok = std::fabs( a - b ) <= tol;
	if( ok ) {
		passCount++;
	} else {
		failCount++;
		std::cout << "  FAIL: " << testName
			<< "  (a=" << a << " b=" << b << " |diff|=" << std::fabs( a - b )
			<< " tol=" << tol << ")" << std::endl;
	}
}

//////////////////////////////////////////////////////////////////////
// A no-op rasterizer output -- the test never inspects a rendered
// image, only the live RayCaster/Scene the render leaves behind.
//////////////////////////////////////////////////////////////////////
class NullRasterizerOutput
	: public virtual IRasterizerOutput
	, public virtual Reference
{
protected:
	virtual ~NullRasterizerOutput() {}
public:
	virtual void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}
	virtual void OutputImage( const IRasterImage&, const Rect*, const unsigned int ) override {}
};

//////////////////////////////////////////////////////////////////////
// Scene text: zero objects (every ray unconditionally misses), one
// bright global radiance map, and the caller-selected rasterizer
// chunk (RGB / NM / HWSS variant).
//////////////////////////////////////////////////////////////////////
static std::string WriteSceneToTempFile( const std::string& sceneText, const char* tag )
{
	char path[512];
	std::snprintf( path, sizeof(path),
		"/tmp/raycaster_envmis_%s_%d.RISEscene",
		tag, static_cast<int>(::getpid()) );

	std::ofstream ofs( path );
	if( !ofs.is_open() ) {
		return std::string();
	}
	ofs << sceneText;
	ofs.close();
	return std::string( path );
}

static std::string BuildEnvOnlyScene( const char* rasterizerChunk )
{
	std::ostringstream ss;
	ss <<
		"RISE ASCII SCENE 7\n"
		"\n"
		"uniformcolor_painter\n"
		"{\n"
		"\tname pnt_env\n"
		"\tcolor 0.7 0.5 0.3\n"
		"}\n"
		"\n"
		"standard_shader\n"
		"{\n"
		"\tname global\n"
		"\tshaderop DefaultDirectLighting\n"
		"}\n"
		"\n"
		<< rasterizerChunk <<
		"\n"
		"film\n"
		"{\n"
		"\twidth 4\n"
		"\theight 4\n"
		"}\n"
		"\n"
		"pinhole_camera\n"
		"{\n"
		"\tlocation 0 0 0\n"
		"\tlookat 0 0 1\n"
		"\tup 0 1 0\n"
		"\tfov 10.0\n"
		"}\n"
		"\n";
	return ss.str();
}

//////////////////////////////////////////////////////////////////////
// Fixture: loads one env-only scene, renders one cheap frame (to run
// RayCaster::AttachScene and build the real LightSampler /
// EnvironmentSampler), and exposes the live IRayCaster* + IScene*
// through the SAME accessor path production code uses
// (Job::SetActiveRasterizerRadianceScale).
//////////////////////////////////////////////////////////////////////
struct Fixture
{
	IJobPriv* pJob;
	IRayCaster* pCaster;
	const IScene* pScene;
	std::string scenePath;

	Fixture() : pJob( 0 ), pCaster( 0 ), pScene( 0 ) {}

	bool Build( const char* rasterizerChunk, const char* tag )
	{
		const std::string sceneText = BuildEnvOnlyScene( rasterizerChunk );
		scenePath = WriteSceneToTempFile( sceneText, tag );
		if( scenePath.empty() ) {
			return false;
		}

		if( !RISE_CreateJobPriv( &pJob ) || !pJob ) {
			return false;
		}

		if( !pJob->LoadAsciiSceneAuto( scenePath.c_str() ) ) {
			return false;
		}

		pJob->RemoveRasterizerOutputs();
		NullRasterizerOutput* pOut = new NullRasterizerOutput();
		GlobalLog()->PrintNew( pOut, __FILE__, __LINE__, "test null output" );
		pJob->GetRasterizer()->AddRasterizerOutput( pOut );
		safe_release( pOut );

		std::srand( g_renderSeed++ );
		if( !pJob->Rasterize() ) {
			return false;
		}

		PixelBasedRasterizerHelper* pHelper =
			dynamic_cast<PixelBasedRasterizerHelper*>( pJob->GetRasterizer() );
		if( !pHelper ) {
			return false;
		}
		pCaster = pHelper->GetRayCaster();
		pScene = pJob->GetScene();
		return pCaster != 0 && pScene != 0 && pScene->GetGlobalRadianceMap() != 0;
	}

	~Fixture()
	{
		safe_release( pJob );
		if( !scenePath.empty() ) {
			std::remove( scenePath.c_str() );
		}
	}
};

static const Vector3 kMissDir = Vector3Ops::Normalize( Vector3( 0.2, 0.5, 1.0 ) );

//////////////////////////////////////////////////////////////////////
// RGB (CastRay)
//////////////////////////////////////////////////////////////////////
static void TestRGB()
{
	std::cout << "Test RGB: RayCaster::CastRay explicit-global-map MIS partner" << std::endl;

	Fixture fx;
	const bool built = fx.Build(
			"pixelpel_rasterizer\n"
			"{\n"
			"\tsamples 1\n"
			"\tpixel_filter box\n"
			"\toidn_denoise FALSE\n"
			"\tradiance_map pnt_env\n"
			"\tradiance_background TRUE\n"
			"}\n",
			"rgb" );
	Check( built, "RGB fixture builds" );
	if( !built ) {
		return;
	}

	const IRadianceMap* pGlobal = fx.pScene->GetGlobalRadianceMap();
	const LightSampler* pLS = fx.pCaster->GetLightSampler();
	Check( pLS != 0, "RGB: LightSampler available" );
	if( !pLS ) return;
	const EnvironmentSampler* pES = pLS->GetEnvironmentSampler();
	Check( pES != 0 && pES->IsValid(), "RGB: EnvironmentSampler built and valid" );
	if( !pES || !pES->IsValid() ) return;

	RuntimeContext rc( GlobalRNG(), RuntimeContext::PASS_NORMAL, false );
	const Ray ray( Point3( 0, 0, 0 ), kMissDir );
	const RasterizerState rast = nullRasterizerState;

	const Scalar envPdf = pES->Pdf( ray.Dir() );
	Check( envPdf > 0, "RGB: env pdf positive at test direction" );

	const RISEPel rawRadiance = pGlobal->GetRadiance( ray, rast );

	// --- Case 1: positive bsdfPdf -- env-NEE's complementary partner ---
	{
		const Scalar bsdfPdfs[] = { 0.1, 1.0, 5.0 };
		for( double bsdfPdf : bsdfPdfs )
		{
			IRayCaster::RAY_STATE rs;
			rs.bsdfPdf = bsdfPdf;

			RISEPel cNull( 0, 0, 0 );
			fx.pCaster->CastRay( rc, rast, ray, cNull, rs, 0, 0 );

			RISEPel cExplicit( 0, 0, 0 );
			fx.pCaster->CastRay( rc, rast, ray, cExplicit, rs, 0, pGlobal );

			// Independent analytic reference: power-2 heuristic recomputed
			// locally (NOT calling PathTransportUtilities::PowerHeuristic
			// or RayCasterEnvEscapeMISWeight), applied to the raw radiance
			// queried directly above.
			const Scalar w = ( bsdfPdf * bsdfPdf ) / ( bsdfPdf * bsdfPdf + envPdf * envPdf );
			const RISEPel expected = rawRadiance * w;

			char label[128];
			std::snprintf( label, sizeof(label),
				"RGB: null-map == explicit-global-map @ bsdfPdf=%g", bsdfPdf );
			CheckClose( cNull.r, cExplicit.r, 1e-9, label );
			CheckClose( cNull.g, cExplicit.g, 1e-9, label );
			CheckClose( cNull.b, cExplicit.b, 1e-9, label );

			std::snprintf( label, sizeof(label),
				"RGB: explicit-global-map matches analytic MIS weight @ bsdfPdf=%g", bsdfPdf );
			CheckClose( cExplicit.r, expected.r, 1e-9, label );
			CheckClose( cExplicit.g, expected.g, 1e-9, label );
			CheckClose( cExplicit.b, expected.b, 1e-9, label );

			// Pin the partition of unity against the SAME shared function
			// LightSampler's own env-NEE weight resolves to (its
			// non-optimal-MIS branch calls
			// `PowerHeuristic(envPdf, pBsdf)` directly -- see
			// LightSampler.cpp's `EvaluateInScattering`/NEE-at-scatter-point
			// env arm, ~:2652) rather than only the hand-derived `w`
			// formula above, so a future change to either side's
			// heuristic (or a divergence between the two) is caught
			// here: the escape weight and the actual NEE weight for the
			// same (bsdfPdf, envPdf) pair must sum to exactly 1.
			const Scalar w_bsdf_real = PathTransportUtilities::PowerHeuristic( bsdfPdf, envPdf );
			const Scalar w_nee_real = PathTransportUtilities::PowerHeuristic( envPdf, bsdfPdf );
			std::snprintf( label, sizeof(label),
				"RGB: escape weight + LightSampler's actual env-NEE weight partition to 1 @ bsdfPdf=%g", bsdfPdf );
			CheckClose( w_bsdf_real + w_nee_real, Scalar(1), 1e-12, label );

			std::snprintf( label, sizeof(label),
				"RGB: explicit-global-map matches PowerHeuristic (the function LightSampler's env-NEE actually calls) @ bsdfPdf=%g", bsdfPdf );
			CheckClose( cExplicit.r, rawRadiance.r * w_bsdf_real, 1e-9, label );
			CheckClose( cExplicit.g, rawRadiance.g * w_bsdf_real, 1e-9, label );
			CheckClose( cExplicit.b, rawRadiance.b * w_bsdf_real, 1e-9, label );
		}
	}

	// --- Case 2: bsdfPdf == 0 (delta) -- both arms stay RAW/unweighted ---
	{
		IRayCaster::RAY_STATE rs;
		rs.bsdfPdf = 0;

		RISEPel cNull( 0, 0, 0 );
		fx.pCaster->CastRay( rc, rast, ray, cNull, rs, 0, 0 );

		RISEPel cExplicit( 0, 0, 0 );
		fx.pCaster->CastRay( rc, rast, ray, cExplicit, rs, 0, pGlobal );

		CheckClose( cNull.r, rawRadiance.r, 1e-9, "RGB: bsdfPdf=0 null-map stays raw (r)" );
		CheckClose( cExplicit.r, rawRadiance.r, 1e-9, "RGB: bsdfPdf=0 explicit-map stays raw (r)" );
	}

	// --- Case 3: genuinely distinct per-object override map keeps full weight ---
	{
		IPainter* pOtherPainter = 0;
		RISE_API_CreateUniformColorPainter( &pOtherPainter, RISEPel( 0.1, 0.9, 0.4 ), eSpectrumKind_Illuminant );
		IRadianceMap* pOtherMap = 0;
		RISE_API_CreateRadianceMap( &pOtherMap, *pOtherPainter, 1.0 );

		IRayCaster::RAY_STATE rs;
		rs.bsdfPdf = 1.0;	// positive -- would be weighted if (mis-)treated as the global map

		RISEPel cOther( 0, 0, 0 );
		fx.pCaster->CastRay( rc, rast, ray, cOther, rs, 0, pOtherMap );
		const RISEPel rawOther = pOtherMap->GetRadiance( ray, rast );

		CheckClose( cOther.r, rawOther.r, 1e-9, "RGB: distinct override map keeps full weight (not global's partner)" );

		safe_release( pOtherMap );
		safe_release( pOtherPainter );
	}
}

//////////////////////////////////////////////////////////////////////
// NM (CastRayNM)
//////////////////////////////////////////////////////////////////////
static void TestNM()
{
	std::cout << "Test NM 550nm: RayCaster::CastRayNM explicit-global-map MIS partner" << std::endl;

	Fixture fx;
	const bool built = fx.Build(
			"pixelintegratingspectral_rasterizer\n"
			"{\n"
			"\tsamples 1\n"
			"\tpixel_filter box\n"
			"\tnmbegin 380\n"
			"\tnmend 720\n"
			"\tnum_wavelengths 4\n"
			"\tspectral_samples 1\n"
			"\thwss false\n"
			"\toidn_denoise FALSE\n"
			"\tradiance_map pnt_env\n"
			"\tradiance_background TRUE\n"
			"}\n",
			"nm" );
	Check( built, "NM fixture builds" );
	if( !built ) {
		return;
	}

	const IRadianceMap* pGlobal = fx.pScene->GetGlobalRadianceMap();
	const LightSampler* pLS = fx.pCaster->GetLightSampler();
	Check( pLS != 0, "NM: LightSampler available" );
	if( !pLS ) return;
	const EnvironmentSampler* pES = pLS->GetEnvironmentSampler();
	Check( pES != 0 && pES->IsValid(), "NM: EnvironmentSampler built and valid" );
	if( !pES || !pES->IsValid() ) return;

	RuntimeContext rc( GlobalRNG(), RuntimeContext::PASS_NORMAL, false );
	const Ray ray( Point3( 0, 0, 0 ), kMissDir );
	const RasterizerState rast = nullRasterizerState;
	const Scalar nm = 550.0;

	const Scalar envPdf = pES->Pdf( ray.Dir() );
	Check( envPdf > 0, "NM: env pdf positive at test direction" );

	const Scalar rawRadiance = pGlobal->GetRadianceNM( ray, rast, nm );
	Check( rawRadiance > 0, "NM: raw env radiance positive at 550nm" );

	const Scalar bsdfPdfs[] = { 0.1, 1.0, 5.0 };
	for( double bsdfPdf : bsdfPdfs )
	{
		IRayCaster::RAY_STATE rs;
		rs.bsdfPdf = bsdfPdf;

		Scalar cNull = 0;
		fx.pCaster->CastRayNM( rc, rast, ray, cNull, rs, nm, 0, 0 );

		Scalar cExplicit = 0;
		fx.pCaster->CastRayNM( rc, rast, ray, cExplicit, rs, nm, 0, pGlobal );

		const Scalar w = ( bsdfPdf * bsdfPdf ) / ( bsdfPdf * bsdfPdf + envPdf * envPdf );
		const Scalar expected = rawRadiance * w;

		char label[128];
		std::snprintf( label, sizeof(label), "NM: null-map == explicit-global-map @ bsdfPdf=%g", bsdfPdf );
		CheckClose( cNull, cExplicit, 1e-9, label );

		std::snprintf( label, sizeof(label), "NM: explicit-global-map matches analytic MIS weight @ bsdfPdf=%g", bsdfPdf );
		CheckClose( cExplicit, expected, 1e-9, label );
	}

	// bsdfPdf == 0 (delta) invariant
	{
		IRayCaster::RAY_STATE rs;
		rs.bsdfPdf = 0;
		Scalar cNull = 0, cExplicit = 0;
		fx.pCaster->CastRayNM( rc, rast, ray, cNull, rs, nm, 0, 0 );
		fx.pCaster->CastRayNM( rc, rast, ray, cExplicit, rs, nm, 0, pGlobal );
		CheckClose( cNull, rawRadiance, 1e-9, "NM: bsdfPdf=0 null-map stays raw" );
		CheckClose( cExplicit, rawRadiance, 1e-9, "NM: bsdfPdf=0 explicit-map stays raw" );
	}

	// Distinct per-object override map invariant
	{
		IPainter* pOtherPainter = 0;
		RISE_API_CreateUniformColorPainter( &pOtherPainter, RISEPel( 0.1, 0.9, 0.4 ), eSpectrumKind_Illuminant );
		IRadianceMap* pOtherMap = 0;
		RISE_API_CreateRadianceMap( &pOtherMap, *pOtherPainter, 1.0 );

		IRayCaster::RAY_STATE rs;
		rs.bsdfPdf = 1.0;

		Scalar cOther = 0;
		fx.pCaster->CastRayNM( rc, rast, ray, cOther, rs, nm, 0, pOtherMap );
		const Scalar rawOther = pOtherMap->GetRadianceNM( ray, rast, nm );

		CheckClose( cOther, rawOther, 1e-9, "NM: distinct override map keeps full weight" );

		safe_release( pOtherMap );
		safe_release( pOtherPainter );
	}
}

//////////////////////////////////////////////////////////////////////
// HWSS (CastRayHWSS)
//////////////////////////////////////////////////////////////////////
static void TestHWSS()
{
	std::cout << "Test HWSS: RayCaster::CastRayHWSS explicit-global-map MIS partner" << std::endl;

	Fixture fx;
	const bool built = fx.Build(
			"pixelintegratingspectral_rasterizer\n"
			"{\n"
			"\tsamples 1\n"
			"\tpixel_filter box\n"
			"\tnmbegin 380\n"
			"\tnmend 720\n"
			"\tnum_wavelengths 4\n"
			"\tspectral_samples 1\n"
			"\thwss true\n"
			"\toidn_denoise FALSE\n"
			"\tradiance_map pnt_env\n"
			"\tradiance_background TRUE\n"
			"}\n",
			"hwss" );
	Check( built, "HWSS fixture builds" );
	if( !built ) {
		return;
	}

	const IRadianceMap* pGlobal = fx.pScene->GetGlobalRadianceMap();
	const LightSampler* pLS = fx.pCaster->GetLightSampler();
	Check( pLS != 0, "HWSS: LightSampler available" );
	if( !pLS ) return;
	const EnvironmentSampler* pES = pLS->GetEnvironmentSampler();
	Check( pES != 0 && pES->IsValid(), "HWSS: EnvironmentSampler built and valid" );
	if( !pES || !pES->IsValid() ) return;

	RuntimeContext rc( GlobalRNG(), RuntimeContext::PASS_NORMAL, false );
	const Ray ray( Point3( 0, 0, 0 ), kMissDir );
	const RasterizerState rast = nullRasterizerState;
	const IORStack iorStack( 1.0 );
	SampledWavelengths swl = SampledWavelengths::SampleEquidistant( 0.37, 380.0, 720.0 );

	const Scalar envPdf = pES->Pdf( ray.Dir() );
	Check( envPdf > 0, "HWSS: env pdf positive at test direction" );

	Scalar rawRadiance[SampledWavelengths::N];
	for( unsigned int i = 0; i < SampledWavelengths::N; i++ ) {
		rawRadiance[i] = pGlobal->GetRadianceNM( ray, rast, swl.lambda[i] );
	}

	const Scalar bsdfPdfs[] = { 0.1, 1.0, 5.0 };
	for( double bsdfPdf : bsdfPdfs )
	{
		IRayCaster::RAY_STATE rs;
		rs.bsdfPdf = bsdfPdf;

		Scalar cNull[SampledWavelengths::N];
		fx.pCaster->CastRayHWSS( rc, rast, ray, cNull, rs, swl, 0, 0, iorStack );

		Scalar cExplicit[SampledWavelengths::N];
		fx.pCaster->CastRayHWSS( rc, rast, ray, cExplicit, rs, swl, 0, pGlobal, iorStack );

		const Scalar w = ( bsdfPdf * bsdfPdf ) / ( bsdfPdf * bsdfPdf + envPdf * envPdf );

		for( unsigned int i = 0; i < SampledWavelengths::N; i++ )
		{
			char label[160];
			std::snprintf( label, sizeof(label),
				"HWSS: null-map == explicit-global-map @ bsdfPdf=%g lambda[%u]", bsdfPdf, i );
			CheckClose( cNull[i], cExplicit[i], 1e-9, label );

			std::snprintf( label, sizeof(label),
				"HWSS: explicit-global-map matches analytic MIS weight @ bsdfPdf=%g lambda[%u]", bsdfPdf, i );
			CheckClose( cExplicit[i], rawRadiance[i] * w, 1e-9, label );
		}
	}

	// bsdfPdf == 0 (delta) invariant
	{
		IRayCaster::RAY_STATE rs;
		rs.bsdfPdf = 0;
		Scalar cNull[SampledWavelengths::N];
		Scalar cExplicit[SampledWavelengths::N];
		fx.pCaster->CastRayHWSS( rc, rast, ray, cNull, rs, swl, 0, 0, iorStack );
		fx.pCaster->CastRayHWSS( rc, rast, ray, cExplicit, rs, swl, 0, pGlobal, iorStack );
		for( unsigned int i = 0; i < SampledWavelengths::N; i++ ) {
			CheckClose( cNull[i], rawRadiance[i], 1e-9, "HWSS: bsdfPdf=0 null-map stays raw" );
			CheckClose( cExplicit[i], rawRadiance[i], 1e-9, "HWSS: bsdfPdf=0 explicit-map stays raw" );
		}
	}

	// Distinct per-object override map invariant
	{
		IPainter* pOtherPainter = 0;
		RISE_API_CreateUniformColorPainter( &pOtherPainter, RISEPel( 0.1, 0.9, 0.4 ), eSpectrumKind_Illuminant );
		IRadianceMap* pOtherMap = 0;
		RISE_API_CreateRadianceMap( &pOtherMap, *pOtherPainter, 1.0 );

		IRayCaster::RAY_STATE rs;
		rs.bsdfPdf = 1.0;

		Scalar cOther[SampledWavelengths::N];
		fx.pCaster->CastRayHWSS( rc, rast, ray, cOther, rs, swl, 0, pOtherMap, iorStack );

		for( unsigned int i = 0; i < SampledWavelengths::N; i++ ) {
			const Scalar rawOther = pOtherMap->GetRadianceNM( ray, rast, swl.lambda[i] );
			CheckClose( cOther[i], rawOther, 1e-9, "HWSS: distinct override map keeps full weight" );
		}

		safe_release( pOtherMap );
		safe_release( pOtherPainter );
	}
}

int main()
{
	std::cout << "=== RayCasterEnvEscapeMISTest (DL-53) ===" << std::endl;

	TestRGB();
	TestNM();
	TestHWSS();

	std::cout << "Passed: " << passCount << "   Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
