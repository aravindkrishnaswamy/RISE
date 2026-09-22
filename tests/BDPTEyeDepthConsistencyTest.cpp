//////////////////////////////////////////////////////////////////////
//
//  BDPTEyeDepthConsistencyTest.cpp - Red-proof and consistency gate
//    for DL-210: BDPT eye subpath surface bounce truncation parity
//    between Pel (RGB) and NM (spectral) walks.
//
//  THE DEFECT (pre-fix):
//    In `BDPTIntegrator.cpp` `GenerateEyeSubpathImpl`:
//    `eyeSurfaceBounces` was declared `[[maybe_unused]] unsigned int eyeSurfaceBounces = 0;`
//    and its depth check was guarded by `if constexpr( Traits::is_nm )`:
//
//        if constexpr( Traits::is_nm ) {
//            if( eyeSurfaceBounces >= maxEyeDepth ) {
//                break;
//            }
//            eyeSurfaceBounces++;
//        }
//
//    Because of `if constexpr( Traits::is_nm )`, the Pel (RGB) eye walk
//    never checked `eyeSurfaceBounces >= maxEyeDepth`! Instead, on scenes
//    with no media, the Pel eye walk ran up to
//    `maxEyeTotalDepth = maxEyeDepth + stabilityConfig.maxVolumeBounce`
//    (where default `max_volume_bounce` is 64).
//    Consequently, on any multi-bounce scene, Pel BDPT generated up to
//    64 extra surface bounces beyond `max_eye_depth`, evaluating deeper
//    indirect transport than authored and creating a severe discrepancy
//    between Pel and NM BDPT when `max_eye_depth` was constrained.
//    In contrast, `GenerateLightSubpathImpl` unconditionally capped
//    `surfaceBounces >= maxLightDepth` for both Pel and NM.
//
//  THE FIX:
//    1. Remove `[[maybe_unused]]` from `eyeSurfaceBounces = 0;`.
//    2. Remove `if constexpr( Traits::is_nm )` wrapper around the
//       `eyeSurfaceBounces >= maxEyeDepth` check so it applies
//       unconditionally to both Pel and NM walks, matching
//       `GenerateLightSubpathImpl`.
//
//  WHAT THIS TEST PINS:
//    1. Subpath Generation Level: Directly traces eye subpaths in an
//       enclosed diffuse box with `max_eye_depth` = 1, 2, and 3.
//       Asserts that Pel and NM walks strictly obey the surface bounce cap
//       and produce identical surface vertex counts.
//       (Pre-fix: Pel generates > maxEyeDepth surface vertices on every
//       multi-bounce ray, failing the assertion).
//    2. Render Level: Renders an enclosed multi-bounce diffuse scene
//       under `bdpt_pel_rasterizer` and `bdpt_spectral_rasterizer` with
//       constrained `max_eye_depth 2`.
//       Asserts that Pel BDPT and NM BDPT agree on mean radiance within
//       Monte Carlo tolerance.
//       (Pre-fix: Pel BDPT accumulates 64 extra indirect bounces,
//       rendering substantially brighter than NM BDPT).
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <vector>
#include <cmath>
#include <string>

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
#include "../src/Library/Interfaces/IScene.h"
#include "../src/Library/Interfaces/ICamera.h"
#include "../src/Library/Interfaces/IRayCaster.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/Color/Color_Template.h"
#include "../src/Library/Utilities/RuntimeContext.h"
#include "../src/Library/Shaders/BDPTIntegrator.h"
#include "../src/Library/Utilities/SobolSampler.h"
#include "../src/Library/Rendering/PixelBasedRasterizerHelper.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const std::string& testName )
{
	if( condition ) {
		passCount++;
	} else {
		failCount++;
		std::cout << "  FAIL: " << testName << std::endl;
	}
}

class CapturingRasterizerOutput
	: public virtual IRasterizerOutput
	, public virtual Reference
{
public:
	std::vector<RISEColor> pixels;
	unsigned int width;
	unsigned int height;

	CapturingRasterizerOutput() : width(0), height(0) {}

protected:
	virtual ~CapturingRasterizerOutput() {}

public:
	virtual void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}

	virtual void OutputImage(
		const IRasterImage& pImage,
		const Rect*,
		const unsigned int ) override
	{
		width = pImage.GetWidth();
		height = pImage.GetHeight();
		pixels.resize( width * height );
		for( unsigned int y = 0; y < height; y++ ) {
			for( unsigned int x = 0; x < width; x++ ) {
				pixels[y * width + x] = pImage.GetPEL( x, y );
			}
		}
	}
};

