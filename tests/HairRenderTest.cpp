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
//    4b. Point-lit BACKLIT groom: the full-sphere NEE guard.  A groom
//       lit only from behind is lit entirely through its fibres -- the
//       TT / TTs transport that lives BELOW the shading normal, which
//       `LightSampler` refused to sample until
//       `IMaterial::ScattersFullSphere()` existed.  Asserts on PT's own
//       per-unit-power reading rather than on a PT/BDPT ratio, because
//       in this configuration BDPT is the unconverged side.
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
//  SECOND BASELINE SHIFT -- the full-sphere NEE fix.  Every furnace and
//    backlit number below was re-measured after
//    `IMaterial::ScattersFullSphere()` made LightSampler's three
//    surface-cosine gates capability-aware, so hair's transmissive
//    hemisphere is reachable by next-event estimation (see
//    LightSampler::EvaluateDirectLighting's FULL-SPHERE NEE block and
//    HairBSDF.h section 5).  Bisecting ACROSS that commit, expect the
//    furnaces to read ~1.3 % LOW (RGB 0.988, hwss=false 0.993), the
//    medulla furnace ~12 % low (0.883), and the backlit groom 6-8x low.
//    Each section below carries its own before/after table.
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
//
// `medullaLines` is appended verbatim inside the `hair_material`
// chunk.  Empty (the default) means the medulla slots are never
// mentioned, which is exactly the pre-Phase-3 configuration every
// existing test in this file assumes -- so their scene text, and
// therefore their numbers, are untouched by Phase 3.
static std::string GroomInEnvCommon(
	const char* tierParam, double tierValue,
	unsigned int width, unsigned int height,
	unsigned int count, unsigned int segments,
	unsigned int seed,
	const char* medullaLines = "" )
{
	std::ostringstream ss;
	ss <<
		"standard_shader\n{\n\tname global\n\tshaderop DefaultDirectLighting\n}\n\n"
		"film\n{\n\twidth " << width << "\n\theight " << height << "\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 0 3.2\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 34.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_env\n\tcolor 1.0 1.0 1.0\n}\n\n"
		"scalar_painter\n{\n\tname pnt_tier\n\tvalue " << tierValue << "\n}\n\n"
		"hair_material\n{\n\tname mat_hair\n\t" << tierParam << " pnt_tier\n" << medullaLines << "}\n\n"
		"sphere_geometry\n{\n\tname scalp\n\tradius 1.0\n}\n\n"
		"hair_geometry\n{\n\tname groom\n\tbase_geometry scalp\n\tcount " << count
		<< "\n\tsegments " << segments << "\n\tlength 0.24\n\twidth_root 0.035\n\twidth_tip 0.012"
		<< "\n\tbase_detail 22\n\tseed " << seed << "\n}\n\n"
		"standard_object\n{\n\tname groom_obj\n\tgeometry groom\n\tmaterial mat_hair\n\tposition 0 0 0\n}\n\n";
	return ss.str();
}

