//////////////////////////////////////////////////////////////////////
//
//  HairRenderTest.cpp - Slice E (Phase-1 closeout) render-level
//    regression suite for the Chiang hair/fur BCSDF + `hair_geometry`
//    groom (docs/HAIR_FUR_DESIGN.md).  Everything below this line is
//    an END-TO-END check: real scenes, loaded through the CST scene
//    parser exactly as a user's .RISEscene would be, rendered through
//    a real rasterizer, and the OUTPUT IMAGE inspected -- as opposed
//    to HairBSDFTest.cpp (direct BCSDF sample/eval/pdf calls),
//    HairGeometryTest.cpp (intersection/BVH unit checks), and
//    HairGeneratorTest.cpp (groom-recipe behavioural checks via the
//    direct C++ API).  Those three prove the pieces are individually
//    correct; this file proves they compose correctly through parsing
//    + geometry generation + material evaluation + integration.
//
//  HARNESS PATTERN: copied from EnvLightBalanceTest.cpp (the
//    prescribed precedent) rather than built via direct IJob::AddXxx
//    calls, matching what HairGeneratorTest.cpp's ParseBodyInto /
//    ParseBodyCapturing already do for hair scenes specifically: scene
//    text is assembled as ordinary RISE ASCII SCENE 7 chunks in C++
//    string literals, written to a temp file, loaded via
//    IJobPriv::LoadAsciiSceneViaCst (the real CST path -- the same one
//    `bin/rise` uses), rendered, and captured into memory through a
//    CapturingRasterizerOutput.  This exercises the ACTUAL parser
//    (hair_geometry / hair_material chunk descriptors) rather than
//    bypassing it, which is the point of a render-level suite sitting
//    above the unit-level ones.
//
//  OIDN DENOISE IS EXPLICITLY DISABLED (`oidn_denoise FALSE`) in every
//    rasterizer chunk below.  IRasterizerOutput::OutputDenoisedImage's
//    default implementation forwards to OutputImage with the
//    POST-DENOISE pixels whenever OIDN denoising ran (see
//    src/Library/Interfaces/IRasterizerOutput.h) -- so a
//    CapturingRasterizerOutput that only overrides OutputImage (this
//    file's, and EnvLightBalanceTest's) would silently capture the
//    DENOISED image, not the raw path-traced one, unless denoising is
//    turned off.  For the furnace / melanin / hwss assertions below,
//    which are testing the BCSDF's and integrator's own energy
//    bookkeeping, mixing in OIDN's albedo/normal-guided prefilter
//    would conflate two very different error sources -- and
//    docs/HAIR_FUR_DESIGN.md section 6.5 separately flags OIDN-on-hair
//    as its own open measurement question, not something to fold in
//    here by accident.
//
//  ASSERTION GROUPS (each documents its own tolerance rationale and
//    what a failure would mean):
//    1. White furnace (RGB, spectral hwss=false, spectral hwss=true):
//       sigma_a=0 groom in a uniform env must read at the env's own
//       luminance -- the render-level twin of HairBSDFTest's SPF-side
//       furnace identity.
//    2. HWSS invariant: a melanin groom rendered spectrally with
//       hwss=true must agree with hwss=false to within MC noise (the
//       reference-free invariant from the integrator matrix arc,
//       CLAUDE.md "High-Value Facts").
//    3. Melanin ladder: mean luminance strictly decreases across
//       eumelanin 0.3 -> 1.3 -> 4.0 (three separate single-groom
//       renders, same framing, differing only in eumelanin).
//    4. PT-vs-BDPT sanity: one small hair scene, a documented LOOSE
//       tolerance citing the section 6.2 reciprocity caveat (the
//       Chiang model as implemented is not exactly reciprocal, so
//       BDPT -- whose MIS weights assume reciprocity -- is expected to
//       agree with PT well but not to MC-exactness).
//
//  MEASUREMENT BASELINE.  Every number quoted below was measured on
//    this machine AFTER the env-MIS fix in
//    src/Library/Shaders/PathTracingIntegrator.cpp (the surface-escape
//    env block in `IntegrateFromHitTemplated`).  Before that fix the
//    RGB and non-HWSS-spectral path tracers added every BSDF-sampled
//    environment hit at MIS weight 1 on top of a correctly
//    MIS-weighted env-NEE, so any env-lit PT render read
//    `1 + w_nee` times too bright -- +17.7 % on a Lambertian white
//    furnace, +4.0 % on this file's groom furnace.  If you are
//    bisecting a pre-fix commit, expect every furnace number here to
//    read HIGH by roughly that much.
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
#include <algorithm>
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
// CapturingRasterizerOutput — identical in shape to
// EnvLightBalanceTest's.  Deliberately does NOT override
// OutputPreDenoisedImage / OutputDenoisedImage; see the file header
// for why every scene below sets `oidn_denoise FALSE` instead.
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
	double luminance;	// mean(mean[0..2]) -- the scalar used by most checks below
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
		"/tmp/hair_render_test_%s_%d.RISEscene",
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