// Enclosed diffuse box: 6 Lambertian walls forming a 4x4x4 cube [-2, 2]^3.
// Camera is at (0, 0, 0) pointing at (0, 0, -1).
// Overhead area luminaire on the ceiling at y = 1.99.
static const char* kEnclosedBoxSceneTemplate =
	"RISE ASCII SCENE 7\n"
	"film\n"
	"{\n"
	"\twidth 32\n"
	"\theight 32\n"
	"}\n"
	"\n"
	"pinhole_camera\n"
	"{\n"
	"\tlocation 0 0 0\n"
	"\tlookat 0 0 -1\n"
	"\tup 0 1 0\n"
	"\tfov 60.0\n"
	"}\n"
	"\n"
	"uniformcolor_painter\n"
	"{\n"
	"\tname pnt_diffuse\n"
	"\tcolor 0.8 0.8 0.8\n"
	"}\n"
	"\n"
	"lambertian_material\n"
	"{\n"
	"\tname mat_diffuse\n"
	"\treflectance pnt_diffuse\n"
	"}\n"
	"\n"
	"uniformcolor_painter\n"
	"{\n"
	"\tname pnt_lum\n"
	"\tcolor 20.0 20.0 20.0\n"
	"}\n"
	"\n"
	"lambertian_luminaire_material\n"
	"{\n"
	"\tname mat_lum\n"
	"\texitance pnt_lum\n"
	"\tscale 1.0\n"
	"\tmaterial none\n"
	"}\n"
	"\n"
	"clippedplane_geometry\n"
	"{\n"
	"\tname floor_geom\n"
	"\tpta -2 -2 -2\n"
	"\tptb 2 -2 -2\n"
	"\tptc 2 -2 2\n"
	"\tptd -2 -2 2\n"
	"}\n"
	"standard_object\n"
	"{\n"
	"\tname obj_floor\n"
	"\tgeometry floor_geom\n"
	"\tmaterial mat_diffuse\n"
	"}\n"
	"\n"
	"clippedplane_geometry\n"
	"{\n"
	"\tname ceiling_geom\n"
	"\tpta -2 2 -2\n"
	"\tptb -2 2 2\n"
	"\tptc 2 2 2\n"
	"\tptd 2 2 -2\n"
	"}\n"
	"standard_object\n"
	"{\n"
	"\tname obj_ceiling\n"
	"\tgeometry ceiling_geom\n"
	"\tmaterial mat_diffuse\n"
	"}\n"
	"\n"
	"clippedplane_geometry\n"
	"{\n"
	"\tname back_geom\n"
	"\tpta -2 -2 -2\n"
	"\tptb -2 2 -2\n"
	"\tptc 2 2 -2\n"
	"\tptd 2 -2 -2\n"
	"}\n"
	"standard_object\n"
	"{\n"
	"\tname obj_back\n"
	"\tgeometry back_geom\n"
	"\tmaterial mat_diffuse\n"
	"}\n"
	"\n"
	"clippedplane_geometry\n"
	"{\n"
	"\tname front_geom\n"
	"\tpta -2 -2 2\n"
	"\tptb 2 -2 2\n"
	"\tptc 2 2 2\n"
	"\tptd -2 2 2\n"
	"}\n"
	"standard_object\n"
	"{\n"
	"\tname obj_front\n"
	"\tgeometry front_geom\n"
	"\tmaterial mat_diffuse\n"
	"}\n"
	"\n"
	"clippedplane_geometry\n"
	"{\n"
	"\tname left_geom\n"
	"\tpta -2 -2 -2\n"
	"\tptb -2 -2 2\n"
	"\tptc -2 2 2\n"
	"\tptd -2 2 -2\n"
	"}\n"
	"standard_object\n"
	"{\n"
	"\tname obj_left\n"
	"\tgeometry left_geom\n"
	"\tmaterial mat_diffuse\n"
	"}\n"
	"\n"
	"clippedplane_geometry\n"
	"{\n"
	"\tname right_geom\n"
	"\tpta 2 -2 -2\n"
	"\tptb 2 2 -2\n"
	"\tptc 2 2 2\n"
	"\tptd 2 -2 2\n"
	"}\n"
	"standard_object\n"
	"{\n"
	"\tname obj_right\n"
	"\tgeometry right_geom\n"
	"\tmaterial mat_diffuse\n"
	"}\n"
	"\n"
	"clippedplane_geometry\n"
	"{\n"
	"\tname light_geom\n"
	"\tpta -0.5 1.99 -0.5\n"
	"\tptb 0.5 1.99 -0.5\n"
	"\tptc 0.5 1.99 0.5\n"
	"\tptd -0.5 1.99 0.5\n"
	"}\n"
	"standard_object\n"
	"{\n"
	"\tname obj_light\n"
	"\tgeometry light_geom\n"
	"\tmaterial mat_lum\n"
	"}\n";

