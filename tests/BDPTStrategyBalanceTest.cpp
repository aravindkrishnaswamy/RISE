//////////////////////////////////////////////////////////////////////
//
//  BDPTStrategyBalanceTest.cpp - End-to-end correctness check that
//    BDPT's MIS-weighted strategies sum to an unbiased estimator.
//
//    PROPERTY: For any scene, the unidirectional path tracer (PT) and
//    bidirectional path tracer (BDPT) must converge to the same image
//    in the limit.  At finite samples they should agree on the **mean
//    radiance** to within sampling noise.  When BDPT MIS is broken
//    (some strategy excluded from the denominator, weights not summing
//    to 1, contribution scaled wrong), BDPT diverges from PT by a
//    fixed bias that does NOT shrink with more samples.
//
//    REGRESSION FAMILY THIS TEST GUARDS AGAINST:
//      - Delta-light NEE excluded from MIS denominator (the original
//        firefly bug — light-tracing splats double-counted, BDPT mean
//        ran ~22% above PT).
//      - Splat normalization off by a factor.
//      - Throughput accumulation off at a vertex type.
//      - Any future "skip rule" added to MISWeight that turns out to
//        exclude a valid strategy.
//
//    APPROACH: render a few minimal scenes, each exercising a
//    different BDPT topology, with both pathtracing_pel_rasterizer (PT)
//    and bdpt_pel_rasterizer (BDPT).  Capture the rendered radiance
//    buffer into memory via a custom IRasterizerOutput (no file I/O),
//    compute the per-channel mean, and assert BDPT's mean matches PT's
//    mean within a tolerance generous enough to absorb sampling variance
//    at low spp but tight enough to catch the canonical bias modes.
//    (The reference was the legacy `pixelpel_rasterizer` until
//    2026-09-05 -- see the block above `kRasterizerPT` for why it had to
//    go and what it cost.)
//
//    Topologies exercised:
//      A. Delta-position light (omni) over a Lambertian surface
//         — the canonical s=1 NEE vs s>=2 LT MIS partition.
//      B. Area light (mesh luminaire) over a Lambertian surface
//         — the canonical s=0 (eye-hits-emitter) vs s=1 NEE vs
//         s>=2 LT MIS partition.
//      C. Mixed (delta + area) light selection
//         — exercises the unified light-selection MIS plus the
//         per-strategy partition simultaneously.
//      D. Orthographic (delta-DIRECTION) camera + mesh area emitter
//         — the phantom t==1 light-tracing strategy must be skipped.
//      E. Backlit thin weave curtain, delta omni light
//         — a MIXED delta+continuum vertex stays connectible for NEE.
//      F. Gapped thin weave curtain + full-width mesh area emitter
//         — s=0 through a chain whose middle vertex was sampled as a
//         delta, competing with s=1 NEE at that same mixed vertex.
//
//    Tolerance: 8% relative on the mean RGB.  At 32 spp, 64x64 images
//    Monte Carlo noise on the mean of a smooth scene is sub-percent;
//    multi-threaded BDPT splat-accumulation order adds run-to-run
//    non-determinism on the order of 1-2 brightness units (well below
//    8%).  The known-bias firefly bug produced 22% — this tolerance
//    catches it cleanly.  If tightening this tolerance ever flags a
//    "real" mismatch, that's a real BDPT correctness regression and
//    the right fix is in the integrator, not the test.
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
// CapturingRasterizerOutput
//
// Stores the final rendered image into a vector for in-test pixel
// analysis.  Replaces the scene's FileRasterizerOutput so the test
// doesn't write images to disk.  The captured pixels are in linear
// radiance (the rasterizer's internal format), exactly the values the
// integrator produced before any tone-mapping or sRGB encoding.
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

//////////////////////////////////////////////////////////////////////
// ImageStats — per-channel statistics over the linear radiance buffer.
//
// Why we track more than the mean:
//
//   - mean catches systematic bias (the canonical "BDPT is 22% off")
//     but a few firefly pixels can skew the mean WITHOUT being the
//     mean's dominant signal at moderate sample counts.
//   - max catches "explosive single-pixel" fireflies, the kind that
//     render to saturated white in 8-bit and dominate denoiser output.
//   - p99 catches a class of bug where ~1% of pixels are wildly off
//     while the mean stays close to PT.
//   - median is a robust mean — if median agrees but mean doesn't,
//     the bias is concentrated in a small number of bright outliers.
//
// All four together let a test express "BDPT pixels look like PT pixels
// in expectation AND in tail behaviour" — exactly the property a
// correct MIS-weighted estimator must satisfy.
//////////////////////////////////////////////////////////////////////
struct ImageStats
{
	double mean[3];
	double median[3];
	double p99[3];
	double max[3];
	bool   valid;
};

static double Percentile( std::vector<double>& v, double p )
{
	if( v.empty() ) return 0.0;
	std::sort( v.begin(), v.end() );
	const size_t idx = static_cast<size_t>(
		std::min<double>( double(v.size()-1), std::round( p * (v.size()-1) ) ) );
	return v[idx];
}