//////////////////////////////////////////////////////////////////////
// Scene assembly helpers.
//
// All scenes share the same shape: a single sphere-grown groom, a
// uniform-radiance environment (the furnace enclosure / neutral
// light), and one rasterizer.  `count`/`segments`/resolution are kept
// small throughout to hold the whole suite's wall time down (measured
// totals are reported in the file-level comment at the bottom).
//////////////////////////////////////////////////////////////////////

// One groom, one material tier, in a uniform L=1.0 environment.
// `tierParam` is "sigma_a" or "eumelanin"; `tierValue` is its value.
// `count`/`segments` control groom density/cost; `width`/`height` the
// film.  Returns everything EXCEPT the rasterizer + file output chunk
// so callers can append whichever rasterizer they need.
static std::string GroomInEnvCommon(
	const char* tierParam, double tierValue,
	unsigned int width, unsigned int height,
	unsigned int count, unsigned int segments,
	unsigned int seed )
{
	std::ostringstream ss;
	ss <<
		"standard_shader\n{\n\tname global\n\tshaderop DefaultDirectLighting\n}\n\n"
		"film\n{\n\twidth " << width << "\n\theight " << height << "\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 0 3.2\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 34.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_env\n\tcolor 1.0 1.0 1.0\n}\n\n"
		"scalar_painter\n{\n\tname pnt_tier\n\tvalue " << tierValue << "\n}\n\n"
		"hair_material\n{\n\tname mat_hair\n\t" << tierParam << " pnt_tier\n}\n\n"
		"sphere_geometry\n{\n\tname scalp\n\tradius 1.0\n}\n\n"
		"hair_geometry\n{\n\tname groom\n\tbase_geometry scalp\n\tcount " << count
		<< "\n\tsegments " << segments << "\n\tlength 0.24\n\twidth_root 0.035\n\twidth_tip 0.012"
		<< "\n\tbase_detail 22\n\tseed " << seed << "\n}\n\n"
		"standard_object\n{\n\tname groom_obj\n\tgeometry groom\n\tmaterial mat_hair\n\tposition 0 0 0\n}\n\n";
	return ss.str();
}

