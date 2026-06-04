//////////////////////////////////////////////////////////////////////
//
//  AutoRasterizerTest.cpp - End-to-end check that the `auto_rasterizer`
//    dispatcher (Phase 1 of docs/AUTO_RASTERIZER_DESIGN.md) delegates
//    correctly to the concrete PT / BDPT / VCM rasterizers.
//
//    The dispatcher is a thin IRasterizer wrapper: at the first render-
//    time entry it picks an integrator (Phase 1: author pin, else PT)
//    and forwards every call to a concrete rasterizer built with that
//    integrator's canonical defaults.  "Delegates correctly" therefore
//    has two halves, and this test checks BOTH:
//
//      1. STRONG / direct — after the render, the wrapper reports the
//         integrator it actually resolved to (ResolvedIntegrator()).
//         This catches a mis-route even when the integrators agree on
//         the mean (PT/BDPT/VCM are all unbiased and converge to the
//         same image, so a radiance-only check could NOT tell "auto bdpt
//         secretly ran PT").
//
//      2. END-TO-END — the captured radiance of `auto_rasterizer`
//         pinned to X matches a bare `X_pel_rasterizer` on the same
//         scene, within sampling tolerance.  This confirms the wrapper
//         wired the delegate up identically (same caster, sampler,
//         filter, defaults) rather than e.g. dropping the outputs or the
//         progressive config on the floor.
//
//    Pins exercised: pt, bdpt, vcm, and `integrator auto` + omitted
//    (both -> PT).  Comparison is in-process raw radiance via a
//    capturing IRasterizerOutput (no EXR round-trip).
//
//    Tolerance: 8% relative on the mean (same rationale as
//    BDPTStrategyBalanceTest — multi-thread splat-accumulation order is
//    the only run-to-run wobble for a fixed integrator, well under 8%).
//    The ResolvedIntegrator() assertions are exact.
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
#include "../src/Library/Utilities/RasterizerDefaults.h"   // AutoIntegratorChoice
#include "../src/Library/Rendering/AutoRasterizer.h"        // ResolvedIntegrator()

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const std::string& testName )
{
	if( condition ) {
		passCount++;
	} else {
		failCount++;
		std::cout << "  FAIL: " << testName << std::endl;
	}
}

//////////////////////////////////////////////////////////////////////
// CapturingRasterizerOutput — store the final image into memory so the
// test reads raw linear radiance (no file I/O, no LDR encoding).  Same
// pattern as BDPTStrategyBalanceTest.
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

	virtual void OutputImage( const IRasterImage& pImage, const Rect*, const unsigned int ) override
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
	double p99[3];
	double max[3];
	bool   valid;
	AutoIntegratorChoice resolved;   // valid only when the rasterizer was an AutoRasterizer
	bool   wasAuto;                  // true if GetRasterizer() down-cast to AutoRasterizer
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
	s.resolved = AutoIntegratorChoice::Auto;
	s.wasAuto = false;
	if( cap.pixels.empty() ) {
		return s;
	}

	std::vector<double> ch[3];
	for( int c = 0; c < 3; c++ ) ch[c].reserve( cap.pixels.size() );

	// Compare the COMPOSITED-over-black radiance (base * coverage-alpha),
	// the quantity the sensor integrates, so PT's coverage-alpha vs
	// BDPT/VCM's opaque-alpha silhouette convention doesn't show a
	// spurious edge deficit.  No-op for full-coverage pixels (alpha==1).
	// See BDPTStrategyBalanceTest / INTEGRATOR_BUGFIX_FINDINGS.md Bug 2.
	for( const RISEColor& c : cap.pixels ) {
		const double cov = c.a;
		ch[0].push_back( c.base.r * cov );
		ch[1].push_back( c.base.g * cov );
		ch[2].push_back( c.base.b * cov );
	}

	for( int c = 0; c < 3; c++ ) {
		double sum = 0;
		for( double v : ch[c] ) sum += v;
		s.mean[c] = sum / double(ch[c].size());
		s.p99[c]  = Percentile( ch[c], 0.99 );
		s.max[c]  = ch[c].back();   // after sort
	}
	s.valid = true;
	return s;
}

static std::string WriteSceneToTempFile( const char* sceneText, const char* tag )
{
	char path[512];
	std::snprintf( path, sizeof(path),
		"/tmp/auto_rasterizer_%s_%d.RISEscene", tag, static_cast<int>(::getpid()) );

	std::ofstream ofs( path );
	if( !ofs.is_open() ) {
		return std::string();
	}
	ofs << sceneText;
	ofs.close();
	return std::string( path );
}