static ImageStats ComputeStats( const CapturingRasterizerOutput& cap )
{
	ImageStats s{};
	if( cap.pixels.empty() ) {
		return s;
	}

	std::vector<double> ch[3];
	for( int c = 0; c < 3; c++ ) ch[c].reserve( cap.pixels.size() );

	// Compare the COMPOSITED-OVER-BLACK radiance (base * coverage-alpha) --
	// the per-pixel radiance the sensor actually integrates -- NOT the
	// unpremultiplied surface RGB.  PixelBasedPelRasterizer (PT) and
	// BDPTPelRasterizer resolve partial-coverage silhouette-edge pixels with
	// DIFFERENT alpha conventions: PT keeps unpremultiplied RGB and puts
	// coverage in alpha (alphaSum = hit-count); BDPT bakes coverage into RGB
	// and reports alpha = 1 (alphaSum = all-samples).  Comparing base RGB then
	// shows a spurious deficit at silhouettes (PT reports the full surface
	// radiance L, BDPT reports coverage*L) even though base*alpha = coverage*L
	// -- the image the sensor measures -- agrees to <0.2%.  Multiplying by
	// alpha makes the comparison convention-independent and is a no-op for
	// full-coverage pixels (alpha == 1).  See docs/INTEGRATOR_BUGFIX_FINDINGS.md Bug 2.
	for( const RISEColor& c : cap.pixels ) {
		const double cov = c.a;
		ch[0].push_back( c.base.r * cov );
		ch[1].push_back( c.base.g * cov );
		ch[2].push_back( c.base.b * cov );
	}

	for( int c = 0; c < 3; c++ ) {
		double sum = 0;
		for( double v : ch[c] ) sum += v;
		s.mean[c]   = sum / double(ch[c].size());
		s.median[c] = Percentile( ch[c], 0.50 );
		s.p99[c]    = Percentile( ch[c], 0.99 );
		s.max[c]    = ch[c].back();			// after sort
	}
	s.valid = true;
	return s;
}

//////////////////////////////////////////////////////////////////////
// WriteSceneToTempFile
//
// Writes a scene-string to a unique temp file.  Returns the path on
// success, empty string on failure.  The parser needs a real path
// because some scene chunks resolve relative paths against it; we use
// /tmp directly rather than a portable mkstemp because RISE only
// supports POSIX/macOS for these tests anyway.
//////////////////////////////////////////////////////////////////////
static std::string WriteSceneToTempFile( const char* sceneText, const char* tag )
{
	char path[512];
	std::snprintf( path, sizeof(path),
		"/tmp/bdpt_strategy_balance_%s_%d.RISEscene",
		tag, static_cast<int>(::getpid()) );

	std::ofstream ofs( path );
	if( !ofs.is_open() ) {
		return std::string();
	}
	ofs << sceneText;
	ofs.close();
	return std::string( path );
}

//////////////////////////////////////////////////////////////////////
// RenderAndComputeStats
//
// Loads a scene, replaces its file rasterizer output with our
// capturing one, runs Rasterize, and returns image statistics.
//////////////////////////////////////////////////////////////////////
static ImageStats RenderAndComputeStats( const char* scenePath )
{
	ImageStats result{};

	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) {
		return result;
	}

	if( !pJob->LoadAsciiSceneViaCst( scenePath ) ) {
		safe_release( pJob );
		return result;
	}

	// Drop the scene's file_rasterizeroutput so we don't pollute the
	// rendered/ directory during a test run, and so the test reads the
	// raw radiance buffer instead of an LDR-encoded PNG.
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
// RelativeDiff — relative diff per channel, capped at `absFloor` to
// avoid div-by-zero when both values are near zero.
//////////////////////////////////////////////////////////////////////
static bool ChannelsAgree(
	const double a[3],
	const double b[3],
	double relTol,
	double absFloor )
{
	for( int c = 0; c < 3; c++ ) {
		const double denom = std::fmax( std::fabs(a[c]), absFloor );
		if( std::fabs(a[c] - b[c]) / denom > relTol ) return false;
	}
	return true;
}

static void PrintStats( const char* label, const ImageStats& s )
{
	if( !s.valid ) {
		std::cout << "    " << label << ": INVALID (render failed)" << std::endl;
		return;
	}
	std::cout << "    " << label
	          << ": mean=(" << s.mean[0] << "," << s.mean[1] << "," << s.mean[2] << ")"
	          << " median=(" << s.median[0] << "," << s.median[1] << "," << s.median[2] << ")"
	          << " p99=(" << s.p99[0] << "," << s.p99[1] << "," << s.p99[2] << ")"
	          << " max=(" << s.max[0] << "," << s.max[1] << "," << s.max[2] << ")"
	          << std::endl;
}

