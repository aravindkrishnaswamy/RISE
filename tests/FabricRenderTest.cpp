//////////////////////////////////////////////////////////////////////
//
//  FabricRenderTest.cpp - docs/CLOTH_FABRIC_DESIGN.md section 9.9
//    gate 7 ("HWSS invariant") plus the PT-vs-BDPT parity check the
//    same gate's precedent -- tests/HairRenderTest.cpp -- pairs with
//    it.  END-TO-END, in the HairRenderTest mould: real
//    `RISE ASCII SCENE 7` text, loaded through the CST scene parser
//    exactly as a user's .RISEscene would be (so the
//    `fabric_material` CHUNK and its preset seeding are exercised, not
//    just the C++ class), rendered through a real rasterizer, and the
//    output image inspected.
//
//    The unit-level fabric coverage lives elsewhere and proves
//    different things: LayeredWhiteFurnaceTest (energy posture over
//    each substrate class), SPFBSDFConsistencyTest (reciprocity),
//    SPFPdfConsistencyTest (the sample-then-reprice mixture density).
//    This file proves those pieces compose correctly through
//    parse + preset seeding + frame rotation + integration, and --
//    the gate's actual subject -- that the SPECTRAL path agrees with
//    itself under hero-wavelength stratification.
//
//  WHY THE HWSS INVARIANT IS THE RIGHT GATE.  It is REFERENCE-FREE:
//    `hwss=true` and `hwss=false` are two samplers of the same
//    integrand, so they must agree in the mean at any spp regardless
//    of what the correct answer is.  That makes it a test of the
//    material's spectral plumbing (does every wavelength of the
//    8-wavelength bundle these scenes sample see a consistent sheen
//    alpha, base scale and rotated substrate frame?) with no measured
//    constant to lock in
//    and no reference renderer to trust.  CLAUDE.md's "High-Value
//    Facts" records the same invariant as the tool that survived the
//    integrator matrix's three measurement artefacts.
//
//    A fabric is a genuinely good subject for it, and not a formality:
//    `fabric_material` reads THREE painter slots per shading point
//    (sheen colour through the colour pipe, sheen roughness and weave
//    angle through the scalar pipe), subtracts a baked directional
//    albedo from the substrate per wavelength, and hands the substrate
//    a ROTATED tangent frame.  Any of those going per-wavelength
//    inconsistent -- a sheen colour uplifted as an illuminant instead
//    of a reflectance, a scalar slot resolved per-channel, a rotation
//    applied on one spectral branch and not another -- shows up here
//    as an hwss split and nowhere else in the suite.
//
//  SEEDING, AND WHY THIS FILE SETS IT EXPLICITLY.  A test binary that
//    calls `RISE_CreateJobPriv` directly does NOT go through
//    `src/RISE/commandconsole.cpp`'s `main()`, which is the only place
//    in the tree that calls `srand( GetMilliseconds() )` -- so libc's
//    `rand()` starts from its default state here, and nothing about
//    this binary is wall-clock seeded.  What actually varies run to run
//    is that every render worker constructs
//    `RandomNumberGenerator random;` (default argument `seed = rand()`,
//    RandomNumbers.h) inside `RasterizeDispatchers.h`, concurrently from
//    `ThreadPool::ParallelFor` -- i.e. libc `rand()`'s hidden global
//    state is read and mutated from several threads with no
//    synchronisation.  That is a DATA RACE, not a seeding policy: on a
//    single-worker machine, or under a different libc, it can just as
//    easily collapse to identical or correlated draws.  Treating repeat
//    runs as independent MC samples on the strength of it would be
//    unsupported.
//
//    So `main` takes an OPTIONAL SEED BASE (argv[1], default
//    `kDefaultSeedBase`) and `RenderAndComputeStats` calls
//    `std::srand( seedBase + n )` before each render, `n` counting
//    renders within the run.  Two consequences, and the second is the
//    one the tolerance derivations below rely on:
//      - The default invocation is REPRODUCIBLE in intent: the same
//        renders start from the same `rand()` state every time.
//      - `./FabricRenderTest 2000`, `3000`, ... are independent BY
//        CONSTRUCTION -- different, documented, far-apart seed bases --
//        rather than by hoping a race decorrelates them.  That is how
//        the n = 5 spreads quoted below were produced, and the exact
//        invocations are named there so they can be re-run.
//    NOT claimed: bit-exact reproducibility of a single run.  The
//    worker-side race above still perturbs which worker draws which
//    seed, so two runs at the same base agree closely but not exactly.
//    Fixing that means changing how `RasterizeDispatchers` seeds its
//    workers, which is a renderer change and not this file's to make.
//
//  OIDN DENOISE IS EXPLICITLY DISABLED in every rasterizer chunk
//    below, for the reason HairRenderTest's header spells out:
//    `IRasterizerOutput::OutputDenoisedImage`'s default forwards
//    POST-DENOISE pixels to `OutputImage`, so a capture that only
//    overrides `OutputImage` would silently measure the denoiser.
//    That would be fatal here specifically -- OIDN is free to smooth
//    away exactly the per-wavelength difference the invariant is
//    looking for, which would make the test pass by construction.
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

//! Seed base for this run; see the file header.  Overridable as argv[1] so a
//! tolerance derivation can take genuinely independent samples on demand.
static const unsigned int kDefaultSeedBase = 1000u;
static unsigned int g_seedBase = kDefaultSeedBase;