//////////////////////////////////////////////////////////////////////
// RenderAndComputeStats — load a scene, swap in the capturing output,
// render, and read back stats.  If the active rasterizer is an
// AutoRasterizer, also record which integrator it resolved to (valid
// only after the render, since resolution is lazy).
//////////////////////////////////////////////////////////////////////
static ImageStats RenderAndComputeStats( const char* scenePath )
{
	ImageStats result{};
	result.resolved = AutoIntegratorChoice::Auto;
	result.wasAuto = false;

	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) {
		return result;
	}

	if( !pJob->LoadAsciiScene( scenePath ) ) {
		safe_release( pJob );
		return result;
	}

	pJob->RemoveRasterizerOutputs();

	CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "test capture output" );
	pJob->GetRasterizer()->AddRasterizerOutput( pCap );

	// Borrow the active rasterizer pointer so we can interrogate the
	// resolved integrator after the render (the wrapper resolves lazily
	// at the first RasterizeScene).
	IRasterizer* pRast = pJob->GetRasterizer();

	const bool bRendered = pJob->Rasterize();
	if( !bRendered ) {
		safe_release( pCap );
		safe_release( pJob );
		return result;
	}

	result = ComputeStats( *pCap );

	AutoRasterizer* pAuto = dynamic_cast<AutoRasterizer*>( pRast );
	if( pAuto ) {
		result.wasAuto = true;
		result.resolved = pAuto->ResolvedIntegrator();
	}

	safe_release( pCap );
	safe_release( pJob );
	return result;
}

static bool ChannelsAgree( const double a[3], const double b[3], double relTol, double absFloor )
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
	          << " p99=(" << s.p99[0] << "," << s.p99[1] << "," << s.p99[2] << ")"
	          << " max=(" << s.max[0] << "," << s.max[1] << "," << s.max[2] << ")"
	          << std::endl;
}

//////////////////////////////////////////////////////////////////////
// Scene fragments.  One shared body; the rasterizer chunk varies.  Both
// an omni (delta) light and a mesh area emitter are present so BDPT and
// VCM have real light-subpath / merge transport to exercise (not just a
// degenerate delta-only scene).
//////////////////////////////////////////////////////////////////////
// The shader is emitted BEFORE the rasterizer chunk: Job::Set*Rasterizer
// resolves `defaultshader` (default "global") via the shader manager during
// the rasterizer chunk's Finalize, so the named shader must already be
// parsed.  (Geometry / lights / film are only needed at render time, so they
// can follow the rasterizer chunk.)
static const char* kShader =
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n";

static const char* kSceneCommon =
	"film\n{\n\twidth 32\n\theight 32\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 30.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_albedo\n\tcolor 0.5 0.5 0.5\n}\n\n"
	"lambertian_material\n{\n\tname mat_diffuse\n\treflectance pnt_albedo\n}\n\n"
	"clippedplane_geometry\n{\n\tname quad\n\tpta -1 -1 0\n\tptb 1 -1 0\n\tptc 1 1 0\n\tptd -1 1 0\n}\n\n"
	"standard_object\n{\n\tname obj_quad\n\tgeometry quad\n\tmaterial mat_diffuse\n}\n\n"
	"omni_light\n{\n\tname l_omni\n\tpower 4.0\n\tcolor 1.0 1.0 1.0\n\tposition 0.0 0.0 5.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1.0 1.0 1.0\n}\n\n"
	"lambertian_luminaire_material\n{\n\tname mat_emit\n\texitance pnt_emit\n\tscale 20.0\n\tmaterial none\n}\n\n"
	"clippedplane_geometry\n{\n\tname quad_emit\n\tpta -0.5 0.5 4.0\n\tptb 0.5 0.5 4.0\n\tptc 0.5 -0.5 4.0\n\tptd -0.5 -0.5 4.0\n}\n\n"
	"standard_object\n{\n\tname obj_emit\n\tgeometry quad_emit\n\tmaterial mat_emit\n}\n";

// oidn_denoise false: compare the raw integrator radiance directly (no
// denoiser pass), isolating the delegation from any OIDN device-init
// variability and keeping the test fast.  The flag is forwarded to the
// delegate identically on both sides, so equivalence still holds either way.
static const char* kRefPT =
	"pathtracing_pel_rasterizer\n{\n\tsamples 16\n\tpixel_filter box\n\toidn_denoise false\n}\n";
static const char* kRefBDPT =
	"bdpt_pel_rasterizer\n{\n\tsamples 16\n\tpixel_filter box\n\toidn_denoise false\n}\n";
static const char* kRefVCM =
	"vcm_pel_rasterizer\n{\n\tsamples 16\n\tpixel_filter box\n\toidn_denoise false\n}\n";

static const char* kAutoPT =
	"auto_rasterizer\n{\n\tintegrator pt\n\tsamples 16\n\tpixel_filter box\n\toidn_denoise false\n}\n";
static const char* kAutoBDPT =
	"auto_rasterizer\n{\n\tintegrator bdpt\n\tsamples 16\n\tpixel_filter box\n\toidn_denoise false\n}\n";
static const char* kAutoVCM =
	"auto_rasterizer\n{\n\tintegrator vcm\n\tsamples 16\n\tpixel_filter box\n\toidn_denoise false\n}\n";