static void PrintRelDiff( const char* label, const double a[3], const double b[3], double absFloor )
{
	double d[3];
	for( int c = 0; c < 3; c++ ) {
		const double denom = std::fmax( std::fabs(a[c]), absFloor );
		d[c] = (b[c] - a[c]) / denom * 100.0;
	}
	std::cout << "    " << label << " relative diff: ("
	          << d[0] << "%, " << d[1] << "%, " << d[2] << "%)" << std::endl;
}

//////////////////////////////////////////////////////////////////////
// Scene texts.  Kept small (32x32, 32 spp) so the entire test runs in
// a few seconds.  At those settings the mean over ~1000 pixels x 32
// samples is statistically stable to sub-percent for smooth scenes,
// well below the 8% test tolerance.
//
// Both PT and BDPT versions of each scene MUST be identical except for
// the rasterizer chunk so any difference is attributable to the
// integrator alone.
//////////////////////////////////////////////////////////////////////

// Common camera + geometry fragment, parameterised by rasterizer type.
//
// Lambertian quad at z=0 facing the camera, lit from various sources
// per-test.  Quad albedo is uniform 0.5 so contributions are bounded
// and easy to compare.  `casts_shadows TRUE` and `receives_shadows
// TRUE` keep visibility tests live (don't accidentally short-circuit
// any shadow logic the integrators differ on).

static const char* kSceneCommon =
	"film\n"
	"{\n"
	"\twidth 32\n"
	"\theight 32\n"
	"}\n"
	"\n"
	"pinhole_camera\n"
	"{\n"
	"\tlocation 0 0 3.5\n"
	"\tlookat 0 0 0\n"
	"\tup 0 1 0\n"
	"\tfov 30.0\n"
	"}\n"
	"\n"
	"uniformcolor_painter\n"
	"{\n"
	"\tname pnt_albedo\n"
	"\tcolor 0.5 0.5 0.5\n"
	"}\n"
	"\n"
	"lambertian_material\n"
	"{\n"
	"\tname mat_diffuse\n"
	"\treflectance pnt_albedo\n"
	"}\n"
	"\n"
	"clippedplane_geometry\n"
	"{\n"
	"\tname quad\n"
	"\tpta -1 -1 0\n"
	"\tptb 1 -1 0\n"
	"\tptc 1 1 0\n"
	"\tptd -1 1 0\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_quad\n"
	"\tgeometry quad\n"
	"\tmaterial mat_diffuse\n"
	"}\n";

// Test A: omni point light only (delta-position).  Triggers the bug
// we just fixed if it ever regresses.
// Orthographic-camera variant of kSceneCommon.  An orthographic camera is a
// Dirac delta in DIRECTION (all rays parallel) -- the importance-side analogue
// of a directional light.  Before the IsDeltaDirection fix, BDPT kept the
// phantom t==1 light-tracing strategy in the MIS denominator (weight ~0.999),
// crushing the eye-path NEE strategies and rendering near-black (lum ~1% of PT).
static const char* kSceneCommonOrtho =
	"film\n"
	"{\n"
	"\twidth 32\n"
	"\theight 32\n"
	"}\n"
	"\n"
	"orthographic_camera\n"
	"{\n"
	"\tlocation 0 0 3.5\n"
	"\tlookat 0 0 0\n"
	"\tup 0 1 0\n"
	"\tviewport_scale 2.5 2.5\n"
	"}\n"
	"\n"
	"uniformcolor_painter\n"
	"{\n"
	"\tname pnt_albedo\n"
	"\tcolor 0.5 0.5 0.5\n"
	"}\n"
	"\n"
	"lambertian_material\n"
	"{\n"
	"\tname mat_diffuse\n"
	"\treflectance pnt_albedo\n"
	"}\n"
	"\n"
	"clippedplane_geometry\n"
	"{\n"
	"\tname quad\n"
	"\tpta -1 -1 0\n"
	"\tptb 1 -1 0\n"
	"\tptc 1 1 0\n"
	"\tptd -1 1 0\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_quad\n"
	"\tgeometry quad\n"
	"\tmaterial mat_diffuse\n"
	"}\n";