static std::string MakeSceneFile( const std::string& sceneContent )
{
	char path[512];
	std::snprintf( path, sizeof(path), "/tmp/bdpt_eye_depth_%d.RISEscene",
		static_cast<int>(::getpid()) );
	std::ofstream ofs( path );
	if( !ofs.is_open() ) {
		return std::string();
	}
	ofs << sceneContent;
	ofs.close();
	return std::string( path );
}

static double RenderMean( const std::string& sceneText )
{
	const std::string path = MakeSceneFile( sceneText );
	if( path.empty() ) return -1.0;

	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) {
		std::remove( path.c_str() );
		return -1.0;
	}

	if( !pJob->LoadAsciiSceneViaCst( path.c_str() ) ) {
		safe_release( pJob );
		std::remove( path.c_str() );
		return -1.0;
	}

	pJob->RemoveRasterizerOutputs();
	CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "test capture output" );
	pJob->GetRasterizer()->AddRasterizerOutput( pCap );

	static unsigned renderIndex = 0;
	std::srand( 54321u + renderIndex++ );
	const bool bRendered = pJob->Rasterize();

	double mean = -1.0;
	if( bRendered && !pCap->pixels.empty() ) {
		double sum = 0.0;
		bool finite = true;
		for( const RISEColor& c : pCap->pixels ) {
			const double a = ( c.base.r + c.base.g + c.base.b ) / 3.0;
			if( !std::isfinite( a ) ) { finite = false; break; }
			sum += a;
		}
		if( finite ) {
			mean = sum / double( pCap->pixels.size() );
		}
	}

	safe_release( pCap );
	safe_release( pJob );
	std::remove( path.c_str() );
	return mean;
}

