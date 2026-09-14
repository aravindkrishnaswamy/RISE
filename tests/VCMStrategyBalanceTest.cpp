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
//    pixelpel_rasterizer (PT, the trusted reference) and
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
#include <iostream>
#include <fstream>
#include <vector>
#include <cmath>
#include <limits>
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
		"/tmp/vcm_strategy_balance_%s_%d.RISEscene",
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

	if( !pJob->LoadAsciiSceneViaCst( scenePath ) ) {
		safe_release( pJob );
		return result;
	}

	pJob->RemoveRasterizerOutputs();
	CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "test capture output" );
	pJob->GetRasterizer()->AddRasterizerOutput( pCap );

	// Fresh libc seed per render; worker scheduling still makes repeats
	// non-bit-reproducible. Repeat averages must not reuse one seed.
	static unsigned renderIndex = 0;
	std::srand(1729u + renderIndex++);
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

// CAVEAT (reviewer, debt-26 sibling sweep, 2026-09-05): reference is the
// legacy pixelpel_rasterizer with a DefaultDirectLighting-only chain,
// valid ONLY while every topology is single-bounce direct lighting on a
// flat quad (no scattered ray can carry energy); any topology with a
// transmissive / reflective material or a second SCATTERING surface must switch the
// reference to pathtracing_pel_rasterizer first -- CLOTH_FABRIC_DESIGN.md
// 15 debt 26.
static const char* kRasterizerPT =
	"standard_shader\n"
	"{\n"
	"\tname global\n"
	"\tshaderop DefaultDirectLighting\n"
	"}\n"
	"\n"
	"pixelpel_rasterizer\n"
	"{\n"
	"\tmax_recursion 2\n"
	"\tsamples 32\n"
	"\tlum_samples 1\n"
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
		+ std::to_string(int(tol.meanTol*100)) + "% of PT: " + topologyName ).c_str() );
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
// The reference stays this file's legacy `pixelpel_rasterizer`: both
// topologies are single-bounce direct lighting on one flat quad with a
// non-scattering luminaire, so the direct-lighting-only shader chain
// carries all the energy (the caveat above `kRasterizerPT` is
// satisfied), and a thin lens changes only which ray each film sample
// generates -- which the legacy rasterizer draws from
// `ICamera::GenerateRay` exactly as the modern one does, so PT here has
// the SAME defocus blur as VCM.  CROSS-CHECKED against
// `pathtracing_pel_rasterizer` at the same 512 spp: focused 0.0462
// legacy vs 0.0452 modern (+2.2%), defocused 0.0440 vs 0.0449 (-2.0%),
// both comfortably inside the 8% band.
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
static const char* kRasterizerPT512 =
	"standard_shader\n"
	"{\n"
	"\tname global\n"
	"\tshaderop DefaultDirectLighting\n"
	"}\n"
	"\n"
	"pixelpel_rasterizer\n"
	"{\n"
	"\tmax_recursion 2\n"
	"\tsamples 512\n"
	"\tlum_samples 1\n"
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
// kRasterizerPT warns about (a transmissive material in the scene), so
// the legacy pixelpel + DefaultDirectLighting reference is invalid
// here and a real path tracer is required.
//
// TOLERANCES.  The mean stays at the strict 8%.  p99 and max are
// loosened to 60% / 4x: with only BSDF sampling able to find a
// 0.08-radius emitter through a delta interface, both integrators
// carry genuine specular fireflies whose per-pixel tails do not agree
// at any practical sample count -- measured p99 spread across runs is
// tens of percent while the MEAN, which is the quantity the eta^2 bug
// moves, is stable to ~1%.  Loosening the tail bands rather than
// dropping them keeps a catastrophic tail regression visible.
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
	"\tmax_eye_depth 5\n"
	"\tmax_light_depth 5\n"
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

static const Tolerances kSubmergedTolerances{ 0.08, 0.60, 4.00 };

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
	"\tmax_eye_depth 5\n"
	"\tmax_light_depth 5\n"
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

static void TestSubmergedCeilingMISCombination()
{
	RunTopologyTest( "air ceiling patch, floor + emitter both submerged (eta^2 MIS combination, debt 30 review round 2)",
		std::string( kSceneSubmergedCeiling ), kCeilingTolerances,
		kRasterizerPTCeiling, kRasterizerVCMCeiling );
}

int main()
{
	std::cout << "=== VCMStrategyBalanceTest ===" << std::endl;

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
	TestNonfiniteCandidateRejected();

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;

	return failCount == 0 ? 0 : 1;
}