//////////////////////////////////////////////////////////////////////
// THE PT REFERENCE.  `pathtracing_pel_rasterizer` -- the modern
// progressive path tracer -- NOT the legacy `pixelpel_rasterizer` this
// file used until 2026-09-05 (docs/CLOTH_FABRIC_DESIGN.md §15 debt 26).
//
// The legacy rasterizer executes the scene's `standard_shader` chain
// literally, and a chain of `DefaultDirectLighting` alone is a
// DIRECT-LIGHTING-ONLY render by construction: neither
// `DirectLightingShaderOp` nor the `DefaultEmission` op that
// `Job::AddStandardShader` auto-prepends declares `RequireSPF()`, so
// `StandardShader::Shade` never even calls `ISPF::Scatter` and no
// continuation ray of any kind is cast.  That is correct legacy
// semantics -- a plain `dielectric_material` pane renders BLACK under
// the same chain, and the shipped legacy scenes pair
// `DefaultDirectLighting` with `DefaultRefraction` / `DefaultReflection`
// exactly because of it (scenes/FeatureBased/Caustics/pool_caustics
// .RISEscene) -- but it makes the legacy rasterizer unusable as a
// TRANSPORT reference for any topology whose energy arrives through a
// scattered ray.  Measured on the topology F scene below (32x32,
// 256 spp): legacy `[DefaultEmission, DefaultDirectLighting]` 0.04159,
// the same chain plus `DefaultRefraction` 0.10148, this rasterizer
// 0.10240 -- a 2.46x reference error that adding one op removes.
//
// The two scene strings must still differ ONLY in the rasterizer chunk,
// so this string carries the same `DefaultPathTracing` shader and the
// same `pixel_filter box` as `kRasterizerBDPT`; the modern PT rasterizer
// drives `PathTracingIntegrator` directly and does not consult the
// shader chain, but leaving the two shaders different would reintroduce
// a second free variable.
//////////////////////////////////////////////////////////////////////
static const char* kRasterizerPT =
	"standard_shader\n"
	"{\n"
	"\tname global\n"
	"\tshaderop DefaultPathTracing\n"
	"}\n"
	"\n"
	"pathtracing_pel_rasterizer\n"
	"{\n"
	"\tsamples 32\n"
	"\trr_min_depth 8\n"
	"\tpixel_filter box\n"
	"\toidn_denoise FALSE\n"
	"}\n"
	"\n"
	"file_rasterizeroutput\n"
	"{\n"
	"\tpattern /tmp/bdpt_balance_pt_unused\n"
	"\ttype PNG\n"
	"\tbpp 8\n"
	"\tcolor_space sRGB\n"
	"}\n";

// `oidn_denoise FALSE` on BOTH rasterizer strings (2026-09-05).  The
// capture output only overrides `OutputImage`, and the default
// `OutputDenoisedImage` forwards POST-denoise pixels there, so with the
// denoiser on this test was comparing denoised images -- and OIDN's
// `auto` quality selection is timing-based, so it flipped between
// BALANCED and HIGH from run to run and made topology A's PT mean
// bimodal across otherwise identical invocations.  Off, every topology
// is a straight integrator-vs-integrator comparison and no band moved.
static const char* kRasterizerBDPT =
	"standard_shader\n"
	"{\n"
	"\tname global\n"
	"\tshaderop DefaultPathTracing\n"
	"}\n"
	"\n"
	"bdpt_pel_rasterizer\n"
	"{\n"
	"\tmax_eye_depth 3\n"
	"\tmax_light_depth 3\n"
	"\tsamples 32\n"
	"\tpixel_filter box\n"
	"\toidn_denoise FALSE\n"
	"}\n"
	"\n"
	"file_rasterizeroutput\n"
	"{\n"
	"\tpattern /tmp/bdpt_balance_bdpt_unused\n"
	"\ttype PNG\n"
	"\tbpp 8\n"
	"\tcolor_space sRGB\n"
	"}\n";

static const char* kLightOmni =
	"omni_light\n"
	"{\n"
	"\tname l_omni\n"
	"\tpower 4.0\n"
	"\tcolor 1.0 1.0 1.0\n"
	"\tposition 0.0 0.0 5.0\n"
	"}\n";

//////////////////////////////////////////////////////////////////////
// Glass-in-scene topology fragment.
//
// A 1.5-IOR glass sphere occupies a corner of the scene without
// blocking the receiver's direct line of sight to the camera or the
// emitter.  This way:
//   - Most pixels see the lit receiver directly (stable mean).
//   - A subset of pixels see refracted paths through the glass,
//     exercising specular-vertex MIS in BDPT.
//   - The light-tracing splat strategies still receive contributions
//     that traverse the glass (the source of firefly modes in scenes
//     dominated by dielectrics, like torus_chain_atrium).
//
// Compared to a "glass between camera and receiver" layout, this keeps
// the per-pixel mean dominated by the well-converged direct-lighting
// signal so PT and BDPT can be compared apples-to-apples at low spp;
// the glass paths show up as p99/max signal instead.
//////////////////////////////////////////////////////////////////////
// Mesh emitter: small flat luminary at z=4, normal pointing -z so it
// faces the receiver quad below.  Vertex order reversed (CW from +z)
// so the face normal points TOWARD the quad.  scale=20 keeps the
// rendered values comparable in magnitude to the omni topology.
static const char* kLightMesh =
	"uniformcolor_painter\n"
	"{\n"
	"\tname pnt_emit\n"
	"\tcolor 1.0 1.0 1.0\n"
	"}\n"
	"\n"
	"lambertian_luminaire_material\n"
	"{\n"
	"\tname mat_emit\n"
	"\texitance pnt_emit\n"
	"\tscale 20.0\n"
	"\tmaterial none\n"
	"}\n"
	"\n"
	"clippedplane_geometry\n"
	"{\n"
	"\tname quad_emit\n"
	"\tpta -0.5 0.5 4.0\n"
	"\tptb 0.5 0.5 4.0\n"
	"\tptc 0.5 -0.5 4.0\n"
	"\tptd -0.5 -0.5 4.0\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_emit\n"
	"\tgeometry quad_emit\n"
	"\tmaterial mat_emit\n"
	"}\n";


