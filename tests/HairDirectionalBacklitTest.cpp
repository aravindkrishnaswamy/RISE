//////////////////////////////////////////////////////////////////////
//
//  HairDirectionalBacklitTest.cpp - Residual wave 2 item D
//    (docs/HAIR_FUR_DESIGN.md section 4.1 / HairBSDF.h section 5's
//    "REMAINING SIBLING" entry, now RESOLVED): a focused render-level
//    regression guard for `DirectionalLight::ComputeDirectLighting{,NM}`'s
//    full-sphere NEE fix.
//
//  WHY THIS EXISTS AS ITS OWN FILE.  `tests/HairRenderTest.cpp` already
//    carries the equivalent guard for a POINT light (test 4b, the
//    full-sphere NEE regression for `omni_light`) -- but it is
//    off-limits for this slice (a concurrent read-only reviewer audits
//    the commit that landed `IMaterial::ScattersFullSphere()`, and
//    HairRenderTest.cpp is explicitly in its read set).  `ILight::
//    ComputeDirectLighting`'s `bFullSphereReceiver` parameter reaches
//    `DirectionalLight` through a DIFFERENT code path than the delta-
//    position (point/spot) row test 4b exercises -- LightSampler's
//    Step-1 zero-exitance sweep, which only ever holds ambient and
//    directional lights (see LightSampler.cpp's Step 1: it iterates
//    lights with `radiantExitance() == 0`, and point/spot lights never
//    are) -- so it needed its own guard, not a copy of 4b's.
//
//  HARNESS PATTERN: copied from EnvLightBalanceTest.cpp / HairRenderTest.cpp
//    (the prescribed precedent) -- scene text assembled as RISE ASCII
//    SCENE 7 chunks, written to a temp file, loaded via
//    IJobPriv::LoadAsciiSceneViaCst (the real CST path), rendered, and
//    captured into memory through a CapturingRasterizerOutput.
//    `oidn_denoise FALSE` throughout for the same reason HairRenderTest
//    gives (denoising would conflate the estimator's own bookkeeping
//    with OIDN's albedo/normal-guided prefilter).
//
//  TWO CHECKS:
//
//    1. A groom lit ONLY by a `directional_light` placed BEHIND it
//       (camera at +Z, light direction (0,0,-1) -- "the light is in
//       the -Z direction from every point", per SCENE_CONVENTIONS.md's
//       from-surface-to-light convention) must read a real, non-near-
//       zero luminance.  Every camera-visible strand is lit only
//       THROUGH the fibre (TT/TTs transmissive transport below the
//       shading normal) -- the identical configuration HairRenderTest's
//       point-light test 4b probes, just with the light at infinity
//       instead of a finite position.
//
//    2. A LAMBERTIAN CONTROL -- the same camera, the same backlighting
//       directional_light, a plain sphere with `lambertian_material`
//       instead of the groom.  `IMaterial::ScattersFullSphere()` is
//       false for Lambertian, so `bFullSphereReceiver` is false at this
//       light's call site for this material and the sphere must stay
//       dark (correctly -- an ordinary opaque material genuinely
//       receives nothing from directly behind it).  This is the
//       guard against the fix leaking into materials it was never
//       meant to touch: a stray unconditional `fabs` would light this
//       control up too.
//
//  RED-PROVE (performed manually this session; see `kMinGroomLuminance`'s
//    own comment below for the full number set): reverting
//    `DirectionalLight::ComputeDirectLighting{,NM}`'s `fDot` to the
//    unconditional signed cosine (i.e. as if `bFullSphereReceiver` were
//    always false), rebuilding, and re-running THIS EXACT test dropped
//    check 1's groom luminance from 0.0523975 to 0.00263629 -- a 19.9x
//    drop, driven only by whatever silhouette/edge grazing light leaks
//    in from directions where the OLD signed `fDot > 0` still held for
//    some sampled wi.  This is the same `cosSurface <= 0` rejection
//    HairRenderTest's test 4b documents for the point-light case, just
//    reached through Step 1 instead of the light-table row.  The
//    Lambertian control (check 2) read exactly 0 in BOTH builds, as
//    expected by construction: its material never sets
//    `ScattersFullSphere()`, so the flag was never doing anything for
//    it either way.
//
//  RUNTIME.  Small on purpose (24x24, 700 strands, 4 segments, 96 spp
//    for the groom; 24x24, 32 spp for the single-sphere control) --
//    two PT renders total, both well under the 30s budget this file is
//    asked to keep.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <vector>
#include <cmath>
#include <string>
#include <sstream>
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
// Harness -- shape copied from HairRenderTest.cpp / EnvLightBalanceTest.cpp.
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
	double luminance;	// mean(mean[0..2])
	bool   valid;
};

