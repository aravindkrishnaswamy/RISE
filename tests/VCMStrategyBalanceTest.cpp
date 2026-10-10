//////////////////////////////////////////////////////////////////////
//
//  VCMStrategyBalanceTest.cpp - End-to-end correctness check that
//    VCM's combined VC+VM strategies produce an unbiased estimator
//    on scenes where VM should not be active.
//
//    PROPERTY: For non-caustic scenes (no specular surfaces in the
//    scene), VCM should converge to the SAME image as the path
//    tracer.  VC alone handles direct lighting cleanly, and the VCM
//    auto-radius pre-pass should detect "no specular surfaces" and
//    disable VM entirely.  When VM is mistakenly enabled on a non-
//    caustic scene, the photon-density-estimation strategy gets
//    significant MIS weight even where its variance is high, and
//    the rendered image acquires visible photon-density splotches
//    at low spp.
//
//    REGRESSION FAMILY THIS TEST GUARDS AGAINST:
//      - foundSpecular check conflating delta-position lights
//        (omni / spot / directional — NEE-friendly) with delta-
//        surface scatters (specular reflection / refraction —
//        VM-required).  The former should NOT enable VM; the
//        latter should.
//      - Auto-radius computation drift that lands too generous a
//        radius on simple scenes.
//      - Any future change to VC/VM MIS weight formulas that
//        breaks the "VC alone is enough for diffuse direct
//        lighting" property.
//      - Convert*Subpath running-quantity bugs that propagate
//        delta-light handling incorrectly into eye-side MIS terms.
//
//    APPROACH: render minimal direct-lighting scenes with both
//    pathtracing_pel_rasterizer (PT, the trusted reference) and
//    vcm_pel_rasterizer (VC+VM enabled).  Capture the rendered
//    radiance buffer in memory via a custom IRasterizerOutput,
//    compute mean / median / p99 / max, and assert agreement within
//    tolerance.
//
//    Topologies exercised:
//      A. Delta-position omni light over a Lambertian surface
//         — the bug that motivated the "exclude delta lights from
//         foundSpecular" fix in VCMRasterizerBase auto-radius
//         pre-pass.  Pre-fix this scene rendered with visible
//         photon-density splotches at 4 spp.
//      B. Mesh area emitter over a Lambertian surface
//         — non-delta light variant; VM should also be disabled
//         (no specular surfaces).
//      C. Mixed delta + mesh light
//         — combines (A) and (B); validates the auto-radius pre-
//         pass walks the entire light subpath set correctly.
//      D. Thin-lens (FINITE-APERTURE) camera at f/22, focused
//      E. The same camera at f/2.8, defocused by 2.4 pixels
//         — VCM's t=1 splat (SplatLightSubpathToCamera) has to
//         SAMPLE a point on the aperture, connect to THAT point and
//         divide by its area density, which is what cancels the
//         1/A_lens inside the thin-lens importance.  Debt 28.
//      F. The same defocused camera with a SIX-BLADED aperture
//         — the polygonal branch of SampleAperture, whose area is
//         0.827 of the circumscribed disk's.
//      G. Orthographic (delta-DIRECTION) camera + mesh emitter
//         — VCM's splat pass must SKIP a delta-direction camera the
//         way BDPT's t==1 branch does.  Debt 28 review A P2-6.
//      H. SUBMERGED Lambertian floor under a delta water box, small
//         sphere emitter and camera both in air
//         — the eta^2 basic-radiance factor at a dielectric interface.
//         VCM's MERGE carries flux from the light side (no factor) and
//         must be paired with an eye subpath that DOES carry 1/n^2;
//         until 2026-09-12 neither side had it and VCM read 1.149x PT
//         here.  Debt 30; see the row and
//         docs/REFRACTIVE_RADIANCE_SCALING.md.
//      I. AIR ceiling patch above a delta water box, diffuse floor and
//         sphere emitter both SUBMERGED (debt 30 review round 2) — a
//         CONSISTENCY PIN, not a red row: the same physical path
//         (emitter -> submerged floor -> refract through the interface
//         -> ceiling patch -> camera) is reached by strategies on BOTH
//         sides of the eta^2 asymmetry at once, and VCM's own MIS
//         mixture of them has to already agree with PT.  VC connections
//         from the ceiling patch's eye vertex that continue (BSDF-
//         sampled) through the interface to the emitter, or to the
//         submerged floor followed by an NEE connection there, both
//         carry the eye-side 1/n^2 debt-30 factor (a RADIANCE-mode walk
//         crossing air -> water).  VCM's t=1-shaped light-tracing splat
//         reaches the SAME ceiling vertex from the emitter side (an
//         IMPORTANCE-mode light subpath refracting water -> air) and
//         correctly carries NO factor (§6 of the doc).  Two different
//         estimators of the same path, on the two sides of an
//         asymmetric rule, already have to reconcile via ordinary MIS
//         — this pins that they do.  Expected GREEN before and after
//         this round's TranslucentSPF fix (which this scene's materials
//         never touch); a future change that reintroduces the factor on
//         only one of the two sides is what would turn this row red.
//
//    Caustic-required scenes are out of scope here for the same
//    reason as in BDPTStrategyBalanceTest: PT under-samples
//    caustics relative to VCM, so PT-vs-VCM mean comparison flags
//    a sampling-efficiency disparity rather than a correctness
//    regression.  Caustic regressions need a baseline-image or
//    ground-truth comparison framework, which is heavier than
//    what this test provides.
//
//    Tolerance: 8% mean / 25% p99 / 100% (2x) max — same as the
//    BDPT test.  The auto-radius / foundSpecular bug produced
//    visibly larger spreads than this on a 256x256 render at
//    4 spp; on the smaller 32x32 grids this test uses, the
//    splotches still trip the p99 and max comparisons cleanly.
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <fstream>
#include <vector>
#include <cmath>
#include <limits>
#include "../src/Library/Interfaces/IOptions.h"
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
#include "../src/Library/Utilities/SobolSampler.h"

using namespace RISE;
using namespace RISE::Implementation;

//! DL-365: per-render Sobol transport VALUE salt. The pixel seed alone is
//! fixed, so an advancing camera RNG does not decorrelate transport QMC.
//! DL-367 additionally resets camera randomness in topology U and its replay
//! probe. Other topology probes retain their existing advancing camera RNG.
//! The command-line seed base selects the transport salt sequence.
static const unsigned int kDefaultSeedBase = 1729u;
static unsigned int g_seedBase = kDefaultSeedBase;
static unsigned int g_renderIndex = 0;
static bool g_dl367CompleteRenderSeed = false;
static const uint32_t kSaltTag = 0x365u;

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
// CapturingRasterizerOutput / ImageStats — same instrumentation as
// BDPTStrategyBalanceTest.  See that file's header comment for the
// rationale on the four metrics (mean / median / p99 / max).
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
	if( cap.pixels.empty() ) return s;

	std::vector<double> ch[3];
	for( int c = 0; c < 3; c++ ) ch[c].reserve( cap.pixels.size() );

	// Compare radiance composited over black, matching the BDPT harness.
	// The legacy PT stores surface RGB plus coverage alpha; VCM may
	// already include coverage in RGB. Raw RGB differs at silhouettes.
	//
	// DL-40: a nonfinite (NaN/Inf) captured component is a broken render,
	// not a statistic -- reject the whole capture (return invalid) before
	// sort/sum ever touches it.  See the matching comment/fix in
	// BDPTStrategyBalanceTest.cpp's ComputeStats (same sibling pattern).
	bool allFinite = true;
	for( const RISEColor& c : cap.pixels ) {
		const double r = c.base.r * c.a, g = c.base.g * c.a, b = c.base.b * c.a;
		if( !std::isfinite( r ) || !std::isfinite( g ) || !std::isfinite( b ) ) {
			allFinite = false;
			break;
		}
		ch[0].push_back( r );
		ch[1].push_back( g );
		ch[2].push_back( b );
	}
	if( !allFinite ) {
		return ImageStats{};   // valid stays false
	}

	for( int c = 0; c < 3; c++ ) {
		double sum = 0;
		for( double v : ch[c] ) sum += v;
		s.mean[c]   = sum / double(ch[c].size());
		s.median[c] = Percentile( ch[c], 0.50 );
		s.p99[c]    = Percentile( ch[c], 0.99 );
		s.max[c]    = ch[c].back();
	}
	s.valid = true;
	return s;
}

// Exact measurement oracle: surface RGB with coverage and already
// composited RGB with alpha one describe the same sensor radiance.
static bool TestCompositedStats()
{
	CapturingRasterizerOutput* cap = new CapturingRasterizerOutput();
	cap->width = 2;
	cap->height = 1;
	cap->pixels = { RISEColor(RISEPel(0.8, 0.4, 0.2), 0.25),
	                RISEColor(RISEPel(0.0, 0.0, 0.0), 0.0) };
	const ImageStats surface = ComputeStats(*cap);
	cap->pixels[0] = RISEColor(RISEPel(0.2, 0.1, 0.05), 1.0);
	const ImageStats composite = ComputeStats(*cap);
	const double expected[] = {0.1, 0.05, 0.025};
	bool valid = surface.valid && composite.valid;
	for (unsigned c = 0; c < 3; ++c) {
		valid = valid && std::fabs(surface.mean[c] - expected[c]) < 1e-12
		              && std::fabs(surface.mean[c] - composite.mean[c]) < 1e-12;
	}
	Check(valid, "capture statistics compose coverage alpha over black");
	if (!valid) {
		std::cout << "  surface mean R=" << surface.mean[0]
		          << " composited mean R=" << composite.mean[0]
		          << " expected R=" << expected[0] << std::endl;
	}
	cap->release();
	return valid;
}

static std::string WriteSceneToTempFile( const char* sceneText, const char* tag )
{
	char path[512];
	std::snprintf( path, sizeof(path),
		"%s/vcm_strategy_balance_%s_%d.RISEscene",
		std::getenv( "TMPDIR" ) ? std::getenv( "TMPDIR" ) : "/tmp", tag, static_cast<int>(::getpid()) );

	std::ofstream ofs( path );
	if( !ofs.is_open() ) return std::string();
	ofs << sceneText;
	ofs.close();
	return std::string( path );
}

static ImageStats RenderAndComputeStats( const char* scenePath, uint64_t* pixelHash = nullptr )
{
	ImageStats result{};

	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) return result;

	if( !pJob->LoadAsciiSceneViaCst( scenePath ) ) {
		safe_release( pJob );
		return result;
	}

	pJob->RemoveRasterizerOutputs();
	CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "test capture output" );
	pJob->GetRasterizer()->AddRasterizerOutput( pCap );

	// DL-365: fresh libc seed AND a fresh Sobol' value salt per render.
	// `std::srand` alone (the pre-DL-365 state of this function) leaves
	// every render of a scene drawing the IDENTICAL Sobol' points --
	// worker-thread scheduling still makes float summation order
	// non-bit-reproducible, but that noise is orders of magnitude
	// smaller than the QMC error a real independent sample carries.  The
	// salt randomizes the transport QMC stream. Camera sampling also
	// consumes GlobalRNG: DL-367 complete-render probes reset it below;
	// older topology measurements leave that stream advancing.
	const uint32_t salt = SobolSequence::HashCombine( g_seedBase + g_renderIndex, kSaltTag );
	SobolSamplerTestHooks::ValueSalt().store( salt );
	const unsigned renderSeed=g_seedBase+g_renderIndex++;
    std::srand(renderSeed);
// DL-367: camera scrambling draws from GlobalRNG, not libc rand.
    if(g_dl367CompleteRenderSeed) GlobalRNG().Reseed(renderSeed);
	const bool bRendered = pJob->Rasterize();
	SobolSamplerTestHooks::ValueSalt().store( 0u );
	if( !bRendered ) {
		safe_release( pCap );
		safe_release( pJob );
		return result;
	}

	result = ComputeStats( *pCap );
    if(pixelHash) {
        uint64_t h=14695981039346656037ull;
        for(const auto& color:pCap->pixels) for(double component:{color.base.r,color.base.g,color.base.b,color.a}) {
            uint64_t bits; std::memcpy(&bits,&component,sizeof(bits));
            for(unsigned j=0;j<8;++j) {h^=(bits>>(8*j))&255u;h*=1099511628211ull;}
        }
        *pixelHash=h;
    }

	safe_release( pCap );
	safe_release( pJob );
	return result;
}