// `maxGlossyBounce` == 0 means "leave it at the rasterizer default
// (UINT_MAX)".  Hair lobes are all tagged eRayReflection
// (docs/HAIR_FUR_DESIGN.md section 6.1 / HairBSDF.h section 5), so
// this is the one bucket that actually governs hair path length.
// `indirectClamp` == 0 means "leave it at the rasterizer default (0 =
// disabled)".  Test 4 uses a nonzero clamp on BOTH PT and BDPT to tame
// the heavy-tailed firefly variance BDPT's light-subpath connections
// show through hair's narrow lobes (empirically discovered while
// tuning that test -- see its own comment for the measured numbers);
// clamping identically on both sides is a standard, unbiased-in-mean-
// enough variance-reduction technique for a MEAN-level sanity check,
// not a correctness change.
//
// EVERY scene in this file is env-lit.  There is deliberately no
// "omit the radiance_map lines" switch: an earlier revision carried a
// `useEnv` parameter documented as "test 4 lights via an explicit
// light source instead", but no caller ever passed false and the
// point-lit configuration it described does not work -- see test 4's
// LIGHTING NOTE for the measured reason it was dropped rather than
// wired up.
static std::string RasterizerPTRgb( unsigned int samples, unsigned int rrMinDepth, unsigned int maxGlossyBounce = 0, double indirectClamp = 0.0 )
{
	std::ostringstream ss;
	ss <<
		"pathtracing_pel_rasterizer\n{\n"
		"\tsamples " << samples << "\n"
		"\trr_min_depth " << rrMinDepth << "\n"
		"\toidn_denoise FALSE\n";
	if( maxGlossyBounce > 0 ) ss << "\tmax_glossy_bounce " << maxGlossyBounce << "\n";
	if( indirectClamp > 0.0 ) ss << "\tindirect_clamp " << indirectClamp << "\n\tdirect_clamp " << indirectClamp << "\n";
	ss << "\tradiance_map pnt_env\n\tradiance_scale 1.0\n\tradiance_background TRUE\n";
	ss <<
		"}\n\n"
		"file_rasterizeroutput\n{\n\tpattern /tmp/hair_render_test_unused\n\ttype PNG\n\tbpp 8\n\tcolor_space sRGB\n}\n";
	return ss.str();
}

static std::string RasterizerPTSpectral( unsigned int samples, bool hwss, unsigned int rrMinDepth )
{
	std::ostringstream ss;
	ss <<
		"pathtracing_spectral_rasterizer\n{\n"
		"\tsamples " << samples << "\n"
		"\trr_min_depth " << rrMinDepth << "\n"
		"\toidn_denoise FALSE\n"
		"\tnmbegin 380\n\tnmend 720\n\tnum_wavelengths 8\n\tspectral_samples 1\n"
		"\thwss " << (hwss ? "true" : "false") << "\n"
		"\tradiance_map pnt_env\n\tradiance_scale 1.0\n\tradiance_background TRUE\n"
		"}\n\n"
		"file_rasterizeroutput\n{\n\tpattern /tmp/hair_render_test_unused\n\ttype PNG\n\tbpp 8\n\tcolor_space sRGB\n}\n";
	return ss.str();
}

static std::string RasterizerBDPTRgb( unsigned int samples, unsigned int maxEyeDepth, unsigned int maxLightDepth, double indirectClamp = 0.0 )
{
	std::ostringstream ss;
	ss <<
		"bdpt_pel_rasterizer\n{\n"
		"\tsamples " << samples << "\n"
		"\tmax_eye_depth " << maxEyeDepth << "\n\tmax_light_depth " << maxLightDepth << "\n"
		"\toidn_denoise FALSE\n";
	if( indirectClamp > 0.0 ) ss << "\tindirect_clamp " << indirectClamp << "\n\tdirect_clamp " << indirectClamp << "\n";
	ss << "\tradiance_map pnt_env\n\tradiance_scale 1.0\n\tradiance_background TRUE\n";
	ss <<
		"}\n\n"
		"file_rasterizeroutput\n{\n\tpattern /tmp/hair_render_test_unused\n\ttype PNG\n\tbpp 8\n\tcolor_space sRGB\n}\n";
	return ss.str();
}


static std::string AssembleScene( const std::string& common, const std::string& rasterizer )
{
	return std::string( "RISE ASCII SCENE 7\n" ) + common + rasterizer;
}

