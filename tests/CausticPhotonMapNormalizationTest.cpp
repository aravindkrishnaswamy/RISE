//////////////////////////////////////////////////////////////////////
//
//  CausticPhotonMapNormalizationTest.cpp - End-to-end calibration check
//    that the caustic photon map renders a refractive caustic at the
//    correct (unbiased) brightness.
//
//    PROPERTY: on a clean refractive caustic that an unbiased estimator
//    can reach, the caustic photon map's region mean must agree with
//    BDPT's (light-tracing splat) region mean.  BDPT is the trusted,
//    density-estimation-free reference here.
//
//    REGRESSION THIS GUARDS AGAINST:
//      The PhotonMap.h radiance estimate applies Jensen's Gaussian gather
//      filter (alpha=0.918, beta=1.953) on top of the 1/(PI r^2) density
//      normalization.  That filter's DISK AVERAGE is ~0.531, not 1, so
//      before the 2026-06-06 fix the estimate was biased LOW by ~1.88x —
//      refractive caustics rendered at ~0.50x the unbiased BDPT value
//      (docs/POOL_CAUSTICS_VCM_OVERCOUNT.md sec 11.5).  The fix divides
//      the estimate by the filter's analytic disk integral so the filter
//      only reshapes the kernel, never rescales its energy.  If that
//      normalization is dropped or the filter constants change without
//      updating it, this test's mean ratio collapses back toward ~0.53
//      and the check below fails.
//
//    SCENE: flat dielectric water sheet at y=2 over a Lambertian floor at
//    y=0, a spot light at y=8, pinhole camera UNDER the water (y=1.8)
//    looking straight down.  The eye rays never cross the water surface,
//    so (a) there is no surface reflection in the line of sight and (b)
//    BDPT's light-tracing splat (light -> water -> floor -> camera, both
//    endpoints under the water) forms the caustic and is unbiased.  ior
//    1.33 keeps the caustic gentle, so the photon map's finite-radius
//    boundary bias is negligible and the calibration scale is what the
//    comparison isolates.
//
//    APPROACH: mirrors VCMStrategyBalanceTest — render the photon-map and
//    BDPT versions of the same scene, capture the radiance buffer in
//    memory via a custom IRasterizerOutput, and assert the per-channel
//    means agree within tolerance.
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <vector>
#include <cmath>
#include <string>
#include <algorithm>
#ifdef _WIN32
	#include <process.h>		// _getpid()
	#define getpid _getpid
#else
	#include <unistd.h>			// getpid()
#endif

#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/Color/Color_Template.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

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

//////////////////////////////////////////////////////////////////////
// CapturingRasterizerOutput / ImageStats — same in-memory capture as
// VCMStrategyBalanceTest: grab the linear-radiance PEL buffer the
// rasterizer produced, independent of any file_rasterizeroutput sink.
//////////////////////////////////////////////////////////////////////
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

struct ImageStats
{
	double mean[3];
	bool   valid;
};

static ImageStats ComputeStats( const CapturingRasterizerOutput& cap )
{
	ImageStats s{};
	if( cap.pixels.empty() ) return s;

	double sum[3] = { 0, 0, 0 };
	for( const RISEColor& c : cap.pixels ) {
		sum[0] += c.base.r;
		sum[1] += c.base.g;
		sum[2] += c.base.b;
	}
	for( int c = 0; c < 3; c++ ) {
		s.mean[c] = sum[c] / double(cap.pixels.size());
	}
	s.valid = true;
	return s;
}

static std::string WriteSceneToTempFile( const char* sceneText, const char* tag )
{
	char path[512];
	std::snprintf( path, sizeof(path),
		"/tmp/caustic_pm_norm_%s_%d.RISEscene",
		tag, static_cast<int>(::getpid()) );

	std::ofstream ofs( path );
	if( !ofs.is_open() ) return std::string();
	ofs << sceneText;
	ofs.close();
	return std::string( path );
}

static ImageStats RenderAndComputeStats( const char* scenePath )
{
	ImageStats result{};

	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) return result;

	if( !pJob->LoadAsciiScene( scenePath ) ) {
		safe_release( pJob );
		return result;
	}

	pJob->RemoveRasterizerOutputs();
	CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "test capture output" );
	pJob->GetRasterizer()->AddRasterizerOutput( pCap );

	const bool bRendered = pJob->Rasterize();
	if( !bRendered ) {
		safe_release( pCap );
		safe_release( pJob );
		return result;
	}

	result = ComputeStats( *pCap );

	safe_release( pCap );
	safe_release( pJob );
	return result;
}

//////////////////////////////////////////////////////////////////////
// Scene fragments — camera-under-water refractive caustic.
//////////////////////////////////////////////////////////////////////

static const char* kSceneCommon =
	"film\n{\n\twidth 48\n\theight 48\n}\n\n"
	"pinhole_camera\n{\n"
	"\tlocation 0 1.8 0.01\n\tlookat 0 0 0\n\tup 0 0 -1\n\tfov 40\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_floor\n\tcolor 0.5 0.5 0.5\n}\n\n"
	"lambertian_material\n{\n\tname floor_mat\n\treflectance pnt_floor\n}\n\n"
	"clippedplane_geometry\n{\n\tname floor_g\n"
	"\tpta -30 0 -30\n\tptb -30 0 30\n\tptc 30 0 30\n\tptd 30 0 -30\n}\n\n"
	"standard_object\n{\n\tname floorobj\n\tgeometry floor_g\n\tmaterial floor_mat\n}\n\n"
	"dielectric_material\n{\n\tname water_mat\n\ttau 1.0 1.0 1.0\n\tior 1.33\n"
	"\tscattering 10000\n\thenyey-greenstein FALSE\n}\n\n"
	"clippedplane_geometry\n{\n\tname water_g\n"
	"\tpta -30 0 -30\n\tptb -30 0 30\n\tptc 30 0 30\n\tptd 30 0 -30\n}\n\n"
	"standard_object\n{\n\tname waterobj\n\tgeometry water_g\n\tposition 0 2 0\n\tmaterial water_mat\n}\n\n"
	"spot_light\n{\n\tname sun\n\tposition 0 8 0\n\ttarget 0 0 0\n"
	"\tcolor 1 1 1\n\tpower 300.0\n\tinner 30\n\touter 65\n}\n";