//////////////////////////////////////////////////////////////////////
// Test 1: Subpath Generation Level (Unit Level)
//
// Directly tests `GenerateEyeSubpath` (Pel) and `GenerateEyeSubpathNM`
// (NM) with `max_eye_depth` set to 1, 2, and 3 on an enclosed diffuse
// box scene where rays undergo multiple scattering events.
//
// Asserts:
// - NM surface vertex count <= maxEyeDepth
// - Pel surface vertex count <= maxEyeDepth
// - Pel surface vertex count == NM surface vertex count
//////////////////////////////////////////////////////////////////////
static void TestEyeSubpathBounceCap()
{
	std::cout << "--- Sub-test 1: Eye subpath surface bounce truncation (unit level) ---" << std::endl;

	std::string baseScene = kEnclosedBoxSceneTemplate;
	baseScene +=
		"standard_shader\n"
		"{\n"
		"\tname global\n"
		"\tshaderop DefaultPathTracing\n"
		"}\n"
		"bdpt_pel_rasterizer\n"
		"{\n"
		"\tmax_eye_depth 2\n"
		"\tmax_light_depth 2\n"
		"\tsamples 4\n"
		"\tpixel_filter box\n"
		"\toidn_denoise FALSE\n"
		"}\n";

	const std::string path = MakeSceneFile( baseScene );
	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) {
		Check( false, "CreateJobPriv failed" );
		return;
	}

	if( !pJob->LoadAsciiSceneViaCst( path.c_str() ) ) {
		safe_release( pJob );
		std::remove( path.c_str() );
		Check( false, "LoadAsciiSceneViaCst failed" );
		return;
	}

	const IScene* pScene = pJob->GetScene();
	const ICamera* pCamera = pScene ? pScene->GetCamera() : nullptr;
	auto* pRaster = dynamic_cast<PixelBasedRasterizerHelper*>( pJob->GetRasterizer() );
	IRayCaster* pCaster = pRaster ? pRaster->GetRayCaster() : nullptr;

	if( !pScene || !pCamera || !pCaster ) {
		safe_release( pJob );
		std::remove( path.c_str() );
		Check( false, "Scene/Camera/Caster setup failed" );
		return;
	}

	RandomNumberGenerator rng;
	RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );

	const unsigned int testedDepths[] = { 1, 2, 3 };

	for( unsigned int maxEye : testedDepths ) {
		StabilityConfig stabilityCfg; // default maxVolumeBounce = 64
		BDPTIntegrator* pBdpt = new BDPTIntegrator( maxEye, 2, stabilityCfg );

		unsigned int pelExceededCount = 0;
		unsigned int nmExceededCount = 0;
		unsigned int mismatchCount = 0;
		unsigned int totalRaysTested = 0;
		unsigned int maxPelSurfaceSeen = 0;

		// Sample across 16 screen points
		for( int sy = 0; sy < 4; sy++ ) {
			for( int sx = 0; sx < 4; sx++ ) {
				const Point2 ptOnScreen( ( sx + 0.5 ) / 4.0, ( sy + 0.5 ) / 4.0 );
				Ray cameraRay;
				if( !pCamera->GenerateRay( rc, cameraRay, ptOnScreen ) ) {
					continue;
				}

				SobolSampler samplerPel( totalRaysTested, 0x1234 );
				SobolSampler samplerNM( totalRaysTested, 0x1234 );

				std::vector<BDPTVertex> eyeVertsPel;
				std::vector<uint32_t> eyeStartsPel;
				pBdpt->GenerateEyeSubpath( rc, cameraRay, ptOnScreen, *pScene, *pCaster,
					samplerPel, eyeVertsPel, eyeStartsPel, nullptr );

				std::vector<BDPTVertex> eyeVertsNM;
				std::vector<uint32_t> eyeStartsNM;
				pBdpt->GenerateEyeSubpathNM( rc, cameraRay, ptOnScreen, *pScene, *pCaster,
					samplerNM, eyeVertsNM, eyeStartsNM, 550.0, nullptr, nullptr );

				unsigned int pelSurfaceVerts = 0;
				for( const auto& v : eyeVertsPel ) {
					if( v.type == BDPTVertex::SURFACE ) pelSurfaceVerts++;
				}

				unsigned int nmSurfaceVerts = 0;
				for( const auto& v : eyeVertsNM ) {
					if( v.type == BDPTVertex::SURFACE ) nmSurfaceVerts++;
				}

				if( pelSurfaceVerts > maxPelSurfaceSeen ) {
					maxPelSurfaceSeen = pelSurfaceVerts;
				}
				if( nmSurfaceVerts > maxEye ) {
					nmExceededCount++;
				}
				if( pelSurfaceVerts > maxEye ) {
					pelExceededCount++;
				}
				if( pelSurfaceVerts != nmSurfaceVerts ) {
					mismatchCount++;
				}

				totalRaysTested++;
			}
		}

		safe_release( pBdpt );

		std::cout << "  max_eye_depth = " << maxEye << ":"
		          << " rays=" << totalRaysTested
		          << " maxPelSurface=" << maxPelSurfaceSeen
		          << " pelExceeded=" << pelExceededCount
		          << " nmExceeded=" << nmExceededCount
		          << " mismatches=" << mismatchCount
		          << std::endl;

		char msgNm[128];
		std::snprintf( msgNm, sizeof(msgNm),
			"DL-210: NM eye subpath surface bounce count <= max_eye_depth (%u)", maxEye );
		Check( nmExceededCount == 0, msgNm );

		char msgPel[128];
		std::snprintf( msgPel, sizeof(msgPel),
			"DL-210: Pel eye subpath surface bounce count <= max_eye_depth (%u)", maxEye );
		Check( pelExceededCount == 0, msgPel );

		char msgMatch[128];
		std::snprintf( msgMatch, sizeof(msgMatch),
			"DL-210: Pel and NM eye subpaths agree on surface bounce count at max_eye_depth (%u)", maxEye );
		Check( mismatchCount == 0, msgMatch );
	}

	safe_release( pJob );
	std::remove( path.c_str() );
}