//////////////////////////////////////////////////////////////////////
// Tolerances per metric.  These reflect what a CORRECT BDPT
// implementation should achieve at our test sample counts:
//
//   meanTol  — 8%.  At 32 spp on smooth scenes the Monte-Carlo-noise
//              floor on the mean is sub-percent; the tolerance is set
//              wide enough to cover multi-thread accumulation order
//              non-determinism (~1-2 brightness units on a 0..1 scale)
//              but tight enough to flag the canonical 22% bias bug.
//
//   p99Tol   — 25%.  The 99th percentile is more variance-sensitive
//              than the mean, so a wider tolerance is appropriate at
//              low spp.  This metric flags "1% of pixels are wildly
//              off" failure modes that don't shift the overall mean.
//
//   maxTol   — 100% (i.e., max can be up to 2x apart).  The single
//              brightest pixel is dominated by sample-level outliers
//              even in correct integrators, so the comparison is
//              loose.  But a 5x or 10x discrepancy here is
//              unmistakable evidence of fireflies — the bug we just
//              fixed gave 3.5x.
//////////////////////////////////////////////////////////////////////
struct Tolerances
{
	double meanTol;
	double p99Tol;
	double maxTol;
};

static const Tolerances kStrictTolerances{ 0.08, 0.25, 1.00 };

//////////////////////////////////////////////////////////////////////
// RunTopologyTest - shared driver for each (PT, BDPT) comparison.
//
// Renders the same scene twice — once with PT, once with BDPT — and
// requires BDPT to match PT on each statistical metric within the
// supplied tolerance.  PT is the trusted reference; BDPT must
// converge to the same image distribution.
//////////////////////////////////////////////////////////////////////
static void RunTopologyTest(
	const char* topologyName,
	const std::string& sceneCommonBlock,
	const Tolerances& tol = kStrictTolerances )
{
	std::cout << "Testing PT-vs-BDPT: " << topologyName << std::endl;

	const std::string ptScene   = std::string("RISE ASCII SCENE 7\n") + kRasterizerPT   + sceneCommonBlock;
	const std::string bdptScene = std::string("RISE ASCII SCENE 7\n") + kRasterizerBDPT + sceneCommonBlock;

	const std::string ptPath   = WriteSceneToTempFile( ptScene.c_str(),   "pt"   );
	const std::string bdptPath = WriteSceneToTempFile( bdptScene.c_str(), "bdpt" );

	if( ptPath.empty() || bdptPath.empty() ) {
		Check( false, ( std::string("temp file write: ") + topologyName ).c_str() );
		return;
	}

	const ImageStats pt   = RenderAndComputeStats( ptPath.c_str() );
	const ImageStats bdpt = RenderAndComputeStats( bdptPath.c_str() );

	PrintStats( "PT  ", pt );
	PrintStats( "BDPT", bdpt );

	std::remove( ptPath.c_str() );
	std::remove( bdptPath.c_str() );

	Check( pt.valid,   ( std::string("PT render produced output: ")   + topologyName ).c_str() );
	Check( bdpt.valid, ( std::string("BDPT render produced output: ") + topologyName ).c_str() );
	if( !pt.valid || !bdpt.valid ) return;

	// Sanity: render must be non-trivially bright.
	const double brightness = pt.mean[0] + pt.mean[1] + pt.mean[2];
	Check( brightness > 1e-4,
		( std::string("PT mean is non-zero: ") + topologyName ).c_str() );

	const double absFloor = 1e-6;
	const bool meanMatch = ChannelsAgree( pt.mean, bdpt.mean, tol.meanTol, absFloor );
	const bool p99Match  = ChannelsAgree( pt.p99,  bdpt.p99,  tol.p99Tol,  absFloor );
	const bool maxMatch  = ChannelsAgree( pt.max,  bdpt.max,  tol.maxTol,  absFloor );

	Check( meanMatch, ( std::string("BDPT mean within ")
		+ std::to_string(int(tol.meanTol*100)) + "% of PT: " + topologyName ).c_str() );
	Check( p99Match,  ( std::string("BDPT p99 within ")
		+ std::to_string(int(tol.p99Tol*100))  + "% of PT: " + topologyName ).c_str() );
	Check( maxMatch,  ( std::string("BDPT max within ")
		+ std::to_string(int(tol.maxTol*100))  + "x of PT: " + topologyName ).c_str() );

	if( !meanMatch ) PrintRelDiff( "mean", pt.mean, bdpt.mean, absFloor );
	if( !p99Match  ) PrintRelDiff( "p99",  pt.p99,  bdpt.p99,  absFloor );
	if( !maxMatch  ) PrintRelDiff( "max",  pt.max,  bdpt.max,  absFloor );
}