static const char* kAutoAuto =
	"auto_rasterizer\n{\n\tintegrator auto\n\tsamples 16\n\tpixel_filter box\n\toidn_denoise false\n}\n";
static const char* kAutoUnset =
	"auto_rasterizer\n{\n\tsamples 16\n\tpixel_filter box\n\toidn_denoise false\n}\n";

static const double kMeanTol  = 0.08;
static const double kP99Tol   = 0.30;
static const double kMaxTol   = 1.50;
static const double kAbsFloor = 1e-6;

static ImageStats RenderVariant( const char* rasterChunk, const char* tag )
{
	const std::string scene = std::string("RISE ASCII SCENE 6\n") + kShader + rasterChunk + kSceneCommon;
	const std::string p = WriteSceneToTempFile( scene.c_str(), tag );
	if( p.empty() ) {
		ImageStats s{}; return s;
	}
	const ImageStats s = RenderAndComputeStats( p.c_str() );
	std::remove( p.c_str() );
	return s;
}

static const char* ChoiceName( AutoIntegratorChoice c )
{
	switch( c ) {
		case AutoIntegratorChoice::PT:   return "pt";
		case AutoIntegratorChoice::BDPT: return "bdpt";
		case AutoIntegratorChoice::VCM:  return "vcm";
		default:                         return "auto";
	}
}

//////////////////////////////////////////////////////////////////////
// One delegation check: render the auto-pinned scene + its concrete
// reference, assert (a) the wrapper resolved to the expected integrator
// and (b) the radiance matches the reference.
//////////////////////////////////////////////////////////////////////
static void CheckDelegation(
	const char* label,
	const char* autoChunk,
	const char* autoTag,
	const char* refChunk,
	const char* refTag,
	AutoIntegratorChoice expected )
{
	std::cout << "Testing auto_rasterizer delegation: " << label << std::endl;

	const ImageStats a   = RenderVariant( autoChunk, autoTag );
	const ImageStats ref = RenderVariant( refChunk,  refTag );

	PrintStats( "auto", a );
	PrintStats( "ref ", ref );

	Check( a.valid,   std::string("auto render produced output: ") + label );
	Check( ref.valid, std::string("ref render produced output: ")  + label );
	if( !a.valid || !ref.valid ) return;

	// (a) STRONG: the dispatcher resolved to the right delegate.
	Check( a.wasAuto, std::string("active rasterizer is an AutoRasterizer: ") + label );
	const bool resolvedOK = ( a.resolved == expected );
	Check( resolvedOK, std::string("resolved to '") + ChoiceName(expected)
		+ "' (got '" + ChoiceName(a.resolved) + "'): " + label );

	// Sanity: the render is non-trivially bright.
	const double brightness = ref.mean[0] + ref.mean[1] + ref.mean[2];
	Check( brightness > 1e-4, std::string("ref mean is non-zero: ") + label );

	// (b) END-TO-END: same integrator, same inputs -> same image.
	const bool meanMatch = ChannelsAgree( ref.mean, a.mean, kMeanTol, kAbsFloor );
	const bool p99Match  = ChannelsAgree( ref.p99,  a.p99,  kP99Tol,  kAbsFloor );
	const bool maxMatch  = ChannelsAgree( ref.max,  a.max,  kMaxTol,  kAbsFloor );

	Check( meanMatch, std::string("auto mean within ")
		+ std::to_string(int(kMeanTol*100)) + "% of ref: " + label );
	Check( p99Match, std::string("auto p99 within ")
		+ std::to_string(int(kP99Tol*100)) + "% of ref: " + label );
	Check( maxMatch, std::string("auto max within ")
		+ std::to_string(int(kMaxTol*100)) + "x of ref: " + label );
}

int main()
{
	std::cout << "=== AutoRasterizerTest ===" << std::endl;

	// Pinned delegations: auto(X) must resolve to X and match X_pel_rasterizer.
	CheckDelegation( "pin pt   -> pathtracing_pel", kAutoPT,   "auto_pt",   kRefPT,   "ref_pt",   AutoIntegratorChoice::PT );
	CheckDelegation( "pin bdpt -> bdpt_pel",        kAutoBDPT, "auto_bdpt", kRefBDPT, "ref_bdpt", AutoIntegratorChoice::BDPT );
	CheckDelegation( "pin vcm  -> vcm_pel",         kAutoVCM,  "auto_vcm",  kRefVCM,  "ref_vcm",  AutoIntegratorChoice::VCM );

	// `integrator auto` and an omitted `integrator` both fall back to PT
	// in Phase 1 (Tier-0 selection), so both must match pathtracing_pel.
	CheckDelegation( "integrator auto  -> PT", kAutoAuto,  "auto_auto",  kRefPT, "ref_pt2", AutoIntegratorChoice::PT );
	CheckDelegation( "integrator unset -> PT", kAutoUnset, "auto_unset", kRefPT, "ref_pt3", AutoIntegratorChoice::PT );

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;

	return failCount == 0 ? 0 : 1;
}