//////////////////////////////////////////////////////////////////////
// Test 2: Multi-bounce Enclosed Scene Render Level
//
// Renders the enclosed diffuse box under:
//   1. `bdpt_pel_rasterizer` with `max_eye_depth 2`, `max_light_depth 2`
//   2. `bdpt_spectral_rasterizer` with `max_eye_depth 2`, `max_light_depth 2`
//
// At `max_eye_depth 2` with default `max_volume_bounce 64`:
// - Pre-fix: Pel BDPT runs up to 66 surface vertices, accumulating
//   substantially more multi-bounce indirect illumination than NM BDPT.
// - Post-fix: Pel BDPT and NM BDPT both truncate at 2 surface vertices
//   and agree on the mean radiance.
//////////////////////////////////////////////////////////////////////
static void TestEnclosedCornellBoxDepthParity()
{
	std::cout << std::endl << "--- Sub-test 2: Enclosed scene render parity (system level) ---" << std::endl;

	std::string scenePel = kEnclosedBoxSceneTemplate;
	scenePel +=
		"standard_shader\n"
		"{\n"
		"\tname global\n"
		"\tshaderop DefaultPathTracing\n"
		"}\n"
		"bdpt_pel_rasterizer\n"
		"{\n"
		"\tmax_eye_depth 2\n"
		"\tmax_light_depth 2\n"
		"\tsamples 64\n"
		"\tpixel_filter box\n"
		"\toidn_denoise FALSE\n"
		"}\n"
		"file_rasterizeroutput\n"
		"{\n"
		"\tpattern rendered/dl210_box_pel\n"
		"\ttype EXR\n"
		"\tbpp 32\n"
		"\tcolor_space Rec709RGB_Linear\n"
		"}\n";

	std::string sceneNM = kEnclosedBoxSceneTemplate;
	sceneNM +=
		"standard_shader\n"
		"{\n"
		"\tname global\n"
		"\tshaderop DefaultPathTracing\n"
		"}\n"
		"bdpt_spectral_rasterizer\n"
		"{\n"
		"\tmax_eye_depth 2\n"
		"\tmax_light_depth 2\n"
		"\tsamples 64\n"
		"\tnum_wavelengths 160\n"
		"\tspectral_samples 1\n"
		"\tpixel_filter box\n"
		"\toidn_denoise FALSE\n"
		"}\n"
		"file_rasterizeroutput\n"
		"{\n"
		"\tpattern rendered/dl210_box_spectral\n"
		"\ttype EXR\n"
		"\tbpp 32\n"
		"\tcolor_space Rec709RGB_Linear\n"
		"}\n";

	const double meanPel = RenderMean( scenePel );
	const double meanNM  = RenderMean( sceneNM );

	std::cout << "  Enclosed box at max_eye_depth 2, max_light_depth 2 (64 spp):" << std::endl;
	std::cout << "    Pel BDPT mean radiance:      " << meanPel << std::endl;
	std::cout << "    Spectral BDPT mean radiance: " << meanNM << std::endl;

	Check( meanPel > 0.0, "Pel BDPT render produced non-zero radiance" );
	Check( meanNM  > 0.0, "Spectral BDPT render produced non-zero radiance" );

	if( meanPel > 0.0 && meanNM > 0.0 ) {
		const double ratio = meanPel / meanNM;
		const double relDiff = std::fabs( meanPel - meanNM ) / meanNM * 100.0;
		std::cout << "    Pel / Spectral ratio:        " << ratio << " (rel diff " << relDiff << "%)" << std::endl;

		// Post-fix: Pel and Spectral BDPT agree within 8% Monte Carlo tolerance.
		// Pre-fix: Pel BDPT runs up to 66 surface vertices and exceeds Spectral BDPT by > 15-30%.
		Check( relDiff < 8.0,
			"DL-210: Pel BDPT and Spectral BDPT agree on mean radiance within 8% at max_eye_depth 2" );
	}
}

int main()
{
	std::cout << "=== BDPTEyeDepthConsistencyTest ===" << std::endl;

	TestEyeSubpathBounceCap();
	TestEnclosedCornellBoxDepthParity();

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;

	return failCount == 0 ? 0 : 1;
}