//! Renders completed so far in this run.  Added to `g_seedBase` so no two
//! renders in one run share a starting state, and so the k-th render of run A
//! and the k-th of run B differ by exactly the two runs' base offset.
static unsigned int g_renderIndex = 0;

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
// CapturingRasterizerOutput -- identical in shape to
// HairRenderTest's / EnvLightBalanceTest's.  Deliberately does NOT
// override OutputPreDenoisedImage / OutputDenoisedImage; see the file
// header for why every scene below sets `oidn_denoise FALSE` instead.
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
	double luminance;	// mean(mean[0..2]) -- the scalar every check below uses
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
		"/tmp/fabric_render_test_%s_%d.RISEscene",
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

	// See the file header: this binary is not wall-clock seeded, so the
	// starting state is set here rather than left to the worker-side
	// `rand()` race.
	std::srand( g_seedBase + g_renderIndex );
	g_renderIndex++;

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
// Scene assembly.
//
// ONE fabric sphere in a uniform L = 1.0 environment.  A sphere,
// rather than the showcase scenes' drape, because it sweeps every
// incidence angle from normal to grazing once and continuously -- so a
// spectral inconsistency confined to one band of incidence angles
// still moves the whole-image mean.
//
// TWO substrate configurations, because they exercise DIFFERENT halves
// of the material:
//
//   "aniso"  -- an anisotropic `ggx_material` (alphax != alphay) with a
//               non-zero `weave_rotation`.  This is the configuration
//               that runs the frame-rotation path (section 9.5): the
//               substrate's `value` / `Scatter` / `Pdf` are all handed
//               a rotated `OrthonormalBasis3D`.  Note the mandatory
//               `fresnel_mode schlick_f0` + `rs` painter -- without
//               them `ggx_material` defaults to `conductor` with no
//               specular reflectance bound and renders BLACK, which
//               would make an hwss comparison trivially pass on two
//               matching zeros.
//
//   "isotropic" -- an `orennayar_material`, the class the cotton /
//               linen / wool presets are calibrated against.  It has
//               no rotation to steer, so it isolates the sheen lobe's
//               own spectral behaviour (colour slot, roughness slot,
//               and the directional-albedo subtraction) from the
//               rotation plumbing.
//
// `fabric` is spelled explicitly on both so the preset's own
// `sheen_roughness` seeding is what the render uses.
//////////////////////////////////////////////////////////////////////

enum SubstrateKind { kAnisoGGX, kOrenNayar };

static std::string FabricSphereCommon(
	SubstrateKind kind,
	const char* preset,
	unsigned int width, unsigned int height )
{
	std::ostringstream ss;
	ss <<
		"standard_shader\n{\n\tname global\n\tshaderop DefaultDirectLighting\n}\n\n"
		"film\n{\n\twidth " << width << "\n\theight " << height << "\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 0 3.2\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 34.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_env\n\tcolor 1.0 1.0 1.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_f0\n\tcolor 0.04 0.04 0.04\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_dye\n\tcolor 0.34 0.27 0.21\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_sheen\n\tcolor 0.88 0.86 0.83\n\tcolorspace Rec709RGB_Linear\n}\n\n";

	// THE GGX ROW EMITS A PRESET-MISMATCH WARNING SINCE PHASE 2, AND THAT IS
	// EXPECTED.  `satin`'s recommended substrate moved from `ggx_material` to
	// `weave_material` on 2026-09-03 (gate 9b's verdict), so this pairing is now
	// the documented FALLBACK rather than the recommendation, and
	// `Job::AddFabricMaterial` says so at warn level.  The configuration is kept
	// UNCHANGED anyway, deliberately: every locked luminance in this file was
	// measured against this exact substrate and this exact `sheen_roughness`
	// 0.12, and swapping the preset to silence a warning would invalidate the
	// numbers the file exists to hold.  What is under test here is a transport
	// invariant (HWSS, and PT-vs-BDPT parity), not a preset recommendation.
	if( kind == kAnisoGGX ) {
		ss <<
			"ggx_material\n{\n\tname base_sub\n\trd pnt_dye\n\trs pnt_f0\n"
			"\talphax 0.34\n\talphay 0.06\n\tfresnel_mode schlick_f0\n}\n\n"
			"fabric_material\n{\n\tname mat_fab\n\tfabric " << preset <<
			"\n\tbase base_sub\n\tsheen_color pnt_sheen\n\tweave_rotation 0.6\n}\n\n";
	} else {
		ss <<
			"orennayar_material\n{\n\tname base_sub\n\treflectance pnt_dye\n\troughness 0.5\n}\n\n"
			"fabric_material\n{\n\tname mat_fab\n\tfabric " << preset <<
			"\n\tbase base_sub\n\tsheen_color pnt_sheen\n}\n\n";
	}

	ss <<
		"sphere_geometry\n{\n\tname sph\n\tradius 1.0\n}\n\n"
		"standard_object\n{\n\tname fab_obj\n\tgeometry sph\n\tmaterial mat_fab\n\tposition 0 0 0\n}\n\n";
	return ss.str();
}

static std::string RasterizerPTRgb( unsigned int samples, unsigned int rrMinDepth, double clamp = 0.0 )
{
	std::ostringstream ss;
	ss <<
		"pathtracing_pel_rasterizer\n{\n"
		"\tsamples " << samples << "\n"
		"\trr_min_depth " << rrMinDepth << "\n"
		"\toidn_denoise FALSE\n";
	if( clamp > 0.0 ) ss << "\tindirect_clamp " << clamp << "\n\tdirect_clamp " << clamp << "\n";
	ss <<
		"\tradiance_map pnt_env\n\tradiance_scale 1.0\n\tradiance_background TRUE\n"
		"}\n\n"
		"file_rasterizeroutput\n{\n\tpattern /tmp/fabric_render_test_unused\n\ttype PNG\n\tbpp 8\n\tcolor_space sRGB\n}\n";
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
		"file_rasterizeroutput\n{\n\tpattern /tmp/fabric_render_test_unused\n\ttype PNG\n\tbpp 8\n\tcolor_space sRGB\n}\n";
	return ss.str();
}