static ImageStats ComputeStats( const CapturingRasterizerOutput& cap )
{
	ImageStats s{};
	if( cap.pixels.empty() ) {
		return s;
	}

	double sum[3] = { 0, 0, 0 };
	for( const RISEColor& c : cap.pixels ) {
		sum[0] += c.base.r;
		sum[1] += c.base.g;
		sum[2] += c.base.b;
	}
	for( int c = 0; c < 3; c++ ) s.mean[c] = sum[c] / double(cap.pixels.size());
	s.luminance = (s.mean[0] + s.mean[1] + s.mean[2]) / 3.0;
	s.valid = true;
	return s;
}

static std::string WriteSceneToTempFile( const std::string& sceneText, const char* tag )
{
	char path[512];
	std::snprintf( path, sizeof(path),
		"/tmp/hair_directional_backlit_test_%s_%d.RISEscene",
		tag, static_cast<int>(::getpid()) );

	std::ofstream ofs( path );
	if( !ofs.is_open() ) {
		return std::string();
	}
	ofs << sceneText;
	ofs.close();
	return std::string( path );
}

static ImageStats RenderAndComputeStats( const std::string& sceneText, const char* tag )
{
	ImageStats result{};

	const std::string path = WriteSceneToTempFile( sceneText, tag );
	if( path.empty() ) {
		return result;
	}

	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) {
		std::remove( path.c_str() );
		return result;
	}

	if( !pJob->LoadAsciiSceneViaCst( path.c_str() ) ) {
		std::remove( path.c_str() );
		safe_release( pJob );
		return result;
	}

	pJob->RemoveRasterizerOutputs();

	CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "test capture output" );
	pJob->GetRasterizer()->AddRasterizerOutput( pCap );

	const bool bRendered = pJob->Rasterize();
	if( bRendered ) {
		result = ComputeStats( *pCap );
	}

	std::remove( path.c_str() );
	safe_release( pCap );
	safe_release( pJob );
	return result;
}

static std::string AssembleScene( const std::string& body )
{
	return std::string( "RISE ASCII SCENE 7\n" ) + body;
}

//////////////////////////////////////////////////////////////////////
// Scene 1: a groom lit ONLY by a directional_light behind it.
// Camera at +Z looking at the origin; `direction 0 0 -1` means "the
// light is in the -Z direction from every point" (SCENE_CONVENTIONS.md:
// direction is FROM surface TO light) -- i.e. behind every camera-
// facing strand, mirroring HairRenderTest test 4b's point-light
// configuration one-for-one, just with the light at infinity.
//////////////////////////////////////////////////////////////////////
static std::string GroomBacklitByDirectionalLight(
	unsigned int width, unsigned int height,
	unsigned int count, unsigned int segments,
	unsigned int samples, double power )
{
	std::ostringstream ss;
	ss <<
		"standard_shader\n{\n\tname global\n\tshaderop DefaultDirectLighting\n}\n\n"
		"film\n{\n\twidth " << width << "\n\theight " << height << "\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 0 3.2\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 34.0\n}\n\n"
		"directional_light\n{\n\tname backlight\n\tdirection 0 0 -1\n\tcolor 1.0 1.0 1.0\n\tpower "
			<< power << "\n}\n\n"
		"scalar_painter\n{\n\tname pnt_tier\n\tvalue 0.9\n}\n\n"
		"hair_material\n{\n\tname mat_hair\n\teumelanin pnt_tier\n}\n\n"
		"sphere_geometry\n{\n\tname scalp\n\tradius 1.0\n}\n\n"
		"hair_geometry\n{\n\tname groom\n\tbase_geometry scalp\n\tcount " << count
		<< "\n\tsegments " << segments << "\n\tlength 0.24\n\twidth_root 0.035\n\twidth_tip 0.012"
		<< "\n\tbase_detail 16\n\tseed 3\n}\n\n"
		"standard_object\n{\n\tname groom_obj\n\tgeometry groom\n\tmaterial mat_hair\n\tposition 0 0 0\n}\n\n"
		"pathtracing_pel_rasterizer\n{\n\tsamples " << samples << "\n\trr_min_depth 8\n\toidn_denoise FALSE\n}\n\n"
		"file_rasterizeroutput\n{\n\tpattern /tmp/hair_directional_backlit_test_unused\n\ttype PNG\n\tbpp 8\n\tcolor_space sRGB\n}\n";
	return ss.str();
}

