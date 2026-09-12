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

	for( const RISEColor& c : cap.pixels ) {
		ch[0].push_back( c.base.r );
		ch[1].push_back( c.base.g );
		ch[2].push_back( c.base.b );
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
	"\toidn_denoise FALSE\n"
	"}\n"
	"\n"
	"file_rasterizeroutput\n"
	"{\n"
	"\tpattern /tmp/vcm_balance_pt_unused\n"
	"\ttype PNG\n"
	"\tbpp 8\n"
	"\tcolor_space sRGB\n"
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
	"\tpattern /tmp/vcm_balance_vcm_unused\n"
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
	"\toidn_denoise FALSE\n"
	"}\n"
	"\n"
	"file_rasterizeroutput\n"
	"{\n"
	"\tpattern /tmp/vcm_balance_pt_unused\n"
	"\ttype PNG\n"
	"\tbpp 8\n"
	"\tcolor_space sRGB\n"
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
	"\tpattern /tmp/vcm_balance_vcm_unused\n"
	"\ttype PNG\n"
	"\tbpp 8\n"
	"\tcolor_space sRGB\n"
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
// Measured on this row, VCM mean / PT mean:
//   both defects present (master):        0.00283 / 0.03661 = 0.077
//   guard only, dVCM untouched:         2.51e-05 / 0.03662 = 0.0007
//   dVCM = 0 only, splat unguarded:       0.03961 / 0.03662 = 1.082
//   both fixed:                           0.03681 / 0.03662 = 1.005
// (each of these is one draw from a run-to-run spread of a few
// percentage points; the band, not the figure, is the contract -- see
// the 5-run measurement below for this row's own quantified spread)
//
// Note the second row: the guard ALONE makes it worse, because with
// dVCM enormous the phantom splat was carrying almost all of what
// little energy VCM produced.  That is why the two land together.  The
// third row is the guard's own red-proof: the 8.2% excess is precisely
// the phantom splat, now at full weight -- against `kStrictTolerances`'s
// 8% mean band, that is only 0.2 PERCENTAGE POINTS outside the edge (A2
// P2-1, debt 28 review round 2): a red-proof that close to its own gate
// is barely a red-proof, since ordinary MC noise could push it back
// inside.  Measured whether it actually is noise: re-running THIS
// red-proof state (guard disabled, dVCM=0 fix kept) five times gave
// mean-relative-diff readings of 8.098%, 8.176%, 8.075%, 8.155%,
// 8.238% -- mean 8.148%, sample stddev ~0.065 pp (relative spread
// ~0.8%, i.e. comfortably < 1%).  The red-proof state is NOT noisy
// enough to occasionally read below 8% by chance; the 0.2 pp margin
// against `kStrictTolerances` was a real coincidence of where the
// bug's magnitude happened to land relative to the file's one shared
// band, not evidence the band itself is well-sized for this topology.
// Per that measurement this row gets its OWN tighter band,
// `kOrthoTolerances` (4% mean, same p99/max as strict): the shipped
// "both fixed" state (0.5% off) sits comfortably inside it, while the
// red-proof's ~8.1% now fails by roughly 4 percentage points instead
// of 0.2 -- an actual regression guard instead of an edge case.
//
// PT is unaffected by any of this, so PT-vs-VCM agreement is the
// invariant.
//////////////////////////////////////////////////////////////////////

// (A2 P2-1, debt 28 review round 2) Row-specific tighter band -- see the
// 5-run red-proof measurement above for the derivation.  p99/max stay at
// the strict values; only the mean band tightens from 8% to 4%.
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
// DIFFERENT scene's diagnostic probe (radius 0.03) at 1.047, and
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
	"\tpattern /tmp/vcm_balance_pt_submerged_unused\n"
	"\ttype PNG\n"
	"\tbpp 8\n"
	"\tcolor_space sRGB\n"
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
	"\tpattern /tmp/vcm_balance_vcm_submerged_unused\n"
	"\ttype PNG\n"
	"\tbpp 8\n"
	"\tcolor_space sRGB\n"
	"}\n";

static const Tolerances kSubmergedTolerances{ 0.08, 0.60, 4.00 };

static void TestSubmergedFloorAreaLight()
{
	RunTopologyTest( "submerged Lambertian floor, sphere emitter in air (eta^2, debt 30)",
		std::string( kSceneSubmergedFloor ), kSubmergedTolerances,
		kRasterizerPTSubmerged, kRasterizerVCMSubmerged );
}

int main()
{
	std::cout << "=== VCMStrategyBalanceTest ===" << std::endl;

	TestDeltaOmniLight();
	TestMeshEmitterOnly();
	TestMixedLights();
	TestThinLensStoppedDown();
	TestThinLensWideOpenDefocused();
	TestThinLensBladedAperture();
	TestOrthographicCamera();
	TestSubmergedFloorAreaLight();

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;

	return failCount == 0 ? 0 : 1;
}