//////////////////////////////////////////////////////////////////////
// 1. White furnace -- RGB, spectral hwss=false, spectral hwss=true.
//
// WHAT THE IDEAL IS, AND WHY IT IS EXACTLY 1.0.  Rays that miss the
// groom read the env directly at exactly 1.0
// (`radiance_background TRUE`; verified -- an identical scene with the
// groom deleted renders 1.000000 in every pixel, min == max).  Rays
// that hit a strand enter a sigma_a == 0 fibre whose per-scatter
// throughput `kray` is exactly 1 by construction (HairBSDFTest's
// SPF-side furnace asserts that at 1e-9), so a correct estimator
// returns the env's own radiance no matter how many times the path
// scatters.  There is therefore NO "background dilution" or "AA
// quantization" slack to spend: every pixel should read 1.0, and any
// deviation is a real estimator error.
//
// AN EARLIER REVISION OF THIS COMMENT WAS WRONG.  It explained the
// then-measured +4.0 % (RGB) / +4.6 % (hwss=false) as background
// mixing plus silhouette quantization and set an 8 % band around it.
// That explanation is mathematically impossible -- a mean over
// {exactly-1.0 background, hair pixels} can only exceed 1.0 if hair
// pixels exceed 1.0 -- and the over-unity turned out to be a real
// defect in the RGB / non-HWSS-spectral path tracer's environment MIS
// (fixed in PathTracingIntegrator.cpp; see the MIS PARTNER RULE
// comment there).  Every BSDF-sampled env hit was being added at
// weight 1 on top of a correctly MIS-weighted env-NEE, so the two
// strategies summed to 1 + w_nee.  Diagnosis chain, for the record:
// filter ruled out (`pixel_filter box` moves the mean by 0.0001);
// noise ruled out (the excess is 1.0398 -> 1.0402 across a 64x spp
// ladder, 96 -> 6144); Russian roulette ruled out (1.0400 +/- 0.0002
// across rr_min_depth 1 -> 100000); env-importance-map resolution
// ruled out (identical at 64 and 1024).  The clincher was a Lambertian
// albedo-1 control on a plain sphere in the same furnace, which read
// 1.154 -- i.e. NOT hair-specific at all -- and whose surface-pixel
// excess matched the closed form INTEGRAL_H (cos/pi) w_nee dw =
// ln(17)/16 = 0.17708 (measured 0.17718) for envPdf = 1/(4 pi) under
// the power-2 heuristic.  With the fix the same Lambertian controls
// read 1.000073 (plane) and 0.999919 (sphere), and EnvLightBalanceTest's
// own albedo-0.5 env-only quad (closed form 0.5) moves from 0.5885 to
// 0.4999.
//
// WHAT IS MEASURED NOW (this machine, post-fix, four repeat runs of
// this binary; RISE's sampler is deterministic per pixel at a fixed
// sample count, so the residual spread below is thread scheduling, not
// a fresh seed):
//
//     RGB PT, samples=384        0.98834 / 0.98859 / 0.98867 / 0.98859
//     spectral hwss=false, 256   0.99489 / 0.99452 / 0.99078 / 0.99374
//     spectral hwss=true,  256   0.94718 / 0.94678 / 0.94714 / 0.94685
//
// RGB and hwss=false now sit 0.51-1.17 % UNDER 1.0.  That residual is
// itself understood and is NOT hair-specific either: LightSampler's
// env-NEE block gates on `cosEnv > 0`, so a full-sphere BSDF (hair's
// TT lobe transmits THROUGH the fibre) gets no NEE strategy at all for
// the below-normal half of the sphere, while the BSDF-sampling side
// still applies its w_bsdf < 1 there -- the two strategies sum to less
// than 1 for those directions.  Confirmed by construction: relaxing
// that single gate to `fabs(cosEnv)` (which is a no-op for ordinary
// BRDFs, whose `value()` returns 0 below the hemisphere -- the
// Lambertian controls stay at 1.0001) moves this furnace from 0.98863
// to 1.00181.  That fix is NOT applied: it needs a per-IBSDF audit of
// below-hemisphere `value()` behaviour plus the matching light-table
// gate, which is a separate workstream from slice E.
//
// TOLERANCES.  `kFurnaceTol` (RGB + hwss=false) is 3 %: it clears the
// worst measured deficit (1.17 %) by 1.83pp, and still fails on the
// 2-3 % scale of a genuinely lost residual lobe (docs/HAIR_FUR_DESIGN.md
// phase-0's description of what this test guards) -- and, importantly,
// it now fails on the +4.0 % over-count above if that regresses.  The
// 8 % band the earlier revision used would have passed it.
//
// hwss=true gets its OWN, asymmetric band, because its 5.3 % deficit
// is a different, pre-existing problem that this test must name rather
// than absorb.  It is the HWSS spectral-bundle bias recorded in
// CLAUDE.md's env-IBL arc ("hwss=true env-only is already 18 % under
// PT at the disc-area baseline"): same direction, hair-independent,
// and demonstrably untouched by the env-MIS fix above (the HWSS loop
// in PathTracingIntegrator.cpp tests the global radiance map FIRST and
// so never had the branch-order bug -- which is precisely why
// hwss=true read 0.947 both before AND after the fix, while hwss=false
// moved 1.046 -> 0.993).  So:
//   * `kFurnaceHwssUnderTol` 9 % -- 3.7pp of headroom over the
//     measured 5.28-5.32 % deficit, pending the spectral-bundle work.
//   * `kFurnaceHwssOverTol` 2 % -- an OVER-unity reading is never
//     legitimate for a sigma_a == 0 furnace, so the upper half of the
//     band stays tight regardless.
// When the spectral-bundle bias is fixed, hwss=true should join the
// other two near 0.99-1.00 and this pair should collapse back to
// `kFurnaceTol`.
//////////////////////////////////////////////////////////////////////
static const double kFurnaceTarget       = 1.0;
static const double kFurnaceTol          = 0.03;	// RGB + hwss=false, see rationale above
static const double kFurnaceHwssUnderTol = 0.09;	// hwss=true lower bound (known spectral-bundle deficit)
static const double kFurnaceHwssOverTol  = 0.02;	// hwss=true upper bound (over-unity is never legitimate)

