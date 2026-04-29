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
	"pinhole_camera\n"
	"{\n"
	"\tlocation 0 0 3.5\n"
	"\tlookat 0 0 0\n"
	"\tup 0 1 0\n"
	"\twidth 32\n"
	"\theight 32\n"
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
	const Tolerances& tol = kStrictTolerances )
{
	std::cout << "Testing PT-vs-VCM: " << topologyName << std::endl;

	const std::string ptScene  = std::string("RISE ASCII SCENE 5\n") + kRasterizerPT  + sceneCommonBlock;
	const std::string vcmScene = std::string("RISE ASCII SCENE 5\n") + kRasterizerVCM + sceneCommonBlock;

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
//////////////////////////////////////////////////////////////////////
static void TestMixedLights()
{
	RunTopologyTest( "mixed delta+mesh lights",
		std::string( kSceneCommon ) + kLightOmni + kLightMesh );
}

int main()
{
	std::cout << "=== VCMStrategyBalanceTest ===" << std::endl;

	TestDeltaOmniLight();
	TestMeshEmitterOnly();
	TestMixedLights();

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;

	return failCount == 0 ? 0 : 1;
}