static std::string RasterizerBDPTRgb( unsigned int samples, unsigned int maxEyeDepth,
                                      unsigned int maxLightDepth, double clamp = 0.0 )
{
	std::ostringstream ss;
	ss <<
		"bdpt_pel_rasterizer\n{\n"
		"\tsamples " << samples << "\n"
		"\tmax_eye_depth " << maxEyeDepth << "\n\tmax_light_depth " << maxLightDepth << "\n"
		"\toidn_denoise FALSE\n";
	if( clamp > 0.0 ) ss << "\tindirect_clamp " << clamp << "\n\tdirect_clamp " << clamp << "\n";
	ss <<
		"\tradiance_map pnt_env\n\tradiance_scale 1.0\n\tradiance_background TRUE\n"
		"}\n\n"
		"file_rasterizeroutput\n{\n\tpattern /tmp/fabric_render_test_unused\n\ttype PNG\n\tbpp 8\n\tcolor_space sRGB\n}\n";
	return ss.str();
}

static std::string AssembleScene( const std::string& common, const std::string& rasterizer )
{
	return std::string( "RISE ASCII SCENE 7\n" ) + common + rasterizer;
}

// No-environment twins of the two rasterizer helpers above, for the
// point-lit backlit-curtain scene below -- HairRenderTest's
// `RasterizerPTRgbNoEnv` / `RasterizerBDPTRgbNoEnv` pattern, verbatim.
static std::string RasterizerPTRgbNoEnv( unsigned int samples, unsigned int rrMinDepth )
{
	std::ostringstream ss;
	ss <<
		"pathtracing_pel_rasterizer\n{\n"
		"\tsamples " << samples << "\n"
		"\trr_min_depth " << rrMinDepth << "\n"
		"\toidn_denoise FALSE\n"
		"}\n\n"
		"file_rasterizeroutput\n{\n\tpattern /tmp/fabric_render_test_unused\n\ttype PNG\n\tbpp 8\n\tcolor_space sRGB\n}\n";
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
		"file_rasterizeroutput\n{\n\tpattern /tmp/fabric_render_test_unused\n\ttype PNG\n\tbpp 8\n\tcolor_space sRGB\n}\n";
	return ss.str();
}

//////////////////////////////////////////////////////////////////////
// 3. BACKLIT SHEER CURTAIN -- docs/CLOTH_FABRIC_DESIGN.md 10 (P2-B).
//
// A `weave_material` "curtain" -- a flat quad patch, camera-facing --
// with an `omni_light` directly BEHIND it (negative Z; camera at
// +3.2 Z looking at the origin, HairRenderTest's backlit-groom
// framing).  Every camera-visible point on the curtain is then lit
// ONLY through the cloth: the delta gap lobe and the diffuse
// transmission lobe this slice adds, both of which live entirely
// below the shading normal -- the full-sphere NEE capability
// `WeaveMaterial::ScattersFullSphere()` turns on under
// `transmission thin`.
//
// `bThin` selects `transmission thin` (linen preset, its own `gap`
// 0.10 and `transmit` 0.25 defaults) vs `transmission none` (the
// P2-A reflection-only baseline, same preset) -- the two share every
// other scene byte, so a luminance difference between them is
// attributable to the transmission lobes alone.
static std::string CurtainWithBackLightCommon(
	bool bThin,
	unsigned int width, unsigned int height,
	double lightPower )
{
	std::ostringstream ss;
	ss <<
		"standard_shader\n{\n\tname global\n\tshaderop DefaultDirectLighting\n}\n\n"
		"film\n{\n\twidth " << width << "\n\theight " << height << "\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 0 3.2\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 34.0\n}\n\n"
		"omni_light\n{\n\tname backlight\n\tposition 0 0 -3.0\n\tcolor 1.0 1.0 1.0\n\tpower "
			<< lightPower << "\n}\n\n"
		"weave_material\n{\n\tname mat_curtain\n\tfabric linen\n\ttransmission "
			<< ( bThin ? "thin" : "none" ) << "\n}\n\n"
		"clippedplane_geometry\n{\n\tname curtain_geo\n"
			"\tpta -1.4 -1.4 0\n\tptb 1.4 -1.4 0\n\tptc 1.4 1.4 0\n\tptd -1.4 1.4 0\n"
			"\tdoublesided TRUE\n}\n\n"
		"standard_object\n{\n\tname curtain_obj\n\tgeometry curtain_geo\n\tmaterial mat_curtain\n\tposition 0 0 0\n}\n\n";
	return ss.str();
}