//////////////////////////////////////////////////////////////////////
// Topology A: delta-position omni light only.
//
// This is THE SCENARIO THAT REVEALED THE FIREFLY BUG: the fix sits in
// BDPTIntegrator::MISWeight's light-side delta-skip rule.  Without the
// fix, BDPT's mean overshot PT by ~22% and the pixel max overshot by
// ~3.5x.  With it, PT and BDPT agree to under 1% mean and within
// sampling noise on max.  This test is the canary for any future
// regression in delta-light MIS handling.
//////////////////////////////////////////////////////////////////////
static void TestDeltaOmniLight()
{
	RunTopologyTest( "delta-position omni light",
		std::string( kSceneCommon ) + kLightOmni );
}

//////////////////////////////////////////////////////////////////////
// Topology B: mesh area emitter only.
//
// Should be untouched by the delta-light MIS fix (no delta vertex in
// the light subpath at all).  Baseline regression check — if this
// starts diverging, the bug is in the contribution formula, splat
// normalisation, or non-delta MIS, not in delta-light handling.
//////////////////////////////////////////////////////////////////////
static void TestMeshEmitterOnly()
{
	RunTopologyTest( "mesh area emitter",
		std::string( kSceneCommon ) + kLightMesh );
}

//////////////////////////////////////////////////////////////////////
// Topology C: omni delta + mesh area, both active.
//
// Exercises the unified light-selection MIS plus the per-strategy
// partition simultaneously.  PT handles both via NEE+emitter-hit;
// BDPT via s=1 NEE, s>=2 LT, and s=0 emitter-hit.  Under correct MIS
// the integrated means are equal.
//////////////////////////////////////////////////////////////////////
static void TestMixedLights()
{
	RunTopologyTest( "mixed delta+mesh lights",
		std::string( kSceneCommon ) + kLightOmni + kLightMesh );
}

// NOTE: A glass-caustic topology was prototyped here and intentionally
// dropped — it confused "BDPT MIS correctness" with "PT's inability to
// sample caustics through dielectrics".  In a scene where light reaches
// the receiver only via a specular SDS chain (camera → glass → diffuse
// → glass → emitter), every (s>0, t) strategy's connection lands on a
// specular vertex (zero connection density), so vanilla BDPT can only
// sample the path through the s=0 strategy.  PT at the same depth
// budget is similarly forced to specular-sample through glass and
// converges much more slowly because Russian-roulette terminations
// rarely produce successful caustic paths.  PT and BDPT both correctly
// converge to the same caustic radiance in the limit, but at any
// finite spp the diff is dominated by their RELATIVE caustic sampling
// efficiency, NOT by an MIS bug in BDPT.  See the long comment in the
// "investigation" notes for the full analysis.  Catching caustic-path
// regressions properly needs SMS-enabled or MLT reference, which is
// out of scope for a fast CI test.

//////////////////////////////////////////////////////////////////////
// Topology D: orthographic (delta-DIRECTION) camera + mesh area emitter.
//
// Regression guard for the orthographic-camera BDPT fix.  An orthographic
// camera has zero density for the t==1 light-tracing strategy; if that
// strategy is not skipped (and excluded from the MIS denominator) BDPT
// renders near-black.  PT is unaffected, so PT-vs-BDPT agreement is the
// invariant.  Before the fix BDPT mean was ~1% of PT (fails meanTol hard).
//////////////////////////////////////////////////////////////////////
static void TestOrthographicCamera()
{
	// Strict tolerances (same as the pinhole topologies).  The ortho scene is
	// the first in this suite with a visible quad SILHOUETTE (the 2x2 quad
	// sits inside a 2.5x2.5 viewport), so it has partial-coverage edge pixels.
	// PT and BDPT use different alpha conventions there (see ComputeStats),
	// which made a raw base-RGB mean comparison show a ~10% deficit; comparing
	// the COMPOSITED radiance (base*alpha, what the sensor measures) the two
	// agree to <0.2%, so strict 8% holds.  Still catches the near-black
	// regression the IsDeltaDirection fix resolved (pre-fix BDPT was ~1.2% of
	// PT) with a >10x margin.  Root cause: docs/INTEGRATOR_BUGFIX_FINDINGS.md Bug 2.
	RunTopologyTest( "orthographic delta-direction camera + mesh emitter",
		std::string( kSceneCommonOrtho ) + kLightMesh, kStrictTolerances );
}