static void TestFurnace()
{
	std::cout << "=== 1. White furnace (render-level) ===" << std::endl;

	const unsigned int W = 40, H = 40, COUNT = 4000, SEG = 6, SEED = 5;
	const std::string common = GroomInEnvCommon( "sigma_a", 0.0, W, H, COUNT, SEG, SEED );

	{
		const ImageStats s = RenderAndComputeStats(
			AssembleScene( common, RasterizerPTRgb( 384, 8 ) ), "furnace_rgb" );
		Check( s.valid, "furnace RGB: render produced output" );
		if( s.valid ) {
			std::cout << "  RGB luminance = " << s.luminance << " (target " << kFurnaceTarget << ")" << std::endl;
			Check( std::fabs( s.luminance - kFurnaceTarget ) <= kFurnaceTol,
				"furnace RGB: mean luminance within tolerance of the env" );
		}
	}
	{
		const ImageStats s = RenderAndComputeStats(
			AssembleScene( common, RasterizerPTSpectral( 256, false, 8 ) ), "furnace_spec_hwss_off" );
		Check( s.valid, "furnace spectral (hwss=false): render produced output" );
		if( s.valid ) {
			std::cout << "  spectral hwss=false luminance = " << s.luminance << std::endl;
			Check( std::fabs( s.luminance - kFurnaceTarget ) <= kFurnaceTol,
				"furnace spectral (hwss=false): mean luminance within tolerance of the env" );
		}
	}
	{
		const ImageStats s = RenderAndComputeStats(
			AssembleScene( common, RasterizerPTSpectral( 256, true, 8 ) ), "furnace_spec_hwss_on" );
		Check( s.valid, "furnace spectral (hwss=true): render produced output" );
		if( s.valid ) {
			std::cout << "  spectral hwss=true luminance = " << s.luminance
				<< "  (expected ~0.947 -- the known HWSS spectral-bundle deficit)" << std::endl;
			// Asymmetric band: the deficit is the documented pre-existing
			// HWSS spectral-bundle bias (see the section comment); an
			// over-unity reading would be a fresh energy-creation bug and
			// is held to the tight side.
			Check( s.luminance >= kFurnaceTarget - kFurnaceHwssUnderTol,
				"furnace spectral (hwss=true): deficit no worse than the known spectral-bundle bias" );
			Check( s.luminance <= kFurnaceTarget + kFurnaceHwssOverTol,
				"furnace spectral (hwss=true): does not exceed the env (no energy created)" );
		}
	}
}