//////////////////////////////////////////////////////////////////////
// 1. HWSS INVARIANT -- docs/CLOTH_FABRIC_DESIGN.md section 9.9 gate 7.
//
// `hwss=true` must agree with `hwss=false` within MC noise on a fabric
// scene, on tests/HairRenderTest.cpp's Phase-1 pattern.
//
// TOLERANCE DERIVATION.  Measured on this machine at samples=256,
// 32x32, `num_wavelengths 8`, `spectral_samples 1`, over n = 5 runs at
// SEED BASES 1000 / 2000 / 3000 / 4000 / 5000 -- i.e.
//   for b in 1000 2000 3000 4000 5000; do ./bin/tests/FabricRenderTest $b; done
// Those are independent BY CONSTRUCTION (see the file header's seeding
// note), not by assuming a data race decorrelates repeat runs:
//
//   row                                  rel. diff across the 5 seed bases
//   satin over anisotropic GGX           0.38 / 0.38 / 0.48 / 0.72 / 0.74 %
//   cotton over Oren-Nayar               0.58 / 0.60 / 0.74 / 0.80 / 0.90 %
//
//   worst observed, either row:          0.90 %
//
// Both rows land an order of magnitude inside the 12 % this suite's
// hair twin needs, and that difference is itself informative: hair's
// residual is the pre-existing spectral-bundle bias acting on a
// strongly wavelength-dependent absorption model (CLAUDE.md's env-IBL
// arc, "hwss=true env-only is already 18 % under"), while a fabric's
// dye is an ordinary reflectance and its sheen an achromatic lobe, so
// the bundle has almost nothing to disagree about.  The residual that
// does show is ordinary MC noise at this spp and film size, not a
// bias -- its SIGN is not even consistent between the two rows within
// a run.
//
// `kHwssTol = 0.03` is ~3.3x headroom over the worst observed run --
// loose enough to absorb machine-to-machine RNG and the tail this
// small a film leaves, tight enough that any of the failure modes the
// header lists (a slot resolved per-channel, an illuminant /
// reflectance routing slip on the sheen colour, a rotation applied on
// one spectral branch and not another) would move it by far more.  A
// regression here is a SPECTRAL PLUMBING bug, not a noise excursion:
// the collapses CLAUDE.md records when HWSS was genuinely broken are
// order 18-50 %, six to sixteen times this band.
//////////////////////////////////////////////////////////////////////
static const double kHwssTol = 0.03;

static double RunHwssRow( SubstrateKind kind, const char* preset, const char* label, const char* tag )
{
	const unsigned int W = 32, H = 32;
	const std::string common = FabricSphereCommon( kind, preset, W, H );

	char tagOff[64], tagOn[64];
	std::snprintf( tagOff, sizeof(tagOff), "%s_hwss_off", tag );
	std::snprintf( tagOn,  sizeof(tagOn),  "%s_hwss_on",  tag );

	const ImageStats sOff = RenderAndComputeStats(
		AssembleScene( common, RasterizerPTSpectral( 256, false, 8 ) ), tagOff );
	const ImageStats sOn = RenderAndComputeStats(
		AssembleScene( common, RasterizerPTSpectral( 256, true, 8 ) ), tagOn );

	char buf[160];
	std::snprintf( buf, sizeof(buf), "%s: hwss=false render produced output", label );
	Check( sOff.valid, buf );
	std::snprintf( buf, sizeof(buf), "%s: hwss=true render produced output", label );
	Check( sOn.valid, buf );
	if( !sOff.valid || !sOn.valid ) return -1.0;

	const double denom = std::fmax( sOff.luminance, 1e-6 );
	const double relDiff = std::fabs( sOn.luminance - sOff.luminance ) / denom;

	std::cout << "  " << label
		<< ": hwss=false luminance = " << sOff.luminance
		<< "   hwss=true luminance = " << sOn.luminance
		<< "   relative difference = " << relDiff << std::endl;

	std::snprintf( buf, sizeof(buf), "%s: hwss=true agrees with hwss=false within tolerance", label );
	Check( relDiff <= kHwssTol, buf );

	// A pair of ZEROS would satisfy the invariant trivially, and the
	// most likely way to get them is the black-GGX trap the scene
	// helper's comment names.  Assert the render is actually lit.
	std::snprintf( buf, sizeof(buf), "%s: render is non-degenerate (not a black frame)", label );
	Check( sOff.luminance > 0.05, buf );

	return relDiff;
}

static void TestHwssInvariant()
{
	std::cout << "=== 1. HWSS invariant (gate 7) ===" << std::endl;
	RunHwssRow( kAnisoGGX,  "satin",  "satin over anisotropic GGX (weave_rotation 0.6)", "aniso" );
	RunHwssRow( kOrenNayar, "cotton", "cotton over Oren-Nayar",                          "onayar" );
}