//////////////////////////////////////////////////////////////////////
// Topology E: backlit thin weave curtain with delta omni light.
//
// Regression guard for the mixed delta+continuum connectibility fix:
// WeaveSPF with transmission thin has a delta gap lobe and a continuous
// diffuse transmission lobe. When subpath generation stochastically chose
// the gap lobe, it set hasNonDelta = false which marked the surface vertex
// non-connectible and dropped NEE on a `gap` fraction of camera rays,
// deflating BDPT by (1 - gap) vs PT (a ~10% under-contribution on linen).
// With GetBSDF() checked directly, BDPT and PT agree within Monte Carlo noise.
//////////////////////////////////////////////////////////////////////
static const char* kSceneBacklitThinCurtain =
	"film\n"
	"{\n"
	"\twidth 32\n"
	"\theight 32\n"
	"}\n\n"
	"pinhole_camera\n"
	"{\n"
	"\tlocation 0 0 3.2\n"
	"\tlookat 0 0 0\n"
	"\tup 0 1 0\n"
	"\tfov 34.0\n"
	"}\n\n"
	"omni_light\n"
	"{\n"
	"\tname backlight\n"
	"\tposition 0 0 -3.0\n"
	"\tcolor 1.0 1.0 1.0\n"
	"\tpower 6.0\n"
	"}\n\n"
	"weave_material\n"
	"{\n"
	"\tname mat_curtain\n"
	"\tfabric linen\n"
	"\ttransmission thin\n"
	"\tgap 0.1\n"
	"}\n\n"
	"clippedplane_geometry\n"
	"{\n"
	"\tname curtain_geo\n"
	"\tpta -1.4 -1.4 0\n\tptb 1.4 -1.4 0\n\tptc 1.4 1.4 0\n\tptd -1.4 1.4 0\n"
	"\tdoublesided TRUE\n"
	"}\n\n"
	"standard_object\n"
	"{\n"
	"\tname curtain_obj\n"
	"\tgeometry curtain_geo\n"
	"\tmaterial mat_curtain\n"
	"\tposition 0 0 0\n"
	"}\n";

static void TestBacklitThinCurtain()
{
	RunTopologyTest( "backlit thin weave curtain (delta omni light)",
		kSceneBacklitThinCurtain, kStrictTolerances );
}

//////////////////////////////////////////////////////////////////////
// Topology F: the AREA-LIGHT TWIN of topology E -- gap-0.1 weave
// curtain in front of a full-width mesh emitter.
//
// This file carried no such twin until 2026-09-05, because the PT
// reference was the legacy `pixelpel_rasterizer` and it reads 0.0416
// here against the modern path tracer's 0.1024 -- see the block under
// `kRasterizerPT` and docs/CLOTH_FABRIC_DESIGN.md §15 debt 26.  With the
// reference switched, the twin belongs here, and it is the ONLY
// topology in this file that puts the s=0 "eye subpath hits the
// emitter" strategy into competition with s=1 NEE THROUGH A CHAIN WHOSE
// MIDDLE VERTEX WAS SAMPLED AS A DELTA:
//
//   - Topology E backlights the same curtain with an OMNI light.  A
//     point light has zero solid angle, so the weave's delta gap lobe
//     -- a deterministic straight-line continuation -- can never land on
//     it: s=0 has zero density and never fires, and the whole image is
//     s=1 NEE against the continuum lobe.
//   - Here the emitter is an area quad covering the curtain's whole
//     footprint, so essentially every gap draw (10% of camera rays)
//     lands on it -- 94.9% of the film's straight-through rays reach the
//     quad, the shortfall being the outermost pixel ring only.
//     s=0-through-a-delta-chain, s=1 NEE at that same mixed vertex (which
//     the debt-23 fix made legal again), and light tracing all compete
//     on the same pixel.  If the s=0 emission strategy took weight 1
//     through the delta chain -- the natural-looking rule, and wrong at a
//     MIXED vertex where NEE can produce the same path -- this is the
//     topology that shows it, at an s=0-sized share (tens of percent),
//     an order of magnitude above the 8% mean band.
//
// The emitter is full-width and only `scale 2.0`, deliberately not a
// small bright one: a small emitter makes the through-gap sighting a
// rare, peaky event whose noise floor swamps the partition error it is
// meant to detect.
//
// This is the same scene as tests/FabricRenderTest.cpp case 8
// (`TestGappedWeaveWithAreaLight`), which keeps it at 256 spp with a
// PT/BDPT/VCM three-way band; here it runs at this file's 32 spp under
// the shared strict tolerances and adds the topology to BDPT's MIS
// partition matrix.  MEASURED on this machine after the reference
// switch (32x32, 32 spp, mean of 3 runs): PT mean
// (0.10802, 0.10411, 0.09630), BDPT (0.10746, 0.10355, 0.09574) --
// BDPT/PT (0.9948, 0.9946, 0.9941) with a run-to-run sigma of 4.8e-4,
// i.e. ~15x inside the 8% mean band; p99 ratio 1.007 (25% band) and
// max ratio 1.009 (2x band).  The channels differ because the weave's
// linen preset is not neutral, not because the integrators disagree.
//////////////////////////////////////////////////////////////////////
static const char* kSceneGappedCurtainAreaLight =
	"film\n"
	"{\n"
	"\twidth 32\n"
	"\theight 32\n"
	"}\n\n"
	"pinhole_camera\n"
	"{\n"
	"\tlocation 0 0 3.2\n"
	"\tlookat 0 0 0\n"
	"\tup 0 1 0\n"
	"\tfov 34.0\n"
	"}\n\n"
	"uniformcolor_painter\n"
	"{\n"
	"\tname pnt_emit_ga\n"
	"\tcolor 1.0 1.0 1.0\n"
	"}\n\n"
	"lambertian_luminaire_material\n"
	"{\n"
	"\tname mat_emit_ga\n"
	"\texitance pnt_emit_ga\n"
	"\tscale 2.0\n"
	"\tmaterial none\n"
	"}\n\n"
	// Winding gives the one-sided luminaire normal +Z, i.e. facing the
	// curtain (and the camera behind it).
	"clippedplane_geometry\n"
	"{\n"
	"\tname quad_emit_ga\n"
	"\tpta -1.4 -1.4 -1.5\n\tptb 1.4 -1.4 -1.5\n"
	"\tptc 1.4 1.4 -1.5\n\tptd -1.4 1.4 -1.5\n"
	"}\n\n"
	"standard_object\n"
	"{\n"
	"\tname obj_emit_ga\n"
	"\tgeometry quad_emit_ga\n"
	"\tmaterial mat_emit_ga\n"
	"}\n\n"
	"weave_material\n"
	"{\n"
	"\tname mat_curtain\n"
	"\tfabric linen\n"
	"\ttransmission thin\n"
	"\tgap 0.1\n"
	"}\n\n"
	"clippedplane_geometry\n"
	"{\n"
	"\tname curtain_geo\n"
	"\tpta -1.4 -1.4 0\n\tptb 1.4 -1.4 0\n\tptc 1.4 1.4 0\n\tptd -1.4 1.4 0\n"
	"\tdoublesided TRUE\n"
	"}\n\n"
	"standard_object\n"
	"{\n"
	"\tname curtain_obj\n"
	"\tgeometry curtain_geo\n"
	"\tmaterial mat_curtain\n"
	"\tposition 0 0 0\n"
	"}\n";