//////////////////////////////////////////////////////////////////////
// Scene 2: the LAMBERTIAN CONTROL -- identical camera and backlighting
// directional_light, a plain sphere with lambertian_material instead
// of the groom.  Must stay dark: IMaterial::ScattersFullSphere() is
// false for Lambertian, so this scene's reading is UNAFFECTED by the
// fix either way, and a stray unconditional fabs would light it up.
//////////////////////////////////////////////////////////////////////
static std::string LambertianControlBacklit(
	unsigned int width, unsigned int height,
	unsigned int samples, double power )
{
	std::ostringstream ss;
	ss <<
		"standard_shader\n{\n\tname global\n\tshaderop DefaultDirectLighting\n}\n\n"
		"film\n{\n\twidth " << width << "\n\theight " << height << "\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 0 3.2\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 34.0\n}\n\n"
		"directional_light\n{\n\tname backlight\n\tdirection 0 0 -1\n\tcolor 1.0 1.0 1.0\n\tpower "
			<< power << "\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_white\n\tcolor 0.8 0.8 0.8\n}\n\n"
		"lambertian_material\n{\n\tname mat_lambert\n\treflectance pnt_white\n}\n\n"
		"sphere_geometry\n{\n\tname ctrl_sphere\n\tradius 1.0\n}\n\n"
		"standard_object\n{\n\tname ctrl_obj\n\tgeometry ctrl_sphere\n\tmaterial mat_lambert\n\tposition 0 0 0\n}\n\n"
		"pathtracing_pel_rasterizer\n{\n\tsamples " << samples << "\n\trr_min_depth 8\n\toidn_denoise FALSE\n}\n\n"
		"file_rasterizeroutput\n{\n\tpattern /tmp/hair_directional_backlit_test_unused\n\ttype PNG\n\tbpp 8\n\tcolor_space sRGB\n}\n";
	return ss.str();
}

//////////////////////////////////////////////////////////////////////
// BOUNDS.
//
//   * `kMinGroomLuminance` -- the MONEY assertion.  RED-PROVED this
//     session by reverting `DirectionalLight::ComputeDirectLighting{,NM}`'s
//     `fDot` to the unconditional signed cosine (i.e. as if
//     `bFullSphereReceiver` were always false), rebuilding, and
//     re-running this exact scene at this exact seed: fixed reading
//     0.0523975, reverted reading 0.00263629 -- a 19.9x drop, driven
//     entirely by whatever silhouette/edge grazing light leaks in from
//     directions where the OLD signed `fDot > 0` still held for some
//     sampled wi.  This bound sits at the geometric mean of the two
//     (~0.0118, rounded), giving ~4.4x headroom on the failing side and
//     ~4.4x headroom on the passing side -- tight enough to catch the
//     exact regression this test exists to guard, loose enough not to
//     be noise-flaky (this scene's PT sampling is deterministic --
//     seeded groom + seeded QMC -- so repeat runs at a fixed build read
//     identically; the 4.4x margins are for a future scene-parameter
//     change, not run-to-run MC spread).
//   * `kMaxLambertianControlLuminance` -- the control must stay near
//     the render's own black-background level (no `radiance_map`, so
//     an unlit pixel is exactly 0); a small allowance covers MC noise
//     from the handful of grazing-angle pixels at the sphere's own
//     silhouette.  Measured exactly 0 both with and without the fix
//     (unaffected by construction -- see the file header).
//////////////////////////////////////////////////////////////////////
static const double kMinGroomLuminance             = 0.0118;
static const double kMaxLambertianControlLuminance  = 0.01;

static void TestDirectionalBacklitGroom()
{
	std::cout << "=== Directional-light backlit groom (full-sphere NEE guard, item D) ===" << std::endl;

	const unsigned int W = 24, H = 24, COUNT = 700, SEG = 4, SAMPLES = 96;
	const double power = 3.0;

	const ImageStats groom = RenderAndComputeStats(
		AssembleScene( GroomBacklitByDirectionalLight( W, H, COUNT, SEG, SAMPLES, power ) ),
		"groom" );

	Check( groom.valid, "directional backlit groom: render produced output" );
	if( !groom.valid ) return;

	std::cout << "  groom luminance = " << groom.luminance << std::endl;

	Check( groom.luminance >= kMinGroomLuminance,
		"MONEY ASSERTION -- a directional_light placed BEHIND the groom still lights it "
		"(pre-fix, DirectionalLight::ComputeDirectLighting's below-horizon fDot<=0 gate rejects "
		"the fibre transmission this scene's transport runs through, dropping this reading "
		"19.9x -- red-proved this session by reverting the fabs; see the bounds comment above)" );

	const ImageStats control = RenderAndComputeStats(
		AssembleScene( LambertianControlBacklit( W, H, 32, power ) ),
		"control" );

	Check( control.valid, "Lambertian control: render produced output" );
	if( !control.valid ) return;

	std::cout << "  Lambertian control luminance = " << control.luminance << std::endl;

	Check( control.luminance <= kMaxLambertianControlLuminance,
		"Lambertian control stays dark -- IMaterial::ScattersFullSphere() is false for an "
		"ordinary opaque material, so the fix does not leak into materials it was never "
		"meant to touch" );
}

int main( int /*argc*/, char* /*argv*/[] )
{
	std::cout << "HairDirectionalBacklitTest -- DirectionalLight full-sphere NEE regression guard (residual wave 2 item D)" << std::endl;

	TestDirectionalBacklitGroom();

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;

	return failCount == 0 ? 0 : 1;
}