//////////////////////////////////////////////////////////////////////
// 2. PT-vs-BDPT parity -- the companion check HairRenderTest pairs
// with its own HWSS gate (its test 4) -- MEASURED AS A DIFFERENCE OF
// DIFFERENCES, because a raw PT-vs-BDPT ratio on an env-lit scene is
// dominated by a bias that is not fabric's.
//
// THE PROBLEM, MEASURED FIRST.  On this scene the raw numbers are
//
//   satin over anisotropic GGX    PT 0.443246   BDPT 0.557192   +25.7 %
//
// and a naive `|BDPT/PT - 1| <= tol` check would fail at any
// defensible tolerance.  That +25.8 % is NOT a fabric defect: RISE's
// BDPT is a documented +28.5 % over closed-form truth on env-only
// scenes (CLAUDE.md "Env-IBL deficit" / docs/PT_ENV_MIS_DOUBLECOUNT.md
// section 4a, where EnvLightBalanceTest bands BDPT's env-only bias at
// exactly that measured value rather than asserting parity).  Every
// scene in this file is env-lit -- a uniform radiance map is the only
// light -- so that partition bias is inherited by construction.
//
// WHAT IS ASSERTED INSTEAD.  The same scene is rendered FOUR times:
// PT and BDPT over the bare `ggx_material` substrate, and PT and BDPT
// over the `fabric_material` that wraps it.  The bare pair measures
// the inherited env-MIS bias on its own; the fabric pair measures that
// bias PLUS whatever the layered model adds.  The assertion is that
// the two ratios AGREE -- i.e. that wrapping a substrate in a fabric
// does not change how far apart the two integrators land.
//
// That is a strictly stronger statement than a raw parity check would
// have been, and it is the one that is actually about this material:
// the sheen's energy subtraction is evaluated from the EYE side in PT
// and from BOTH sides in BDPT's connection strategies, so a
// direction-dependent slip in the `1 - max3(sheen_color) * E(alpha,
// cos)` base scaling separates the two ratios even though it would
// hide inside either one alone.  `fabric_material` is asserted
// reciprocal to `RECIPROCITY_TOL = 1e-6` in SPFBSDFConsistencyTest
// (section 9.9 gate 5a), so there is no model-level asymmetry excuse
// here of the kind hair's twin has to make for the Chiang BCSDF.
//
// TOLERANCE DERIVATION.  Measured on this machine at samples=256 /
// 32x32, over the same n = 5 seed bases (1000 / 2000 / 3000 / 4000 /
// 5000) as the HWSS rows above, independent by construction:
//
//   bare ggx_material      BDPT/PT  1.25115 / 1.25196 / 1.25220 / 1.25232 / 1.25278
//   satin over the same    BDPT/PT  1.25694 / 1.25707 / 1.25741 / 1.25757 / 1.25854
//   |ratio difference|              0.00463 / 0.00487 / 0.00561 / 0.00579 / 0.00622
//
// So the wrapper moves the PT/BDPT relationship by at most 0.62 %
// while the inherited env bias it sits on top of is 25 %: nearly two
// orders of magnitude of separation between the signal and the
// confound, which is exactly what makes this framing worth the two
// extra renders.  Neither integrator needed an `indirect_clamp` to be
// assertable -- unlike hair's twin, whose BDPT connections through
// narrow lobes did -- because this scene has no heavy-tailed transport
// (no specular chains, no small emitters, a uniform env).
//
// `kPtBdptRatioTol = 0.03` is ~4.8x headroom over the worst observed
// run.  Loose enough to absorb machine-to-machine RNG and a change of
// spp; far tighter than the order-30 % excursions CLAUDE.md's BDPT/VCM
// regression history records for a genuine MIS or energy-bookkeeping
// break, and tighter than the 25 % confound it is designed to cancel.
//
// The raw ratios are PRINTED, not asserted, so a future change in the
// env-MIS partition shows up in the log as a moving pair rather than
// as a mysterious failure here.
//////////////////////////////////////////////////////////////////////
static const double kPtBdptRatioTol = 0.03;