static void TestGappedCurtainAreaLight()
{
	RunTopologyTest( "gapped thin weave curtain + mesh area emitter",
		kSceneGappedCurtainAreaLight, kStrictTolerances );
}

//////////////////////////////////////////////////////////////////////
// HOW THE AREA-LIGHT TWIN GOT HERE (history, 2026-09-04 -> 2026-09-05).
//
// Topology F above used to live only in
// tests/FabricRenderTest.cpp::TestGappedWeaveWithAreaLight, and this
// block used to say it could never live here, because THIS FILE'S PT
// REFERENCE COULD NOT RENDER IT: `kRasterizerPT` was the legacy
// `pixelpel_rasterizer`, which reads 0.0416 on that scene against the
// modern path tracer's 0.1024 at the same spp, and raising
// `max_recursion` from 2 to 8 does not recover it.  Asserting
// BDPT == pixelpel there would have banked a 2.46x reference error as
// expected behaviour.
//
// That reading of the evidence was right about the numbers and wrong
// about the cause.  The legacy RASTERIZER is not broken; the legacy
// SHADER CHAIN the scene declared was direct-lighting-only, so no
// continuation ray -- delta gap lobe, dielectric refraction, or plain
// indirect -- was ever cast, or even sampled (neither
// `DirectLightingShaderOp` nor the auto-prepended `DefaultEmission`
// declares `RequireSPF()`, so `StandardShader::Shade` skips
// `ISPF::Scatter` outright).  Adding one op, `DefaultRefraction`, brings
// the legacy rasterizer to 0.10148 against the modern PT's 0.10240 --
// 0.991, from 0.406.  A plain `dielectric_material` pane in the same
// scene renders exactly 0.000000 under the direct-lighting-only chain
// and 0.61119 with `DefaultRefraction` added (modern PT: 0.61116), so
// this is not weave-specific and not a defect: it is the legacy
// shader-op contract, which the shipped legacy scenes already honour
// (scenes/FeatureBased/Caustics/pool_caustics.RISEscene pairs
// `DefaultDirectLighting` with `DefaultRefraction`).
//
// The reference was therefore switched to `pathtracing_pel_rasterizer`
// rather than patched, because a STRATEGY-BALANCE test wants a
// full-transport reference, not a shader chain whose coverage depends on
// which ops the scene string happens to list -- the legacy chain is
// still 0.980 of the modern PT on the SAME curtain at gap 0, where no
// delta lobe exists at all, purely from the indirect bounces it does not
// follow.  Every pre-existing topology agreed with BDPT more tightly
// after the switch than before, with no band loosened; full old-vs-new
// table in docs/CLOTH_FABRIC_DESIGN.md §15 debt 26.  The "PT may be the
// broken one" pre-flight in docs/skills/bdpt-vcm-mis-balance.md keeps
// this as its legacy-rasterizer instance, now with the mechanism.
//////////////////////////////////////////////////////////////////////

int main()
{
	std::cout << "=== BDPTStrategyBalanceTest ===" << std::endl;

	TestDeltaOmniLight();
	TestMeshEmitterOnly();
	TestMixedLights();
	TestOrthographicCamera();
	TestBacklitThinCurtain();
	TestGappedCurtainAreaLight();

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;

	return failCount == 0 ? 0 : 1;
}