static bool ChannelsAgree(
	const double a[3], const double b[3], double relTol, double absFloor )
{
	for( int c = 0; c < 3; c++ ) {
		// DL-40: a nonfinite operand on either side must disagree -- see
		// the matching fix/comment in BDPTStrategyBalanceTest.cpp.
		if( !std::isfinite( a[c] ) || !std::isfinite( b[c] ) ) return false;
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
// DL-40 red-proof: nonfinite candidate statistics must be REJECTED, not
// silently agree.  Sibling of BDPTStrategyBalanceTest.cpp's identically
// named test -- see that file's header comment for the two-layer
// rationale (ComputeStats rejects a poisoned capture; ChannelsAgree
// rejects a poisoned stat directly).
//////////////////////////////////////////////////////////////////////
static void TestNonfiniteCandidateRejected()
{
	std::cout << std::endl << "-- DL-40: nonfinite candidate statistics are rejected --" << std::endl;

	{
		CapturingRasterizerOutput* cap = new CapturingRasterizerOutput();
		cap->width = 2; cap->height = 1;
		cap->pixels.push_back( RISEColor( RISEPel( 0.5, 0.5, 0.5 ), 1.0 ) );
		cap->pixels.push_back( RISEColor( RISEPel( std::nan(""), 0.2, 0.2 ), 1.0 ) );
		const ImageStats s = ComputeStats( *cap );
		Check( !s.valid, "DL-40: ComputeStats rejects a capture with a NaN pixel component (valid==false)" );
		cap->release();
	}
	{
		CapturingRasterizerOutput* cap = new CapturingRasterizerOutput();
		cap->width = 1; cap->height = 1;
		cap->pixels.push_back( RISEColor( RISEPel( 0.3, std::numeric_limits<double>::infinity(), 0.3 ), 1.0 ) );
		const ImageStats s = ComputeStats( *cap );
		Check( !s.valid, "DL-40: ComputeStats rejects a capture with an Inf pixel component (valid==false)" );
		cap->release();
	}
	{
		CapturingRasterizerOutput* cap = new CapturingRasterizerOutput();
		cap->width = 1; cap->height = 1;
		cap->pixels.push_back( RISEColor( RISEPel( 0.4, 0.5, 0.6 ), 1.0 ) );
		const ImageStats s = ComputeStats( *cap );
		Check( s.valid, "DL-40: ComputeStats control -- an all-finite capture stays valid" );
		cap->release();
	}

	{
		const double ref[3]  = { 0.5, 0.5, 0.5 };
		const double cand[3] = { std::nan(""), 0.5, 0.5 };
		Check( !ChannelsAgree( ref, cand, /*relTol=*/1000.0, /*absFloor=*/1e-6 ),
		       "DL-40: ChannelsAgree rejects a NaN candidate[0] even at relTol=1000" );
	}
	{
		const double ref[3]  = { 0.5, 0.5, 0.5 };
		const double cand[3] = { 0.5, 0.5, std::numeric_limits<double>::infinity() };
		Check( !ChannelsAgree( ref, cand, /*relTol=*/1000.0, /*absFloor=*/1e-6 ),
		       "DL-40: ChannelsAgree rejects an Inf candidate[2] even at relTol=1000" );
	}
	{
		const double ref[3]  = { std::nan(""), 0.5, 0.5 };
		const double cand[3] = { 0.5, 0.5, 0.5 };
		Check( !ChannelsAgree( ref, cand, /*relTol=*/1000.0, /*absFloor=*/1e-6 ),
		       "DL-40: ChannelsAgree rejects a NaN REFERENCE too, not just a NaN candidate" );
	}
	{
		const double ref[3]  = { 0.5, 0.5, 0.5 };
		const double cand[3] = { 0.5, 0.5, 0.5 };
		Check( ChannelsAgree( ref, cand, /*relTol=*/0.01, /*absFloor=*/1e-6 ),
		       "DL-40: ChannelsAgree control -- identical finite stats still agree" );
	}
}

//////////////////////////////////////////////////////////////////////
// Scene fragments.  Same geometry/lights as the BDPT test so the two
// suites' coverage is directly comparable; only the rasterizer chunk
// differs.
//////////////////////////////////////////////////////////////////////

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

// REFERENCE (legacy-deprecation Phase 2, 2026-10-10): the modern
// pathtracing_pel_rasterizer.  Until then it was the FROZEN legacy
// pixelpel_rasterizer with a DefaultDirectLighting-only chain, which was
// valid only while every topology is single-bounce direct lighting on a
// flat quad (CLOTH_FABRIC_DESIGN.md 15 debt 26); on these topologies the
// full path tracer carries the same energy, so the switch moves no band.
// Topologies that need a real path tracer for other reasons keep their own
// named references below.
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
	"\tpixel_filter box\n"
	"\toidn_denoise FALSE\n"
	"}\n"
	"\n"
	"file_rasterizeroutput\n"
	"{\n"
	"\tpattern rendered/vcm_balance_pt_unused\n"
	"\ttype EXR\n"
	"\tbpp 32\n"
	"\tcolor_space Rec709RGB_Linear\n"
	"}\n";

// VCM with both VC and VM enabled and merge_radius=0 (auto).  This is
// the configuration that revealed the foundSpecular delta-light bug —
// without the VCMRasterizerBase fix, the auto-radius would activate VM
// on these scenes and produce visible splotches that fail this test.
//
// DL-365 AUDIT (2026-09-28): this depth-3 budget drives topologies A/B/C
// above, whose scene is one flat quad directly lit by the light (no
// second surface exists to bounce off), so the only physically possible
// paths are camera->quad->light -- depth 3 is already headroom, not a
// cap. Confirmed by measurement, not just the geometry argument:
// topology C ("mixed delta+mesh lights", the row with the largest
// VCM/PT gap in this group) reads mean 0.9628 (sd 0.31%) at depth 3 and
// 0.9611 (sd 0.20%) at depth 16, salted n=4 each -- 0.69 pooled sd
// apart, i.e. the same VCM auto-radius/merge-density characteristic at
// both depths, not a truncation.  Left at depth 3.
static const char* kRasterizerVCM =
	"standard_shader\n"
	"{\n"
	"\tname global\n"
	"\tshaderop DefaultPathTracing\n"
	"}\n"
	"\n"
	"vcm_pel_rasterizer\n"
	"{\n"
	"\tmax_eye_depth 3\n"
	"\tmax_light_depth 3\n"
	"\tsamples 32\n"
	"\tmerge_radius 0.0\n"
	"\tvc_enabled true\n"
	"\tvm_enabled true\n"
	"\tpixel_filter box\n"
	"\toidn_denoise FALSE\n"
	"}\n"
	"\n"
	"file_rasterizeroutput\n"
	"{\n"
	"\tpattern rendered/vcm_balance_vcm_unused\n"
	"\ttype EXR\n"
	"\tbpp 32\n"
	"\tcolor_space Rec709RGB_Linear\n"
	"}\n";

static const char* kLightOmni =
	"omni_light\n"
	"{\n"
	"\tname l_omni\n"
	"\tpower 4.0\n"
	"\tcolor 1.0 1.0 1.0\n"
	"\tposition 0.0 0.0 5.0\n"
	"}\n";

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

struct Tolerances
{
	double meanTol;
	double p99Tol;
	double maxTol;
};

static const Tolerances kStrictTolerances{ 0.08, 0.25, 1.00 };

//////////////////////////////////////////////////////////////////////
// RunTopologyTest — render PT and VCM versions of the same scene and
// require VCM to match PT on each statistical metric within tolerance.
// PT is the trusted reference; VCM must converge to the same image
// distribution for non-caustic scenes.
//////////////////////////////////////////////////////////////////////
//! A tolerance as a percent label ("0.5", "8") -- `int( tol * 100 )`
//! printed a 0.5 % band as "0%".
static std::string PercentLabel( const double tol )
{
	char buf[32];
	std::snprintf( buf, sizeof( buf ), "%g", tol * 100.0 );
	return std::string( buf );
}

static void RunTopologyTest(
	const char* topologyName,
	const std::string& sceneCommonBlock,
	const Tolerances& tol = kStrictTolerances,
	// Per-topology rasterizer strings.  Default to this file's shared
	// 32-spp pair; topologies D / E override them with a 512-spp pair
	// because a strongly defocused render is far noisier per pixel (see
	// the comment above SceneCommonThinLens).  The two strings must
	// still differ ONLY in the rasterizer chunk.
	const char* ptRasterizer = kRasterizerPT,
	const char* vcmRasterizer = kRasterizerVCM )
{
	std::cout << "Testing PT-vs-VCM: " << topologyName << std::endl;

	const std::string ptScene  = std::string("RISE ASCII SCENE 7\n") + ptRasterizer  + sceneCommonBlock;
	const std::string vcmScene = std::string("RISE ASCII SCENE 7\n") + vcmRasterizer + sceneCommonBlock;

	const std::string ptPath  = WriteSceneToTempFile( ptScene.c_str(),  "pt"  );
	const std::string vcmPath = WriteSceneToTempFile( vcmScene.c_str(), "vcm" );

	if( ptPath.empty() || vcmPath.empty() ) {
		Check( false, ( std::string("temp file write: ") + topologyName ).c_str() );
		return;
	}

	const ImageStats pt  = RenderAndComputeStats( ptPath.c_str()  );
	const ImageStats vcm = RenderAndComputeStats( vcmPath.c_str() );

	PrintStats( "PT ", pt );
	PrintStats( "VCM", vcm );

	std::remove( ptPath.c_str() );
	std::remove( vcmPath.c_str() );

	Check( pt.valid,  ( std::string("PT render produced output: ")  + topologyName ).c_str() );
	Check( vcm.valid, ( std::string("VCM render produced output: ") + topologyName ).c_str() );
	if( !pt.valid || !vcm.valid ) return;

	const double brightness = pt.mean[0] + pt.mean[1] + pt.mean[2];
	Check( brightness > 1e-4,
		( std::string("PT mean is non-zero: ") + topologyName ).c_str() );

	const double absFloor = 1e-6;
	const bool meanMatch = ChannelsAgree( pt.mean, vcm.mean, tol.meanTol, absFloor );
	const bool p99Match  = ChannelsAgree( pt.p99,  vcm.p99,  tol.p99Tol,  absFloor );
	const bool maxMatch  = ChannelsAgree( pt.max,  vcm.max,  tol.maxTol,  absFloor );

	Check( meanMatch, ( std::string("VCM mean within ")
		+ PercentLabel( tol.meanTol ) + "% of PT: " + topologyName ).c_str() );
	Check( p99Match,  ( std::string("VCM p99 within ")
		+ std::to_string(int(tol.p99Tol*100))  + "% of PT: " + topologyName ).c_str() );
	Check( maxMatch,  ( std::string("VCM max within ")
		+ std::to_string(int(tol.maxTol*100))  + "x of PT: " + topologyName ).c_str() );

	if( !meanMatch ) PrintRelDiff( "mean", pt.mean, vcm.mean, absFloor );
	if( !p99Match  ) PrintRelDiff( "p99",  pt.p99,  vcm.p99,  absFloor );
	if( !maxMatch  ) PrintRelDiff( "max",  pt.max,  vcm.max,  absFloor );
}

//////////////////////////////////////////////////////////////////////
// Topology A: delta-position omni light, no specular surfaces.
//
// THIS IS THE CANARY for the foundSpecular bug.  Without the fix in
// VCMRasterizerBase::PreRenderSetup, the omni light's isDelta=true on
// lightVerts[0] makes the auto-radius pre-pass set foundSpecular=true.
// VM then activates with a too-generous radius, MIS gives photon-density
// estimation significant weight, and at 4-32 spp the rendered image
// has visible photon-density splotches.  After the fix, this test
// converges to PT's clean direct-lighting output.
//////////////////////////////////////////////////////////////////////
static void TestDeltaOmniLight()
{
	RunTopologyTest( "delta-position omni light",
		std::string( kSceneCommon ) + kLightOmni );
}

//////////////////////////////////////////////////////////////////////
// Topology B: mesh area emitter only.
//
// Non-delta light variant.  The auto-radius pre-pass should also set
// foundSpecular=false here (no specular surfaces, no delta lights at
// all).  Baseline regression check — if this starts diverging, the
// bug is in non-delta MIS handling, not delta-light gating.
//////////////////////////////////////////////////////////////////////
static void TestMeshEmitterOnly()
{
	RunTopologyTest( "mesh area emitter",
		std::string( kSceneCommon ) + kLightMesh );
}

//////////////////////////////////////////////////////////////////////
// Topology C: omni delta + mesh area, both active.
//
// Validates that the auto-radius pre-pass walks the entire light-
// subpath set (not just one path) when deciding foundSpecular.  Mixed-
// light scenes are a common configuration and a likely source of
// future regressions if the gating logic is touched.
//
// KNOWN RESIDUAL (2026-09-05, recorded in docs/CLOTH_FABRIC_DESIGN.md
// 15 debt 24's "still open" note): this topology reads VCM/PT ~0.9635,
// unchanged by the debt-24 delta-light wCamera fix (0.9645 -> 0.9643)
// and inside the 8 % band.  Debt 24's closure is exact for a PURE delta
// light and a PURE non-delta light; a mixed-light scene is where a
// residual partition wrinkle would show (light-selection pmf vs the two
// classes' different dVC/dVCM seeding is the first suspect).  Not
// diagnosed.  Do not tighten this band without root-causing it, and do
// not treat a move from 0.9635 as noise.
//////////////////////////////////////////////////////////////////////
static void TestMixedLights()
{
	RunTopologyTest( "mixed delta+mesh lights",
		std::string( kSceneCommon ) + kLightOmni + kLightMesh );
}

//////////////////////////////////////////////////////////////////////
// Topologies D / E: FINITE-APERTURE (thin-lens) camera.  Debt 28.
//
// The same two scenes as BDPTStrategyBalanceTest's topologies G / H;
// that file carries the full derivation of the geometry (a 36 mm
// sensor on a 15.1 mm lens is 100.0 deg of vertical field of view, the
// emitter sits 0.5 above the receiver so the t=1 strategy carries real
// MIS weight, and topology E's `focus_distance 0.03` puts a 2.4-pixel
// circle of confusion -- 1.2 px radius -- on the receiver; that file's
// derivation was corrected from 4.8 px in the debt 28 review, B P2-2,
// where an (S1 - f) that should have been S1 doubled it).
//
// VCM is hurt far worse than BDPT by this defect because its t=1
// weight carries an explicit / mLightSubPathCount: on these scenes
// BDPT's splat layer lands at ~6% MIS weight while VCM's lands near
// full weight, so the same 1/(cos(theta)*A_lens) inflation shows up
// roughly 60x larger in the VCM mean.
//
// The reference is `pathtracing_pel_rasterizer` at 512 spp (the legacy
// `pixelpel_rasterizer` until legacy-deprecation Phase 2, 2026-10-10):
// both topologies are single-bounce direct lighting on one flat quad with
// a non-scattering luminaire, and a thin lens changes only which ray each
// film sample generates (`ICamera::GenerateRay`), so PT here has the SAME
// defocus blur as VCM.  When the legacy reference was in use it was
// CROSS-CHECKED against this modern one at the same 512 spp: focused
// 0.0462 legacy vs 0.0452 modern (+2.2%), defocused 0.0440 vs 0.0449
// (-2.0%), both comfortably inside the 8% band.
//
// 512 spp, not this file's usual 32: at 32 spp the defocused row's mean
// swings ~5% run to run (per-pixel sigma/mu 156% against the focused
// row's 23%), over half the band.  At 512 spp both rows are stable to
// under 1% and the pair costs ~2 s.
//
// MEASURED PRE-FIX on this machine, VCM mean / PT mean (R channel):
//   D (f/22,  focused):   18666.3 / 0.0461453 = 404511x over
//   E (f/2.8, defocused):   302.401 / 0.0451259 =  6701x over
// and POST-FIX (same run configuration):
//   D: 0.0450732 / 0.0460411 = 0.979
//   E: 0.0450605 / 0.0455397 = 0.989
// (each of these is one draw from a run-to-run spread of a few
// percentage points; the band, not the figure, is the contract)
//////////////////////////////////////////////////////////////////////
static std::string SceneCommonThinLens(
	const char* fstop,
	const char* focusDistance,
	// Topology F: a POLYGONAL aperture.  0 keeps the default disk.
	const char* apertureBlades = "0" )
{
	return std::string(
		"film\n"
		"{\n"
		"\twidth 32\n"
		"\theight 32\n"
		"}\n"
		"\n"
		"thinlens_camera\n"
		"{\n"
		"\tlocation 0 0 6\n"
		"\tlookat 0 0 0\n"
		"\tup 0 1 0\n"
		"\tsensor_size 36\n"
		"\tfocal_length 15.1\n"
		"\tfstop " ) + fstop + "\n"
		"\tfocus_distance " + focusDistance + "\n"
		"\taperture_blades " + apertureBlades + "\n"
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
		"\tpta -20 -20 0\n"
		"\tptb 20 -20 0\n"
		"\tptc 20 20 0\n"
		"\tptd -20 20 0\n"
		"}\n"
		"\n"
		"standard_object\n"
		"{\n"
		"\tname obj_quad\n"
		"\tgeometry quad\n"
		"\tmaterial mat_diffuse\n"
		"}\n"
		"\n"
		"uniformcolor_painter\n"
		"{\n"
		"\tname pnt_emit_tl\n"
		"\tcolor 1.0 1.0 1.0\n"
		"}\n"
		"\n"
		"lambertian_luminaire_material\n"
		"{\n"
		"\tname mat_emit_tl\n"
		"\texitance pnt_emit_tl\n"
		"\tscale 20.0\n"
		"\tmaterial none\n"
		"}\n"
		"\n"
		"clippedplane_geometry\n"
		"{\n"
		"\tname quad_emit_tl\n"
		"\tpta -0.5 0.5 0.5\n"
		"\tptb 0.5 0.5 0.5\n"
		"\tptc 0.5 -0.5 0.5\n"
		"\tptd -0.5 -0.5 0.5\n"
		"}\n"
		"\n"
		"standard_object\n"
		"{\n"
		"\tname obj_emit_tl\n"
		"\tgeometry quad_emit_tl\n"
		"\tmaterial mat_emit_tl\n"
		"}\n";
}

// 512-spp twins of kRasterizerPT / kRasterizerVCM -- identical in every
// other respect so the pair still isolates the integrator.
//
// DL-365 AUDIT (2026-09-28): this depth-3 budget drives the thin-lens/
// orthographic topologies (D-G), the same single-bounce flat-quad scene
// as kRasterizerVCM above, just re-imaged through a finite-aperture or
// orthographic camera -- no additional depth is reachable regardless of
// the camera model. Measured at depth 3 vs depth 16 (salted n=4 each,
// mean (sd), pooled-sd separation):
//   f/22 focused:        1.0011 (1.10%) -> 0.9996 (0.64%)  -- 0.17 sd
//   f/2.8 defocused:     0.9966 (1.50%) -> 0.9944 (1.30%)  -- 0.15 sd
//   f/2.8 six-bladed:    0.9934 (0.74%) -> 0.9949 (1.02%)  -- 0.17 sd
//   orthographic:        0.9997 (0.17%) -> 0.9998 (0.15%)  -- 0.08 sd
// All four are well under 1 sd apart -- genuinely converged, left at
// depth 3.
static const char* kRasterizerPT512 =
	"standard_shader\n"
	"{\n"
	"\tname global\n"
	"\tshaderop DefaultPathTracing\n"
	"}\n"
	"\n"
	"pathtracing_pel_rasterizer\n"
	"{\n"
	"\tsamples 512\n"
	"\tpixel_filter box\n"
	"\toidn_denoise FALSE\n"
	"}\n"
	"\n"
	"file_rasterizeroutput\n"
	"{\n"
	"\tpattern rendered/vcm_balance_pt_unused\n"
	"\ttype EXR\n"
	"\tbpp 32\n"
	"\tcolor_space Rec709RGB_Linear\n"
	"}\n";

static const char* kRasterizerVCM512 =
	"standard_shader\n"
	"{\n"
	"\tname global\n"
	"\tshaderop DefaultPathTracing\n"
	"}\n"
	"\n"
	"vcm_pel_rasterizer\n"
	"{\n"
	"\tmax_eye_depth 3\n"
	"\tmax_light_depth 3\n"
	"\tsamples 512\n"
	"\tmerge_radius 0.0\n"
	"\tvc_enabled true\n"
	"\tvm_enabled true\n"
	"\tpixel_filter box\n"
	"\toidn_denoise FALSE\n"
	"}\n"
	"\n"
	"file_rasterizeroutput\n"
	"{\n"
	"\tpattern rendered/vcm_balance_vcm_unused\n"
	"\ttype EXR\n"
	"\tbpp 32\n"
	"\tcolor_space Rec709RGB_Linear\n"
	"}\n";

static void TestThinLensStoppedDown()
{
	RunTopologyTest( "thin-lens f/22 camera, focused (finite aperture)",
		SceneCommonThinLens( "22", "6.0" ),
		kStrictTolerances, kRasterizerPT512, kRasterizerVCM512 );
}

static void TestThinLensWideOpenDefocused()
{
	RunTopologyTest( "thin-lens f/2.8 camera, 2.4 px defocus (finite aperture)",
		SceneCommonThinLens( "2.8", "0.03" ),
		kStrictTolerances, kRasterizerPT512, kRasterizerVCM512 );
}

//////////////////////////////////////////////////////////////////////
// Topology F: SIX-BLADED aperture, f/2.8, same defocus as E.
//
// Runs the shaped-aperture branch of `ThinLensCamera::SampleAperture`
// and the polygonal `GetApertureWorldArea` end to end through VCM's
// splat pass.  Like BDPT's topology I it is a PATH-COVERAGE guard, not
// a shape one: measured, forcing the connection side onto the disk
// branch while the eye rays stay hexagonal leaves VCM/PT at 0.997,
// well inside the band -- one draw from a run-to-run spread of a few
// percentage points; the band, not the figure, is the contract --
// because debt 28's fix means the aperture area cancels out of
// `Importance` and the shape only decides which pixel a splat lands
// in.  The shape/density guarantee is asserted in closed form by
// tests/CameraImportanceTest.cpp Test 1.  See the fuller note above
// BDPTStrategyBalanceTest's topology I.
//
// Defocused on purpose: at the plane of focus every aperture point
// images to the same pixel.
//////////////////////////////////////////////////////////////////////
static void TestThinLensBladedAperture()
{
	RunTopologyTest( "thin-lens f/2.8, six-bladed aperture, defocused",
		SceneCommonThinLens( "2.8", "0.03", "6" ),
		kStrictTolerances, kRasterizerPT512, kRasterizerVCM512 );
}

//////////////////////////////////////////////////////////////////////
// Topology G: ORTHOGRAPHIC (delta-DIRECTION) camera + mesh emitter.
//
// The mirror of BDPTStrategyBalanceTest's topology D, and it was
// missing.  Writing it exposed that VCM's orthographic support was
// broken in TWO places, not one (debt 28 review, A P2-6, and what the
// row found once it existed):
//
//  1. `SplatLightSubpathToCameraImpl` had no delta-direction guard.
//     An orthographic camera has zero density for the light-tracing
//     strategy -- every pixel has its OWN ray origin, so there is no
//     single camera vertex a light vertex can connect to -- and
//     `BDPTIntegrator`'s t==1 branch has skipped it since the
//     IsDeltaDirection fix.  VCM splatted anyway.
//  2. The camera vertex's `emissionPdfW`, which `InitCamera` turns
//     into `dVCM = N / cameraPdfW`, carried
//     `PdfDirection`'s ORTHOGRAPHIC return -- 1/A_image, an AREA
//     density where the recurrence wants a solid-angle one.  dVCM is
//     the MIS mass reserved for the t==1 strategy; for a
//     delta-direction camera that strategy does not exist, so the
//     value must be 0 (exactly what SmallVCM does on the light side
//     for a delta light, and the mirror of the `isDelta` flag BDPT
//     already sets on this same vertex).
//
// Historical orthographic measurements in the introducing commit used
// raw capture RGB and the legacy PT's default filter. They do not specify
// the current composited, box-filtered observable. DL-01's measurement
// cleanup exposed this mismatch; TestCompositedStats red-proves the
// coverage convention independently of any transport code.
//
// Preserve the established 4% mean band (p99/max retain strict values).
// The two transport safeguards above remain required; this harness change
// affects only what is measured at partial-coverage silhouette pixels.
//////////////////////////////////////////////////////////////////////

static const Tolerances kOrthoTolerances{ 0.04, 0.25, 1.00 };

static void TestOrthographicCamera()
{
	// Same alpha-convention note as the BDPT twin: the 2x2 quad inside
	// a 2.5x2.5 viewport gives partial-coverage edge pixels, and
	// ComputeStats compares COMPOSITED radiance so the two conventions
	// do not confound the mean.
	RunTopologyTest( "orthographic delta-direction camera + mesh emitter",
		std::string( kSceneCommonOrtho ) + kLightMesh, kOrthoTolerances );
}

//////////////////////////////////////////////////////////////////////
// Topology H: SUBMERGED Lambertian floor, small sphere emitter in air,
// camera in air (debt 30).
//
// The eye path crosses a delta dielectric interface before it reaches
// the shading point, and the light side reaches that same point two
// different ways: as a BSDF-sampled connection to the emitter (which
// re-crosses the interface, so the eye walk's eta factors cancel) and
// as a MERGE against photons that crossed it in the importance
// direction (where nothing cancels).  Until 2026-09-12 RISE applied no
// eta^2 basic-radiance factor anywhere, so the merge read n^2 = 1.77x
// too bright relative to the connection, and VCM's MIS mixture landed
// between the two.
//
// RED-PROOF on the unfixed library (b6c12301), this row's own run:
//   PT  mean (0.00462878, 0.00462888, 0.00462842)
//   VCM mean (0.00539856, 0.00540048, 0.00539207)
//   mean relative diff (16.63%, 16.67%, 16.50%)   FAIL
// p99 and max were inside the loosened bands even unfixed, so the
// mean is the assertion doing the work.  Green after the fix.
//
// Re-measured 2026-09-12 (debt-30 review round 1): VCM/PT mean =
// 0.00467741/0.00463638 = 1.0088.  The 8% mean band is sized to cover
// VCM's per-run auto-radius merge-density drift (see this row's own
// `effective_radius` log line each run), not to pin the ratio to a
// fixed constant -- docs/REFRACTIVE_RADIANCE_SCALING.md §4 quotes a
// DIFFERENT scene's diagnostic probe (radius 0.03) at 1.046
// (0.004908/0.004690 = 1.0465, rounds to 1.046), and
// RefractiveRadianceScalingTest's row C (same 0.08 radius as this row)
// reads 1.0125 in the same re-measurement; all three are independent
// samples inside this band, not the same number three ways.
//
// REFERENCE.  `kRasterizerPTSubmerged` below, NOT the file's shared
// `kRasterizerPT`: this is the exact topology the caveat above
// kRasterizerPT used to warn about (a transmissive material in the scene,
// which the legacy pixelpel + DefaultDirectLighting reference could not
// carry); it keeps its own named reference.
//
// TOLERANCES.  The mean stays at the strict 8%.  p99 and max are
// loosened to 60% / 4x: with only BSDF sampling able to find a
// 0.08-radius emitter through a delta interface, both integrators
// carry genuine specular fireflies whose per-pixel tails do not agree
// at any practical sample count -- measured p99 spread across runs is
// tens of percent while the MEAN, which is the quantity the eta^2 bug
// moves, is stable to ~1%.  Loosening the tail bands rather than
// dropping them keeps a catastrophic tail regression visible.
//
// DL-365 (2026-09-28, this row's own DL-308 twin): the 8% mean band
// above was sitting on the SAME `max_eye_depth 5` / `max_light_depth 5`
// truncation docs/REFRACTIVE_RADIANCE_SCALING.md section 11 (DL-308)
// found on this suite's row C.  Depth 5 admits exactly one floor bounce
// under the water surface's total internal reflection; the rest of the
// floor's light, trapped there, is cut -- VCM at depth 5 read 0.905 to
// 0.925 of PT (matching PT at `max_diffuse_bounce 1`, the same one-
// bounce truncation), on the band edge, flipping red or green with the
// unsalted Sobol' pattern (a different `argv[1]` reused the identical
// Sobol' points under a different libc `rand()` seed, so a "repeat"
// never saw the QMC error at all -- see the kSaltTag comment above
// `RenderAndComputeStats`).  Depth sweep, salted independent renders,
// VCM/PT mean (sd), n as noted:
//   depth  5 (n=8): 0.9323 (sd 1.15%) -- 2 of 8 runs fail the 8% band
//   depth  8 (n=4): 0.9896 (sd 0.85%)
//   depth 16 (n=8): 1.0027 (sd 0.98%)
//   depth 32 (n=4): 0.9937 (sd 0.75%)
// depth 16 is within noise of depth 32 (means 1.0027 vs 0.9937, well
// under 1 sd apart), so both `max_eye_depth`/`max_light_depth` are now
// 16 for this topology's VCM string (PT stays untruncated, matching
// DL-308's row C fix).  The mean band tightens 8% -> 5%: at depth 16
// the band sits 3-4 sd from the mean in the worse direction: the slice's
// own n=8 batch read sd 0.98% (~4.9 sd), the merge review's independent
// n=9 batch read sd 1.65% (~3.0 sd), pooled n=17 ~3.7 sd -- this TIR-trap
// scene's sd is itself unstable across small batches, so do not quote a
// single batch's figure.  That is comparable to DL-308's own row C margin
// (">= 3.2 sd"), so 5% is not "as tight as the data allows", it is "no
// wider than needed" while keeping roughly the same safety margin DL-308
// set as precedent.  Every render in this file is now salted per
// render index (`SobolSamplerTestHooks::ValueSalt`, DL-308's own
// mechanism) -- see the file-level comment above `RenderAndComputeStats`.
//////////////////////////////////////////////////////////////////////
static const char* kSceneSubmergedFloor =
	"film\n"
	"{\n"
	"\twidth 64\n"
	"\theight 64\n"
	"}\n"
	"\n"
	"pinhole_camera\n"
	"{\n"
	"\tlocation 2.0 2.5 0\n"
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
	"\tpta -1 0.05 1\n"
	"\tptb 1 0.05 1\n"
	"\tptc 1 0.05 -1\n"
	"\tptd -1 0.05 -1\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_quad\n"
	"\tgeometry quad\n"
	"\tmaterial mat_diffuse\n"
	"}\n"
	"\n"
	"dielectric_material\n"
	"{\n"
	"\tname mat_water\n"
	"\tior 1.33\n"
	"\ttau 1.0\n"
	"\tscattering 1000000\n"
	"}\n"
	"\n"
	"box_geometry\n"
	"{\n"
	"\tname geo_water\n"
	"\twidth 2.2\n"
	"\theight 0.3\n"
	"\tdepth 2.2\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname water\n"
	"\tgeometry geo_water\n"
	"\tmaterial mat_water\n"
	"\tposition 0 0.15 0\n"
	"}\n"
	"\n"
	"uniformcolor_painter\n"
	"{\n"
	"\tname pnt_emit_sph\n"
	"\tcolor 1.0 1.0 1.0\n"
	"}\n"
	"\n"
	"lambertian_luminaire_material\n"
	"{\n"
	"\tname mat_emit_sph\n"
	"\texitance pnt_emit_sph\n"
	"\tscale 42.2\n"
	"\tmaterial none\n"
	"}\n"
	"\n"
	"sphere_geometry\n"
	"{\n"
	"\tname geo_emit_sph\n"
	"\tradius 0.08\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_emit_sph\n"
	"\tgeometry geo_emit_sph\n"
	"\tmaterial mat_emit_sph\n"
	"\tposition 0 2.5 0\n"
	"}\n";

static const char* kRasterizerPTSubmerged =
	"standard_shader\n"
	"{\n"
	"\tname global\n"
	"\tshaderop DefaultPathTracing\n"
	"}\n"
	"\n"
	"pathtracing_pel_rasterizer\n"
	"{\n"
	"\tsamples 4096\n"
	"\toidn_denoise FALSE\n"
	"\tpixel_filter box\n"
	"}\n"
	"\n"
	"file_rasterizeroutput\n"
	"{\n"
	"\tpattern rendered/vcm_balance_pt_submerged_unused\n"
	"\ttype EXR\n"
	"\tbpp 32\n"
	"\tcolor_space Rec709RGB_Linear\n"
	"}\n";

static const char* kRasterizerVCMSubmerged =
	"standard_shader\n"
	"{\n"
	"\tname global\n"
	"\tshaderop DefaultPathTracing\n"
	"}\n"
	"\n"
	"vcm_pel_rasterizer\n"
	"{\n"
	"\tmax_eye_depth 16\n"
	"\tmax_light_depth 16\n"
	"\tsamples 2048\n"
	"\tmerge_radius 0.0\n"
	"\tvc_enabled true\n"
	"\tvm_enabled true\n"
	"\tpixel_filter box\n"
	"\toidn_denoise FALSE\n"
	"}\n"
	"\n"
	"file_rasterizeroutput\n"
	"{\n"
	"\tpattern rendered/vcm_balance_vcm_submerged_unused\n"
	"\ttype EXR\n"
	"\tbpp 32\n"
	"\tcolor_space Rec709RGB_Linear\n"
	"}\n";

static const Tolerances kSubmergedTolerances{ 0.05, 0.60, 4.00 };

static void TestSubmergedFloorAreaLight()
{
	RunTopologyTest( "submerged Lambertian floor, sphere emitter in air (eta^2, debt 30)",
		std::string( kSceneSubmergedFloor ), kSubmergedTolerances,
		kRasterizerPTSubmerged, kRasterizerVCMSubmerged );
}

//////////////////////////////////////////////////////////////////////
// Topology I: AIR ceiling patch above a delta water box; a Lambertian
// FLOOR and a small sphere EMITTER both SUBMERGED (debt 30 review
// round 2, C2).
//
// This is the topology named in the review brief where the eye/light
// asymmetry is MIS-COMBINED across strategies rather than isolated to
// one side, as in H (VM-only) and its BDPT twin J (an in-and-out
// cancellation).  The camera sees only the ceiling patch "A"; the only
// way light reaches it is by crossing the delta water surface:
//
//   - VC, continuing from A's eye vertex: the eye subpath scatters
//     (BSDF-sampled, non-delta Lambertian) off A, crosses INTO the
//     water (a RADIANCE-mode medium change: air -> water, so debt 30's
//     (eta_before/eta_after)^2 = 1/1.33^2 = 0.5653 applies), then either
//     hits the emitter directly (no light vertex) or hits the submerged
//     floor "B" and connects to the emitter with an ordinary NEE (both
//     endpoints submerged, so that connection is unobstructed).  Either
//     way, the ONE interface crossing is on the EYE side and carries
//     the eta^2 factor.
//   - VCM's light-tracing splat, from the LIGHT side: a light subpath
//     rooted at the emitter (optionally bouncing off B first) crosses
//     OUT of the water to reach A -- an IMPORTANCE-mode medium change,
//     so debt 30 deliberately applies NO factor (§6) -- then splats A
//     directly to the camera (A and the camera are both in air; no
//     interface intervenes on that connection).
//
// Both are estimators of the SAME physical path family, combined by
// ordinary MIS at the shared vertex A.  This is therefore a
// CONSISTENCY PIN: it is expected to read GREEN both before and after
// this round's TranslucentSPF exit-loop fix (C1), which this scene's
// materials (lambertian_material, dielectric_material,
// lambertian_luminaire_material) never exercise.  What it guards
// against is a FUTURE change that applies the eta^2 factor to only one
// of the two strategies above -- e.g. a splat-side "fix" that adds the
// factor by mistake, or an eye-side change that drops it -- which would
// break this MIS combination while leaving H and J individually green.
//
// REFERENCE.  Plain `pathtracing_pel_rasterizer`, no
// `transparent_shadows` (its default is already false and this scene
// does not set it): PT reaches the emitter and the submerged floor
// purely by BSDF sampling through the delta interface, exactly the
// same family of scatter events VCM's VC strategies use, so it is an
// unbiased reference here (same reasoning as kRasterizerPTSubmerged
// above; this topology needs its own scene, not that one, because the
// ceiling patch and the floor/emitter placement are different).
//
// SPP CHOICE / SE.  Unlike H (a tiny BSDF-sampled emitter directly
// visible from a floor point, which needs 4096 spp to tame specular-
// looking fireflies), this scene's camera never has a chance of
// directly resolving the emitter's disk -- every visible pixel is the
// diffuse ceiling patch, multiple bounces removed from the source, so
// the image is smooth at low spp.  Measured on this scene (2048 spp,
// two renders, `srand` reseeded per run by the CLI's own
// `srand(GetMilliseconds())`, this round's own measurement):
//   run 1 mean 0.012498317, run 2 mean 0.012497488 -- 0.0066% apart.
// At 512 spp the two-run spread was still only 0.095%.  Both are far
// under the "SE < 1%" bar; 2048 spp was kept for headroom, not because
// it was needed.
//
// MEASURED RATIOS (this round, one render each; BDPT and VCM re-run
// once more to confirm run-to-run stability before picking bands):
//   PT   mean 0.012496226  (reference)
//   BDPT mean 0.012631816, 0.012620802 (twin -- topology K in
//        BDPTStrategyBalanceTest.cpp)             -> ratio 1.010-1.011
//   VCM  mean 0.012222825, 0.012205491            -> ratio 0.977-0.978
// p99:  BDPT 0.021637727/0.021515656 -> ratio 1.059-1.065 vs PT's
//       0.020320892; VCM 0.017381286/0.017533874 -> ratio 0.855-0.863.
// max:  BDPT 0.03451538/0.034210205  -> ratio 1.077-1.086 vs PT's
//       0.0317688; VCM 0.022613525/0.02218628     -> ratio 0.698-0.712.
// All measured on a 64x64 render, `pixel_filter box`, `oidn_denoise
// FALSE`, EXR `Rec709RGB_Linear` (read back with a script, not the
// scene's own file_rasterizeroutput -- these tests read pixels via the
// in-memory capture, same as every other topology in this file).
//
// TOLERANCES.  8% mean / 30% p99 / 100% (2x) max.  The mean band is the
// same 8% as every other topology in this file even though the
// measured spread (1-2%) is much smaller than H/J's -- there is no
// tiny-emitter firefly tail here to force a looser band, so this row
// does NOT need H/J's loosened 60%/4x tail bands either; 30%/100% still
// comfortably covers the observed ~6-14% (p99) and ~8-30% (max) spread
// with headroom for a different machine or sample count.
//
// MEASURED COUNTERFACTUAL (debt 30 review round 3, C4).  This
// topology's own header claims it "guards" the eye-side/light-side
// asymmetry, but recorded no counterfactual to back that up.  Measured
// once, this round: temporarily applied `RadianceEtaScale` inside
// `GenerateLightSubpathImpl` (BDPTIntegrator.cpp, mirroring the
// eye-side site in `GenerateEyeSubpathImpl` verbatim -- wrong on
// purpose, since IMPORTANCE-mode walks must get no factor by
// construction, §1/§6), rebuilt, ran this test twice (independent
// unsynchronized-`rand()` runs -- this binary is not seed-argv driven):
// VCM/PT mean relative diff +45.6067%, +45.7639% (ratio 1.4561, 1.4576;
// spread 0.0016, ~0.11% of the mean -- tight, as expected: both runs
// draw from the same wrong code, not from two different physical
// models). Reverted immediately after
// (`git diff --stat src/` empty, library rebuilt clean). This is
// DRAMATICALLY outside the 8% band (+45.6-45.8% vs a =8% gate) -- the
// band catches a wrongly-applied light-side factor with enormous
// margin, roughly 5.7x the gate width. No tightening is needed or
// useful: the true failure signal here is ~46 percentage points, not a
// few points hiding near the edge of 8%. See BDPTStrategyBalanceTest's
// topology K header for the same experiment's BDPT twin, which reaches
// the OPPOSITE conclusion for the opposite reason -- topology K's
// gate does NOT catch the identical wrong edit at all.
//
// DL-365 AUDIT (2026-09-28): this topology shares topology H's
// mechanism -- the floor and emitter are both submerged, reached only
// by crossing the delta water surface, so `max_eye_depth`/
// `max_light_depth 5` admits only a bounded number of floor bounces.
// Salted independent renders, VCM/PT mean (sd), n=4 each:
//   depth  5: 0.9619 (sd 0.14%)
//   depth 16: 0.9945 (sd 0.27%)
// The move (+3.26 pp, ~15 pooled sd) is far past the "moves > 1 sd"
// threshold, so this row's VCM depth is raised to 16 alongside
// topology H's.  The 8% mean band was never at risk at either depth
// (worst-case deficit 3.8% at depth 5, 0.55% at depth 16) so it is left
// unchanged -- this fix corrects the truncation, not a flaky gate.
//////////////////////////////////////////////////////////////////////
static const char* kSceneSubmergedCeiling =
	"film\n"
	"{\n"
	"\twidth 64\n"
	"\theight 64\n"
	"}\n"
	"\n"
	"pinhole_camera\n"
	"{\n"
	"\tlocation 0 0.4 0.15\n"
	"\tlookat 0 0.6 0\n"
	"\tup 0 1 0\n"
	"\tfov 60.0\n"
	"}\n"
	"\n"
	"uniformcolor_painter\n"
	"{\n"
	"\tname pnt_albedo_a\n"
	"\tcolor 0.5 0.5 0.5\n"
	"}\n"
	"\n"
	"lambertian_material\n"
	"{\n"
	"\tname mat_diffuse_a\n"
	"\treflectance pnt_albedo_a\n"
	"}\n"
	"\n"
	"clippedplane_geometry\n"
	"{\n"
	"\tname quad_a\n"
	"\tpta -0.6 0.6 0.6\n"
	"\tptb 0.6 0.6 0.6\n"
	"\tptc 0.6 0.6 -0.6\n"
	"\tptd -0.6 0.6 -0.6\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_a\n"
	"\tgeometry quad_a\n"
	"\tmaterial mat_diffuse_a\n"
	"}\n"
	"\n"
	"uniformcolor_painter\n"
	"{\n"
	"\tname pnt_albedo_b\n"
	"\tcolor 0.5 0.5 0.5\n"
	"}\n"
	"\n"
	"lambertian_material\n"
	"{\n"
	"\tname mat_diffuse_b\n"
	"\treflectance pnt_albedo_b\n"
	"}\n"
	"\n"
	"clippedplane_geometry\n"
	"{\n"
	"\tname quad_b\n"
	"\tpta -1 0.05 1\n"
	"\tptb 1 0.05 1\n"
	"\tptc 1 0.05 -1\n"
	"\tptd -1 0.05 -1\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_b\n"
	"\tgeometry quad_b\n"
	"\tmaterial mat_diffuse_b\n"
	"}\n"
	"\n"
	"dielectric_material\n"
	"{\n"
	"\tname mat_water\n"
	"\tior 1.33\n"
	"\ttau 1.0\n"
	"\tscattering 1000000\n"
	"}\n"
	"\n"
	"box_geometry\n"
	"{\n"
	"\tname geo_water\n"
	"\twidth 2.2\n"
	"\theight 0.3\n"
	"\tdepth 2.2\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname water\n"
	"\tgeometry geo_water\n"
	"\tmaterial mat_water\n"
	"\tposition 0 0.15 0\n"
	"}\n"
	"\n"
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
	"\tscale 5.0\n"
	"\tmaterial none\n"
	"}\n"
	"\n"
	"sphere_geometry\n"
	"{\n"
	"\tname geo_emit\n"
	"\tradius 0.06\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_emit\n"
	"\tgeometry geo_emit\n"
	"\tmaterial mat_emit\n"
	"\tposition 0 0.18 0\n"
	"}\n";

static const char* kRasterizerPTCeiling =
	"standard_shader\n"
	"{\n"
	"\tname global\n"
	"\tshaderop DefaultPathTracing\n"
	"}\n"
	"\n"
	"pathtracing_pel_rasterizer\n"
	"{\n"
	"\tsamples 2048\n"
	"\toidn_denoise FALSE\n"
	"\tpixel_filter box\n"
	"}\n"
	"\n"
	"file_rasterizeroutput\n"
	"{\n"
	"\tpattern rendered/vcm_balance_pt_ceiling_unused\n"
	"\ttype EXR\n"
	"\tbpp 32\n"
	"\tcolor_space Rec709RGB_Linear\n"
	"}\n";

static const char* kRasterizerVCMCeiling =
	"standard_shader\n"
	"{\n"
	"\tname global\n"
	"\tshaderop DefaultPathTracing\n"
	"}\n"
	"\n"
	"vcm_pel_rasterizer\n"
	"{\n"
	"\tmax_eye_depth 16\n"
	"\tmax_light_depth 16\n"
	"\tsamples 1024\n"
	"\tmerge_radius 0.0\n"
	"\tvc_enabled true\n"
	"\tvm_enabled true\n"
	"\tpixel_filter box\n"
	"\toidn_denoise FALSE\n"
	"}\n"
	"\n"
	"file_rasterizeroutput\n"
	"{\n"
	"\tpattern rendered/vcm_balance_vcm_ceiling_unused\n"
	"\ttype EXR\n"
	"\tbpp 32\n"
	"\tcolor_space Rec709RGB_Linear\n"
	"}\n";

static const Tolerances kCeilingTolerances{ 0.08, 0.30, 1.00 };

//! Topology L only (DL-103 closure, 2026-09-17) -- see that
//! topology's own comment for the isolated-A/B evidence.  MEAN only;
//! p99/max stay at the shared values.
static const Tolerances kSchlickTopologyLTolerances{ 0.02, 0.25, 1.00 };

static void TestSubmergedCeilingMISCombination()
{
	RunTopologyTest( "air ceiling patch, floor + emitter both submerged (eta^2 MIS combination, debt 30 review round 2)",
		std::string( kSceneSubmergedCeiling ), kCeilingTolerances,
		kRasterizerPTCeiling, kRasterizerVCMCeiling );
}

//////////////////////////////////////////////////////////////////////
// Topology L: MULTI-LOBE `schlick_material` (DL-69).
//
// Every other topology in this file uses `lambertian_material`, whose
// SPF emits exactly ONE lobe -- so the lobe-selection probability is
// trivially 1 and the material's aggregate BSDF value IS its only
// lobe's value.  That made this file structurally blind to DL-69:
// BDPT/VCM's shared non-delta eye/light throughput paired the material's
// AGGREGATE `IBSDF::value()` (all lobes summed) with the ONE
// stochastically-selected lobe's own density, an N-times over-count
// for N accepted lobes with overlapping support.
//
// `schlick_material` is the canonical such material: `SchlickSPF::
// Scatter` pushes a cosine-weighted diffuse lobe AND a Schlick
// half-vector specular lobe into the same container, both non-delta,
// both over the same upper hemisphere, each carrying its OWN
// conditional pdf.  tests/SchlickLobePairingTest.cpp measures the
// resulting over-count in closed form: exactly 2.00x the BRDF
// integral at 0/30/60 deg incidence.
//
// SCENE.  The receiver wall (z=0, +-1, normal +Z) fills the frame at
// fov 30 (half-height 3.5*tan(15 deg) = 0.938).  The floor (y=-1,
// z in [0,2], normal +Y) is outside the frustum, and the area emitter
// -- a 12 x 12 quad in the z=4.2 plane, normal -Z, i.e. a large
// softbox BEHIND the camera (which sits at z=3.5 looking toward -Z) --
// is behind the near plane, so the image is pure receiver radiance
// with no emitter pixels and no background.  Both non-emitting
// surfaces are `schlick_material`, which puts a multi-lobe vertex on
// BOTH subpaths:
//   - eye side: camera -> wall (v1) -> floor or emitter (v2) -> ...
//     The v1 scatter throughput multiplies every strategy of length
//     >= 3, including the s=0 emitter-hit that competes with v1's NEE.
//   - light side: emitter -> wall or floor (l1) -> the other (l2) ->
//     ...  The l1 scatter throughput multiplies every s >= 3
//     connection and splat.
// The emitter's size and proximity (12 x 12 units at z=4.2, so it
// subtends a large solid angle from every point on the wall) are
// deliberate: they give the BSDF-sampling strategies real MIS weight
// against NEE, so the over-count lands in the mean rather than being
// MIS-suppressed.
//
// WHAT THIS TOPOLOGY DOES *NOT* EXERCISE (review, 2026-09-14).  The
// scene has NO specular surfaces and NO delta lights, so the VCM
// auto-radius pre-pass reports `foundSpecular = false` and VCM
// DISABLES VERTEX MERGING for it -- exactly like this file's topology
// A/B (see their comments).  So the DL-69 red-proof here covers VCM's
// VERTEX CONNECTION half only; the merge half shares the same two
// subpath generators and therefore the same fix, but no test in this
// repository red-proves DL-69 through a merge.  A merge-exercising
// variant would need a specular caster, which would also change what
// the PT reference can reach and is a separate piece of work.
//
// Depth budgets are matched explicitly (VCM max_eye_depth /
// max_light_depth 5 vs PT's `max_diffuse_bounce` / `max_glossy_bounce`
// 5) because unlike the single-bounce Lambertian topologies above,
// this scene has real interreflection and an unequal budget would be
// a second free variable.
//
// DL-365 AUDIT (2026-09-28): unlike topology H/I this scene has no
// delta interface and no TIR trap -- it is an open Schlick-walled
// corner lit by a large area emitter, so it is genuinely converged at
// depth 5, not merely under-tested.  Salted independent renders,
// VCM/PT mean (sd), n=4 each (this pair also drives topology AB below,
// measured the same way):
//   topology L:  depth 5: 0.99915 (sd 0.03%); depth 16: 1.00011 (sd 0.13%) -- 0.92 pooled sd apart
//   topology AB: depth 5: 0.99915 (sd 0.07%); depth 16: 0.99958 (sd 0.04%) -- 0.74 pooled sd apart
// Both moves are under 1 sd, so both topologies stay at depth 5 --
// raising it would only add render cost for no measurable change.
//
// MEAN BAND TIGHTENED TO 2% BY DL-103's CLOSURE (2026-09-17,
// docs/DL103_PT_ESCAPE_MIS_PARTNER.md).  Until that row, un-guided PT's
// escape-side MIS partner at a multi-lobe SPF was the SELECTED lobe's
// own density rather than the material's aggregate, so this row's own
// REFERENCE carried a Schlick-specific bias.  Isolated A/B on
// `PathTracingIntegrator.cpp` alone (n = 4 each side, same scene, same
// rasterizer strings):
//
//   pre-fix   PT 0.0601696 +/- 0.0000120   VCM 0.0632495 +/- 0.0000012
//             VCM/PT 1.05119 +/- 0.00020   (+5.119%)
//   post-fix  PT 0.0632650 +/- 0.0000084   VCM 0.0632485 +/- 0.0000039
//             VCM/PT 0.99974 +/- 0.00011   (-0.026%)
//
// PT moved +5.14%, VCM moved -0.002% -- VCM's own MIS densities come
// from `BDPTVertex::pdfFwd`, which DL-69 already made the aggregate, so
// the PT-side fix is the whole movement.  See the BDPT twin's comment
// for what a pass here does and does not claim (DL-127 is still open
// and both integrators consume `kray`, so a residual from that row can
// sit inside this agreement).
//////////////////////////////////////////////////////////////////////
static const char* kSceneSchlickMultiLobeL =
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
	"\tname pnt_rd\n"
	"\tcolor 0.4 0.4 0.4\n"
	"}\n"
	"\n"
	"uniformcolor_painter\n"
	"{\n"
	"\tname pnt_rs\n"
	"\tcolor 0.4 0.4 0.4\n"
	"}\n"
	"\n"
	"scalar_painter\n"
	"{\n"
	"\tname pnt_rough\n"
	"\tvalue 0.5\n"
	"}\n"
	"\n"
	"scalar_painter\n"
	"{\n"
	"\tname pnt_iso\n"
	"\tvalue 1.0\n"
	"}\n"
	"\n"
	"schlick_material\n"
	"{\n"
	"\tname mat_schlick\n"
	"\trd pnt_rd\n"
	"\trs pnt_rs\n"
	"\troughness pnt_rough\n"
	"\tisotropy pnt_iso\n"
	"}\n"
	"\n"
	"clippedplane_geometry\n"
	"{\n"
	"\tname quad_wall\n"
	"\tpta -1 -1 0\n"
	"\tptb 1 -1 0\n"
	"\tptc 1 1 0\n"
	"\tptd -1 1 0\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_wall\n"
	"\tgeometry quad_wall\n"
	"\tmaterial mat_schlick\n"
	"}\n"
	"\n"
	"clippedplane_geometry\n"
	"{\n"
	"\tname quad_floor\n"
	"\tpta -1 -1 0\n"
	"\tptb -1 -1 2\n"
	"\tptc 1 -1 2\n"
	"\tptd 1 -1 0\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_floor\n"
	"\tgeometry quad_floor\n"
	"\tmaterial mat_schlick\n"
	"}\n"
	"\n"
	"uniformcolor_painter\n"
	"{\n"
	"\tname pnt_emit_l\n"
	"\tcolor 1.0 1.0 1.0\n"
	"}\n"
	"\n"
	"lambertian_luminaire_material\n"
	"{\n"
	"\tname mat_emit_l\n"
	"\texitance pnt_emit_l\n"
	"\tscale 0.5\n"
	"\tmaterial none\n"
	"}\n"
	"\n"
	"clippedplane_geometry\n"
	"{\n"
	"\tname quad_emit_l\n"
	"\tpta -6 -6 4.2\n"
	"\tptb -6 6 4.2\n"
	"\tptc 6 6 4.2\n"
	"\tptd 6 -6 4.2\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_emit_l\n"
	"\tgeometry quad_emit_l\n"
	"\tmaterial mat_emit_l\n"
	"}\n";

//////////////////////////////////////////////////////////////////////
// 256-spp, depth-matched PT / VCM twins for topology L.  Identical in
// every respect except the rasterizer chunk (and, inside it, the depth
// budget and sample count).
//////////////////////////////////////////////////////////////////////
static const char* kRasterizerPTSchlickL =
	"standard_shader\n"
	"{\n"
	"\tname global\n"
	"\tshaderop DefaultPathTracing\n"
	"}\n"
	"\n"
	"pathtracing_pel_rasterizer\n"
	"{\n"
	"\tsamples 256\n"
	"\trr_min_depth 8\n"
	"\tmax_diffuse_bounce 5\n"
	"\tmax_glossy_bounce 5\n"
	"\tpixel_filter box\n"
	"\toidn_denoise FALSE\n"
	"}\n"
	"\n"
	"file_rasterizeroutput\n"
	"{\n"
	"\tpattern rendered/vcm_balance_pt_unused\n"
	"\ttype EXR\n"
	"\tbpp 32\n"
	"\tcolor_space Rec709RGB_Linear\n"
	"}\n";

static const char* kRasterizerVCMSchlickL =
	"standard_shader\n"
	"{\n"
	"\tname global\n"
	"\tshaderop DefaultPathTracing\n"
	"}\n"
	"\n"
	"vcm_pel_rasterizer\n"
	"{\n"
	"\tmax_eye_depth 5\n"
	"\tmax_light_depth 5\n"
	"\tsamples 256\n"
	"\tmerge_radius 0.0\n"
	"\tvc_enabled true\n"
	"\tvm_enabled true\n"
	"\tpixel_filter box\n"
	"\toidn_denoise FALSE\n"
	"}\n"
	"\n"
	"file_rasterizeroutput\n"
	"{\n"
	"\tpattern rendered/vcm_balance_vcm_unused\n"
	"\ttype EXR\n"
	"\tbpp 32\n"
	"\tcolor_space Rec709RGB_Linear\n"
	"}\n";


//////////////////////////////////////////////////////////////////////
// DL-302: `coated_material` with a COAT NORMAL over a BASE normal-map
// modifier (wall + floor).  The coat normal decodes from the PRE-modifier
// frame; a BDPT/VCM vertex rebuild that dropped that frame
// (BDPTVertex::coatDecodeOnb / PathVertexEval::PopulateRIGFromVertex)
// would decode it from the MODIFIED frame at connection time while the
// walk used the original one, splitting PT from the bidirectional
// integrators.  Dark sharp-coat wall so the coat lobe carries the image; band max(0.1%, 3 sigma): with the replay dropped BDPT reads -0.27% (5.8 sigma), with it -0.0007%.
//////////////////////////////////////////////////////////////////////
static std::string SceneCoatNormalOverNormalMap()
{
	std::string s( kSceneSchlickMultiLobeL );
	const std::string head = "schlick_material\n{\n\tname mat_schlick\n";
	const size_t a = s.find( head );
	const size_t b = ( a == std::string::npos ) ? std::string::npos : s.find( "}\n", a );
	if( a == std::string::npos || b == std::string::npos ) return std::string();
	s.replace( a, b + 2 - a,
		"uniformcolor_painter\n{\n\tname pnt_coatmap\n\tcolor 0.85 0.5 1.0\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_basemap\n\tcolor 0.5 0.85 1.0\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		"normal_map_modifier\n{\n\tname nm_cn\n\tnormal_map pnt_basemap\n\tscale 1.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_cn_dark\n\tcolor 0.05 0.05 0.05\n}\n\n"
		"lambertian_material\n{\n\tname mat_cn_base\n\treflectance pnt_cn_dark\n}\n\n"
		"coated_material\n{\n\tname mat_schlick\n\tbase mat_cn_base\n\tcoat_weight 1\n\tcoat_ior 1.5\n"
		"\tcoat_roughness 0.03\n\tcoat_normal pnt_coatmap\n}\n" );
	const std::string objOld = "\tmaterial mat_schlick\n}";
	size_t pos = 0;
	int n = 0;
	while( ( pos = s.find( objOld, pos ) ) != std::string::npos ) {
		const std::string objNew = "\tmaterial mat_schlick\n\tmodifier nm_cn\n}";
		s.replace( pos, objOld.size(), objNew );
		pos += objNew.size();
		++n;
	}
	return ( n == 2 ) ? s : std::string();
}

static bool MeasureCoatNormalSalted( const char* rasterizer, const std::string& sceneText, int n, double& mean, double& sd )
{
	const std::string path = WriteSceneToTempFile( ( std::string( "RISE ASCII SCENE 7\n" ) + rasterizer + sceneText ).c_str(), "dl302_coatnormal" );
	if( path.empty() ) return false;
	std::vector<double> v;
	for( int i = 0; i < n; i++ ) {
		SobolSamplerTestHooks::ValueSalt().store( SobolSequence::HashCombine( 0x302u, unsigned( i ) ) );
		const ImageStats st = RenderAndComputeStats( path.c_str() );
		if( !st.valid ) break;
		v.push_back( ( st.mean[0] + st.mean[1] + st.mean[2] ) / 3.0 );
	}
	SobolSamplerTestHooks::ValueSalt().store( 0u );
	std::remove( path.c_str() );
	if( int( v.size() ) != n ) return false;
	double sum = 0;
	for( double x : v ) sum += x;
	mean = sum / n;
	double ss = 0;
	for( double x : v ) ss += ( x - mean ) * ( x - mean );
	sd = std::sqrt( ss / ( n - 1 ) );
	return std::isfinite( mean ) && mean > 0;
}

static void TestCoatNormalOverNormalMap()
{
	std::cout << "Testing DL-302 coat_normal over normal_map_modifier (PT vs VCM, n = 4 salted)" << std::endl;
	const std::string scene = SceneCoatNormalOverNormalMap();
	Check( !scene.empty(), "DL-302 coat-normal scene assembled from topology L's fixture" );
	if( scene.empty() ) return;
	double pt = 0, ptSd = 0, bi = 0, biSd = 0;
	const bool ok = MeasureCoatNormalSalted( kRasterizerPTSchlickL, scene, 4, pt, ptSd )
		&& MeasureCoatNormalSalted( kRasterizerVCMSchlickL, scene, 4, bi, biSd );
	Check( ok, "DL-302 renders produced output" );
	if( !ok ) return;
	const double rel = bi / pt - 1.0;
	const double se = std::sqrt( ( ptSd / pt ) * ( ptSd / pt ) + ( biSd / bi ) * ( biSd / bi ) ) / 2.0;
	std::printf( "    PT %.7f (sd %.7f)  VCM %.7f (sd %.7f)  VCM/PT %+.4f%% (se %.4f%%)\n",
		pt, ptSd, bi, biSd, 100.0 * rel, 100.0 * se );
	Check( std::fabs( rel ) <= std::max( 0.001, 3.0 * se ), "DL-302 VCM mean agrees with PT within max(0.1%, 3 sigma)" );
}

static void TestSchlickMultiLobe()
{
	RunTopologyTest( "multi-lobe schlick_material wall + floor, area emitter (DL-69)",
		std::string( kSceneSchlickMultiLobeL ), kSchlickTopologyLTolerances,
		kRasterizerPTSchlickL, kRasterizerVCMSchlickL );
}

//////////////////////////////////////////////////////////////////////
// Topology AB: `polished_material` wall + floor (DL-285, 2026-09-28) --
// BDPTStrategyBalanceTest's topology AB, VCM twin.  Until DL-285 the
// material's `GetBSDF()` was a bare Lambertian while its SPF sampled a
// Fresnel coat plus a (1-F) substrate; VCM's merges and connections price
// `value`, its continuations `kray`, and it read -1.77 % under PT on this
// scene (pre-fix library, 256 spp; n = 6 salted 1024-spp renders:
// -1.746 % +- 0.008 % sem).  Post-fix -0.10 % (256 spp) / -0.041 % +-
// 0.009 % (n = 6, 1024 spp).  Band 0.5 % on the mean.
//////////////////////////////////////////////////////////////////////
static void TestPolishedAB()
{
	std::string s( kSceneSchlickMultiLobeL );
	const std::string head = "schlick_material\n{\n\tname mat_schlick\n";
	const size_t a = s.find( head );
	const size_t b = ( a == std::string::npos ) ? std::string::npos : s.find( "}\n", a );
	Check( a != std::string::npos && b != std::string::npos, "topology AB: schlick_material chunk found in topology L" );
	if( a != std::string::npos && b != std::string::npos ) {
		s.replace( a, b + 2 - a,
			"polished_material\n{\n\tname mat_schlick\n\treflectance pnt_rd\n"
			"\ttau 0.9\n\tior 1.5\n\tscattering 20\n}\n" );
	}
	static const Tolerances kPolishedABTolerances{ 0.005, 0.25, 1.00 };
	RunTopologyTest( "polished_material wall + floor (AB), VCM vs PT (DL-285)",
		s, kPolishedABTolerances, kRasterizerPTSchlickL, kRasterizerVCMSchlickL );
}

//////////////////////////////////////////////////////////////////////
// Topology J: biospec_skin_material receiver, mesh area emitter
// (DL-126) -- VCM's twin of BDPTStrategyBalanceTest's topology N.
//
// `BioSpecSkinMaterial::GetBSDF()` returns null (`!isConnectible`), so
// this vertex can never be an NEE/connection endpoint under either
// integrator, and the emitter is placed exactly as BDPT's twin scene
// places it (this file's own `kSceneCommon` + `kLightMesh`, material
// swapped) -- irrelevant here anyway, since PT's own NEE arm is gated
// on a non-null `IBSDF*` before it ever reaches a hemisphere test.  The
// whole image is carried by the shared eye/light subpath generator's
// BSDF-sampled continuation (`BDPTIntegrator.cpp`'s
// `GenerateEyeSubpathImpl`/`GenerateLightSubpathImpl`, which VCM reuses
// unmodified) pricing `BioSpecSkinSPF::Scatter`'s `kray = 1`,
// `.pdf == 0` re-emission -- exactly the DL-126 pattern.  Pre-fix,
// VCM's eye AND light subpaths both `break` at this vertex's first
// non-delta scatter and the render goes BLACK; PT is unaffected.  When
// this topology was added, this file's default `kRasterizerPT` was the
// LEGACY `pixelpel_rasterizer` + `DefaultDirectLighting`, which never
// calls `ISPF::Scatter` at all and would have read pure black for BOTH
// pre- and post-fix VCM here -- so this topology pairs a modern-PT
// reference (`pathtracing_pel_rasterizer`,
// mirroring `BDPTStrategyBalanceTest.cpp`'s `kRasterizerPT`) with the
// existing `kRasterizerVCM` (already `DefaultPathTracing`-driven).
//////////////////////////////////////////////////////////////////////
static const char* kSceneNullBSDFSkin =
	"film\n"
	"{\n"
	"\twidth 32\n"
	"\theight 32\n"
	"}\n\n"
	"pinhole_camera\n"
	"{\n"
	"\tlocation 0 0 3.5\n"
	"\tlookat 0 0 0\n"
	"\tup 0 1 0\n"
	"\tfov 30.0\n"
	"}\n\n"
	"biospec_skin_material\n"
	"{\n"
	"\tname mat_skin\n"
	"}\n\n"
	"clippedplane_geometry\n"
	"{\n"
	"\tname quad_skin\n"
	"\tpta -1 -1 0\n\tptb 1 -1 0\n\tptc 1 1 0\n\tptd -1 1 0\n"
	"}\n\n"
	"standard_object\n"
	"{\n"
	"\tname obj_skin\n"
	"\tgeometry quad_skin\n"
	"\tmaterial mat_skin\n"
	"}\n";

// DL-365 (2026-09-28): 32 spp on this small (32x32) scene reads sd
// ~7% on the VCM/PT mean ratio (salted, n=8, both depth 3 and depth 16
// -- depth is NOT the driver here, see the file-level DL-365 comment
// above kRasterizerVCM), so the shared 8% band flips red on roughly
// half of all salted seeds -- exactly the "flip red/green" symptom
// DL-365 was filed to fix, but from sample count, not a depth cap: a
// biospec_skin_material re-emission (`kray=1, pdf=0`) is a spiky,
// high-variance estimator at low spp, on BOTH integrators (PT's own
// mean swings +/-6% across the same seeds).  Raised to 512 spp on both
// sides (matching this file's own precedent for noisy topologies D-G,
// "512-spp twins"), keeping `kRasterizerVCM`'s depth 3 -- this
// topology gets its OWN VCM string so A/B/C's shared `kRasterizerVCM`
// is untouched.  Salted n=8 at 512 spp: mean 0.9959, sd 0.78% (was
// mean 1.026, sd 7.14% at 32 spp) -- the band now sits >= 10 sd away.
static const char* kRasterizerPTModernBasic =
	"standard_shader\n"
	"{\n"
	"\tname global\n"
	"\tshaderop DefaultPathTracing\n"
	"}\n"
	"\n"
	"pathtracing_pel_rasterizer\n"
	"{\n"
	"\tsamples 512\n"
	"\tpixel_filter box\n"
	"\toidn_denoise FALSE\n"
	"}\n"
	"\n"
	"file_rasterizeroutput\n"
	"{\n"
	"\tpattern rendered/vcm_balance_pt_nullbsdf_unused\n"
	"\ttype EXR\n"
	"\tbpp 32\n"
	"\tcolor_space Rec709RGB_Linear\n"
	"}\n";

static const char* kRasterizerVCMNullBSDF =
	"standard_shader\n"
	"{\n"
	"\tname global\n"
	"\tshaderop DefaultPathTracing\n"
	"}\n"
	"\n"
	"vcm_pel_rasterizer\n"
	"{\n"
	"\tmax_eye_depth 3\n"
	"\tmax_light_depth 3\n"
	"\tsamples 512\n"
	"\tmerge_radius 0.0\n"
	"\tvc_enabled true\n"
	"\tvm_enabled true\n"
	"\tpixel_filter box\n"
	"\toidn_denoise FALSE\n"
	"}\n"
	"\n"
	"file_rasterizeroutput\n"
	"{\n"
	"\tpattern rendered/vcm_balance_vcm_nullbsdf_unused\n"
	"\ttype EXR\n"
	"\tbpp 32\n"
	"\tcolor_space Rec709RGB_Linear\n"
	"}\n";

static void TestNullBSDFMaterialContinuation()
{
	RunTopologyTest( "biospec_skin_material receiver, mesh area emitter (DL-126)",
		std::string( kSceneNullBSDFSkin ) + kLightMesh, kStrictTolerances,
		kRasterizerPTModernBasic, kRasterizerVCMNullBSDF );
}

//////////////////////////////////////////////////////////////////////
// Topology U: rough `subsurfacescattering_material` sheets (DL-307) --
// VCM's twin of BDPTStrategyBalanceTest's topology U (the same scene).
//
// VCM builds its subpaths with BDPT's shared generators, which used to
// BREAK on an empty scatter container before the BSSRDF entry branch
// (DL-307): a rough front reflection drawn below the horizon is dropped
// by the SPF and the subsurface branch it would have taken with
// probability Ft was lost.  Unlike BDPT, VCM did not reach PT on this
// scene even with that fix -- it read ~5% under PT with or without
// merging, a separate, pre-existing defect in VCM's MIS at BSSRDF /
// random-walk entry vertices (DL-317, see TestSSSBarrierDL317).  This
// row was a two-sided PIN of that state ([-5.72%, -5.00%]) until
// DL-317 closed it; it is now a PARITY band.
//
// Measured (32x32, 2048 spp, salted Sobol', this row as written):
// DL-307 (pre/post, n = 16 interleaved): -6.000% / -5.242%.  DL-317
// fixed build, six independent salted runs (`--dl317-only <seed>`,
// seeds 11/101/202/303/404/505): -0.148, +0.161, -0.003, -0.103,
// +0.183, -0.208% -- mean -0.02%, sd 0.163% (both sides salted, so
// that IS the decorrelated ratio sd).  Band +/- 0.8% = 4.9 sd; the
// pre-DL-317 -5.2% is 27 sd outside it.
//
// THE EMITTER IS SINGLE-SIDED ON PURPOSE (DL-320, 2026-09-28).  Its
// winding faces the sheets (-Z), and `clippedplane_geometry` defaults to
// `doublesided TRUE`.  Since DL-320 a double-sided emitter emits from
// BOTH faces for every strategy, so half of its light subpaths would
// leave upward into empty space; the light-side strategies DL-317 was
// about would then carry less of the image and the row would see less
// of the defect (pre-DL-317 it read -2.93% with the default, against
// -5.9% for the same scene single-sided -- the review's isolation:
// double-sided -5.90% -> -3.50%, `doublesided FALSE` -5.88% -> -5.87%,
// a Lambertian control +0.01% / +0.02%, salted n = 2).  `doublesided
// FALSE` keeps the row sensitive to a regression of DL-317.
//////////////////////////////////////////////////////////////////////
static const char* kSceneRoughSSSU =
	"film\n{\n\twidth 32\n\theight 32\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 30.0\n}\n\n"
	"subsurfacescattering_material\n{\n\tname mat_sss\n\tior 1.3\n\tabsorption 0.1\n"
		"\tscattering 1.0\n\tg 0.0\n\troughness 0.3\n}\n\n"
	"clippedplane_geometry\n{\n\tname quad_wall\n\tpta -1 -1 0\n\tptb 1 -1 0\n\tptc 1 1 0\n\tptd -1 1 0\n}\n\n"
	"standard_object\n{\n\tname obj_wall\n\tgeometry quad_wall\n\tmaterial mat_sss\n}\n\n"
	"clippedplane_geometry\n{\n\tname quad_floor\n\tpta -1 -1 0\n\tptb -1 -1 2\n\tptc 1 -1 2\n\tptd 1 -1 0\n}\n\n"
	"standard_object\n{\n\tname obj_floor\n\tgeometry quad_floor\n\tmaterial mat_sss\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_emit_u\n\tcolor 1.0 1.0 1.0\n}\n\n"
	"lambertian_luminaire_material\n{\n\tname mat_emit_u\n\texitance pnt_emit_u\n\tscale 0.5\n\tmaterial none\n}\n\n"
	"clippedplane_geometry\n{\n\tname quad_emit_u\n\tpta -6 -6 4.2\n\tptb -6 6 4.2\n\tptc 6 6 4.2\n\tptd 6 -6 4.2\n\tdoublesided FALSE\n}\n\n"
	"standard_object\n{\n\tname obj_emit_u\n\tgeometry quad_emit_u\n\tmaterial mat_emit_u\n}\n";

static const char* kRasterizerPTRoughSSSU =
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 2048\n\tprogressive_samples_per_pass 1\n\trr_min_depth 8\n\tmax_diffuse_bounce 5\n"
		"\tmax_glossy_bounce 5\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n"
	"file_rasterizeroutput\n{\n\tpattern rendered/vcm_balance_pt_unused\n\ttype EXR\n\tbpp 32\n"
		"\tcolor_space Rec709RGB_Linear\n}\n";

static const char* kRasterizerVCMRoughSSSU =
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"vcm_pel_rasterizer\n{\n\tmax_eye_depth 5\n\tmax_light_depth 5\n\tsamples 2048\n\tprogressive_samples_per_pass 1\n\tmerge_radius 0.0\n"
		"\tvc_enabled true\n\tvm_enabled true\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n"
	"file_rasterizeroutput\n{\n\tpattern rendered/vcm_balance_vcm_unused\n\ttype EXR\n\tbpp 32\n"
		"\tcolor_space Rec709RGB_Linear\n}\n";

static void MeasureRoughSSSU(unsigned samples, bool gate=false, double radius=-1, bool strict=false, bool unmatched=false);
static void TestRoughSSSEmptyContainerU()
{
    MeasureRoughSSSU(2048,true);
}

// DL-367 measurement: distinguish sample-count drift from salt noise without
// changing transport. The default gate uses n=8 at its original 2048 spp.
// Independent SMS-free PT reference; original +/-0.8% physical limit retained.
// Match PT and merging-off to VM-on’s forced one sample per pass. The old
// 32-sample reference changes the finite-budget mean; --dl367-unmatched-only
// retains that strict red comparator. This isolates the reference confound,
// not the still-unattributed native batching dependence (DL-367 remains open).
// DL-367: a value salt must identify a complete render, including camera RNG.
// Single-thread replay is exact; different salts must still change the image.
static void TestRoughSSSReplay(unsigned repeats) {
 const unsigned savedSeed=g_seedBase,savedIndex=g_renderIndex;
 const uint32_t savedSalt=SobolSamplerTestHooks::ValueSalt().load();
 const bool savedComplete=g_dl367CompleteRenderSeed;g_dl367CompleteRenderSeed=true;
 for(const char* raster:{kRasterizerPTRoughSSSU,kRasterizerVCMRoughSSSU}) {
  std::string r=raster;auto pos=r.find("samples 2048");r.replace(pos,12,"samples 64");
  auto path=WriteSceneToTempFile((std::string("RISE ASCII SCENE 7\n")+r+kSceneRoughSSSU).c_str(),"dl367_replay");
  Check(!path.empty(),"DL367 replay fixture writes");if(path.empty())continue;
  uint64_t previous=0;unsigned mismatches=0;double squared=0,sum=0;
  for(unsigned i=0;i<repeats;++i) {
   uint64_t a=0,b=0;g_seedBase=367800+101*i;g_renderIndex=0;
   const auto first=RenderAndComputeStats(path.c_str(),&a);
   g_renderIndex=0;const auto second=RenderAndComputeStats(path.c_str(),&b);
   Check(first.valid&&second.valid,"DL367 replay pair finite");
   Check(a==b,"DL367 identical complete-render seed reproduces all pixels");
   if(a!=b)++mismatches;
   const double diff=first.mean[0]-second.mean[0];squared+=diff*diff;sum+=diff;
   if(i)Check(a!=previous,"DL367 distinct salts produce distinct pixels");
   previous=a;
  }
  std::printf("DL367 replay %s n=%u pairs mismatches=%u mean_delta=%.9g sd_delta=%.9g\n",raster==kRasterizerPTRoughSSSU?"PT":"VCM-on",repeats,mismatches,sum/repeats,std::sqrt(std::max(0.,(squared-sum*sum/repeats)/(repeats-1))));
  std::remove(path.c_str());
 }
 g_seedBase=savedSeed;g_renderIndex=savedIndex;g_dl367CompleteRenderSeed=savedComplete;
 SobolSamplerTestHooks::ValueSalt().store(savedSalt);
}

static void MeasureRoughSSSU(unsigned samples, bool gate, double radius, bool strict, bool unmatched) {
 const unsigned savedSeed=g_seedBase, savedIndex=g_renderIndex;
 const bool savedComplete=g_dl367CompleteRenderSeed;g_dl367CompleteRenderSeed=true;
 auto replace=[](std::string s,const std::string& a,const std::string& b){auto p=s.find(a);if(p!=std::string::npos)s.replace(p,a.size(),b);return s;};
 std::vector<double> pt,on,off;
 for(unsigned i=0;i<8;++i) {
  g_seedBase=367001+101*i; g_renderIndex=0;
  auto render=[&](const char* raster,bool merging){
   std::string r=replace(raster,"samples 2048","samples "+std::to_string(samples));
   if(unmatched) r=replace(r,"progressive_samples_per_pass 1","progressive_samples_per_pass 32");
   if(radius>=0) {
    char text[64];std::snprintf(text,sizeof(text),"%.17g",radius);
    r=replace(r,"merge_radius 0.0","merge_radius "+std::string(text));
   }
   if(!merging)r=replace(r,"vm_enabled true","vm_enabled false");
   auto path=WriteSceneToTempFile((std::string("RISE ASCII SCENE 7\n")+r+kSceneRoughSSSU).c_str(),"dl367");
   auto stats=RenderAndComputeStats(path.c_str());std::remove(path.c_str());
   Check(stats.valid,"DL367 probe produces finite output");
   return stats.valid ? (stats.mean[0]+stats.mean[1]+stats.mean[2])/3 : -1.;
  };
  pt.push_back(render(kRasterizerPTRoughSSSU,true));
  on.push_back(render(kRasterizerVCMRoughSSSU,true));
  off.push_back(render(kRasterizerVCMRoughSSSU,false));
 }
 g_seedBase=savedSeed;g_renderIndex=savedIndex;g_dl367CompleteRenderSeed=savedComplete;
 auto stat=[](const std::vector<double>& v){double m=0,ss=0;for(double x:v)m+=x;m/=v.size();for(double x:v)ss+=(x-m)*(x-m);return std::make_pair(m,std::sqrt(ss/(v.size()-1)/v.size()));};
 auto p=stat(pt),a=stat(on),b=stat(off);
 std::printf("DL367 radius=%g PT-sd=%.9g on-sd=%.9g off-sd=%.9g\n",radius,p.second*std::sqrt(8.),a.second*std::sqrt(8.),b.second*std::sqrt(8.));
 std::printf("DL367 spp=%u n=8 PT %.9g SE %.9g VM-on %.9g SE %.9g VM-off %.9g SE %.9g on/PT %.8g off/PT %.8g on3sigma %.9g off3sigma %.9g\n",samples,p.first,p.second,a.first,a.second,b.first,b.second,a.first/p.first,b.first/p.first,3*std::hypot(a.second,p.second),3*std::hypot(b.second,p.second));
 if(strict) {
  Check(std::fabs(a.first-p.first)<=3*std::hypot(a.second,p.second),"DL367 strict VM/PT parity at 3SE");
  Check(std::fabs(b.first-p.first)<=3*std::hypot(b.second,p.second),"DL367 strict VM-off/PT parity at 3SE");
 }
 if(gate) {
  const double ratio=a.first/p.first, uncertainty=3*std::hypot(a.second,p.second)/p.first;
  std::printf("DL367 default n=8 PT sd=%.9g on sd=%.9g off sd=%.9g ratio3SE=%.9g physical band=0.008\n",p.second*std::sqrt(8.),a.second*std::sqrt(8.),b.second*std::sqrt(8.),uncertainty);
  Check(std::isfinite(ratio)&&std::fabs(ratio-1)<=.008,"topology U salted mean retains +/-0.8% parity limit");
  Check(uncertainty<=.008,"topology U salted mean resolves the 0.8% precision budget");
 }
}

//////////////////////////////////////////////////////////////////////
// Topology W: NARROW-FOV light-tracing splat (DL-294) -- VCM twin of
// BDPTStrategyBalanceTest topology W (docs/DL294_NARROW_FOV_SPLAT.md).
//
// W1: BDPT's topology W scene verbatim -- a spot aimed up at a perfect
// mirror, the reflected beam lighting a Lambertian floor that a pinhole
// at fov 2 deg (16 x 16) sees edge to edge.  The floor's light is a
// delta-light caustic: VCM reaches it only by the t = 1 splat and by
// merging.  Closed form rho/pi * I / 3^2 (mirror image of the spot).
// W2: the same framing with the spot pointing straight DOWN at the
// floor from (0, 4, 0), no mirror -- an ordinary directly lit diffuse
// floor, rho/pi * I / 4^2.  Under VCM's balance heuristic the splat
// carries a real share of even this plain frame, which is where
// DL-294's "-1.45 % no-sheet L0" came from.
//
// Before DL-294 the camera cut the splat at its nominal [0, 16) film
// while SplatFilm rounds in [-0.5, 15.5): one half-pixel strip per axis
// was lost.  Measured (n = 3 runs each, isolated A/B):
//   W1 pre -0.731 +/- 0.036 %   post -0.367 +/- 0.022 %
//   W2 pre -1.463 +/- 0.001 %   post +0.021 +/- 0.000 %
// W1's post-fix residual is VCM's merge-radius blur (auto radius 0.02)
// reaching the beam's penumbra from the frame edge, not the splat --
// BDPT's twin reads +0.07 % on the same scene.  Bands: W1 0.6 %, W2
// 0.5 %.
//////////////////////////////////////////////////////////////////////
static const char* kSceneNarrowFovCommonW =
	"film\n{\n\twidth 16\n\theight 16\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 1 1.2\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 2.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_floor\n\tcolor 0.5 0.5 0.5\n\tcolorspace Rec709RGB_Linear\n}\n\n"
	"lambertian_material\n{\n\tname mat_floor\n\treflectance pnt_floor\n}\n\n"
	"clippedplane_geometry\n{\n\tname geo_floor\n\tpta -1 0 1\n\tptb 1 0 1\n\tptc 1 0 -1\n\tptd -1 0 -1\n\tdoublesided TRUE\n}\n\n"
	"standard_object\n{\n\tname obj_floor\n\tgeometry geo_floor\n\tmaterial mat_floor\n}\n\n";

static const char* kSceneNarrowFovMirrorW1 =
	"uniformcolor_painter\n{\n\tname pnt_mirror\n\tcolor 1 1 1\n\tcolorspace Rec709RGB_Linear\n}\n\n"
	"perfectreflector_material\n{\n\tname mat_mirror\n\treflectance pnt_mirror\n}\n\n"
	"clippedplane_geometry\n{\n\tname geo_mirror\n\tpta -4 2 4\n\tptb 4 2 4\n\tptc 4 2 -4\n\tptd -4 2 -4\n\tdoublesided TRUE\n}\n\n"
	"standard_object\n{\n\tname obj_mirror\n\tgeometry geo_mirror\n\tmaterial mat_mirror\n}\n\n"
	"spot_light\n{\n\tname lgt\n\tposition 0 1 0\n\ttarget 0 2 0\n\tinner 2.0\n\touter 2.6\n\tcolor 1 1 1\n\tpower 16\n}\n";

static const char* kSceneNarrowFovDirectW2 =
	"spot_light\n{\n\tname lgt\n\tposition 0 4 0\n\ttarget 0 0 0\n\tinner 1.5\n\touter 2.0\n\tcolor 1 1 1\n\tpower 16\n}\n";

static const char* kRasterizerVCMNarrowFovW =
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"vcm_pel_rasterizer\n{\n\tmax_eye_depth 8\n\tmax_light_depth 8\n\tsamples 1024\n\tmerge_radius 0.0\n"
	"\tvc_enabled true\n\tvm_enabled true\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n"
	"file_rasterizeroutput\n{\n\tpattern rendered/vcm_balance_vcm_unused\n\ttype EXR\n\tbpp 32\n\tcolor_space Rec709RGB_Linear\n}\n";

static void RunNarrowFovRowW( const char* label, const char* lightBlock, const double dist, const double tol )
{
	const std::string scene = std::string( "RISE ASCII SCENE 7\n" ) + kRasterizerVCMNarrowFovW + kSceneNarrowFovCommonW + lightBlock;
	const std::string path = WriteSceneToTempFile( scene.c_str(), "narrowfov_w" );
	const ImageStats st = path.empty() ? ImageStats{} : RenderAndComputeStats( path.c_str() );
	if( !path.empty() ) std::remove( path.c_str() );
	Check( st.valid, ( std::string( "Topology W: VCM render produced output: " ) + label ).c_str() );
	if( !st.valid ) return;
	const double expected = 0.5 / 3.14159265358979323846 * 16.0 / ( dist * dist );
	const double m = ( st.mean[0] + st.mean[1] + st.mean[2] ) / 3.0;
	char buf[256];
	std::snprintf( buf, sizeof(buf),
		"Topology W (%s): VCM mean %.6f vs closed form rho/pi*I/%.0f^2 = %.6f (%+.3f%%), within %g%% (DL-294)",
		label, m, dist, expected, 100.0 * ( m / expected - 1.0 ), 100.0 * tol );
	std::cout << "    " << buf << std::endl;
	Check( std::fabs( m / expected - 1.0 ) <= tol, buf );
}

static void TestNarrowFovSplatW()
{
	std::cout << "Testing topology W: narrow-fov (2 deg) splat vs closed form (DL-294)" << std::endl;
	RunNarrowFovRowW( "W1 spot -> mirror -> floor caustic", kSceneNarrowFovMirrorW1, 3.0, 0.006 );
	RunNarrowFovRowW( "W2 spot -> floor, no mirror", kSceneNarrowFovDirectW2, 4.0, 0.005 );
}


//////////////////////////////////////////////////////////////////////
// DL-317: VCM's MIS at a BSSRDF / random-walk jump.
//
// A subsurface event relocates the path from the hit where it went in
// to a sampled ENTRY vertex.  The relocation is not an edge -- nothing
// connects the two points and the jump's reverse density is never
// evaluated -- so the only strategies for a path through it are the
// ones that split it on the far side of the subpath that sampled the
// jump.  Pre-DL-317 VCM (a) reserved MIS mass at the entry for the
// connection ACROSS the jump (dVCM = 1/pdfSurface), (b) reserved a
// phantom NEE at a NON-connectible random-walk entry (the non-specular
// onward update, DL-126's pattern), and (c) kept every strategy at or
// past a LIGHT-side jump, whose own MIS chain was closed over itself --
// a second copy of the eye-sampled family (measured: with (a) and (b)
// fixed alone, the wall-dominated V row below read +3.5% merging on,
// +2.65% off).  The fix is a barrier: zero state at the entry, the
// ordinary onward update only from a connectible entry, and a
// partition BY PATH between the two jump families -- a light-side jump
// the eye family can cover (any NEE / connection / merge in the light
// segment, or s=0) ends the usable light subpath; one it cannot cover
// (a delta light feeding a random-walk entry: rows D1/D2) is kept
// (VCMIntegrator.cpp, BSSRDFEntryVertexState; BDPTUtilities::
// LightSegmentEyeWitness / LightJumpPartition -- per strategy and under
// the eye depth caps since DL-380).  Cutting the light family everywhere --
// the first DL-317 fix, 3763e998 -- left that class estimated by
// nothing (D1/D2 -97%; external review).
//
// Rows, all PT-referenced (PT samples only the eye side of a jump, as
// BDPT effectively does), VCM with merging ON and OFF:
//   F  white FURNACE (uniform environment radiance 1, the SSS sphere
//      fills the frame): a conservative random walk (absorption 0,
//      roughness 0.3) reads ~1 under PT (0.9975: the walk's own depth
//      truncation); the dipole diffusion profile is not exactly
//      conservative (PT ~0.954), so that row gates parity only.
//   B  a BACKLIT sphere filling the frame (emitter hidden behind it):
//      random walk roughness 0.8, and smooth diffusion.
//   V  BDPTStrategyBalanceTest's topology V (random-walk sphere on
//      Lambertian walls, front-lit) -- the row that sees (c): a light
//      walk exits the sphere onto the walls.
// Pre-fix (32x32, salted, VCM/PT - 1, merging on / off; n = 2 for F,
// 4 for B and V, from probe renders of these exact scenes):
//   F random walk -28.4% / -28.3%, diffusion -18.9% / -18.9%;
//   B random walk -85.3% / -85.2%, smooth diffusion -83.2% / -83.3%;
//   V -15.0% / -15.1%; U (the row below, merging on) -5.3%.
// Isolated red-proof (base VCMIntegrator.cpp, `--dl317-only 11`):
// 11 passed / 12 failed -- every VCM/PT ratio, the F1 floor and U red;
// only the on/off agreement checks pass (the defect hit both modes).
// Bands are >= 4.6 times the RATIO sd measured on the fixed build over
// six independent salted runs of these exact rows (`--dl317-only <seed>`,
// seeds 11/101/202/303/404/505), each row's three ratios pooled:
//   F1 0.042% sd -> 0.25%;  F2 0.24% -> 1.25%;  B1 2.4% -> 12%
//   (random-walk fireflies; F1 is the precise random-walk gate);
//   B2 0.55% -> 3%;  V 0.14% -> 0.7%.
// Post-fix readings (same runs, mean VCM/PT - 1, merging on / off):
//   F1 -0.005% / +0.008%, F2 -0.064% / -0.020%, B1 +0.03% / +0.59%,
//   B2 -0.52% / -0.36%, V +0.014% / +0.011%.  (B2's small negative
//   offset is shared by BDPT -- -0.33% in a 4-run probe -- so it is a
//   PT-vs-bidirectional depth/diffusion difference, not this barrier.)
//////////////////////////////////////////////////////////////////////
static const char* kSceneFurnaceHeadDL317 =
	"film\n{\n\twidth 32\n\theight 32\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 1.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 20.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_env_f\n\tcolor 1 1 1\n}\n\n"
	"sphere_geometry\n{\n\tname sph_f\n\tradius 1.0\n}\n\n"
	"standard_object\n{\n\tname obj_sph_f\n\tgeometry sph_f\n\tmaterial mat_f\n}\n\n";

static const char* kMatFurnaceRandomWalkDL317 =
	"randomwalk_sss_material\n{\n\tname mat_f\n\tior 1.3\n\tabsorption 0.0\n\tscattering 4.0\n\tg 0.0\n\troughness 0.3\n}\n\n";

static const char* kMatFurnaceDiffusionDL317 =
	"subsurfacescattering_material\n{\n\tname mat_f\n\tior 1.3\n\tabsorption 0.0\n\tscattering 4.0\n\tg 0.0\n\troughness 0.0\n}\n\n";

//! Backlit sphere: a small single-sided emitter BEHIND the sphere, facing
//! it, hidden from the camera by it; only subsurface transport lights the
//! frame.
static const char* kSceneBacklitTailDL317 =
	"sphere_geometry\n{\n\tname sph_b\n\tradius 0.6\n}\n\n"
	"standard_object\n{\n\tname obj_sph_b\n\tgeometry sph_b\n\tmaterial mat_b\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_emit_b\n\tcolor 1.0 1.0 1.0\n}\n\n"
	"lambertian_luminaire_material\n{\n\tname mat_emit_b\n\texitance pnt_emit_b\n\tscale 4.0\n\tmaterial none\n}\n\n"
	"clippedplane_geometry\n{\n\tname quad_emit_b\n\tpta -0.35 -0.35 -1.2\n\tptb 0.35 -0.35 -1.2\n\tptc 0.35 0.35 -1.2\n\tptd -0.35 0.35 -1.2\n\tdoublesided FALSE\n}\n\n"
	"standard_object\n{\n\tname obj_emit_b\n\tgeometry quad_emit_b\n\tmaterial mat_emit_b\n}\n";

static const char* kSceneBacklitHeadDL317 =
	"film\n{\n\twidth 32\n\theight 32\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 25.0\n}\n\n";

static const char* kMatBacklitRandomWalkDL317 =
	"randomwalk_sss_material\n{\n\tname mat_b\n\tior 1.3\n\tabsorption 0.1\n\tscattering 10.0\n\tg 0.0\n\troughness 0.8\n}\n\n";

static const char* kMatBacklitSmoothDiffusionDL317 =
	"subsurfacescattering_material\n{\n\tname mat_b\n\tior 1.3\n\tabsorption 0.1\n\tscattering 1.0\n\tg 0.0\n\troughness 0.0\n}\n\n";

//! BDPTStrategyBalanceTest's topology V scene, verbatim.
static const char* kSceneRandomWalkSphereVDL317 =
	"film\n{\n\twidth 32\n\theight 32\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 30.0\n}\n\n"
	"randomwalk_sss_material\n{\n\tname mat_rw\n\tior 1.3\n\tabsorption 0.1\n"
		"\tscattering 10.0\n\tg 0.0\n\troughness 0.8\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_alb_v\n\tcolor 0.5 0.5 0.5\n}\n\n"
	"lambertian_material\n{\n\tname mat_lamb_v\n\treflectance pnt_alb_v\n}\n\n"
	"clippedplane_geometry\n{\n\tname quad_wall\n\tpta -1 -1 0\n\tptb 1 -1 0\n\tptc 1 1 0\n\tptd -1 1 0\n}\n\n"
	"standard_object\n{\n\tname obj_wall\n\tgeometry quad_wall\n\tmaterial mat_lamb_v\n}\n\n"
	"clippedplane_geometry\n{\n\tname quad_floor\n\tpta -1 -1 0\n\tptb -1 -1 2\n\tptc 1 -1 2\n\tptd 1 -1 0\n}\n\n"
	"standard_object\n{\n\tname obj_floor\n\tgeometry quad_floor\n\tmaterial mat_lamb_v\n}\n\n"
	"sphere_geometry\n{\n\tname sph_v\n\tradius 0.55\n}\n\n"
	"standard_object\n{\n\tname obj_sph_v\n\tgeometry sph_v\n\tmaterial mat_rw\n\tposition 0 -0.45 0.6\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_emit_v\n\tcolor 1.0 1.0 1.0\n}\n\n"
	"lambertian_luminaire_material\n{\n\tname mat_emit_v\n\texitance pnt_emit_v\n\tscale 0.5\n\tmaterial none\n}\n\n"
	"clippedplane_geometry\n{\n\tname quad_emit_v\n\tpta -6 -6 4.2\n\tptb -6 6 4.2\n\tptc 6 6 4.2\n\tptd 6 -6 4.2\n}\n\n"
	"standard_object\n{\n\tname obj_emit_v\n\tgeometry quad_emit_v\n\tmaterial mat_emit_v\n}\n";

//! PT / VCM (merging on or off) chunks at depth 16 for the DL-317 rows.
//! `envPainter` non-null adds a uniform environment from that painter.
static std::string RasterizerDL317( const char* kind, int spp, const char* envPainter )
{
	char buf[1024];
	char env[256] = "";
	if( envPainter ) {
		std::snprintf( env, sizeof(env), "\tradiance_map %s\n\tradiance_background TRUE\n", envPainter );
	}
	if( std::strcmp( kind, "pt" ) == 0 ) {
		std::snprintf( buf, sizeof(buf), "pathtracing_pel_rasterizer\n{\n\tsamples %d\n\trr_min_depth 8\n"
			"\tmax_diffuse_bounce 16\n\tmax_glossy_bounce 16\n%s\tpixel_filter box\n\toidn_denoise FALSE\n}\n", spp, env );
	} else {
		std::snprintf( buf, sizeof(buf), "vcm_pel_rasterizer\n{\n\tmax_eye_depth 16\n\tmax_light_depth 16\n"
			"\tsamples %d\n\tmerge_radius 0.0\n\tvc_enabled true\n\tvm_enabled %s\n%s\tpixel_filter box\n\toidn_denoise FALSE\n}\n",
			spp, std::strcmp( kind, "vcm" ) == 0 ? "true" : "false", env );
	}
	return std::string( "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n" ) + buf
		+ "\nfile_rasterizeroutput\n{\n\tpattern rendered/vcm_balance_unused\n\ttype EXR\n\tbpp 32\n"
		"\tcolor_space Rec709RGB_Linear\n}\n";
}

//! Mean achromatic image value over `reps` salted renders (each render
//! gets its own salt through RenderAndComputeStats).  The rasterizer chunk
//! goes AFTER the body so an environment painter is already declared.
static bool RenderMeanDL317( const std::string& body, const std::string& rasterizer, int reps, double& out )
{
	const std::string scene = std::string( "RISE ASCII SCENE 7\n" ) + body + rasterizer;
	const std::string path = WriteSceneToTempFile( scene.c_str(), "dl317" );
	if( path.empty() ) return false;
	double sum = 0;
	bool ok = true;
	for( int i = 0; i < reps && ok; i++ ) {
		const ImageStats st = RenderAndComputeStats( path.c_str() );
		ok = st.valid;
		if( ok ) sum += ( st.mean[0] + st.mean[1] + st.mean[2] ) / 3.0;
	}
	std::remove( path.c_str() );
	out = sum / double( reps );
	return ok && std::isfinite( out ) && out > 0;
}

static void CheckRatioDL317( const std::string& label, double ref, double test, double band )
{
	const double rel = test / ref - 1.0;
	char buf[512];
	std::snprintf( buf, sizeof(buf), "%s: %.7f vs %.7f  %+.3f%%  (band +/- %.2f%%)",
		label.c_str(), test, ref, 100.0 * rel, 100.0 * band );
	std::cout << "    " << buf << std::endl;
	Check( std::isfinite( rel ) && std::fabs( rel ) <= band, buf );
}

//! PT, VCM merging on, VCM merging off; VCM/PT for both and on/off.
static void RunSSSBarrierRowDL317( const char* name, const std::string& body, const char* envPainter,
	int spp, int reps, double band, double closedFormFloor )
{
	std::cout << "Testing DL-317 " << name << std::endl;
	double pt = 0, on = 0, off = 0;
	const bool ok = RenderMeanDL317( body, RasterizerDL317( "pt", spp, envPainter ), reps, pt )
		&& RenderMeanDL317( body, RasterizerDL317( "vcm", spp, envPainter ), reps, on )
		&& RenderMeanDL317( body, RasterizerDL317( "vcmnovm", spp, envPainter ), reps, off );
	Check( ok, ( std::string( "DL-317 renders produced output: " ) + name ).c_str() );
	if( !ok ) return;
	CheckRatioDL317( std::string( "DL-317 VCM (merging on) / PT: " ) + name, pt, on, band );
	CheckRatioDL317( std::string( "DL-317 VCM (merging off) / PT: " ) + name, pt, off, band );
	CheckRatioDL317( std::string( "DL-317 VCM merging on / off: " ) + name, off, on, band );
	if( closedFormFloor > 0 ) {
		char buf[512];
		std::snprintf( buf, sizeof(buf), "DL-317 %s: furnace reads >= %.3f (PT %.5f, VCM on %.5f, off %.5f)",
			name, closedFormFloor, pt, on, off );
		std::cout << "    " << buf << std::endl;
		Check( pt >= closedFormFloor && on >= closedFormFloor && off >= closedFormFloor, buf );
	}
}

//////////////////////////////////////////////////////////////////////
// DL-317 review P1: rows lit by a DELTA light the eye family cannot
// reach from a random-walk entry.  A random-walk entry admits no NEE,
// connection or merge, so the eye family reaches the light from it only
// by BSDF-sampling onward -- which never hits a point light.  The path
// omni -> sphere -> walk -> exit -> wall -> camera therefore exists ONLY
// in the light-sampled jump family; cutting that family everywhere (the
// first DL-317 fix, 3763e998) left it estimated by nothing.  The wall is
// lit only through the sphere: the light sits behind the wall plane, off
// its edge.  Metric: the mean over wall-only pixels (sphere masked with a
// one-pixel dilation), VCM merging on / off against PT.
//////////////////////////////////////////////////////////////////////
static const char* kSceneDeltaLitRandomWalkHeadDL317 =
	"film\n{\n\twidth 32\n\theight 32\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 30.0\n}\n\n"
	"randomwalk_sss_material\n{\n\tname mat_rw\n\tior 1.3\n\tabsorption 0.1\n"
		"\tscattering 10.0\n\tg 0.0\n\troughness 0.3\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_alb_d\n\tcolor 0.5 0.5 0.5\n}\n\n"
	"lambertian_material\n{\n\tname mat_lamb_d\n\treflectance pnt_alb_d\n}\n\n"
	"clippedplane_geometry\n{\n\tname quad_wall\n\tpta -1 -1 0\n\tptb 1 -1 0\n\tptc 1 1 0\n\tptd -1 1 0\n}\n\n"
	"standard_object\n{\n\tname obj_wall\n\tgeometry quad_wall\n\tmaterial mat_lamb_d\n}\n\n"
	"sphere_geometry\n{\n\tname sph_d\n\tradius 0.55\n}\n\n"
	"standard_object\n{\n\tname obj_sph_d\n\tgeometry sph_d\n\tmaterial mat_rw\n\tposition 0 -0.45 0.6\n}\n\n";

static const char* kLightOmniDL317 =
	"omni_light\n{\n\tname lgt\n\tpower 20\n\tcolor 1 1 1\n\tposition 1.8 -0.4 -0.3\n}\n";

static const char* kLightSpotDL317 =
	"spot_light\n{\n\tname lgt\n\tposition 1.8 -0.4 -0.3\n\ttarget 0 -0.45 0.6\n\tinner 20.0\n\touter 30.0\n\tcolor 1 1 1\n\tpower 20\n}\n";

//! True for a pixel whose (dilated) footprint misses the sphere of the
//! delta-lit fixture: pinhole at (0,0,3.5), fov 30, 32x32, sphere r 0.55
//! at (0,-0.45,0.6).  `row` counts from the top of the image.
//! `SphereHitsDL317` counts how many of the nine dilated sample points hit
//! the sphere: 0 is a wall pixel, 9 a sphere-interior pixel (DL-375).
static int SphereHitsDL317( int row, int col )
{
	const double t = std::tan( 15.0 * 3.14159265358979323846 / 180.0 );
	const double c[3] = { 0.0, -0.45, 0.6 }, o[3] = { 0.0, 0.0, 3.5 };
	int hits = 0;
	for( int di = -1; di <= 1; di++ ) for( int dj = -1; dj <= 1; dj++ ) {
		const double x = ( col + dj + 0.5 ) / 16.0 - 1.0, y = 1.0 - ( row + di + 0.5 ) / 16.0;
		double d[3] = { x * t, y * t, -1.0 };
		const double l = std::sqrt( d[0]*d[0] + d[1]*d[1] + d[2]*d[2] );
		for( double& v : d ) v /= l;
		const double oc[3] = { o[0]-c[0], o[1]-c[1], o[2]-c[2] };
		const double b = d[0]*oc[0] + d[1]*oc[1] + d[2]*oc[2];
		const double q = b*b - ( oc[0]*oc[0] + oc[1]*oc[1] + oc[2]*oc[2] - 0.55*0.55 );
		if( q > 0 ) hits++;
	}
	return hits;
}

static bool WallPixelDL317( int row, int col ) { return SphereHitsDL317( row, col ) == 0; }
static bool SpherePixelDL375( int row, int col ) { return SphereHitsDL317( row, col ) == 9; }

//! Mean over wall pixels (or, `spherePixels`, over sphere-interior pixels:
//! DL-375) of `reps` salted renders (salting as in RenderAndComputeStats).
static bool RenderWallMeanDL317( const std::string& scene, int reps, double& out, bool spherePixels = false )
{
	const std::string path = WriteSceneToTempFile( scene.c_str(), "dl317wall" );
	if( path.empty() ) return false;
	double sum = 0;
	bool ok = true;
	for( int r = 0; r < reps && ok; r++ ) {
		ok = false;
		IJobPriv* pJob = nullptr;
		if( !RISE_CreateJobPriv( &pJob ) || !pJob ) break;
		if( pJob->LoadAsciiSceneViaCst( path.c_str() ) ) {
			pJob->RemoveRasterizerOutputs();
			CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
			GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "test capture output" );
			pJob->GetRasterizer()->AddRasterizerOutput( pCap );
			SobolSamplerTestHooks::ValueSalt().store( SobolSequence::HashCombine( g_seedBase + g_renderIndex, kSaltTag ) );
			std::srand( g_seedBase + g_renderIndex++ );
			const bool bRendered = pJob->Rasterize();
			SobolSamplerTestHooks::ValueSalt().store( 0u );
			if( bRendered && pCap->width == 32 && pCap->height == 32 ) {
				double acc = 0; int cnt = 0;
				for( int row = 0; row < 32; row++ ) for( int col = 0; col < 32; col++ ) {
					if( spherePixels ? !SpherePixelDL375( row, col ) : !WallPixelDL317( row, col ) ) continue;
					const RISEColor& px = pCap->pixels[ row * 32 + col ];
					acc += ( px.base.r + px.base.g + px.base.b ) * px.a / 3.0;
					cnt++;
				}
				if( cnt > 0 && std::isfinite( acc ) ) { sum += acc / cnt; ok = true; }
			}
			safe_release( pCap );
		}
		safe_release( pJob );
	}
	std::remove( path.c_str() );
	out = sum / double( reps );
	return ok && out > 0;
}

static void RunDeltaLitWallRowDL317( const char* name, const char* light, int spp, int reps, double band )
{
	std::cout << "Testing DL-317 " << name << std::endl;
	const std::string body = std::string( kSceneDeltaLitRandomWalkHeadDL317 ) + light;
	double pt = 0, on = 0, off = 0;
	const bool ok = RenderWallMeanDL317( std::string( "RISE ASCII SCENE 7\n" ) + body + RasterizerDL317( "pt", spp, nullptr ), reps, pt )
		&& RenderWallMeanDL317( std::string( "RISE ASCII SCENE 7\n" ) + body + RasterizerDL317( "vcm", spp, nullptr ), reps, on )
		&& RenderWallMeanDL317( std::string( "RISE ASCII SCENE 7\n" ) + body + RasterizerDL317( "vcmnovm", spp, nullptr ), reps, off );
	Check( ok, ( std::string( "DL-317 renders produced output: " ) + name ).c_str() );
	if( !ok ) return;
	CheckRatioDL317( std::string( "DL-317 wall-only VCM (merging on) / PT: " ) + name, pt, on, band );
	CheckRatioDL317( std::string( "DL-317 wall-only VCM (merging off) / PT: " ) + name, pt, off, band );
	CheckRatioDL317( std::string( "DL-317 wall-only VCM merging on / off: " ) + name, off, on, band );
}

//! Bands: >= 4.6 x the ratio sd over four salted runs at 2048 spp
//! (`--dl317-delta-only` seeds 101/202/303/404), on and off pooled:
//! D1 3.1% -> 15% (an omni sends few light walks into the sphere, and
//! the wall is reached only by light-family splats and connections);
//! D2 0.65% -> 3.5%.  Readings, VCM/PT - 1 merging on / off: D1
//! -1.2% / -1.7%, D2 -0.33% / -0.31% (post-fix).  On the first DL-317
//! fix (3763e998, the light family cut everywhere) every ratio read
//! -97% .. -98%; on master 43e9f3bb8 the external review measured D1
//! ~-51% (phantom MIS mass at the entry).
static void TestDeltaLitWallDL317()
{
	RunDeltaLitWallRowDL317( "D1 omni behind the wall lights a random-walk sphere; wall lit only through it",
		kLightOmniDL317, 2048, 1, 0.15 );
	RunDeltaLitWallRowDL317( "D2 spot behind the wall lights a random-walk sphere; wall lit only through it",
		kLightSpotDL317, 2048, 1, 0.035 );
}

//////////////////////////////////////////////////////////////////////
// DL-375: the SAME delta-lit fixture, the SPHERE's own pixels.  The
// random-walk sphere is seen directly, so the path is light -> sphere ->
// walk -> exit -> camera.  Before DL-375 a random-walk entry admitted no
// NEE / connection / merge on either side, so no VCM strategy reached it
// (eye family: BSDF sampling onward never hits a point light; light
// family: the light-side entry could not splat) -- VCM read ~-98.6%,
// while PT reaches it by NEE at the exit through RandomWalkEntryBSDF.
// The random-walk entry is now connectible (Sw = the exact-Fresnel
// RandomWalkEntryBSDF, MIS density the walk's cosine exit), the eye
// family owns the path, and the light-side jump is cut there
// (LightSegmentEyeWitness reads the entry's connectibility).
//////////////////////////////////////////////////////////////////////
static void RunDeltaLitSphereRowDL375( const char* name, const char* light, int spp, double band )
{
	std::cout << "Testing DL-375 " << name << std::endl;
	const std::string body = std::string( kSceneDeltaLitRandomWalkHeadDL317 ) + light;
	double pt = 0, on = 0, off = 0;
	const bool ok = RenderWallMeanDL317( std::string( "RISE ASCII SCENE 7\n" ) + body + RasterizerDL317( "pt", spp, nullptr ), 1, pt, true )
		&& RenderWallMeanDL317( std::string( "RISE ASCII SCENE 7\n" ) + body + RasterizerDL317( "vcm", spp, nullptr ), 1, on, true )
		&& RenderWallMeanDL317( std::string( "RISE ASCII SCENE 7\n" ) + body + RasterizerDL317( "vcmnovm", spp, nullptr ), 1, off, true );
	Check( ok, ( std::string( "DL-375 renders produced output: " ) + name ).c_str() );
	if( !ok ) return;
	CheckRatioDL317( std::string( "DL-375 sphere-only VCM (merging on) / PT: " ) + name, pt, on, band );
	CheckRatioDL317( std::string( "DL-375 sphere-only VCM (merging off) / PT: " ) + name, pt, off, band );
}

static void TestDeltaLitSphereDL375()
{
	RunDeltaLitSphereRowDL375( "S1 omni lights a directly-seen random-walk sphere (sphere pixels)",
		kLightOmniDL317, 2048, 0.05 );
	RunDeltaLitSphereRowDL375( "S2 spot lights a directly-seen random-walk sphere (sphere pixels)",
		kLightSpotDL317, 2048, 0.05 );
}

//////////////////////////////////////////////////////////////////////
// DL-380: the D1/D2 wall rows under DEPTH CAPS.  The eye family covers
// light -> sphere -> walk -> exit -> wall -> camera by NEE / connection /
// merge at its random-walk entry, which needs TWO eye surface vertices
// (the wall, then the sphere it jumps from).  At `max_eye_depth 1` it has
// no strategy, so the light family (splat / connection at the wall) must
// keep the path; the partition used to decide from vertex types alone and
// cut it (-98.6% merging on).  E16/L1 is the mirror: the light walk is the
// truncated one, the eye family owns the path, nothing may be counted
// twice.  Reference: PT, which has no subpath caps.
//////////////////////////////////////////////////////////////////////
static std::string RasterizerDL380( const char* kind, int spp, int eyeDepth, int lightDepth )
{
	if( std::strcmp( kind, "pt" ) == 0 ) {
		return RasterizerDL317( "pt", spp, nullptr );
	}
	char buf[1024];
	std::snprintf( buf, sizeof(buf), "vcm_pel_rasterizer\n{\n\tmax_eye_depth %d\n\tmax_light_depth %d\n"
		"\tsamples %d\n\tmerge_radius 0.0\n\tvc_enabled true\n\tvm_enabled %s\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n",
		eyeDepth, lightDepth, spp, std::strcmp( kind, "vcm" ) == 0 ? "true" : "false" );
	return std::string( "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n" ) + buf
		+ "\nfile_rasterizeroutput\n{\n\tpattern rendered/vcm_balance_unused\n\ttype EXR\n\tbpp 32\n"
		"\tcolor_space Rec709RGB_Linear\n}\n";
}

//! Mean and sd of the wall-pixel mean over `n` independently salted renders.
static bool WallMeanSdDL380( const std::string& scene, int n, double& mean, double& sd )
{
	double sum = 0, sumSq = 0;
	for( int i = 0; i < n; i++ ) {
		double w = 0;
		if( !RenderWallMeanDL317( scene, 1, w ) ) return false;
		sum += w; sumSq += w * w;
	}
	mean = sum / n;
	sd = n > 1 ? std::sqrt( std::max( 0.0, ( sumSq - n * mean * mean ) / ( n - 1 ) ) ) : 0;
	return true;
}

static void RunDepthCappedWallRowDL380( const char* name, const char* light, int eyeDepth, int lightDepth,
	int spp, int n, double pt, double band )
{
	std::cout << "Testing DL-380 " << name << std::endl;
	const std::string body = std::string( "RISE ASCII SCENE 7\n" ) + kSceneDeltaLitRandomWalkHeadDL317 + light;
	double on = 0, onSd = 0, off = 0, offSd = 0;
	const bool ok = WallMeanSdDL380( body + RasterizerDL380( "vcm", spp, eyeDepth, lightDepth ), n, on, onSd )
		&& WallMeanSdDL380( body + RasterizerDL380( "vcmnovm", spp, eyeDepth, lightDepth ), n, off, offSd );
	Check( ok, ( std::string( "DL-380 renders produced output: " ) + name ).c_str() );
	if( !ok ) return;
	std::printf( "    merging on %.7f (sd %.7f), off %.7f (sd %.7f), n %d\n", on, onSd, off, offSd, n );
	CheckRatioDL317( std::string( "DL-380 wall-only VCM (merging on) / PT: " ) + name, pt, on, band );
	CheckRatioDL317( std::string( "DL-380 wall-only VCM (merging off) / PT: " ) + name, pt, off, band );
}

//! Bands are the DL-317 D1/D2 bands (>= 4.6 sd of the ratio).
static void TestDepthCappedWallDL380()
{
	const int n = 3;
	double ptOmni = 0, ptSpot = 0, sdOmni = 0, sdSpot = 0;
	const std::string head = std::string( "RISE ASCII SCENE 7\n" ) + kSceneDeltaLitRandomWalkHeadDL317;
	const bool ok = WallMeanSdDL380( head + kLightOmniDL317 + RasterizerDL380( "pt", 2048, 16, 16 ), n, ptOmni, sdOmni )
		&& WallMeanSdDL380( head + kLightSpotDL317 + RasterizerDL380( "pt", 2048, 16, 16 ), n, ptSpot, sdSpot );
	Check( ok, "DL-380 PT references produced output" );
	if( !ok ) return;
	std::printf( "    PT wall reference: D1 omni %.7f (sd %.7f), D2 spot %.7f (sd %.7f), n %d\n",
		ptOmni, sdOmni, ptSpot, sdSpot, n );
	RunDepthCappedWallRowDL380( "D1 omni, max_eye_depth 1 / max_light_depth 16", kLightOmniDL317, 1, 16, 2048, n, ptOmni, 0.15 );
	RunDepthCappedWallRowDL380( "D2 spot, max_eye_depth 1 / max_light_depth 16", kLightSpotDL317, 1, 16, 2048, n, ptSpot, 0.035 );
	RunDepthCappedWallRowDL380( "D2 spot, max_eye_depth 16 / max_light_depth 1 (light truncated: no double count)",
		kLightSpotDL317, 16, 1, 2048, n, ptSpot, 0.035 );
}

// Eight fixed salted triples keep F1 independent of earlier suite renders.
// Ratio SD is measured across complete renders, not across correlated pixels.
static void TestFurnaceF1Salted()
{
    const unsigned savedSeed=g_seedBase, savedIndex=g_renderIndex;
    std::vector<double> values[3];
    bool valid=true;
    for(unsigned rep=0;rep<8;++rep) {
        g_seedBase=savedSeed+317001+101*rep; g_renderIndex=0;
        for(int mode=0;mode<3;++mode) {
            double value=0;
            const char* kind=mode==0?"pt":mode==1?"vcm":"vcmnovm";
            const bool ok=RenderMeanDL317(std::string(kMatFurnaceRandomWalkDL317)+kSceneFurnaceHeadDL317,
                RasterizerDL317(kind,256,"pnt_env_f"),1,value);
            Check(ok,"F1 salted render produces finite output"); valid=valid&&ok;
            values[mode].push_back(value);
        }
    }
    g_seedBase=savedSeed; g_renderIndex=savedIndex;
    if(!valid) return;
    auto stats=[](const std::vector<double>& v) {
        double mean=0,ss=0;for(double x:v) mean+=x;mean/=v.size();
        for(double x:v) ss+=(x-mean)*(x-mean);
        return std::make_pair(mean,std::sqrt(ss/(v.size()-1)));
    };
    for(int mode=0;mode<3;++mode) {
        const auto st=stats(values[mode]);
        std::printf("F1 mode=%d n=8 mean=%.9g sd=%.9g\n",mode,st.first,st.second);
        Check(st.first>=.99,"F1 salted conservative furnace >= 0.99");
    }
    const int numerator[3]={1,2,1},denominator[3]={0,0,2};
    for(int comparison=0;comparison<3;++comparison) {
        std::vector<double> ratios;
        for(unsigned rep=0;rep<8;++rep) ratios.push_back(values[numerator[comparison]][rep]/values[denominator[comparison]][rep]);
        const auto st=stats(ratios);
        const double band=3*st.second/std::sqrt(8.0)+1e-6;
        unsigned oldFailures=0;for(double ratio:ratios) if(std::fabs(ratio-1)>.0025) ++oldFailures;
        std::printf("F1 comparison=%d n=8 ratio=%.9g sd=%.9g 3SE=%.9g old-single-failures=%u\n",comparison,st.first,st.second,band,oldFailures);
        Check(band<=.0025,"F1 salted ratio resolves the existing 0.25% precision budget");
        Check(std::fabs(st.first-1)<=band,"F1 salted ratio includes parity in three measured SE");
        Check(std::fabs(st.first-1)<=.0025,"F1 salted mean retains strict 0.25% physical limit");
    }
}

static void TestSSSBarrierDL317()
{
	TestFurnaceF1Salted();
	RunSSSBarrierRowDL317( "F2 white furnace, smooth diffusion (parity only)",
		std::string( kMatFurnaceDiffusionDL317 ) + kSceneFurnaceHeadDL317, "pnt_env_f",
		256, 1, 0.0125, 0 );
	RunSSSBarrierRowDL317( "B1 backlit random-walk sphere (roughness 0.8) filling the frame",
		std::string( kSceneBacklitHeadDL317 ) + kMatBacklitRandomWalkDL317 + kSceneBacklitTailDL317, nullptr,
		1024, 3, 0.12, 0 );
	RunSSSBarrierRowDL317( "B2 backlit smooth-diffusion sphere filling the frame",
		std::string( kSceneBacklitHeadDL317 ) + kMatBacklitSmoothDiffusionDL317 + kSceneBacklitTailDL317, nullptr,
		1024, 3, 0.03, 0 );
	TestDeltaLitWallDL317();
	TestDeltaLitSphereDL375();
	RunSSSBarrierRowDL317( "V front-lit random-walk sphere on Lambertian walls (light walks exit onto the walls)",
		kSceneRandomWalkSphereVDL317, nullptr,
		1024, 1, 0.007, 0 );
}

//////////////////////////////////////////////////////////////////////
// DL-333 / DL-356 consistency pins (2026-10-02): topology V's Lambertian
// wall + floor around a SMOOTH (roughness 0) diffusion
// `subsurfacescattering_material` sphere, once analytic and once
// tessellated (`displaced_geometry`, displacement none, detail 64 -- an
// indexed triangle mesh, the class DL-356 suspected).  DL-356 quoted VCM
// +21 % (merging on) / +61..71 % (off) over PT on the shipped
// `vcm_sss_dragon`; re-measured on master and at the DL-317 merge
// revision, merging off agrees with PT and the merging-on excess is the
// authored fixed `merge_radius 0.1`'s kernel bias (it shrinks with the
// radius and a Lambertian dragon shows it too) -- see
// docs/MIS_HEURISTICS.md section 4a, "DL-333 / DL-356".  These rows gate
// that diffusion SSS on an analytic and on a meshed sphere is VCM / PT
// consistent with merging on (automatic radius) and off.  Green on the
// base as well (a pin, not a red-proof).  Measured (32x32, 1024 spp,
// salted, n = 3): VCM off / PT +0.05 % / +0.06 %, on / PT +0.08 % /
// +0.10 %, per-render sd ~0.1 %.
//////////////////////////////////////////////////////////////////////
static std::string SmoothDiffusionSphereBodyDL333( bool tessellated )
{
	std::string s =
		"film\n{\n\twidth 32\n\theight 32\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 30.0\n}\n\n"
		"subsurfacescattering_material\n{\n\tname mat_sss\n\tior 1.3\n\tabsorption 0.1\n"
			"\tscattering 1.0\n\tg 0.0\n\troughness 0.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_alb_w\n\tcolor 0.5 0.5 0.5\n}\n\n"
		"lambertian_material\n{\n\tname mat_lamb_w\n\treflectance pnt_alb_w\n}\n\n"
		"clippedplane_geometry\n{\n\tname quad_wall\n\tpta -1 -1 0\n\tptb 1 -1 0\n\tptc 1 1 0\n\tptd -1 1 0\n}\n\n"
		"standard_object\n{\n\tname obj_wall\n\tgeometry quad_wall\n\tmaterial mat_lamb_w\n}\n\n"
		"clippedplane_geometry\n{\n\tname quad_floor\n\tpta -1 -1 0\n\tptb -1 -1 2\n\tptc 1 -1 2\n\tptd 1 -1 0\n}\n\n"
		"standard_object\n{\n\tname obj_floor\n\tgeometry quad_floor\n\tmaterial mat_lamb_w\n}\n\n"
		"sphere_geometry\n{\n\tname sph_w\n\tradius 0.55\n}\n\n";
	if( tessellated ) {
		s += "displaced_geometry\n{\n\tname sph_w_mesh\n\tbase_geometry sph_w\n\tdetail 64\n"
			"\tdisplacement none\n\tdisp_scale 0\n}\n\n";
	}
	s += std::string( "standard_object\n{\n\tname obj_sph_w\n\tgeometry " ) + ( tessellated ? "sph_w_mesh" : "sph_w" )
		+ "\n\tmaterial mat_sss\n\tposition 0 -0.45 0.6\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_emit_w\n\tcolor 1.0 1.0 1.0\n}\n\n"
		"lambertian_luminaire_material\n{\n\tname mat_emit_w\n\texitance pnt_emit_w\n\tscale 0.5\n\tmaterial none\n}\n\n"
		"clippedplane_geometry\n{\n\tname quad_emit_w\n\tpta -6 -6 4.2\n\tptb -6 6 4.2\n\tptc 6 6 4.2\n\tptd 6 -6 4.2\n}\n\n"
		"standard_object\n{\n\tname obj_emit_w\n\tgeometry quad_emit_w\n\tmaterial mat_emit_w\n}\n";
	return s;
}

static void TestSmoothDiffusionSphereDL333()
{
	RunSSSBarrierRowDL317( "DL-333 pin: smooth diffusion analytic sphere on Lambertian walls",
		SmoothDiffusionSphereBodyDL333( false ), nullptr, 1024, 3, 0.006, 0 );
	RunSSSBarrierRowDL317( "DL-333 pin: smooth diffusion tessellated sphere on Lambertian walls",
		SmoothDiffusionSphereBodyDL333( true ), nullptr, 1024, 3, 0.006, 0 );
}

//////////////////////////////////////////////////////////////////////
// Topology X: AUTOMATIC merge radius on a small lit patch inside a
// large black room (DL-319).
//
// A 0.14-wide Lambertian patch, lit directly by a sphere emitter, seen
// by a 2-degree pinhole that frames only its middle; around everything
// a black (absorbing) room of radius 20, and a mirror sphere far below
// the patch, out of view, whose only job is to give the light pre-pass
// a delta surface so VCM enables merging at all.  Nothing but the
// emitter lights the visible face (the room returns nothing, the
// mirror's light reaches only the patch's underside), so the image is
// the closed form rho/pi * E with E = M * (R/d)^2 * cos(theta) per
// footprint point (a sphere's projected solid angle; M the luminaire's
// exitance, L = M/pi).
//
// Before DL-319 the auto radius was 1 % of the median light-subpath
// segment, and nearly every light segment runs to the room wall 20
// units out: radius ~0.19, wider than the patch, so the merge's kernel
// estimate under-read it.  The radius is now clipped to 8 eye-side
// pixel footprints at the camera's merge vertices (~0.02 here).
// Measured (VCM/closed form, one salted render per seed base 11/12/13,
// 2048 spp, 2026-10-01): pre-DL-319 0.7852 / 0.7804 / 0.7676 (auto
// radius 0.195, 73 footprints); post 1.0004 / 0.9997 / 1.0001 (radius
// 0.0214, eye-clipped).  Band 1 % (> 20 sd of the post-fix spread).
//////////////////////////////////////////////////////////////////////
namespace TopologyX
{
	const double kPi = 3.14159265358979323846;
	const double kYPatch = 0.05, kPatch = 0.07, kRho = 0.5;
	const double kEmitX = 0.8, kEmitY = 2.5, kEmitZ = 0.0, kEmitR = 0.4, kEmitScale = 1.688;
	const double kCamY = 2.5, kCamZ = 0.001, kFovDeg = 2.0;

	//! Pixel mean over the pinhole footprint (16 x 16 grid, square film,
	//! half-extent tan(fov/2)).  `inside` reports whether every footprint
	//! sample landed on the patch.
	double ClosedForm( bool& inside )
	{
		inside = true;
		const double t = std::tan( 0.5 * kFovDeg * kPi / 180.0 );
		double fx = 0.0, fy = -kCamY, fz = -kCamZ;
		const double fl = std::sqrt( fx * fx + fy * fy + fz * fz );
		fx /= fl; fy /= fl; fz /= fl;
		// right = forward x up(0,0,1), up2 = right x forward.
		double rx = fy, ry = -fx, rz = 0.0;
		const double rl = std::sqrt( rx * rx + ry * ry + rz * rz );
		rx /= rl; ry /= rl; rz /= rl;
		const double ux = ry * fz - rz * fy, uy = rz * fx - rx * fz, uz = rx * fy - ry * fx;
		const int G = 16;
		double sum = 0.0;
		for( int i = 0; i < G; i++ ) {
			for( int j = 0; j < G; j++ ) {
				const double u = ( ( i + 0.5 ) / G * 2.0 - 1.0 ) * t;
				const double v = ( ( j + 0.5 ) / G * 2.0 - 1.0 ) * t;
				const double dx = fx + u * rx + v * ux, dy = fy + u * ry + v * uy, dz = fz + u * rz + v * uz;
				const double s = ( kYPatch - kCamY ) / dy;
				const double px = s * dx, pz = kCamZ + s * dz;
				if( std::fabs( px ) >= kPatch || std::fabs( pz ) >= kPatch ) inside = false;
				const double vx = kEmitX - px, vy = kEmitY - kYPatch, vz = kEmitZ - pz;
				const double d2 = vx * vx + vy * vy + vz * vz;
				const double cosT = vy / std::sqrt( d2 );
				sum += kEmitScale * ( kEmitR * kEmitR / d2 ) * cosT;
			}
		}
		return kRho / kPi * sum / double( G * G );
	}

	std::string Scene()
	{
		char buf[4096];
		std::snprintf( buf, sizeof(buf),
			"film\n{\n\twidth 32\n\theight 32\n}\n\n"
			"pinhole_camera\n{\n\tlocation 0.0 %g %g\n\tlookat 0 0 0\n\tup 0 0 1\n\tfov %g\n}\n\n"
			"uniformcolor_painter\n{\n\tname pnt_albedo\n\tcolor %g %g %g\n\tcolorspace Rec709RGB_Linear\n}\n\n"
			"lambertian_material\n{\n\tname mat_diffuse\n\treflectance pnt_albedo\n}\n\n"
			"clippedplane_geometry\n{\n\tname quad\n"
			"\tpta -%g %g %g\n\tptb %g %g %g\n\tptc %g %g -%g\n\tptd -%g %g -%g\n}\n\n"
			"standard_object\n{\n\tname obj_quad\n\tgeometry quad\n\tmaterial mat_diffuse\n}\n\n"
			"uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1.0 1.0 1.0\n\tcolorspace Rec709RGB_Linear\n}\n\n"
			"lambertian_luminaire_material\n{\n\tname mat_emit\n\texitance pnt_emit\n\tscale %g\n\tmaterial none\n}\n\n"
			"sphere_geometry\n{\n\tname geo_emit\n\tradius %g\n}\n\n"
			"standard_object\n{\n\tname obj_emit\n\tgeometry geo_emit\n\tmaterial mat_emit\n\tposition %g %g %g\n}\n\n"
			"uniformcolor_painter\n{\n\tname pnt_mirror\n\tcolor 1 1 1\n\tcolorspace Rec709RGB_Linear\n}\n\n"
			"perfectreflector_material\n{\n\tname mat_mirror\n\treflectance pnt_mirror\n}\n\n"
			"sphere_geometry\n{\n\tname geo_mirror\n\tradius 1.0\n}\n\n"
			"standard_object\n{\n\tname obj_mirror\n\tgeometry geo_mirror\n\tmaterial mat_mirror\n\tposition 0 -4 0\n}\n\n"
			"uniformcolor_painter\n{\n\tname pnt_black\n\tcolor 0 0 0\n}\n\n"
			"lambertian_material\n{\n\tname mat_black\n\treflectance pnt_black\n}\n\n"
			"sphere_geometry\n{\n\tname room_geo\n\tradius 20\n}\n\n"
			"standard_object\n{\n\tname room\n\tgeometry room_geo\n\tmaterial mat_black\n}\n",
			kCamY, kCamZ, kFovDeg, kRho, kRho, kRho,
			kPatch, kYPatch, kPatch, kPatch, kYPatch, kPatch, kPatch, kYPatch, kPatch, kPatch, kYPatch, kPatch,
			kEmitScale, kEmitR, kEmitX, kEmitY, kEmitZ );
		return std::string( buf );
	}

	const char* kRasterizerVCM =
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		"vcm_pel_rasterizer\n{\n\tmax_eye_depth 5\n\tmax_light_depth 5\n\tsamples 2048\n\tmerge_radius 0.0\n"
		"\tvc_enabled true\n\tvm_enabled true\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";
}

static void TestAutoRadiusSmallPatchX()
{
	std::cout << "Testing topology X: VCM automatic merge radius, small patch in a large black room (DL-319)" << std::endl;
	bool inside = false;
	const double expected = TopologyX::ClosedForm( inside );
	Check( expected > 0 && inside, "Topology X: closed form evaluated with the whole footprint on the patch" );
	if( !( expected > 0 ) ) return;

	const std::string scene = std::string( "RISE ASCII SCENE 7\n" ) + TopologyX::kRasterizerVCM + TopologyX::Scene();
	const std::string path = WriteSceneToTempFile( scene.c_str(), "autoradius_x" );
	const ImageStats st = path.empty() ? ImageStats{} : RenderAndComputeStats( path.c_str() );
	if( !path.empty() ) std::remove( path.c_str() );
	Check( st.valid, "Topology X: VCM render produced output" );
	if( !st.valid ) return;
	const double m = ( st.mean[0] + st.mean[1] + st.mean[2] ) / 3.0;
	const double tol = 0.01;
	char buf[256];
	std::snprintf( buf, sizeof(buf),
		"Topology X: VCM mean %.6f vs closed form %.6f (ratio %.4f), within %g%% (DL-319)",
		m, expected, m / expected, 100.0 * tol );
	std::cout << "    " << buf << std::endl;
	Check( std::fabs( m / expected - 1.0 ) <= tol, buf );
}

//////////////////////////////////////////////////////////////////////
// Topology Y: SEVERAL equal luminaries (DL-348).
//
// A Lambertian floor (rho 0.5) under N equal, single-sided, face-down
// 0.8 x 0.8 emitter quads at height 1.4 (N = 1 at the centre; N = 2 at
// x = +/-1.5; N = 4 at (+/-1.5, +/-1.5)), seen by an orthographic
// camera framing the floor square [-2, 2]^2.  The quads reflect
// nothing (`material none`), so the image is the closed form
// rho/pi * mean(E), E = M * sum_i F_i (the point-to-parallel-rectangle
// form factor) -- no interreflection exists.  Two controls: the SAME
// four quads as ONE luminary (an indexed mesh, so one light-table
// entry), and a pinhole view of the four-light scene against PT.  The
// last row adds a uniform environment to the four quads (vs PT).
//
// Defect (DL-348): `PdfSelectLuminary`, the selection probability the
// eye-hits-emitter strategy (VCM `EvaluateS0Impl`, BDPT s = 0) uses in
// its MIS weight, returned the LIGHT-BVH's shading-point-dependent pmf
// whenever the BVH was built (`light_bvh` defaults TRUE; built for 2+
// lights), while every strategy it competes with -- NEE (s = 1), the
// t = 1 splat, the s >= 2 connections, merging -- roots its light
// subpath through `SampleLight`, the shading-point-INDEPENDENT alias
// table.  Two different selection densities for one strategy is a
// partition-of-unity violation; one light (no BVH) and one mesh
// luminary are immune by construction, which is the row's own
// discriminator.  Numbers: docs/DL348_MULTI_LUMINARY_SELECTION_PDF.md.
//////////////////////////////////////////////////////////////////////
namespace TopologyY
{
	const double kPi = 3.14159265358979323846;
	const double kRho = 0.5;
	const double kH = 1.4;			// emitter height above the floor
	const double kHalf = 0.4;		// emitter half-width
	const double kScale = 6.0;		// exitance M (white painter x scale)
	const double kOff = 1.5;		// emitter centre offset
	const double kView = 4.0;		// orthographic viewport width
	const double kEnvL = 0.05;		// uniform environment radiance (env row; small enough
								// that the quads carry ~70 % of the image)
	const int kSpp = 64;
	const int kRepeats = 4;

	struct Layout { int n; double cx[4]; double cy[4]; bool oneMesh; };

	const Layout kOne    = { 1, { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, false };
	const Layout kTwo    = { 2, { -kOff, kOff, 0, 0 }, { 0, 0, 0, 0 }, false };
	const Layout kFour   = { 4, { -kOff, kOff, -kOff, kOff }, { -kOff, -kOff, kOff, kOff }, false };
	const Layout kFourMesh = { 4, { -kOff, kOff, -kOff, kOff }, { -kOff, -kOff, kOff, kOff }, true };

	double CornerF( const double a, const double b, const double h )
	{
		const double A = a / h, B = b / h;
		const double sA = std::sqrt( 1.0 + A * A ), sB = std::sqrt( 1.0 + B * B );
		return ( A / sA * std::atan( B / sA ) + B / sB * std::atan( A / sB ) ) / ( 2.0 * kPi );
	}

	//! Point (x, y) on the floor to a parallel rectangle at height h.
	double RectF( double x, double y, double x1, double x2, double y1, double y2, double h )
	{
		return CornerF( x2 - x, y2 - y, h ) - CornerF( x1 - x, y2 - y, h )
			- CornerF( x2 - x, y1 - y, h ) + CornerF( x1 - x, y1 - y, h );
	}

	//! Orthographic image mean: rho/pi * mean(E) over [-kView/2, kView/2]^2.
	double ClosedForm( const Layout& L )
	{
		const int N = 400;
		double sum = 0;
		for( int j = 0; j < N; j++ ) {
			for( int i = 0; i < N; i++ ) {
				const double x = -kView / 2 + ( i + 0.5 ) * kView / N;
				const double y = -kView / 2 + ( j + 0.5 ) * kView / N;
				for( int k = 0; k < L.n; k++ ) {
					sum += kScale * RectF( x, y, L.cx[k] - kHalf, L.cx[k] + kHalf,
						L.cy[k] - kHalf, L.cy[k] + kHalf, kH );
				}
			}
		}
		return kRho / kPi * sum / double( N * N );
	}

	std::string Fmt( const char* f, double a = 0, double b = 0, double c = 0, double d = 0,
		double e = 0, double g = 0, double h = 0, double i = 0, double j = 0, double k = 0,
		double l = 0, double m = 0 )
	{
		char buf[1024];
		std::snprintf( buf, sizeof(buf), f, a, b, c, d, e, g, h, i, j, k, l, m );
		return std::string( buf );
	}

	std::string Scene( const Layout& L, const bool pinhole )
	{
		std::string s = "film\n{\n\twidth 32\n\theight 32\n}\n\n";
		if( pinhole ) {
			s += "pinhole_camera\n{\n\tlocation 0 0 1\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 90\n}\n\n";
		} else {
			s += Fmt( "orthographic_camera\n{\n\tlocation 0 0 1\n\tlookat 0 0 0\n\tup 0 1 0\n"
				"\tviewport_scale %g %g\n}\n\n", kView, kView );
		}
		s += Fmt( "uniformcolor_painter\n{\n\tname pnt_floor\n\tcolor %g %g %g\n}\n\n", kRho, kRho, kRho );
		s += "lambertian_material\n{\n\tname mat_floor\n\treflectance pnt_floor\n}\n\n"
			"clippedplane_geometry\n{\n\tname geo_floor\n"
			"\tpta -200 -200 0\n\tptb 200 -200 0\n\tptc 200 200 0\n\tptd -200 200 0\n\tdoublesided FALSE\n}\n\n"
			"standard_object\n{\n\tname floor\n\tgeometry geo_floor\n\tmaterial mat_floor\n}\n\n"
			"uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1 1 1\n}\n\n";
		s += Fmt( "lambertian_luminaire_material\n{\n\tname mat_emit\n\texitance pnt_emit\n\tscale %g\n\tmaterial none\n}\n\n", kScale );
		// Face-down winding: normal = Cross( ptb - pta, ptd - pta ) = -Z.
		if( L.oneMesh ) {
			s += "indexedmesh_geometry\n{\n\tname geo_emit\n";
			for( int k = 0; k < L.n; k++ ) {
				const double x1 = L.cx[k] - kHalf, x2 = L.cx[k] + kHalf;
				const double y1 = L.cy[k] - kHalf, y2 = L.cy[k] + kHalf;
				s += Fmt( "\tvertex %g %g %g\n\tvertex %g %g %g\n\tvertex %g %g %g\n\tvertex %g %g %g\n",
					x1, y2, kH, x2, y2, kH, x2, y1, kH, x1, y1, kH );
			}
			for( int k = 0; k < L.n; k++ ) {
				const double b = 4.0 * k;
				s += Fmt( "\ttriangle %g %g %g\n\ttriangle %g %g %g\n", b, b + 1, b + 2, b, b + 2, b + 3 );
			}
			s += "\tdouble_sided FALSE\n\tface_normals TRUE\n}\n\n"
				"standard_object\n{\n\tname emit\n\tgeometry geo_emit\n\tmaterial mat_emit\n}\n\n";
		} else {
			for( int k = 0; k < L.n; k++ ) {
				const double x1 = L.cx[k] - kHalf, x2 = L.cx[k] + kHalf;
				const double y1 = L.cy[k] - kHalf, y2 = L.cy[k] + kHalf;
				char nm[32];
				std::snprintf( nm, sizeof(nm), "emit%d", k );
				s += std::string( "clippedplane_geometry\n{\n\tname geo_" ) + nm + "\n";
				s += Fmt( "\tpta %g %g %g\n\tptb %g %g %g\n\tptc %g %g %g\n\tptd %g %g %g\n\tdoublesided FALSE\n}\n\n",
					x1, y2, kH, x2, y2, kH, x2, y1, kH, x1, y1, kH );
				s += std::string( "standard_object\n{\n\tname " ) + nm + "\n\tgeometry geo_" + nm
					+ "\n\tmaterial mat_emit\n}\n\n";
			}
		}
		return s;
	}

	std::string Rasterizer( const char* kind, const bool env )
	{
		std::string envLines;
		std::string envPainter;
		if( env ) {
			envPainter = Fmt( "uniformcolor_painter\n{\n\tname pnt_env\n\tcolor %g %g %g\n}\n\n", kEnvL, kEnvL, kEnvL );
			envLines = "\tradiance_map pnt_env\n\tradiance_scale 1.0\n";
		}
		std::string s = envPainter +
			"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n";
		const std::string common = Fmt( "\tsamples %g\n\tpixel_filter box\n\toidn_denoise FALSE\n", double( kSpp ) ) + envLines;
		if( std::strcmp( kind, "pt" ) == 0 ) {
			s += "pathtracing_pel_rasterizer\n{\n" + common + "}\n\n";
		} else if( std::strcmp( kind, "bdpt" ) == 0 ) {
			s += "bdpt_pel_rasterizer\n{\n\tmax_eye_depth 4\n\tmax_light_depth 4\n" + common + "}\n\n";
		} else {
			s += "vcm_pel_rasterizer\n{\n\tmax_eye_depth 4\n\tmax_light_depth 4\n\tmerge_radius 0.0\n"
				"\tvc_enabled true\n\tvm_enabled true\n" + common + "}\n\n";
		}
		return s;
	}

	struct Stat { double mean; double sd; bool ok; };

	Stat RenderN( const std::string& scene, const char* tag )
	{
		Stat st{ 0, 0, true };
		std::vector<double> v;
		for( int r = 0; r < kRepeats; r++ ) {
			const std::string path = WriteSceneToTempFile( scene.c_str(), tag );
			const ImageStats is = path.empty() ? ImageStats{} : RenderAndComputeStats( path.c_str() );
			if( !path.empty() ) std::remove( path.c_str() );
			if( !is.valid ) { st.ok = false; return st; }
			v.push_back( ( is.mean[0] + is.mean[1] + is.mean[2] ) / 3.0 );
		}
		for( double x : v ) st.mean += x;
		st.mean /= double( v.size() );
		double ss = 0;
		for( double x : v ) ss += ( x - st.mean ) * ( x - st.mean );
		st.sd = std::sqrt( ss / double( v.size() - 1 ) );
		return st;
	}

	//! Render `kind` n times on (layout, camera, env) and check its mean
	//! against `ref` within `tol` (tol < 0: a reference row, printed and
	//! only checked for a valid render).  Returns the measured stat.
	Stat Row( const char* label, const char* kind, const Layout& L, const bool pinhole,
		const bool env, const double ref, const double tol )
	{
		const std::string scene = std::string( "RISE ASCII SCENE 7\n" ) + Rasterizer( kind, env ) + Scene( L, pinhole );
		const Stat st = RenderN( scene, kind );
		char buf[512];
		if( tol < 0 ) {
			std::snprintf( buf, sizeof(buf), "Topology Y (DL-348): %s: %s mean %.6f (sd %.6f, n=%d salted)",
				label, kind, st.mean, st.sd, kRepeats );
			std::cout << "    " << buf << std::endl;
			Check( st.ok && st.mean > 0, buf );
			return st;
		}
		std::snprintf( buf, sizeof(buf),
			"Topology Y (DL-348): %s: %s mean %.6f (sd %.6f, n=%d salted) vs ref %.6f -> ratio %.4f, within %g%%",
			label, kind, st.mean, st.sd, kRepeats, ref, ref > 0 ? st.mean / ref : -1.0, 100.0 * tol );
		std::cout << "    " << buf << std::endl;
		Check( st.ok && ref > 0 && std::fabs( st.mean / ref - 1.0 ) <= tol, buf );
		return st;
	}
}

static void TestSeveralLuminariesY()
{
	using namespace TopologyY;
	std::cout << "Testing topology Y: several equal luminaries, ortho closed form + pinhole/env vs PT (DL-348)" << std::endl;

	// Closed-form cross-check of RectF against brute-force quadrature.
	{
		const int N = 600;
		const double x = 0.7, y = -0.3, x1 = -kOff - kHalf, x2 = -kOff + kHalf, y1 = -kHalf, y2 = kHalf;
		double q = 0;
		const double dx = ( x2 - x1 ) / N, dy = ( y2 - y1 ) / N;
		for( int j = 0; j < N; j++ ) for( int i = 0; i < N; i++ ) {
			const double u = x1 + ( i + 0.5 ) * dx - x, v = y1 + ( j + 0.5 ) * dy - y;
			const double d2 = u * u + v * v + kH * kH;
			q += kH * kH / ( kPi * d2 * d2 );
		}
		q *= dx * dy;
		const double a = RectF( x, y, x1, x2, y1, y2, kH );
		Check( std::fabs( a - q ) < 1e-6, "Topology Y: analytic form factor matches brute-force quadrature" );
	}

	// Post-fix every row reads 0.998-1.002 (per-render sd <= 0.4 %, so the
	// n = 4 mean's sd <= 0.2 %; PT itself sits 0.1 % under the closed form).
	const double tolCF = 0.006;
	const struct { const char* label; const Layout* L; } ortho[] = {
		{ "1 luminary, ortho vs closed form", &kOne },
		{ "2 luminaries, ortho vs closed form", &kTwo },
		{ "4 luminaries, ortho vs closed form", &kFour },
		{ "4 quads as ONE mesh luminary (control), ortho vs closed form", &kFourMesh },
	};
	for( const auto& r : ortho ) {
		const double cf = ClosedForm( *r.L );
		Row( r.label, "pt", *r.L, false, false, cf, tolCF );
		Row( r.label, "vcm", *r.L, false, false, cf, tolCF );
		Row( r.label, "bdpt", *r.L, false, false, cf, tolCF );
	}

	// Pinhole and env + 4 lights: PT is the reference.
	const Stat ptPin = Row( "4 luminaries, pinhole (PT reference)", "pt", kFour, true, false, 0.0, -1.0 );
	Row( "4 luminaries, pinhole vs PT", "vcm", kFour, true, false, ptPin.mean, tolCF );
	Row( "4 luminaries, pinhole vs PT", "bdpt", kFour, true, false, ptPin.mean, tolCF );
	const Stat ptEnv = Row( "env + 4 luminaries, ortho (PT reference)", "pt", kFour, false, true, 0.0, -1.0 );
	// The environment rows are noisier (BDPT per-render sd ~1.2 %), so a
	// 2 % band (> 4 sd of the n = 4 mean).  A NON-REGRESSION control, not
	// a red row: pre-fix it read VCM 0.9972 / BDPT 1.0062 -- the env takes
	// most of SampleLight's selection mass, so NEE to a quad is rare and
	// the eye-hit strategy carries the quads at weight ~1 under either
	// selection pmf.  It pins the (1 - envSelectProb) share the fix keeps.
	const double tolEnv = 0.02;
	Row( "env + 4 luminaries, ortho vs PT", "vcm", kFour, false, true, ptEnv.mean, tolEnv );
	Row( "env + 4 luminaries, ortho vs PT", "bdpt", kFour, false, true, ptEnv.mean, tolEnv );
}

int main( int argc, char** argv )
{
	std::cout << "=== VCMStrategyBalanceTest ===" << std::endl;


	// DL-365: an optional trailing numeric argument is a seed-base
	// override for the isolated filters below, matching
	// RefractiveRadianceScalingTest's `main` -- a different seed base
	// gives an independent SALTED sample of the filtered topology (see
	// the kSaltTag comment above RenderAndComputeStats), not just a
	// different libc rand() draw.
	auto ApplySeedOverride = [&]( int idx ) {
		if( argc > idx && argv[idx] ) {
			const long v = std::strtol( argv[idx], nullptr, 10 );
			if( v > 0 ) g_seedBase = (unsigned int)v;
		}
	};

 if(argc>=2 && std::strcmp(argv[1],"--f1-salts")==0) {
  ApplySeedOverride(2);TestFurnaceF1Salted();
  std::cout<<passCount<<" passed, "<<failCount<<" failed\n";return failCount?1:0;
 }
 if(argc>=2 && std::strcmp(argv[1],"--dl367-replay-only")==0) {
  if(argc!=2 || GlobalOptions().ReadInt("force_number_of_threads",0)!=1) {
   std::cerr<<"DL367 replay takes no arguments and requires force_number_of_threads 1\n";return 1;
  }
  TestRoughSSSReplay(8);std::cout<<passCount<<" passed, "<<failCount<<" failed\n";return failCount?1:0;
 }
 if(argc>=2 && std::strcmp(argv[1],"--u-salted-only")==0) {
  const unsigned seed=g_seedBase,index=g_renderIndex;
  TestRoughSSSEmptyContainerU();
  Check(g_seedBase==seed,"topology U preserves the caller's salt base");
  Check(g_renderIndex==index,"topology U preserves the caller's render salt index");
  std::cout<<passCount<<" passed, "<<failCount<<" failed\n";return failCount?1:0;
 }
 if(argc>=2 && (std::strcmp(argv[1],"--dl367-strict-only")==0 || std::strcmp(argv[1],"--dl367-unmatched-only")==0)) {
  Check(argc==2,"DL367 strict probe takes no additional arguments");
  if(argc!=2)return 1;
  const bool fixed=GlobalOptions().ReadBool("vcm_disable_progressive_radius",false);
  Check(fixed,"DL367 strict probe requires vcm_disable_progressive_radius true");
  if(!fixed)return 1;
  MeasureRoughSSSU(8192,false,.02,true,std::strcmp(argv[1],"--dl367-unmatched-only")==0);
  std::cout<<passCount<<" passed, "<<failCount<<" failed\n";return failCount?1:0;
 }
 // Diagnostic sweep only: disable the progressive schedule in RISE_OPTIONS_FILE
 // to hold this authored radius fixed across every pass/sample budget.
 if(argc>=2 && std::strcmp(argv[1],"--dl367-radius-only")==0) {
  Check(argc==4,"DL367 fixed-radius probe requires sample budget and radius arguments");
  if(argc!=4)return 1;
  char *sampleEnd=nullptr,*radiusEnd=nullptr;
  const unsigned long samples=std::strtoul(argv[2],&sampleEnd,10);
  const double radius=std::strtod(argv[3],&radiusEnd);
  const bool valid=samples>0 && samples<=std::numeric_limits<unsigned>::max()
   && sampleEnd && *sampleEnd=='\0' && radiusEnd && *radiusEnd=='\0'
   && std::isfinite(radius) && radius>0;
  Check(valid,"DL367 fixed-radius probe requires positive finite radius and sample budget");
  if(valid)MeasureRoughSSSU(static_cast<unsigned>(samples),false,radius);
  return failCount?1:0;
 }
 if(argc>=3 && std::strcmp(argv[1],"--dl367-only")==0) {MeasureRoughSSSU(std::strtoul(argv[2],nullptr,10));return failCount?1:0;}

	// DL-317: the delta-lit random-walk wall rows (D1/D2) alone.
	if( argc >= 2 && std::strcmp( argv[1], "--dl317-delta-only" ) == 0 ) {
		ApplySeedOverride( 2 );
		TestDeltaLitWallDL317();
		std::cout << "Passed: " << passCount << "\nFailed: " << failCount << std::endl;
		return failCount == 0 ? 0 : 1;
	}

	// DL-375: the directly-seen delta-lit random-walk sphere rows, plus
	// the D1/D2 wall rows they share a fixture with.
	if( argc >= 2 && std::strcmp( argv[1], "--dl375-only" ) == 0 ) {
		ApplySeedOverride( 2 );
		TestDeltaLitSphereDL375();
		TestDeltaLitWallDL317();
		std::cout << "Passed: " << passCount << "\nFailed: " << failCount << std::endl;
		return failCount == 0 ? 0 : 1;
	}

	// DL-380: the depth-capped by-path partition rows alone.
	if( argc >= 2 && std::strcmp( argv[1], "--dl380-only" ) == 0 ) {
		ApplySeedOverride( 2 );
		TestDepthCappedWallDL380();
		std::cout << "Passed: " << passCount << "\nFailed: " << failCount << std::endl;
		return failCount == 0 ? 0 : 1;
	}

	// DL-317: the SSS barrier rows (and topology U) alone.
	if( argc >= 2 && std::strcmp( argv[1], "--dl302-only" ) == 0 ) {
		TestCoatNormalOverNormalMap();
		std::cout << "Passed: " << passCount << "\nFailed: " << failCount << std::endl;
		return failCount == 0 ? 0 : 1;
	}

	if( argc >= 2 && std::strcmp( argv[1], "--dl317-only" ) == 0 ) {
		ApplySeedOverride( 2 );
		TestSSSBarrierDL317();
		TestRoughSSSEmptyContainerU();
		std::cout << "Passed: " << passCount << "\nFailed: " << failCount << std::endl;
		return failCount == 0 ? 0 : 1;
	}

	// DL-333 / DL-356: the smooth-diffusion sphere pins alone.
	if( argc >= 2 && std::strcmp( argv[1], "--dl333-only" ) == 0 ) {
		ApplySeedOverride( 2 );
		TestSmoothDiffusionSphereDL333();
		std::cout << "Passed: " << passCount << "\nFailed: " << failCount << std::endl;
		return failCount == 0 ? 0 : 1;
	}

	// DL-348: salted mean of a shipped scene file, `--scene-mean <path>
	// <n> <spp> <res>` -- film width/height and every `samples` line are
	// overridden; the scene's own outputs are replaced by the capture.
	// Used for the slice's shipped-scene before/after (opt-in only).
	if( argc >= 6 && std::strcmp( argv[1], "--scene-mean" ) == 0 ) {
		const int n = std::atoi( argv[3] ), spp = std::atoi( argv[4] ), res = std::atoi( argv[5] );
		std::ifstream ifs( argv[2] );
		if( !ifs.is_open() || n < 1 ) { std::cout << "cannot open " << argv[2] << std::endl; return 1; }
		std::string text, line;
		while( std::getline( ifs, line ) ) {
			const size_t k = line.find_first_not_of( " \t" );
			auto key = [&]( const char* w ) {
				const size_t L = std::strlen( w );
				return k != std::string::npos && line.compare( k, L, w ) == 0 &&
					( line.size() == k + L || line[k + L] == ' ' || line[k + L] == '\t' );
			};
			if( key( "samples" ) ) line = "\tsamples " + std::to_string( spp );
			else if( key( "width" ) || key( "height" ) ) line = line.substr( 0, k + ( key( "width" ) ? 5 : 6 ) ) + " " + std::to_string( res );
			text += line + "\n";
		}
		const std::string path = WriteSceneToTempFile( text.c_str(), "scene_mean" );
		std::vector<double> v;
		for( int i = 0; i < n; i++ ) {
			const ImageStats st = RenderAndComputeStats( path.c_str() );
			if( !st.valid ) { std::cout << "render failed" << std::endl; break; }
			v.push_back( ( st.mean[0] + st.mean[1] + st.mean[2] ) / 3.0 );
			std::printf( "scene-mean %s render %d: %.7f\n", argv[2], i, v.back() );
		}
		std::remove( path.c_str() );
		double m = 0, ss = 0;
		for( double x : v ) m += x;
		m /= std::max<size_t>( v.size(), 1 );
		for( double x : v ) ss += ( x - m ) * ( x - m );
		std::printf( "scene-mean %s: mean %.7f sd %.7f n=%zu\n", argv[2], m,
			v.size() > 1 ? std::sqrt( ss / ( v.size() - 1 ) ) : 0.0, v.size() );
		return 0;
	}

	// DL-348: topology Y alone.
	if( argc >= 2 && std::strcmp( argv[1], "--y-only" ) == 0 ) {
		ApplySeedOverride( 2 );
		TestSeveralLuminariesY();
		std::cout << "Passed: " << passCount << "\nFailed: " << failCount << std::endl;
		return failCount == 0 ? 0 : 1;
	}

	// DL-319: topology X alone.
	if( argc >= 2 && std::strcmp( argv[1], "--x-only" ) == 0 ) {
		ApplySeedOverride( 2 );
		TestAutoRadiusSmallPatchX();
		std::cout << "Passed: " << passCount << "\nFailed: " << failCount << std::endl;
		return failCount == 0 ? 0 : 1;
	}

	if( argc >= 2 && std::strcmp( argv[1], "--sss-only" ) == 0 ) {
		ApplySeedOverride( 2 );
		TestRoughSSSEmptyContainerU();
		std::cout << "Passed: " << passCount << "\nFailed: " << failCount << std::endl;
		return failCount == 0 ? 0 : 1;
	}

	// DL-294: topology W (narrow-fov splat) alone, salted as in the full run.
	if( argc >= 2 && std::strcmp( argv[1], "--narrow-fov-only" ) == 0 ) {
		ApplySeedOverride( 1 );
		TestNarrowFovSplatW();
		std::cout << "Passed: " << passCount << "\nFailed: " << failCount << std::endl;
		return failCount == 0 ? 0 : 1;
	}

	// DL-365 red-proof / fix isolation: run topology H alone.
	if( argc >= 2 && std::strcmp( argv[1], "--h-only" ) == 0 ) {
		ApplySeedOverride( 2 );
		TestSubmergedFloorAreaLight();
		std::cout << "Passed: " << passCount << "\nFailed: " << failCount << std::endl;
		return failCount == 0 ? 0 : 1;
	}

	// DL-365 audit isolation: topology I (the ~L1518 depth-5 pair).
	if( argc >= 2 && std::strcmp( argv[1], "--i-only" ) == 0 ) {
		ApplySeedOverride( 2 );
		TestSubmergedCeilingMISCombination();
		std::cout << "Passed: " << passCount << "\nFailed: " << failCount << std::endl;
		return failCount == 0 ? 0 : 1;
	}

	// DL-365 audit isolation: topology L (the ~L1741 depth-5 pair,
	// matched to PT's `max_diffuse_bounce`/`max_glossy_bounce` 5).
	if( argc >= 2 && std::strcmp( argv[1], "--l-only" ) == 0 ) {
		ApplySeedOverride( 2 );
		TestSchlickMultiLobe();
	TestCoatNormalOverNormalMap();
		std::cout << "Passed: " << passCount << "\nFailed: " << failCount << std::endl;
		return failCount == 0 ? 0 : 1;
	}

	// DL-365 audit isolation: topology AB, which shares topology L's
	// exact depth-5 rasterizer pair (kRasterizerPTSchlickL /
	// kRasterizerVCMSchlickL) with a polished_material substitution.
	if( argc >= 2 && std::strcmp( argv[1], "--ab-only" ) == 0 ) {
		ApplySeedOverride( 2 );
		TestPolishedAB();
		std::cout << "Passed: " << passCount << "\nFailed: " << failCount << std::endl;
		return failCount == 0 ? 0 : 1;
	}

	// DL-365 audit isolation: topology J (null-BSDF biospec skin),
	// which reuses kRasterizerVCM (the depth-3 pair) against a DIFFERENT
	// scene than A/B/C -- found flaky under salting, not in the brief's
	// original list.
	if( argc >= 2 && std::strcmp( argv[1], "--j-only" ) == 0 ) {
		ApplySeedOverride( 2 );
		TestNullBSDFMaterialContinuation();
		std::cout << "Passed: " << passCount << "\nFailed: " << failCount << std::endl;
		return failCount == 0 ? 0 : 1;
	}

	// DL-365 audit isolation: topologies A/B/C, the file's shared
	// depth-3 pair (~L524/917's first occurrence).
	if( argc >= 2 && std::strcmp( argv[1], "--abc-only" ) == 0 ) {
		ApplySeedOverride( 2 );
		TestDeltaOmniLight();
		TestMeshEmitterOnly();
		TestMixedLights();
		std::cout << "Passed: " << passCount << "\nFailed: " << failCount << std::endl;
		return failCount == 0 ? 0 : 1;
	}

	// DL-365 audit isolation: the thin-lens/ortho topologies sharing the
	// 512-spp depth-3 pair (~L917/926's second occurrence).
	if( argc >= 2 && std::strcmp( argv[1], "--thinlens-only" ) == 0 ) {
		ApplySeedOverride( 2 );
		TestThinLensStoppedDown();
		TestThinLensWideOpenDefocused();
		TestThinLensBladedAperture();
		TestOrthographicCamera();
		std::cout << "Passed: " << passCount << "\nFailed: " << failCount << std::endl;
		return failCount == 0 ? 0 : 1;
	}

	ApplySeedOverride( 1 );

	if (!TestCompositedStats()) return 1;

	TestDeltaOmniLight();
	TestMeshEmitterOnly();
	TestMixedLights();
	TestThinLensStoppedDown();
	TestThinLensWideOpenDefocused();
	TestThinLensBladedAperture();
	TestOrthographicCamera();
	TestSubmergedFloorAreaLight();
	TestSubmergedCeilingMISCombination();
	TestSchlickMultiLobe();
	TestCoatNormalOverNormalMap();
	TestPolishedAB();
	TestNullBSDFMaterialContinuation();
	TestRoughSSSEmptyContainerU();
	TestSSSBarrierDL317();
	TestDepthCappedWallDL380();
	TestSmoothDiffusionSphereDL333();
	TestNonfiniteCandidateRejected();
	TestNarrowFovSplatW();
	TestAutoRadiusSmallPatchX();
	TestSeveralLuminariesY();

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;

	return failCount == 0 ? 0 : 1;
}