//////////////////////////////////////////////////////////////////////
// 2. HWSS invariant -- melanin groom, spectral PT, hwss=true vs
// hwss=false must agree within MC noise.  This is the same
// reference-free invariant the env-IBL integrator arc used
// (CLAUDE.md "High-Value Facts": "validate hair scenes with
// hwss=true == hwss=false as a reference-free invariant").
//
// Tolerance rationale: measured at samples=256 / 32x32 on a
// eumelanin=1.3 groom across repeated runs, POST the env-MIS fix (see
// the file header's MEASUREMENT BASELINE), across four repeat runs of
// this binary hwss=false luminance sits at 0.16891-0.17158 and
// hwss=true at 0.16044-0.16095 -- a relative difference of 4.71% /
// 5.91% / 6.16% / 6.48%.  (Pre-fix the same pair read 0.170-0.172 vs
// 0.160-0.161, i.e. 5.8-6.9%: the fix moved the hwss=false side only,
// exactly as expected since the HWSS loop never carried the
// branch-order bug.)  12% leaves ~1.85x headroom over the worst
// observed run while still catching the kind of collapse the
// env-IBL arc saw when HWSS was genuinely broken (order 50%+, per
// CLAUDE.md's "hwss=true env-only is already 18% under" and worse
// pre-fix numbers).  This section's residual and the furnace's
// hwss=true deficit are the SAME pre-existing spectral-bundle bias
// seen two ways.
//////////////////////////////////////////////////////////////////////
static const double kHwssTol = 0.12;

static void TestHwssInvariant()
{
	std::cout << "=== 2. HWSS invariant (melanin groom, spectral PT) ===" << std::endl;

	const unsigned int W = 32, H = 32, COUNT = 3000, SEG = 6, SEED = 9;
	const std::string common = GroomInEnvCommon( "eumelanin", 1.3, W, H, COUNT, SEG, SEED );

	const ImageStats sOff = RenderAndComputeStats(
		AssembleScene( common, RasterizerPTSpectral( 256, false, 8 ) ), "hwss_off" );
	const ImageStats sOn = RenderAndComputeStats(
		AssembleScene( common, RasterizerPTSpectral( 256, true, 8 ) ), "hwss_on" );

	Check( sOff.valid, "hwss=false: render produced output" );
	Check( sOn.valid,  "hwss=true: render produced output" );
	if( !sOff.valid || !sOn.valid ) return;

	std::cout << "  hwss=false luminance = " << sOff.luminance
		<< "   hwss=true luminance = " << sOn.luminance << std::endl;

	const double denom = std::fmax( sOff.luminance, 1e-6 );
	const double relDiff = std::fabs( sOn.luminance - sOff.luminance ) / denom;
	std::cout << "  relative difference = " << relDiff << std::endl;
	Check( relDiff <= kHwssTol, "hwss=true agrees with hwss=false within tolerance" );
}