// The bare-substrate twin of FabricSphereCommon( kAnisoGGX, ... ):
// byte-identical scene text except that the object binds `base_sub`
// directly instead of the `fabric_material` that wraps it.  Written as
// its own helper rather than a flag on the shared one so the two scenes
// are visibly the same scene.
static std::string BareGGXSphereCommon( unsigned int width, unsigned int height )
{
	std::ostringstream ss;
	ss <<
		"standard_shader\n{\n\tname global\n\tshaderop DefaultDirectLighting\n}\n\n"
		"film\n{\n\twidth " << width << "\n\theight " << height << "\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 0 3.2\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 34.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_env\n\tcolor 1.0 1.0 1.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_f0\n\tcolor 0.04 0.04 0.04\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_dye\n\tcolor 0.34 0.27 0.21\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		"ggx_material\n{\n\tname base_sub\n\trd pnt_dye\n\trs pnt_f0\n"
		"\talphax 0.34\n\talphay 0.06\n\tfresnel_mode schlick_f0\n}\n\n"
		"sphere_geometry\n{\n\tname sph\n\tradius 1.0\n}\n\n"
		"standard_object\n{\n\tname fab_obj\n\tgeometry sph\n\tmaterial base_sub\n\tposition 0 0 0\n}\n\n";
	return ss.str();
}

// Renders `common` through PT and BDPT and returns BDPT/PT, or -1 on a
// failed render.  Both luminances are printed so the raw pair stays
// visible in the log even though only the ratio is asserted on.
static double BdptOverPtRatio( const std::string& common, const char* label, const char* tag )
{
	char tagPt[64], tagBd[64], buf[192];
	std::snprintf( tagPt, sizeof(tagPt), "%s_pt",   tag );
	std::snprintf( tagBd, sizeof(tagBd), "%s_bdpt", tag );

	const ImageStats sPt = RenderAndComputeStats(
		AssembleScene( common, RasterizerPTRgb( 256, 8 ) ), tagPt );
	const ImageStats sBdpt = RenderAndComputeStats(
		AssembleScene( common, RasterizerBDPTRgb( 256, 8, 8 ) ), tagBd );

	std::snprintf( buf, sizeof(buf), "%s: PT render produced output", label );
	Check( sPt.valid, buf );
	std::snprintf( buf, sizeof(buf), "%s: BDPT render produced output", label );
	Check( sBdpt.valid, buf );
	if( !sPt.valid || !sBdpt.valid ) return -1.0;

	std::snprintf( buf, sizeof(buf), "%s: render is non-degenerate (not a black frame)", label );
	Check( sPt.luminance > 0.05, buf );

	const double ratio = sBdpt.luminance / std::fmax( sPt.luminance, 1e-6 );
	std::cout << "  " << label
		<< ": PT = " << sPt.luminance
		<< "   BDPT = " << sBdpt.luminance
		<< "   BDPT/PT = " << ratio << std::endl;
	return ratio;
}

static void TestPtVsBdpt()
{
	std::cout << "=== 2. PT-vs-BDPT parity (ratio-of-ratios; the env-MIS bias cancels) ===" << std::endl;

	const unsigned int W = 32, H = 32;

	const double rBare = BdptOverPtRatio(
		BareGGXSphereCommon( W, H ), "bare ggx_material", "bare" );
	const double rFab = BdptOverPtRatio(
		FabricSphereCommon( kAnisoGGX, "satin", W, H ), "satin over the same GGX", "fabric" );

	if( rBare < 0.0 || rFab < 0.0 ) return;

	const double delta = std::fabs( rFab - rBare );
	std::cout << "  |ratio difference| = " << delta
		<< "   (tolerance " << kPtBdptRatioTol << ")" << std::endl;
	Check( delta <= kPtBdptRatioTol,
		"PT-vs-BDPT: the fabric wrapper does not change the PT/BDPT relationship" );
}

//////////////////////////////////////////////////////////////////////
// 3. Backlit sheer curtain -- docs/CLOTH_FABRIC_DESIGN.md 10 (P2-B).
//
// MONEY ASSERTION: a `transmission thin` curtain lit from behind reads
// substantially brighter than the SAME curtain (same preset, same
// light, same everything else) with `transmission none` -- the
// glow-through cue slice P2-B exists to add.
//
// MEASURED (this machine, PT 256 spp, 32x32, `omni_light` power 6.0 at
// (0,0,-3), camera at (0,0,3.2) looking at the origin, seeds 1000 /
// 2000 / 3000):
//   transmission none: mean luminance EXACTLY 0 (no env, no other
//     light path, and an opaque curtain -- correct: nothing reaches
//     the camera).
//   transmission thin:  7.78e-5 / 7.65e-5 / 7.31e-5 -- non-degenerate
//     and essentially seed-independent, because this number is
//     entirely the diffuse transmission lobe evaluated through NEE
//     (the delta lobe's ray is a deterministic straight-line
//     continuation and a point light has zero solid angle, so it is
//     never hit by chance; only NEE, using `value()`'s continuum
//     transmit term, can see a point light through this material).
// `kMinBrightnessRatio` divides by `none`'s literal 0 as `>= inf`, so
// the ratio assertion is written the other way around: `thin` must
// itself clear a small absolute floor while `none` stays exactly 0.
//
// PT-vs-BDPT ON THE THIN SCENE IS NOT ASSERTED, AND THAT IS A REAL,
// DISCLOSED GAP -- not an oversight.  Measured BDPT (same seeds,
// same scene, `indirect_clamp`/`direct_clamp` OFF): 0.02528 / 0.02528
// / 0.02528 -- i.e. BDPT reads ~325-346x BROADER than PT, tightly
// REPRODUCIBLE across seeds (not the seed-to-seed spread a heavy tail
// would show).  Reducing `indirect_clamp`/`direct_clamp` to 0.01 drops
// BDPT to 0.00801 (still ~100x PT); to 0.001 it drops to 0.000801 --
// i.e. the clamped MEAN tracks the clamp CEILING almost exactly at
// every clamp level tried, which is the signature of a persistently-
// hit near-singular contribution, not a rare firefly a clamp
// ordinarily tames.  The likely cause: this is RISE's first FLAT,
// zero-thickness, `ScattersFullSphere()` geometry (HairMaterial, the
// only prior full-sphere material, is a curve with real cross-section
// separation between its "front" and "back").  A BDPT light subpath
// from a point light travelling toward the curtain and a camera
// subpath vertex on the same infinitesimally-thin quad can land at
// literally the SAME 3D point from opposite sides, and a vertex-to-
// vertex connection's geometry term is `1/distance^2` -- unbounded as
// that distance goes to zero, a degeneracy a curved or volumetric
// surface does not have.  This was NOT chased further: it is very
// likely a BDPT/connection-geometry issue for flat full-sphere
// materials in general, not a `weave_material` BSDF defect --
// `value()`/`Pdf()` for the transmission lobes are independently
// verified reciprocal to 0 error (SPFBSDFConsistencyTest Part E2),
// energy-bounded (LayeredWhiteFurnaceTest rows 50-51) and correctly
// normalised over the full sphere (SPFPdfConsistencyTest's P2-B
// block) by three UNRELATED harnesses that do not go through BDPT at
// all -- but confirming the diagnosis and fixing it is integrator
// work outside this slice's scope (the material, not BDPT's connection
// strategy).  Recorded here rather than silenced with a loose bound:
// see docs/CLOTH_FABRIC_DESIGN.md 10's debts list.
static const double kMinBrightnessAbsolute = 1e-6;

static void TestBacklitSheerCurtain()
{
	std::cout << "=== 3. Backlit sheer curtain (P2-B glow-through) ===" << std::endl;

	const unsigned int W = 32, H = 32;
	const double power = 6.0;

	const ImageStats sNone = RenderAndComputeStats(
		AssembleScene( CurtainWithBackLightCommon( false, W, H, power ),
			RasterizerPTRgbNoEnv( 256, 8 ) ), "curtain_pt_none" );
	const ImageStats sThin = RenderAndComputeStats(
		AssembleScene( CurtainWithBackLightCommon( true, W, H, power ),
			RasterizerPTRgbNoEnv( 256, 8 ) ), "curtain_pt_thin" );

	Check( sNone.valid && sThin.valid, "curtain: both PT renders (none / thin) produced output" );
	if( !sNone.valid || !sThin.valid ) return;

	std::cout << "  PT: transmission none = " << sNone.luminance
		<< "   transmission thin = " << sThin.luminance << std::endl;

	// `transmission none` is an OPAQUE curtain with no environment and
	// no other light path to the camera, so exactly 0 is the correct
	// answer, not merely "small" -- asserting an upper bound near 0
	// would accept a material that leaks a little light it should not.
	Check( sNone.luminance == 0.0,
		"curtain: `transmission none` is EXACTLY black (opaque, no other light path)" );
	Check( sThin.luminance >= kMinBrightnessAbsolute,
		"curtain: MONEY ASSERTION -- `transmission thin` glows through from behind while "
		"`transmission none` (the same scene otherwise) is exactly black" );

	// RESOLVED (docs/CLOTH_FABRIC_DESIGN.md section 15 debt 21).  What
	// looked like a PT full-sphere NEE/MIS bug (the diffuse-transmission
	// lobe's response scaling close to `transmit^2` instead of linearly)
	// was NOT an MIS-weighting defect at all: the (p_light, p_bsdf) and
	// (bsdfPdf, p_nee) pairs the power heuristic combines were verified
	// to partition to 1 exactly, as they must algebraically.  The real
	// bug was a shadow-ray SELF-INTERSECTION in
	// `RayBilinearPatchIntersection` (shared by every
	// `clippedplane_geometry` caller): the self-hit rejection compared
	// `dRange` against the fixed absolute `NEARZERO` (1e-12), but a
	// self-intersecting ray's `dRange` is FP noise that scales with the
	// coordinate magnitude, not machine epsilon in absolute terms --
	// measured at 1e-12 to 3e-12 on this scene's world-scale coordinates,
	// straddling the threshold and spuriously self-shadowing ~94% of
	// this curtain's own NEE shadow rays toward a light behind it (a
	// point light's zero solid angle never triggers the analogous
	// continuation-ray path, which is why the delta-light case in this
	// same file was never affected).  As `transmit` grows, PT's MIS
	// weight legitimately shifts share from the (broken) NEE strategy to
	// the (unaffected) BSDF-sampling strategy, so the combined estimate
	// climbed from mostly-broken toward mostly-correct -- an artificial
	// super-linear curve manufactured by a masking geometry bug, not a
	// weight-partition defect.  Fixed by making the self-intersection
	// floor scale-relative (see `RayBilinearPatchIntersection.cpp`);
	// every caller (shadow rays AND primary/continuation rays) benefits
	// from the one fix.  See `TestAreaLitSheerWeave` below for the
	// dedicated regression.

	// PT-vs-BDPT on the thin scene is measured and PRINTED, but NOT
	// asserted -- see the block comment above this function for the
	// measured ~100-350x discrepancy (tracks the render's own clamp
	// ceiling at every clamp level tried, i.e. not simply MC noise).
	// Diagnosed as an INTEGRATOR limitation (BDPTUtilities::GeometricTerm's
	// unguarded vertex-connection term on a flat, zero-thickness
	// full-sphere surface), not a weave_material defect, and recorded as
	// docs/CLOTH_FABRIC_DESIGN.md section 15 debt 20 /
	// docs/RENDERING_INTEGRATORS.md's known-limitations section rather
	// than hidden behind a loose bound or chased into BDPT's connection
	// code, which is out of this slice's scope.
	const ImageStats bThin = RenderAndComputeStats(
		AssembleScene( CurtainWithBackLightCommon( true, W, H, power ),
			RasterizerBDPTRgbNoEnv( 256, 8, 8 ) ), "curtain_bdpt_thin" );
	Check( bThin.valid, "curtain: BDPT render (thin) produced output" );
	if( !bThin.valid ) return;

	const double btRatio = bThin.luminance / std::fmax( sThin.luminance, 1e-12 );
	std::cout << "  thin: PT = " << sThin.luminance << "   BDPT = " << bThin.luminance
		<< "   BDPT/PT = " << btRatio
		<< "   (NOT asserted -- see CLOTH_FABRIC_DESIGN.md 15 debt 20 / RENDERING_INTEGRATORS.md known limitations)" << std::endl;
}

//////////////////////////////////////////////////////////////////////
// 4. AREA-LIT SHEER WEAVE -- docs/CLOTH_FABRIC_DESIGN.md 15 debt 21
// (RESOLVED).  Regression guard for the shadow-ray self-intersection
// fix in `RayBilinearPatchIntersection.cpp`.
//
// A `weave_material` curtain (`transmission thin`, `gap 0`) directly
// in front of a MESH area light (`lambertian_luminaire_material` on a
// `clippedplane_geometry`, well-separated so BDPT's own unguarded
// vertex-connection geometric term -- debt 20, a SEPARATE, still-open
// integrator limitation on this same material class -- does not fire;
// confirmed empirically during the debt-21 diagnosis, and NOT the same
// scene as `TestBacklitSheerCurtain`'s point light + touching-distance
// curtain, which DOES trip debt 20).  Debt 21's actual mechanism had
// nothing to do with the mesh vs. delta light distinction (both share
// the same shadow-ray self-intersection producer), but a MESH light is
// what the original bug report used and what exercises the
// `LightSampler::EvaluateDirectLighting` mesh-luminary MIS branch
// (`p_light` / `p_bsdf` / `PowerHeuristic`) end to end, so it stays the
// regression's own light type.
//
// Two invariants, restored by the fix:
//   (a) PT is LINEAR in `transmit`: PT(1.0)/PT(0.1) ~= 10, not the
//       ~46.7x the pre-fix spurious self-shadowing produced.
//   (b) PT and BDPT AGREE (this geometry does not trip debt 20): their
//       means at `transmit = 1.0` match within a few percent.
//
// MEASURED (this machine, PT/BDPT both 1024 spp, 16x16,
// `oidn_denoise FALSE`, seed base 1000): PT(0.1) = 0.02591,
// PT(1.0) = 0.25871 (ratio 9.987); BDPT(1.0) = 0.25868
// (PT/BDPT = 1.0001).  Tolerances below are set well outside that
// margin for seed-to-seed MC noise, not tuned to this exact run.
static std::string AreaLitCurtainCommon(
	double transmit,
	unsigned int width, unsigned int height )
{
	std::ostringstream ss;
	ss <<
		"standard_shader\n{\n\tname global\n\tshaderop DefaultDirectLighting\n}\n\n"
		"film\n{\n\twidth " << width << "\n\theight " << height << "\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 0 3\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_window\n\tcolor 1.0 1.0 1.0\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		"lambertian_luminaire_material\n{\n\tname mat_window\n\texitance pnt_window\n\tmaterial none\n\tscale 1.0\n}\n\n"
		"clippedplane_geometry\n{\n\tname window_quad\n"
			"\tpta -3.0 -3.0 -1.5\n\tptb 3.0 -3.0 -1.5\n\tptc 3.0 3.0 -1.5\n\tptd -3.0 3.0 -1.5\n"
			"\tdoublesided TRUE\n}\n\n"
		"standard_object\n{\n\tname window_obj\n\tgeometry window_quad\n\tmaterial mat_window\n\tposition 0 0 0\n}\n\n"
		"weave_material\n{\n\tname mat_curtain\n\tfabric custom\n\ttransmission thin\n\tgap 0.0\n"
			"\twarp_transmit " << transmit << "\n\tweft_transmit " << transmit << "\n}\n\n"
		"clippedplane_geometry\n{\n\tname curtain_quad\n"
			"\tpta -2.0 -2.0 0\n\tptb 2.0 -2.0 0\n\tptc 2.0 2.0 0\n\tptd -2.0 2.0 0\n"
			"\tdoublesided TRUE\n}\n\n"
		"standard_object\n{\n\tname curtain_obj\n\tgeometry curtain_quad\n\tmaterial mat_curtain\n\tposition 0 0 0\n}\n\n";
	return ss.str();
}

static const double kAreaLitLinearityTol = 0.10;	// +/-10% of the ideal 10x ratio
static const double kAreaLitPtBdptRatioTol = 0.05;	// +/-5% of PT/BDPT == 1

static void TestAreaLitSheerWeave()
{
	std::cout << "=== 4. Area-lit sheer weave (debt 21 regression: shadow-ray self-intersection) ===" << std::endl;

	const unsigned int W = 16, H = 16;
	const unsigned int samples = 1024;

	const ImageStats ptLo = RenderAndComputeStats(
		AssembleScene( AreaLitCurtainCommon( 0.1, W, H ), RasterizerPTRgbNoEnv( samples, 6 ) ),
		"arealit_pt_t01" );
	const ImageStats ptHi = RenderAndComputeStats(
		AssembleScene( AreaLitCurtainCommon( 1.0, W, H ), RasterizerPTRgbNoEnv( samples, 6 ) ),
		"arealit_pt_t10" );
	const ImageStats bdptHi = RenderAndComputeStats(
		AssembleScene( AreaLitCurtainCommon( 1.0, W, H ), RasterizerBDPTRgbNoEnv( samples, 6, 6 ) ),
		"arealit_bdpt_t10" );

	Check( ptLo.valid && ptHi.valid && bdptHi.valid,
		"area-lit weave: all three renders (PT t=0.1, PT t=1.0, BDPT t=1.0) produced output" );
	if( !ptLo.valid || !ptHi.valid || !bdptHi.valid ) return;

	Check( ptLo.luminance > 1e-6 && ptHi.luminance > 1e-6,
		"area-lit weave: both PT renders are non-degenerate (not black frames)" );

	const double linearityRatio = ptHi.luminance / std::fmax( ptLo.luminance, 1e-12 );
	const double ptBdptRatio = ptHi.luminance / std::fmax( bdptHi.luminance, 1e-12 );

	std::cout << "  PT(t=0.1) = " << ptLo.luminance
		<< "   PT(t=1.0) = " << ptHi.luminance
		<< "   BDPT(t=1.0) = " << bdptHi.luminance << std::endl;
	std::cout << "  PT linearity ratio [PT(1.0)/PT(0.1)] = " << linearityRatio
		<< "   (ideal 10.0, tolerance +/-" << (kAreaLitLinearityTol * 100.0) << "%)" << std::endl;
	std::cout << "  PT/BDPT at t=1.0 = " << ptBdptRatio
		<< "   (ideal 1.0, tolerance +/-" << (kAreaLitPtBdptRatioTol * 100.0) << "%)" << std::endl;

	Check( std::fabs( linearityRatio - 10.0 ) <= 10.0 * kAreaLitLinearityTol,
		"area-lit weave: PT is LINEAR in `transmit` (debt 21 fixed -- was ~46.7x pre-fix, exponent ~1.7)" );
	Check( std::fabs( ptBdptRatio - 1.0 ) <= kAreaLitPtBdptRatioTol,
		"area-lit weave: PT and BDPT agree within MC noise on this geometry (debt 20's G-term singularity does not fire here)" );
}

//////////////////////////////////////////////////////////////////////
// main
//////////////////////////////////////////////////////////////////////
int main( int argc, char** argv )
{
	if( argc > 1 ) {
		const long v = std::strtol( argv[1], nullptr, 10 );
		if( v > 0 ) g_seedBase = (unsigned int)v;
	}

	std::cout << "FabricRenderTest -- docs/CLOTH_FABRIC_DESIGN.md 9.9 gate 7" << std::endl;
	std::cout << "seed base = " << g_seedBase
		<< "  (pass a different one as argv[1] for an independent sample)" << std::endl;
	std::cout << "==========================================================" << std::endl;

	TestHwssInvariant();
	TestPtVsBdpt();
	TestBacklitSheerCurtain();
	TestAreaLitSheerWeave();

	std::cout << "==========================================================" << std::endl;
	std::cout << "Passed: " << passCount << "   Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