// Caustic photon map (estimator under test).  DefaultDirectLighting is
// present but contributes ~0 on the floor (the shadow ray to the spot is
// blocked by the specular water), so the caustic photon map supplies the
// floor illumination.
static const char* kRasterizerPhotonMap =
	"advanced_shader\n{\n\tname global\n"
	"\tshaderop DefaultDirectLighting 1 100 +\n"
	"\tshaderop DefaultRefraction 1 100 +\n"
	"\tshaderop DefaultReflection 1 100 +\n"
	"\tshaderop DefaultCausticPelPhotonMap 1 100 +\n}\n\n"
	"pixelpel_rasterizer\n{\n\tmax_recursion 8\n\tsamples 16\n\tlum_samples 4\n"
	"\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"caustic_pel_photonmap\n{\n\tnum 600000\n\tmax_recursion 10\n\tmin_importance 0.001\n"
	"\tpower_scale 1.0\n\tbranch TRUE\n\treflect TRUE\n\trefract TRUE\n}\n\n"
	"caustic_pel_gather\n{\n\tmax_photons 120\n}\n\n"
	"file_rasterizeroutput\n{\n\tpattern /tmp/caustic_pm_norm_pm_unused\n"
	"\ttype PNG\n\tbpp 8\n\tcolor_space sRGB\n}\n";

// BDPT (unbiased light-tracing-splat reference).
static const char* kRasterizerBDPT =
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"bdpt_pel_rasterizer\n{\n\tmax_eye_depth 10\n\tmax_light_depth 10\n\tsamples 320\n"
	"\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"file_rasterizeroutput\n{\n\tpattern /tmp/caustic_pm_norm_bdpt_unused\n"
	"\ttype PNG\n\tbpp 8\n\tcolor_space sRGB\n}\n";

//////////////////////////////////////////////////////////////////////
// The fixed estimate lands within a few percent of BDPT at ior 1.33;
// the pre-fix bug renders at ~0.53x.  A 22% band cleanly separates the
// two while tolerating the modest MC noise of these small/low-sample
// renders.
//////////////////////////////////////////////////////////////////////
static const double kMeanTol = 0.22;

static void RunCausticCalibrationTest()
{
	std::cout << "Testing caustic-photon-map vs BDPT: refractive caustic (ior 1.33)" << std::endl;

	const std::string pmScene   = std::string("RISE ASCII SCENE 6\n") + kRasterizerPhotonMap + kSceneCommon;
	const std::string bdptScene = std::string("RISE ASCII SCENE 6\n") + kRasterizerBDPT       + kSceneCommon;

	const std::string pmPath   = WriteSceneToTempFile( pmScene.c_str(),   "pm"   );
	const std::string bdptPath = WriteSceneToTempFile( bdptScene.c_str(), "bdpt" );

	if( pmPath.empty() || bdptPath.empty() ) {
		Check( false, "temp scene file write" );
		return;
	}

	const ImageStats bdpt = RenderAndComputeStats( bdptPath.c_str() );
	const ImageStats pm   = RenderAndComputeStats( pmPath.c_str()   );

	std::remove( pmPath.c_str() );
	std::remove( bdptPath.c_str() );

	Check( bdpt.valid, "BDPT render produced output" );
	Check( pm.valid,   "photon-map render produced output" );
	if( !bdpt.valid || !pm.valid ) return;

	const double brightness = bdpt.mean[0] + bdpt.mean[1] + bdpt.mean[2];
	Check( brightness > 1e-3, "BDPT caustic is non-trivially bright" );
	if( brightness <= 1e-3 ) return;

	// luminance ratio (gray scene -> channels equal; average for stability)
	const double pmL   = ( pm.mean[0]   + pm.mean[1]   + pm.mean[2]   ) / 3.0;
	const double bdptL = ( bdpt.mean[0] + bdpt.mean[1] + bdpt.mean[2] ) / 3.0;
	const double ratio = ( bdptL > 0 ) ? pmL / bdptL : 0.0;

	std::cout << "    BDPT mean L=" << bdptL
	          << "   photon-map mean L=" << pmL
	          << "   ratio=" << ratio << std::endl;

	const bool ok = std::fabs( ratio - 1.0 ) <= kMeanTol;
	Check( ok, "caustic photon map within 22% of unbiased BDPT (guards the "
	           "Gaussian-filter normalization; pre-fix bug rendered ~0.53x)" );
	if( !ok ) {
		std::cout << "    ratio " << ratio << " outside [" << (1.0 - kMeanTol)
		          << ", " << (1.0 + kMeanTol) << "] — caustic photon-map "
		             "calibration regressed (filter normalization?)" << std::endl;
	}
}

int main()
{
	std::cout << "=== CausticPhotonMapNormalizationTest ===" << std::endl;

	RunCausticCalibrationTest();

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;

	return failCount == 0 ? 0 : 1;
}