//////////////////////////////////////////////////////////////////////
// 3. Melanin ladder -- mean luminance strictly decreasing across
// eumelanin 0.3 -> 1.3 -> 4.0.  Three independent single-groom
// renders (same framing/count/segments/seed, differing only in
// eumelanin) rather than one three-groom image, so the assertion is a
// plain whole-image mean comparison with no region-of-interest
// extraction needed.
//////////////////////////////////////////////////////////////////////
static void TestMelaninLadder()
{
	std::cout << "=== 3. Melanin ladder (RGB PT) ===" << std::endl;

	const unsigned int W = 32, H = 32, COUNT = 3000, SEG = 6, SEED = 9;
	const double ce[3] = { 0.3, 1.3, 4.0 };
	double lum[3];

	for( int i = 0; i < 3; i++ ) {
		const std::string common = GroomInEnvCommon( "eumelanin", ce[i], W, H, COUNT, SEG, SEED );
		char tag[64];
		std::snprintf( tag, sizeof(tag), "ladder_%d", i );
		const ImageStats s = RenderAndComputeStats(
			AssembleScene( common, RasterizerPTRgb( 256, 8 ) ), tag );

		char label[96];
		std::snprintf( label, sizeof(label), "ladder eumelanin=%.1f: render produced output", ce[i] );
		Check( s.valid, label );
		lum[i] = s.valid ? s.luminance : -1.0;
		std::cout << "  eumelanin=" << ce[i] << "  luminance=" << lum[i] << std::endl;
	}

	if( lum[0] < 0 || lum[1] < 0 || lum[2] < 0 ) return;

	Check( lum[0] > lum[1], "melanin ladder: eumelanin 0.3 brighter than 1.3" );
	Check( lum[1] > lum[2], "melanin ladder: eumelanin 1.3 brighter than 4.0" );
	Check( lum[0] > lum[2], "melanin ladder: eumelanin 0.3 brighter than 4.0" );
}

//////////////////////////////////////////////////////////////////////
// 4. PT-vs-BDPT sanity -- LOOSE tolerance, per docs/HAIR_FUR_DESIGN.md
// section 6.2 / HairBSDF.h section 5 ("NOT RECIPROCAL"): the Chiang
// model as implemented is not exactly reciprocal (near-field
// h-conditioning + cuticle-tilt sign convention), and RISE's BDPT MIS
// weights assume reciprocity, so bidirectional hair renders carry a
// small model-level bias PT does not.  Expect "good but not
// MC-exact" agreement, per the bdpt-vcm-mis-balance skill's standing
// PT-vs-X pattern -- NOT a tight tolerance the way EnvLightBalanceTest
// uses on ordinary (reciprocal) Lambertian scenes.
//
// LIGHTING NOTE -- THIS TEST IS ENV-LIT, AND THAT IS THE MEASURED
// CHOICE, NOT A LEFTOVER.  An earlier revision of this comment claimed
// env lighting had been REJECTED for this test (as compounding hair's
// non-reciprocity with the documented env+BDPT MIS-partition bias,
// CLAUDE.md "Env-IBL deficit") and that the test lit via an explicit
// light source instead -- while the code below shipped env lighting.
// The code was right and the comment was wrong; here is what a
// point-lit variant actually does, measured on this machine.
//
// A discrete `omni_light` version of this exact scene (same groom,
// same 256 spp / 32x32, no `radiance_map`) renders PT and BDPT
// 4.3x APART -- PT 0.0132 vs BDPT 0.0575 at one light power, PT 0.0773
// vs BDPT 0.3305 at another, i.e. the ratio is scale-invariant, so it
// is a systematic bias and not fireflies.  Root cause: LightSampler's
// light-table NEE breaks out on `cosSurface <= 0`, so PT cannot light
// a hair fibre from BEHIND -- and hair's TT lobe, which carries most
// of its energy, is exactly that transport.  BDPT's light subpath has
// no such gate and reaches those vertices.  Relaxing that one gate to
// `fabs(...)` as a diagnostic moves PT 0.0773 -> 0.1451 and closes
// roughly half the gap (4.3x -> 2.3x); the rest is not yet
// characterised.  Fixing it properly is a full-sphere-NEE workstream
// (per-IBSDF below-hemisphere audit + the matching env-NEE gate --
// see the furnace section's note on the same defect), not something
// this test can wire around.  Until then a point-lit PT-vs-BDPT
// comparison has no tolerance at which it is both passing and
// meaningful, so this test stays on the env.
//
// The env-lit configuration, by contrast, is stable and is what is
// measured below.  Note the "~8% to ~48% run to run" swing the earlier
// comment attributed to env lighting was really the clamp being absent
// -- see (a).
//
// TUNING NOTE -- the clamp is load-bearing:
//   (a) BDPT's light-subpath connections through hair's narrow lobes
//       show real heavy-tailed (firefly-like) per-pixel variance -- at
//       256 spp / 32x32 with NO clamp, repeated runs of the SAME scene
//       gave relative PT/BDPT differences of 11%, 14%, 19%, 28% and one
//       as high as 48%, while PT's own luminance stayed rock-stable
//       across every run.  That is the signature of a few
//       high-throughput BDPT connection samples dominating the mean,
//       not a shifting systematic bias -- so `indirect_clamp 1.5` /
//       `direct_clamp 1.5` (identical on both PT and BDPT, applied
//       symmetrically so it is a shared variance-reduction step, not a
//       one-sided thumb on the scale) tames it.
//
// Tolerance rationale: with the clamp, POST the env-MIS fix (file
// header, MEASUREMENT BASELINE), four repeat runs of this binary gave
//   PT   0.28903 / 0.28897 / 0.28851 / 0.28858
//   BDPT 0.29693 / 0.29756 / 0.29724 / 0.29738
// i.e. a relative difference of 2.73% / 2.97% / 3.02% / 3.05% -- the
// same stable few-percent the pre-fix build showed (the fix moves PT
// only, and this groom's eumelanin 0.9 absorption keeps the env-escape
// term small).  10% leaves ~3.3x headroom over the worst observed run
// for machine-to-machine RNG differences while still catching a genuine
// regression (order 30%+, matching the unclamped runs' own worst
// excursions and the collapse sizes recorded elsewhere in CLAUDE.md's
// BDPT/VCM regression history).  It also stays well inside the 4.3x
// the point-lit configuration above would produce, so this tolerance
// cannot be satisfied by accident.
//////////////////////////////////////////////////////////////////////
static const double kPtBdptTol = 0.10;