// POINT-LIT twin of GroomInEnvCommon: the SAME groom, framing and
// material tier, with the uniform environment replaced by a single
// `omni_light` placed BEHIND the groom (negative Z; the camera is at
// +3.2 Z looking at the origin).  Every camera-visible strand is then
// lit only through the fibre -- the TT / TTs transmissive transport
// that lives entirely BELOW the shading normal.
//
// This is the configuration test 4b measures, and it is the sharpest
// probe of the full-sphere NEE capability in the suite: with the
// capability off, `LightSampler`'s light-table row bails on
// `cosSurface <= 0` and PT cannot light these strands AT ALL, while
// BDPT's s == 1 row (which uses `fabs` at the eye vertex) can -- so
// the two integrators disagree by a factor, not a percentage.
//
// The callers pair this with a rasterizer string built by the
// `*NoEnv` helpers below; mixing it with the env-lit rasterizers
// would add an environment back and defeat the point.
static std::string GroomWithBackLightCommon(
	const char* tierParam, double tierValue,
	unsigned int width, unsigned int height,
	unsigned int count, unsigned int segments,
	unsigned int seed,
	double lightPower )
{
	std::ostringstream ss;
	ss <<
		"standard_shader\n{\n\tname global\n\tshaderop DefaultDirectLighting\n}\n\n"
		"film\n{\n\twidth " << width << "\n\theight " << height << "\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 0 3.2\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 34.0\n}\n\n"
		"omni_light\n{\n\tname backlight\n\tposition 0 0 -3.0\n\tcolor 1.0 1.0 1.0\n\tpower "
			<< lightPower << "\n}\n\n"
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
// These two helpers always emit `radiance_map`, and their `*NoEnv`
// twins below always omit it, rather than either taking a `useEnv`
// flag: a scene is either env-lit (tests 1-4) or point-lit (test 4b),
// and keeping that a choice of helper rather than a boolean makes each
// call site say which at a glance.  An earlier revision did carry a
// `useEnv` parameter, but no caller ever passed false because the
// point-lit configuration it was meant for could not be asserted on
// until the full-sphere NEE fix -- see test 4b.
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


// No-environment twins of the two rasterizer helpers above, for the
// point-lit scenes.  Identical in every other respect -- same
// `oidn_denoise FALSE`, same clamp plumbing -- so a PT-vs-BDPT
// comparison built from them differs from test 4's only in the light.
static std::string RasterizerPTRgbNoEnv( unsigned int samples, unsigned int rrMinDepth, double indirectClamp = 0.0 )
{
	std::ostringstream ss;
	ss <<
		"pathtracing_pel_rasterizer\n{\n"
		"\tsamples " << samples << "\n"
		"\trr_min_depth " << rrMinDepth << "\n"
		"\toidn_denoise FALSE\n";
	if( indirectClamp > 0.0 ) ss << "\tindirect_clamp " << indirectClamp << "\n\tdirect_clamp " << indirectClamp << "\n";
	ss <<
		"}\n\n"
		"file_rasterizeroutput\n{\n\tpattern /tmp/hair_render_test_unused\n\ttype PNG\n\tbpp 8\n\tcolor_space sRGB\n}\n";
	return ss.str();
}

static std::string RasterizerBDPTRgbNoEnv( unsigned int samples, unsigned int maxEyeDepth, unsigned int maxLightDepth, double indirectClamp = 0.0 )
{
	std::ostringstream ss;
	ss <<
		"bdpt_pel_rasterizer\n{\n"
		"\tsamples " << samples << "\n"
		"\tmax_eye_depth " << maxEyeDepth << "\n\tmax_light_depth " << maxLightDepth << "\n"
		"\toidn_denoise FALSE\n";
	if( indirectClamp > 0.0 ) ss << "\tindirect_clamp " << indirectClamp << "\n\tdirect_clamp " << indirectClamp << "\n";
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
// THE SECOND DEFECT THIS TEST FOUND, AND ITS FIX.  An earlier revision
// of this comment recorded a residual 0.5-1.2 % DEFICIT here as
// understood-but-unfixed, and named its cause exactly: LightSampler's
// env-NEE block gated on `cosEnv > 0`, so a full-sphere BSDF (hair's
// TT / TTs lobes transmit THROUGH the fibre) got no NEE strategy at
// all for the below-normal half of the sphere, while the BSDF-sampling
// side still applied its `w_bsdf < 1` there -- the two strategies
// summed to less than 1 over that whole half.  That gate is now
// capability-aware (`IMaterial::ScattersFullSphere()`, overridden TRUE
// by HairMaterial; see LightSampler::EvaluateDirectLighting's
// FULL-SPHERE NEE block for the MIS-partition derivation) and the
// deficit is CLOSED.
//
// WHAT IS MEASURED NOW.  READ THESE AS SAMPLES (repeat runs of this
// binary), NOT as measured bounds -- RISE's film accumulation is
// thread-order dependent, so no two runs of the same scene agree bit
// for bit (verified directly: EnvLightBalanceTest's own printed means
// differ in the 5th decimal between back-to-back runs), and n = 4
// under-samples the tails.  An earlier revision presented such
// quadruples as if their min/max bracketed the achievable range; they
// do not.  Every tolerance below is sized off these values with
// explicit headroom, never off their min/max as if it were a bound.
//
// Each cell is n = 4, measured on this machine, and the "gate on"
// column is the SAME build with `HairMaterial::ScattersFullSphere()`
// forced to `false` -- an in-place A/B, everything else identical:
//
//                              gate on (before)        capability (now)
//   RGB PT, samples=384        0.98836 0.98898         1.00125 1.00156
//                              0.98867 0.98852         1.00164 1.00174
//   spectral hwss=false, 256   0.99294 0.99518         1.00596 1.00649
//                              0.99204 0.99386         1.00741 1.00825
//   spectral hwss=true,  256   0.94806 0.94784         0.96018 0.96020
//                              0.94768 0.94737         0.95955 0.95957
//
// i.e. RGB moves from 1.15 % UNDER to 0.16 % OVER, hwss=false from
// 0.65 % under to 0.70 % over, and hwss=true's deficit shrinks from
// 5.2 % to 4.0 %.  The RGB / hwss=false residual is now a small
// OVER-count of the same order as the deficit it replaced, which is
// what an MIS partition that closes to within MC noise looks like.
//
// TOLERANCES.  `kFurnaceTol` (RGB + hwss=false) TIGHTENS from 3 % to
// 2 %: the worst post-fix deviation is +0.83 % (hwss=false), so 2 %
// leaves 1.2pp of headroom while still failing on the 2-3 % scale of a
// genuinely lost residual lobe (docs/HAIR_FUR_DESIGN.md phase-0's
// description of what this test guards) AND on the +4.0 % env-MIS
// over-count if that ever regresses.  It was 3 % only to clear the
// 1.17 % NEE-partition deficit that no longer exists.
//
// hwss=true keeps its OWN, asymmetric band, because its remaining
// deficit is a different, pre-existing problem this test must name
// rather than absorb.  It is the HWSS spectral-bundle bias recorded in
// CLAUDE.md's env-IBL arc ("hwss=true env-only is already 18 % under
// PT at the disc-area baseline"): same direction, hair-independent,
// and demonstrably NOT the NEE-partition defect -- the full-sphere fix
// moved it by only 1.2pp (5.2 % -> 4.0 %) while it moved RGB by
// 1.3pp and fully closed it.  So:
//   * `kFurnaceHwssUnderTol` TIGHTENS from 9 % to 7 % -- 3.0pp of
//     headroom over the measured 4.04 % deficit, pending the
//     spectral-bundle work.
//   * `kFurnaceHwssOverTol` 2 % -- an OVER-unity reading is never
//     legitimate for a sigma_a == 0 furnace, so the upper half of the
//     band stays tight regardless.  UNCHANGED: hwss=true reads 0.960,
//     nowhere near it.
// When the spectral-bundle bias is fixed, hwss=true should join the
// other two near 1.00 and this pair should collapse back to
// `kFurnaceTol`.
//////////////////////////////////////////////////////////////////////
static const double kFurnaceTarget       = 1.0;
static const double kFurnaceTol          = 0.02;	// RGB + hwss=false, see rationale above
static const double kFurnaceHwssUnderTol = 0.07;	// hwss=true lower bound (known spectral-bundle deficit)
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
// 1b. FUR MEDULLA FURNACE (Yan et al. 2017, docs/HAIR_FUR_DESIGN.md
//     Phase 3).  The same white-furnace measurement as test 1, with
//     `medulla_ratio 0.7` / `medulla_scatter 2.0` -- i.e. with the two
//     extra scattered lobes carrying most of the transmitted energy
//     (at that tau the ballistic survival of one medulla crossing is
//     only ~0.12, so roughly 88 % of TT leaves through the TTs lobe).
//
//     WHY IT IS WORTH A WHOLE RENDER.  HairBSDFTest group 16 already
//     integrates the BCSDF's own sphere integral at a much larger
//     (kappa, sigma_m, g) grid and to a far tighter tolerance, so this
//     is NOT the primary energy gate.  What it adds is everything the
//     unit test cannot see: that the medulla parameters survive the
//     ASCII parser and Job's painter resolution, that the new lobes'
//     sampled directions survive geometry generation and the h /
//     tangent plumbing, and that a full PT integration through a real
//     groom does not lose or duplicate their energy.  A dropped lobe
//     shows up here as a dimmer render, an over-weighted one as a
//     brighter one, against the SAME target and tolerance test 1 uses.
//
//     IT NOW SHARES TEST 1's TOLERANCE, AND THAT IS THE HEADLINE.
//     It did not always.  A medulla-on groom used to read 0.883 where
//     the medulla-free one read 0.988 -- an 11.6 % gap that was NEVER
//     an energy leak in the model (HairBSDFTest group 16 integrates
//     this very configuration's sphere integral at 0.999994).  It was
//     the env-NEE partition defect documented at length above test 1:
//     LightSampler gated env-NEE on `cosEnv > 0`, so a full-sphere
//     BSDF got no NEE strategy at all below the shading normal while
//     the BSDF-sampling side still applied its `w_bsdf < 1` there --
//     hit far harder here, because the medulla-scattered lobes are
//     broad and diffuse by construction and push much more energy into
//     the below-normal hemisphere than the narrow unscattered TT lobe
//     does, so the un-partitioned half was correspondingly larger.
//
//     That defect is FIXED (`IMaterial::ScattersFullSphere()`; see
//     LightSampler::EvaluateDirectLighting's FULL-SPHERE NEE block),
//     and the prediction the earlier revision of this comment made --
//     that both furnaces would land in the same place -- is confirmed
//     by measurement.  n = 4 repeat runs each, same build, the "before"
//     column taken with `ScattersFullSphere()` forced to `false`:
//
//                        gate on (before)          capability (now)
//       medulla-free     0.98836  0.98898          1.00125  1.00156
//                        0.98867  0.98852          1.00164  1.00174
//       medulla k=0.7    0.88303  0.88290          1.00215  1.00151
//                        0.88396  0.88184          1.00152  1.00138
//
//     The two now agree to 0.03 %, which is exactly the
//     "energy-preserving redistribution" claim: the medulla splits TT
//     into TT + TTs without creating or destroying energy, and with the
//     integrator able to reach both halves the render says so.
//
//     SO THIS TEST'S OWN BOUNDS COLLAPSE INTO TEST 1's, exactly as the
//     earlier revision instructed.  Gone, in this commit:
//       * `kMedullaFurnaceUnderTol` (0.15) -- it existed only to clear
//         the 11.7 % partition deficit, which is now 0.2 % OVER unity;
//       * `kMedullaFurnaceOverTol` (0.03) -- subsumed by kFurnaceTol's
//         tighter 0.02, which is now the assertion on both sides;
//       * `kMedullaFurnaceMustBeBelow` (0.95) -- the "did medulla_ratio
//         actually reach the model" pin.  It CANNOT survive: post-fix
//         the medulla-on and medulla-free readings are 0.03 % apart, so
//         NO render-level luminance assertion can distinguish them.
//         That is not lost coverage -- it is the same question answered
//         by HairMaterialChunkTest's medulla group, whose money
//         assertions ("medulla_ratio 0 is BIT-IDENTICAL to omitting the
//         slots" and "medulla_ratio 0.7 actually reaches the model and
//         changes it") test the parser / painter-resolution plumbing
//         directly, without depending on any integrator behaviour at
//         all.  What THIS test still adds, and the unit test cannot,
//         is that the new lobes' sampled directions survive geometry
//         generation and the h / tangent plumbing and that a full PT
//         integration through a real groom neither loses nor duplicates
//         their energy -- which is precisely what a shared
//         `|reading - 1| <= kFurnaceTol` band asserts.
static void TestMedullaFurnace()
{
	std::cout << "=== 1b. Fur medulla white furnace (kappa = 0.7) ===" << std::endl;

	const unsigned int W = 40, H = 40, COUNT = 4000, SEG = 6, SEED = 5;
	const char* kMedulla =
		"\tmedulla_ratio 0.7\n\tmedulla_scatter 2.0\n\tmedulla_g 0.4\n";

	const std::string common = GroomInEnvCommon( "sigma_a", 0.0, W, H, COUNT, SEG, SEED, kMedulla );

	const ImageStats s = RenderAndComputeStats(
		AssembleScene( common, RasterizerPTRgb( 384, 8 ) ), "furnace_medulla_rgb" );
	Check( s.valid, "medulla furnace: render produced output" );
	if( s.valid ) {
		std::cout << "  medulla RGB luminance = " << s.luminance
			<< " (target " << kFurnaceTarget
			<< " -- same band as test 1; was ~0.883 before the full-sphere NEE fix)" << std::endl;
		Check( std::fabs( s.luminance - kFurnaceTarget ) <= kFurnaceTol,
			"medulla furnace: MONEY ASSERTION -- the medulla-scattered lobes REDISTRIBUTE "
			"energy without creating or destroying it (same band as the medulla-free furnace)" );
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
// eumelanin=1.3 groom, POST the env-MIS fix (see the file header's
// MEASUREMENT BASELINE).  Across n = 4 repeat runs of this binary --
// four SAMPLES, not bounds; see the furnace section's note on why a
// deterministic sampler makes repeat runs under-state the true spread
// -- hwss=false luminance came in around 0.16891-0.17158 and hwss=true
// around 0.16044-0.16095, a relative difference of 4.71% / 5.91% /
// 6.16% / 6.48%.  (Pre-fix the same pair read 0.170-0.172 vs
// 0.160-0.161, i.e. 5.8-6.9%: the fix moved the hwss=false side only,
// exactly as expected since the HWSS loop never carried the
// branch-order bug.)  12% leaves ~1.85x headroom over the worst
// observed run while still catching the kind of collapse the
// env-IBL arc saw when HWSS was genuinely broken (order 50%+, per
// CLAUDE.md's "hwss=true env-only is already 18% under" and worse
// pre-fix numbers).  This section's residual and the furnace's
// hwss=true deficit are the SAME pre-existing spectral-bundle bias
// seen two ways.
//
// UNMOVED BY THE FULL-SPHERE NEE FIX, and measured to be so -- worth
// recording, because that fix DID move the furnace's hwss=true number
// (0.9478 -> 0.9595).  Here, n = 3-4 runs each: capability off
// 5.93 % / 6.04 % / 5.84 %, capability on 5.64 % / 5.72 % / 5.73 % /
// 5.88 %.  On this absorbing groom the fix raises hwss=false and
// hwss=true by nearly the same factor, so their RATIO barely changes --
// consistent with the residual being the spectral-bundle bias rather
// than anything about the NEE partition, which is why this tolerance is
// left at 12 %.
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
// The code was right and the comment was wrong.
//
// A point-lit variant of this scene USED to be impossible to assert
// on: PT and BDPT rendered it 4.3x apart, because LightSampler's
// light-table NEE bailed on `cosSurface <= 0` and PT could not light a
// hair fibre from BEHIND -- exactly where hair's TT lobe carries most
// of its energy -- while BDPT's light subpath had no such gate.  That
// defect is now FIXED (`IMaterial::ScattersFullSphere()`), and the
// point-lit configuration has its own test: see test 4b, which
// measures PT recovering 8.1-8.2x of that transport.  This test stays
// env-lit because the two configurations probe DIFFERENT rows of the
// same estimator -- env-NEE (an MIS partition that has to close) here,
// the delta-light row (w = 1, pure missing transport) there -- and
// both are worth keeping.
//
// The env-lit configuration, by contrast, is stable and is what is
// measured below.  About the "~8% to ~48% run to run" swing an earlier
// revision of this comment recorded: an intermediate revision then
// re-attributed that swing wholesale to the missing clamp.  That
// re-attribution was never measured and is NOT asserted here.  What is
// actually known is that TWO things were in play at once -- the env-MIS
// partition bug (file header) inflating PT, and genuine heavy-tailed
// BDPT connection variance that the clamp tames (see (a) below) -- and
// no experiment separated their contributions.  Both were present;
// neither has been quantified against the other.
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
// header, MEASUREMENT BASELINE), n = 4 repeat runs of this binary gave
// -- again four SAMPLES, not bounds; a later run read PT 0.289152 and
// BDPT 0.297799, outside BOTH quadruples below, at a relative
// difference of 2.99% --
//   PT   0.28903 / 0.28897 / 0.28851 / 0.28858
//   BDPT 0.29693 / 0.29756 / 0.29724 / 0.29738
// i.e. a relative difference of 2.73% / 2.97% / 3.02% / 3.05% -- the
// same stable few-percent the pre-fix build showed (the fix moves PT
// only, and this groom's eumelanin 0.9 absorption keeps the env-escape
// term small).  10% leaves ~3.3x headroom over the worst observed run
// for machine-to-machine RNG differences while still catching a genuine
// regression (order 30%+, matching the unclamped runs' own worst
// excursions and the collapse sizes recorded elsewhere in CLAUDE.md's
// BDPT/VCM regression history).
//
// UNCHANGED BY THE FULL-SPHERE NEE FIX, and measured to be so.  This
// test's groom is env-lit and mostly ABOVE-horizon, so the fix moves it
// only slightly: n = 4 post-fix runs read PT 0.28942 / 0.28967 /
// 0.28971 / 0.28982 against BDPT 0.29722 / 0.29665 / 0.29680 / 0.29696,
// a relative difference of 2.70% / 2.41% / 2.45% / 2.46% -- versus
// 2.61% / 2.69% / 2.84% on the same build with
// `HairMaterial::ScattersFullSphere()` forced to `false`.  The 10%
// tolerance is left alone: it was never sized off the NEE partition,
// and the residual it covers is the non-reciprocity caveat above.
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

//////////////////////////////////////////////////////////////////////
// 4b. POINT-LIT BACKLIT -- the full-sphere NEE regression guard.
// Same groom, framing and material tier as test 4, with the uniform
// environment replaced by a single `omni_light` BEHIND the groom.
// Every camera-visible strand is then lit only THROUGH the fibre: the
// TT / TTs transmissive transport, which lives entirely below the
// shading normal.
//
// WHY THIS TEST EXISTS, AND WHY IT DID NOT BEFORE.  Test 4's LIGHTING
// NOTE recorded this exact configuration as UNTESTABLE, and named the
// reason: `LightSampler`'s light-table row bailed on
// `cosSurface <= 0`, so PT could not light a strand from behind AT
// ALL, while BDPT's s == 1 row -- which uses `fabs` at the eye vertex
// (BDPTIntegrator's `absCosEye`, BDPTUtilities' `GeometricTerm` and
// `GeometricTermSurfaceMedium`, all three unconditional) -- reached
// the same transport with no gate.  `IMaterial::ScattersFullSphere()`
// now lets `LightSampler` use |cos| for hair
// (LightSampler::EvaluateDirectLighting's FULL-SPHERE NEE block), and
// this configuration becomes measurable.
//
// THE DELTA-LIGHT ROW HAS NO MIS PARTNER, which is why it closes so
// cleanly: a point light cannot be BSDF-sampled, so NEE carries it at
// w = 1 on both hemispheres.  Nothing had to be re-partitioned here --
// the below-horizon direct term was simply absent and is now present.
//
// MEASURED, capability ON vs the same build with
// `HairMaterial::ScattersFullSphere()` forced to `false` (an in-place
// A/B; everything else identical).  PT, unclamped, 256 spp, 32x32,
// 2000 strands, mean image luminance DIVIDED BY light power:
//
//                        capability OFF          capability ON
//     power 4.0     0.000122 0.000135        0.000997 0.000877
//                   0.000134 0.000145        0.000923
//     power 1.0     0.000115 0.000112        0.000936 0.000926
//                   0.000125 0.000118        0.000921
//
// i.e. PT recovers 6.4-8.2x of the transport.  Both readings should be
// -- and are -- INDEPENDENT of light power, because a delta light's NEE
// row carries w = 1 and is exactly linear in emitted power; the ON
// column drifts 0.2-6.5 % between the two powers, which is MC spread at
// 256 spp on a 32x32 film.
//
// (The 4.3x figure test 4's note used to quote was measured with
// `indirect_clamp 1.5` active, which on this scene at power 40 removes
// ~60 % of PT's post-fix energy.  Unclamped, the missing transport is
// 8x, not 4.3x -- the clamp was masking half of it.)
//
// WHY THIS IS NOT WRITTEN AS A PT-vs-BDPT RATIO TEST, WHICH IS WHAT
// TEST 4's NOTE WOULD HAVE PREDICTED.  Post-fix, PT is the STABLE side
// here and BDPT is not.  Measured on this scene at `indirect_clamp
// 1.5`, BDPT's luminance PER UNIT POWER across light powers
// 40 / 8 / 2 / 0.5 reads 0.000629 / 0.00131 / 0.00196 / 0.00273 -- a
// 4.3x swing, where PT's stays flat to 4 % -- and UNCLAMPED BDPT gave
// 0.912, 0.148 and 0.203 on three near-identical configurations of the
// same scene.  That is heavy-tailed connection variance through hair's
// narrow lobes: the same phenomenon test 4's TUNING NOTE documents,
// far worse here because the ONLY light is a point source behind
// narrow transmissive fibres, so essentially every BDPT connection
// threads a low-probability path.  BDPT's 256-spp mean is therefore
// NOT a converged reference in this configuration, and a ratio against
// it cannot be tightened no matter how correct PT is.
//
// (Note what that means for the residual: the 1.75x BDPT/PT this test
// still records is NOT a remaining transport gap in PT.  It is
// dominated by where `indirect_clamp 1.5` cuts BDPT's heavy tail --
// which is why it moves with light power while PT's reading does not.
// PT's own scale-invariance across an 4x power range, and its
// agreement with BDPT to 2.4-2.7 % on the ENV-lit twin in test 4, are
// the evidence that PT is now complete.)
//
// So the assertions below are anchored on PT's own reading, with the
// BDPT ratio kept only as a catastrophic-regression net.
//
// BOUNDS (all sized off n = 3-4 repeat runs, both A/B columns):
//   * `kBacklitPtPerPowerMin` / `Max` bracket PT's per-unit-power
//     luminance, and BOTH power readings must land inside.  The min
//     (5.0e-4) sits 1.75x below the lowest measured ON reading
//     (8.77e-4) and 3.4x ABOVE the highest OFF reading (1.45e-4), so
//     it fails hard the moment the gate regresses and has real
//     headroom in the passing direction.  The max (2.0e-3) is 2.0x
//     above the highest ON reading and catches an OVER-count -- e.g. a
//     future unconditional `fabs` that double-counts, or a lost `w`.
//   * The two readings' relative drift is PRINTED but deliberately NOT
//     asserted.  A delta light's NEE row is exactly linear in emitted
//     power, so drift "should" be zero -- but at 256 spp on a 32x32
//     film it measured 0.002 / 0.053 / 0.065 / 0.119 across four ON
//     runs, and the capability-OFF column drifts 0.056-0.227, so no
//     bound both passes ON reliably and fails OFF.  It would be a flaky
//     assertion pretending to be an invariant.  The min/max band above
//     is the real guard, and it independently catches the failure mode
//     drift was meant to cover: `indirect_clamp 1.5` at power 40 pulls
//     the per-unit-power reading down to 3.65e-4, BELOW the 5.0e-4
//     floor.  (A milder clamp would not be caught by either; the drift
//     print is there for a human reading the log.)  Raising the sample
//     count enough to bound drift is not worth this suite's wall time.
//   * `kBacklitBdptRatioMax` 3.0 -- measured 1.72 / 1.75 / 1.77 at
//     power 40 with the clamp on both sides; the capability-off build
//     reads 15.50 / 15.83 / 16.25 on the identical pair.  1.7x
//     headroom in the passing direction, 5x separation from the
//     failing one.  Loose on purpose, per the BDPT-variance paragraph.
//////////////////////////////////////////////////////////////////////
static const double kBacklitPtPerPowerMin = 5.0e-4;
static const double kBacklitPtPerPowerMax = 2.0e-3;
static const double kBacklitBdptRatioMax  = 3.0;

static void TestBacklitFullSphereNEE()
{
	std::cout << "=== 4b. Point-lit backlit groom (full-sphere NEE guard) ===" << std::endl;

	const unsigned int W = 32, H = 32, COUNT = 2000, SEG = 6, SEED = 3;

	// --- PT, unclamped, at two light powers -------------------------
	const double pwA = 4.0, pwB = 1.0;

	const ImageStats ptA = RenderAndComputeStats(
		AssembleScene( GroomWithBackLightCommon( "eumelanin", 0.9, W, H, COUNT, SEG, SEED, pwA ),
			RasterizerPTRgbNoEnv( 256, 8 ) ), "backlit_pt_p4" );
	const ImageStats ptB = RenderAndComputeStats(
		AssembleScene( GroomWithBackLightCommon( "eumelanin", 0.9, W, H, COUNT, SEG, SEED, pwB ),
			RasterizerPTRgbNoEnv( 256, 8 ) ), "backlit_pt_p1" );

	Check( ptA.valid && ptB.valid, "backlit: both PT renders produced output" );
	if( !ptA.valid || !ptB.valid ) return;

	const double perPowerA = ptA.luminance / pwA;
	const double perPowerB = ptB.luminance / pwB;
	std::cout << "  PT per-unit-power: power " << pwA << " -> " << perPowerA
		<< " ; power " << pwB << " -> " << perPowerB << std::endl;

	// MONEY ASSERTION.  PT must actually light these strands.  With the
	// hemisphere gate in place this reads 1.2e-4 -- not merely "dim
	// because the groom is backlit", but missing the entire
	// below-horizon direct term.
	Check( perPowerA >= kBacklitPtPerPowerMin && perPowerB >= kBacklitPtPerPowerMin,
		"backlit: MONEY ASSERTION -- PT lights the groom from BEHIND at BOTH powers "
		"(a hemisphere-gated NEE reads 6-8x lower here)" );
	Check( perPowerA <= kBacklitPtPerPowerMax && perPowerB <= kBacklitPtPerPowerMax,
		"backlit: MONEY ASSERTION -- and does not OVER-count that term" );

	// Printed, not asserted -- see the section comment's bounds list for
	// why (too noisy at 256 spp to bound without flakiness; the band above
	// is the guard).  A drift that grows well past ~0.12 is worth a human
	// look: this row is exactly linear in emitted power by construction.
	const double scaleDrift =
		std::fabs( perPowerA - perPowerB ) / std::fmax( perPowerB, 1e-12 );
	std::cout << "  PT scale drift across powers = " << scaleDrift
		<< " (diagnostic only)" << std::endl;

	// --- BDPT sanity net, clamped on both sides ---------------------
	// Loose by construction: BDPT is the high-variance side here, see
	// the section comment.  This exists to catch a catastrophic
	// regression, not to measure agreement.
	const double pwC = 40.0;
	const std::string commonC = GroomWithBackLightCommon( "eumelanin", 0.9, W, H, COUNT, SEG, SEED, pwC );
	const ImageStats ptC = RenderAndComputeStats(
		AssembleScene( commonC, RasterizerPTRgbNoEnv( 256, 8, 1.5 ) ), "backlit_pt_c" );
	const ImageStats bdptC = RenderAndComputeStats(
		AssembleScene( commonC, RasterizerBDPTRgbNoEnv( 256, 4, 4, 1.5 ) ), "backlit_bdpt_c" );

	Check( ptC.valid && bdptC.valid, "backlit: clamped PT and BDPT renders produced output" );
	if( !ptC.valid || !bdptC.valid ) return;

	const double ratio = bdptC.luminance / std::fmax( ptC.luminance, 1e-12 );
	std::cout << "  clamped PT = " << ptC.luminance << "  BDPT = " << bdptC.luminance
		<< "  BDPT/PT = " << ratio << std::endl;
	Check( ratio <= kBacklitBdptRatioMax,
		"backlit: BDPT/PT within the loose net (15.5-16.3x with the hemisphere gate in place)" );
}

int main( int /*argc*/, char* /*argv*/[] )
{
	std::cout << "HairRenderTest -- render-level Chiang hair BCSDF + hair_geometry regression suite" << std::endl;

	TestFurnace();
	TestMedullaFurnace();
	TestHwssInvariant();
	TestMelaninLadder();
	TestPtVsBdpt();
	TestBacklitFullSphereNEE();

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;

	return failCount == 0 ? 0 : 1;
}