static void TestPtVsBdpt()
{
	std::cout << "=== 4. PT-vs-BDPT sanity (loose, reciprocity caveat) ===" << std::endl;

	const unsigned int W = 32, H = 32, COUNT = 2000, SEG = 6, SEED = 3;
	const std::string common = GroomInEnvCommon( "eumelanin", 0.9, W, H, COUNT, SEG, SEED );

	const ImageStats pt = RenderAndComputeStats(
		AssembleScene( common, RasterizerPTRgb( 256, 8, 0, 1.5 ) ), "ptvbdpt_pt" );
	const ImageStats bdpt = RenderAndComputeStats(
		AssembleScene( common, RasterizerBDPTRgb( 256, 4, 4, 1.5 ) ), "ptvbdpt_bdpt" );

	Check( pt.valid,   "PT-vs-BDPT: PT render produced output" );
	Check( bdpt.valid, "PT-vs-BDPT: BDPT render produced output" );
	if( !pt.valid || !bdpt.valid ) return;

	std::cout << "  PT luminance = " << pt.luminance << "   BDPT luminance = " << bdpt.luminance << std::endl;

	const double denom = std::fmax( pt.luminance, 1e-6 );
	const double relDiff = std::fabs( bdpt.luminance - pt.luminance ) / denom;
	std::cout << "  relative difference = " << relDiff << std::endl;
	Check( relDiff <= kPtBdptTol, "PT-vs-BDPT: mean luminance within the documented loose tolerance" );
}

int main( int /*argc*/, char* /*argv*/[] )
{
	std::cout << "HairRenderTest -- render-level Chiang hair BCSDF + hair_geometry regression suite" << std::endl;

	TestFurnace();
	TestHwssInvariant();
	TestMelaninLadder();
	TestPtVsBdpt();

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;

	return failCount == 0 ? 0 : 1;
}
