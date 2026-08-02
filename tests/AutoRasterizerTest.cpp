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
#include <sstream>
#include <vector>
#include <cmath>
#include <cctype>
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
#include "../src/Library/Interfaces/IProgressCallback.h"
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
	std::string resolvedName;        // ResolvedIntegratorName() — the cross-UI string API
	bool   queryApiOK;               // IsAutoDispatcher() && ResolveReason() non-empty
	unsigned int probeRenders;       // LastProbeRenders(); G10 must short-circuit Tier 2
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
static ImageStats RenderAndComputeStats(
	const char* scenePath,
	const bool addEmissiveHomogeneous = false )
{
	ImageStats result{};
	result.resolved = AutoIntegratorChoice::Auto;
	result.wasAuto = false;

	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) {
		return result;
	}

	if( !pJob->LoadAsciiSceneViaCst( scenePath ) ) {
		safe_release( pJob );
		return result;
	}

	if( addEmissiveHomogeneous ) {
		const double zero[3] = { 0.0, 0.0, 0.0 };
		const double emission[3] = { 1.0, 0.5, 0.25 };
		if( !pJob->AddHomogeneousMediumSpectral(
			"auto_test_emissive", zero, zero, emission, "", "", "isotropic", 0.0 ) ||
			!pJob->SetGlobalMedium( "auto_test_emissive" ) ) {
			safe_release( pJob );
			return result;
		}
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
		result.resolvedName = pAuto->ResolvedIntegratorName();
		result.queryApiOK = pAuto->IsAutoDispatcher() && pAuto->ResolveReason()[0] != '\0';
		result.probeRenders = pAuto->LastProbeRenders();
	}

	safe_release( pCap );
	safe_release( pJob );
	return result;
}

static ImageStats RenderFireReferencePreview(
	const char* scenePath,
	const char* rasterizerKeyword )
{
	ImageStats result{};
	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv(&pJob) || !pJob ) return result;
	if( !pJob->LoadAsciiSceneViaCst(scenePath) ||
		!pJob->SetFilm(32,48,1.0) ||
		!pJob->SetRasterizerParameter(rasterizerKeyword,"samples","256") ||
		!pJob->SetRasterizerParameter(rasterizerKeyword,"oidn_denoise","false") ) {
		safe_release(pJob);
		return result;
	}

	pJob->RemoveRasterizerOutputs();
	CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew(pCap,__FILE__,__LINE__,"fire preview capture output");
	pJob->GetRasterizer()->AddRasterizerOutput(pCap);
	if( pJob->Rasterize() ) result = ComputeStats(*pCap);
	safe_release(pCap);
	safe_release(pJob);
	return result;
}

static void TestFirePelPreviewDivergence()
{
	const ImageStats pel = RenderFireReferencePreview(
		"scenes/Tests/Volumes/pt_fire_phase_a_pel_preview.RISEscene",
		"pathtracing_pel_rasterizer" );
	const ImageStats spectral = RenderFireReferencePreview(
		"scenes/Tests/Volumes/pt_fire_phase_a_spectral.RISEscene",
		"pathtracing_spectral_rasterizer" );
	Check( pel.valid && spectral.valid,
		"Phase-A Pel and spectral reference scenes both render in-process" );
	if( !pel.valid || !spectral.valid ) return;

	double divergence[3] = {0.0,0.0,0.0};
	for( unsigned int channel = 0; channel < 3u; ++channel ) {
		const double scale = std::max(std::fabs(spectral.mean[channel]),1e-12);
		divergence[channel] = std::fabs(pel.mean[channel]-spectral.mean[channel])/scale;
	}
	std::cout << "  Phase-A Pel/spectral means Pel=(" << pel.mean[0] << ","
		<< pel.mean[1] << "," << pel.mean[2] << ") spectral=("
		<< spectral.mean[0] << "," << spectral.mean[1] << ","
		<< spectral.mean[2] << ") divergence=(" << divergence[0] << ","
		<< divergence[1] << "," << divergence[2] << ")" << std::endl;

	// Recorded on the 32x48, 256-spp Phase-A reference across repeated
	// multi-thread runs: (1.13%,0.73%,0.08%) and (0.16%,2.51%,0.20%).
	// The 5% cap leaves measured accumulation-order headroom while remaining
	// a useful gross-regression guard.  These are consistency-only bounds,
	// deliberately not an absolute Pel radiance or fidelity requirement.
	Check( divergence[0] < 0.05 && divergence[1] < 0.05 &&
		divergence[2] < 0.05,
		"Pel preview channel-mean divergence stays inside the recorded bound" );
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
static const char* kAutoProbe =
	"auto_rasterizer\n{\n\tintegrator auto\n\tprobe true\n\tsamples 16\n\tpixel_filter box\n\toidn_denoise false\n}\n";

// --- Phase-1b spectral fixtures.  Same shared body (kSceneCommon, RGB
// painters JH-uplifted to spectra); only the rasterizer-chunk family changes
// to *_spectral_ / auto_spectral_rasterizer.  Both sides of each pair use the
// SAME spectral params so the auto-pinned delegate is the same estimator as the
// bare *_spectral_ ref (delegation correctness is proven exactly by the Pel-VCM
// pair, which matches to +0.05% at 16 spp — no chromatic noise on a grey scene).
// The spectral equivalence pairs use samples 64 / spectral_samples 4 (16x
// effective) rather than 16 / 1: hero-wavelength chromatic noise on the grey
// scene gave flaky ~10% (VCM) / ~8% (BDPT) per-channel auto-vs-ref deltas at
// 16 / 1 — two independent stochastic renders of the same spectral integrator.
// Converging that noise below the 8% mean gate keeps the tolerance honest
// instead of widening it; the cost is trivial on the 32x32 fixture.  PT is the
// least noisy and passed at 16 / 1, but is bumped too for a uniform spectral
// block.  kAutoSpecAuto stays at 16 / 1 — it drives a static-routing CHOICE
// assertion, not a pixel-equivalence check.
static const char* kRefPTSpec =
	"pathtracing_spectral_rasterizer\n{\n\tsamples 64\n\tnum_wavelengths 8\n\tspectral_samples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n";
static const char* kRefBDPTSpec =
	"bdpt_spectral_rasterizer\n{\n\tsamples 64\n\tnum_wavelengths 8\n\tspectral_samples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n";
static const char* kRefVCMSpec =
	"vcm_spectral_rasterizer\n{\n\tsamples 64\n\tnum_wavelengths 8\n\tspectral_samples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n";
static const char* kAutoSpecPT =
	"auto_spectral_rasterizer\n{\n\tintegrator pt\n\tsamples 64\n\tnum_wavelengths 8\n\tspectral_samples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n";
static const char* kAutoSpecBDPT =
	"auto_spectral_rasterizer\n{\n\tintegrator bdpt\n\tsamples 64\n\tnum_wavelengths 8\n\tspectral_samples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n";
static const char* kAutoSpecVCM =
	"auto_spectral_rasterizer\n{\n\tintegrator vcm\n\tsamples 64\n\tnum_wavelengths 8\n\tspectral_samples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n";
static const char* kAutoSpecAuto =
	"auto_spectral_rasterizer\n{\n\tintegrator auto\n\tsamples 16\n\tnum_wavelengths 8\n\tspectral_samples 1\n\tpixel_filter box\n\toidn_denoise false\n}\n";

// --------------------------------------------------------------------
// Phase-2 fixtures.  Raw-string scene fragments (real tabs / newlines)
// composed into bodies below to toggle the two routing signals the
// static tier reads: "transmissive material present" and "positional
// (point/spot/omni) light present".
// --------------------------------------------------------------------

// A clear glass sphere -> a transmissive (CouldLightPassThrough) material.
// Append to a body to add the "dielectric present" signal.
static const char* kGlassSphere = R"SCENE(
uniformcolor_painter
{
	name pnt_glasstau
	color 1.0 1.0 1.0
}
dielectric_material
{
	name mat_glass
	tau 0.9999
	ior 1.5
}
sphere_geometry
{
	name glassball
	radius 0.4
}
standard_object
{
	name obj_glass
	geometry glassball
	position 0.0 0.0 0.5
	material mat_glass
}
)SCENE";

// Same diffuse receiver + mesh AREA emitter as kSceneCommon, but with NO
// omni light -> lit purely by the area emitter (no positional delta
// light).  A complete renderable body on its own.
static const char* kSceneAreaLitOnly = R"SCENE(
film
{
	width 32
	height 32
}
pinhole_camera
{
	location 0 0 3.5
	lookat 0 0 0
	up 0 1 0
	fov 30.0
}
uniformcolor_painter
{
	name pnt_albedo
	color 0.5 0.5 0.5
}
lambertian_material
{
	name mat_diffuse
	reflectance pnt_albedo
}
clippedplane_geometry
{
	name quad
	pta -1 -1 0
	ptb 1 -1 0
	ptc 1 1 0
	ptd -1 1 0
}
standard_object
{
	name obj_quad
	geometry quad
	material mat_diffuse
}
uniformcolor_painter
{
	name pnt_emit
	color 1.0 1.0 1.0
}
lambertian_luminaire_material
{
	name mat_emit
	exitance pnt_emit
	scale 20.0
	material none
}
clippedplane_geometry
{
	name quad_emit
	pta -0.5 0.5 4.0
	ptb 0.5 0.5 4.0
	ptc 0.5 -0.5 4.0
	ptd -0.5 -0.5 4.0
}
standard_object
{
	name obj_emit
	geometry quad_emit
	material mat_emit
}
)SCENE";

// Non-emissive heterogeneous medium definition.  The tests attach it once as
// the global medium and once to an object interior so both scan paths are
// independently observable.
static const char* kHeterogeneousMediumDefinition = R"SCENE(
painter_heterogeneous_medium
{
	name auto_test_heterogeneous
	absorption 0 0 0
	scattering 0 0 0
	emission 0 0 0
	phase isotropic
	density_painter pnt_albedo
	resolution 2
	color_to_scalar luminance
	bbox_min -1 -1 -1
	bbox_max 1 1 1
}
)SCENE";

static const char* kGlobalHeterogeneousMedium = R"SCENE(
global_medium
{
	medium auto_test_heterogeneous
}
)SCENE";

static const char* kGlassSphereWithInteriorMedium = R"SCENE(
uniformcolor_painter
{
	name pnt_interior_glasstau
	color 1.0 1.0 1.0
}
dielectric_material
{
	name mat_interior_glass
	tau 0.9999
	ior 1.5
}
sphere_geometry
{
	name interior_glassball
	radius 0.4
}
standard_object
{
	name obj_interior_glass
	geometry interior_glassball
	position 0.0 0.0 0.5
	material mat_interior_glass
	interior_medium auto_test_heterogeneous
}
)SCENE";

static const double kMeanTol  = 0.08;
static const double kP99Tol   = 0.30;
static const double kMaxTol   = 1.50;
static const double kAbsFloor = 1e-6;

static ImageStats RenderVariant( const char* rasterChunk, const char* tag )
{
	const std::string scene = std::string("RISE ASCII SCENE 7\n") + kShader + rasterChunk + kSceneCommon;
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
	Check( a.queryApiOK && a.resolvedName == ChoiceName(expected),
		std::string("query API (name+reason) consistent: ") + label );
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

//////////////////////////////////////////////////////////////////////
// Phase-2 static-routing harness: render `body` under an auto_rasterizer
// (integrator auto/unset) and assert the dispatcher RESOLVED to the
// expected integrator.  Asserts the routing decision, not pixels — the
// static tier's job is the choice (PT/BDPT/VCM all converge to the same
// image, so a radiance check could not tell a mis-route apart).
//////////////////////////////////////////////////////////////////////
static ImageStats RenderSceneBody(
	const char* rasterChunk,
	const std::string& body,
	const char* tag,
	const bool addEmissiveHomogeneous = false )
{
	const std::string scene = std::string("RISE ASCII SCENE 7\n") + kShader + rasterChunk + body;
	const std::string p = WriteSceneToTempFile( scene.c_str(), tag );
	if( p.empty() ) {
		ImageStats s{}; return s;
	}
	const ImageStats s = RenderAndComputeStats( p.c_str(), addEmissiveHomogeneous );
	std::remove( p.c_str() );
	return s;
}

static void CheckStaticRoute(
	const char* label,
	const char* autoChunk,
	const std::string& body,
	const char* tag,
	AutoIntegratorChoice expected,
	const bool addEmissiveHomogeneous = false,
	const bool expectNoProbeRenders = false )
{
	std::cout << "Testing auto_rasterizer static routing: " << label << std::endl;
	const ImageStats a = RenderSceneBody(
		autoChunk, body, tag, addEmissiveHomogeneous );
	PrintStats( "auto", a );
	Check( a.valid, std::string("render produced output: ") + label );
	if( !a.valid ) return;
	Check( a.wasAuto, std::string("active rasterizer is an AutoRasterizer: ") + label );
	Check( a.queryApiOK && a.resolvedName == ChoiceName(expected),
		std::string("query API (name+reason) consistent: ") + label );
	const bool ok = ( a.resolved == expected );
	Check( ok, std::string("resolved to '") + ChoiceName(expected)
		+ "' (got '" + ChoiceName(a.resolved) + "'): " + label );
	if( expectNoProbeRenders ) {
		Check( a.probeRenders == 0u,
			std::string("G10 short-circuits Tier-2 probe renders: ") + label );
	}
}

//////////////////////////////////////////////////////////////////////
// Phase-4 probe routing harness.  The Tier-2 render-time probe must be
// exercised on REAL transport (the static blind spots gi_spheres /
// ggx_showcase are byte-identical in cheap static signals — only a probe
// render can tell them apart; a synthetic Cornell box is too weak to make
// BDPT win σ²·T).  So these tests take a real corpus scene, swap its
// rasterizer chunk for `auto_rasterizer { probe true }`, shrink the film
// for speed (σ²·T + median-lum are per-pixel statistics, so the routing
// decision survives downscaling — verified gi_spheres BDPT 44× @320px →
// 483× @128px), and assert the resolved integrator.
//
// The probe is gated on production spp >= auto_probe_activation_spp; the
// test lowers that to 1 (and sets probe spp/scale low) via a temp options
// file in main().  Phase-1/2 scenes carry no `probe` line (default off),
// so they are untouched by the lowered activation gate.
//////////////////////////////////////////////////////////////////////

static std::string ReadFileToString( const char* path )
{
	std::ifstream ifs( path, std::ios::binary );
	if( !ifs.is_open() ) {
		return std::string();
	}
	std::ostringstream ss;
	ss << ifs.rdbuf();
	return ss.str();
}

static std::string Trimmed( const std::string& s )
{
	size_t a = 0, b = s.size();
	while( a < b && std::isspace( static_cast<unsigned char>( s[a] ) ) ) ++a;
	while( b > a && std::isspace( static_cast<unsigned char>( s[b-1] ) ) ) --b;
	return s.substr( a, b - a );
}

// Split into lines (without trailing '\n'); remembers whether each ended
// with a newline is irrelevant — we re-join with '\n'.
static std::vector<std::string> SplitLines( const std::string& text )
{
	std::vector<std::string> lines;
	std::string cur;
	for( char c : text ) {
		if( c == '\n' ) { lines.push_back( cur ); cur.clear(); }
		else if( c != '\r' ) { cur.push_back( c ); }
	}
	lines.push_back( cur );
	return lines;
}

// Find the chunk whose header line (trimmed) satisfies `match`, returning
// [headerIdx, closeBraceIdx].  RISE ascii chunks don't nest and braces sit
// on their own lines (CLAUDE.md), so we find the header, the next '{' line,
// then brace-count to the matching '}'.  Returns false if not found.
static bool FindChunk( const std::vector<std::string>& lines,
	bool (*match)( const std::string& ), size_t& outStart, size_t& outEnd )
{
	for( size_t i = 0; i < lines.size(); ++i ) {
		if( !match( Trimmed( lines[i] ) ) ) continue;
		// find the opening brace on a subsequent line
		size_t j = i + 1;
		while( j < lines.size() && Trimmed( lines[j] ) != "{" ) {
			// only blanks / comments may precede the brace
			const std::string t = Trimmed( lines[j] );
			if( !t.empty() && t[0] != '#' ) { j = lines.size(); break; }
			++j;
		}
		if( j >= lines.size() ) continue;
		int depth = 0;
		for( size_t k = j; k < lines.size(); ++k ) {
			const std::string t = Trimmed( lines[k] );
			if( t == "{" ) ++depth;
			else if( t == "}" ) { if( --depth == 0 ) { outStart = i; outEnd = k; return true; } }
		}
	}
	return false;
}

static bool IsRasterizerHeader( const std::string& t )
{
	// A bare chunk header that is exactly a *_rasterizer token.
	if( t.size() < 11 ) return false;
	if( t.find( ' ' ) != std::string::npos || t.find( '\t' ) != std::string::npos ) return false;
	const std::string suffix = "_rasterizer";
	return t.size() >= suffix.size() &&
	       t.compare( t.size() - suffix.size(), suffix.size(), suffix ) == 0;
}

static bool IsFilmHeader( const std::string& t ) { return t == "film"; }

// Read a corpus scene and return a variant whose rasterizer chunk is an
// `auto_rasterizer { probe true }` and whose film is `dim x dim`.  Empty
// string on failure (scene missing / no rasterizer chunk).
static std::string MakeAutoProbeScene( const char* corpusPath, unsigned int samples, unsigned int dim )
{
	const std::string text = ReadFileToString( corpusPath );
	if( text.empty() ) {
		return std::string();
	}
	std::vector<std::string> lines = SplitLines( text );

	size_t rs, re;
	if( !FindChunk( lines, IsRasterizerHeader, rs, re ) ) {
		return std::string();
	}

	char autoChunk[256];
	std::snprintf( autoChunk, sizeof(autoChunk),
		"auto_rasterizer\n{\n\tintegrator auto\n\tprobe true\n\tsamples %u\n\tpixel_filter box\n\toidn_denoise false\n}",
		samples );

	// Replace the rasterizer chunk lines [rs, re] with the auto chunk.
	std::vector<std::string> out;
	for( size_t i = 0; i < lines.size(); ++i ) {
		if( i == rs ) { out.push_back( autoChunk ); i = re; continue; }
		out.push_back( lines[i] );
	}

	// Shrink the film chunk (if present) to dim x dim for probe + render speed.
	size_t fs, fe;
	if( FindChunk( out, IsFilmHeader, fs, fe ) ) {
		char filmChunk[128];
		std::snprintf( filmChunk, sizeof(filmChunk),
			"film\n{\n\twidth %u\n\theight %u\n}", dim, dim );
		std::vector<std::string> out2;
		for( size_t i = 0; i < out.size(); ++i ) {
			if( i == fs ) { out2.push_back( filmChunk ); i = fe; continue; }
			out2.push_back( out[i] );
		}
		out.swap( out2 );
	}

	std::string joined;
	for( size_t i = 0; i < out.size(); ++i ) { joined += out[i]; joined.push_back( '\n' ); }
	return joined;
}

// Render `corpusPath` through the probe and assert it resolves to `expected`.
static void CheckProbeRoute(
	const char* label, const char* corpusPath, const char* tag,
	AutoIntegratorChoice expected )
{
	std::cout << "Testing auto_rasterizer probe routing: " << label << std::endl;
	// samples >= 2: Job::GetSamplingAndFilterElements only creates a pixel
	// sampler when numPixelSamples > 1, and the probe needs a non-null
	// sampler to clone at probe spp.  (Production samples don't affect the
	// probe decision — the probe renders at its own auto_probe_spp.)
	const std::string scene = MakeAutoProbeScene( corpusPath, /*samples*/ 4, /*dim*/ 128 );
	if( scene.empty() ) {
		Check( false, std::string("corpus scene readable + has a rasterizer chunk: ") + label
			+ " (" + corpusPath + ")" );
		return;
	}
	const std::string p = WriteSceneToTempFile( scene.c_str(), tag );
	if( p.empty() ) { Check( false, std::string("temp scene written: ") + label ); return; }
	const ImageStats a = RenderAndComputeStats( p.c_str() );
	std::remove( p.c_str() );

	PrintStats( "probe", a );
	Check( a.valid,   std::string("probe render produced output: ") + label );
	if( !a.valid ) return;
	Check( a.wasAuto, std::string("active rasterizer is an AutoRasterizer: ") + label );
	Check( a.queryApiOK && a.resolvedName == ChoiceName(expected),
		std::string("query API (name+reason) consistent: ") + label );
	const bool ok = ( a.resolved == expected );
	Check( ok, std::string("probe resolved to '") + ChoiceName(expected)
		+ "' (got '" + ChoiceName(a.resolved) + "'): " + label );
}

//////////////////////////////////////////////////////////////////////
// CheckProbeRouteNotVCM -- Pel-domain sibling of
// CheckSpectralProbeRouteNotVCM (see its header), for a probe case whose
// documented invariant is "a VCM gate correctly kept VCM OUT" and whose
// PT-vs-BDPT fallthrough tail is the design-acknowledged noise band.
//
// Used for jewel_vault (the §6.2 transport-reach over-fire lock).  Its
// σ²·T ratio was measured over 50 runs at 0.07..1.07 with occasional
// spikes past τ_bdpt=1.35 (a genuine BDPT resolution seen in the wild) —
// because jewel_vault is documented as "the NOISIEST PT" at probe spp
// (AutoRasterizer.cpp §6.2) and the σ² signal is multi-thread float-
// accumulation nondeterminism (no seed to pin).  The stable, meaningful
// lock is the WINSORIZED reach gate keeping VCM out (raw VCM mean spiked
// past 1.50 ~2.6% of the time pre-fix); the PT-vs-BDPT tail is not a
// stable decision the probe promises at spp 4.  Contrast the stable
// strict cases kept on CheckProbeRoute: gi_spheres (ratio 11..32, big
// BDPT win), ggx_showcase (0.11..0.38), env_only (0.00), corridor
// (0.00..0.12) — all far from τ, none flaky.
static void CheckProbeRouteNotVCM(
	const char* label, const char* corpusPath, const char* tag )
{
	std::cout << "Testing auto_rasterizer probe routing: " << label << std::endl;
	const std::string scene = MakeAutoProbeScene( corpusPath, /*samples*/ 4, /*dim*/ 128 );
	if( scene.empty() ) {
		Check( false, std::string("corpus scene readable + has a rasterizer chunk: ") + label
			+ " (" + corpusPath + ")" );
		return;
	}
	const std::string p = WriteSceneToTempFile( scene.c_str(), tag );
	if( p.empty() ) { Check( false, std::string("temp scene written: ") + label ); return; }
	const ImageStats a = RenderAndComputeStats( p.c_str() );
	std::remove( p.c_str() );

	PrintStats( "probe", a );
	Check( a.valid,   std::string("probe render produced output: ") + label );
	if( !a.valid ) return;
	Check( a.wasAuto, std::string("active rasterizer is an AutoRasterizer: ") + label );
	Check( a.queryApiOK && a.resolvedName == ChoiceName(a.resolved),
		std::string("query API (name+reason) consistent with resolved choice: ") + label );
	Check( a.resolved != AutoIntegratorChoice::VCM,
		std::string("probe kept VCM out (reach gate rejects the over-fire; resolved '")
		+ ChoiceName(a.resolved) + "'): " + label );
}

//////////////////////////////////////////////////////////////////////
// Phase-1b spectral probe harness.  Spectral sibling of MakeAutoProbeScene:
// swap the rasterizer chunk for an `auto_spectral_rasterizer { probe true }`
// carrying the scene's spectral-core params, strip the legacy caustic
// photon-map chunks (the pure spectral integrators the dispatcher delegates
// to bypass the shader-op chain, so the photon map is dead weight + 30x probe
// cost), and shrink the film.
//////////////////////////////////////////////////////////////////////
static bool IsPhotonMapChunk( const std::string& t )
{
	return t == "caustic_spectral_photonmap" || t == "caustic_spectral_gather"
	    || t == "caustic_pel_photonmap"      || t == "caustic_pel_gather";
}

static std::string MakeAutoSpectralProbeScene(
	const char* corpusPath, const char* specParams, unsigned int samples, unsigned int dim )
{
	const std::string text = ReadFileToString( corpusPath );
	if( text.empty() ) {
		return std::string();
	}
	std::vector<std::string> lines = SplitLines( text );

	size_t rs, re;
	if( !FindChunk( lines, IsRasterizerHeader, rs, re ) ) {
		return std::string();
	}

	char autoChunk[512];
	std::snprintf( autoChunk, sizeof(autoChunk),
		"auto_spectral_rasterizer\n{\n\tintegrator auto\n\tprobe true\n\tsamples %u\n%s\tpixel_filter box\n\toidn_denoise false\n}",
		samples, specParams );

	std::vector<std::string> out;
	for( size_t i = 0; i < lines.size(); ++i ) {
		if( i == rs ) { out.push_back( autoChunk ); i = re; continue; }
		out.push_back( lines[i] );
	}

	// Strip the legacy photon-map chunks (re-find after each erase since
	// indices shift).  The pure spectral integrators never consume them.
	{
		size_t ps, pe;
		while( FindChunk( out, IsPhotonMapChunk, ps, pe ) ) {
			out.erase( out.begin() + ps, out.begin() + pe + 1 );
		}
	}

	size_t fs, fe;
	if( FindChunk( out, IsFilmHeader, fs, fe ) ) {
		char filmChunk[128];
		std::snprintf( filmChunk, sizeof(filmChunk),
			"film\n{\n\twidth %u\n\theight %u\n}", dim, dim );
		std::vector<std::string> out2;
		for( size_t i = 0; i < out.size(); ++i ) {
			if( i == fs ) { out2.push_back( filmChunk ); i = fe; continue; }
			out2.push_back( out[i] );
		}
		out.swap( out2 );
	}

	std::string joined;
	for( size_t i = 0; i < out.size(); ++i ) { joined += out[i]; joined.push_back( '\n' ); }
	return joined;
}

// (An exact-choice spectral probe-route helper — the Pel-domain
// CheckProbeRoute's sibling — previously lived here but had only one
// caller, the spectral_caustic case, which is deliberately marginal in
// its PT-vs-BDPT tail and now uses CheckSpectralProbeRouteNotVCM below.
// If a future spectral probe route needs an EXACT assertion, it's a
// near-copy of CheckProbeRoute against MakeAutoSpectralProbeScene.)

//////////////////////////////////////////////////////////////////////
// CheckSpectralProbeRouteNotVCM -- variant for a scene whose probe
// decision is DELIBERATELY marginal in its PT-vs-BDPT tail.  The only
// invariant this scene locks is the documented one: the transport-reach
// gate keeps VCM OUT (VCM-spectral's luminance-proxy merge can't reach
// the dispersive caustic -- SPECTRAL_PARITY_AUDIT §3, design doc
// §6.2.2).  After the reach gate rejects VCM, the general σ²·T
// PT-vs-BDPT fallthrough reads right in the "marginal, not cheaply
// separable, low-stakes" band the probe is DESIGNED to treat as noise
// (see AutoRasterizer::RunProbe's τ_bdpt comment: marginal ~1.5× BDPT
// wins collapse to ~1.1× at probe spp and correctly stay PT).  At the
// test's deliberately-cheap probe config (spp 4, quarter-res, 2
// variance renders) that reading straddles τ_bdpt run-to-run, because
// the σ² signal is multi-thread float-accumulation nondeterminism BY
// DESIGN (the QMC stream itself is deterministic -- there is no seed to
// pin).  So an exact-PT assertion here is an assertion on noise; assert
// the real invariant (never VCM) instead, which faithfully guards the
// VCM-spectral-can't-reach regression without depending on the coin-flip
// tail the probe intentionally leaves marginal.
//////////////////////////////////////////////////////////////////////
static void CheckSpectralProbeRouteNotVCM(
	const char* label, const char* corpusPath, const char* specParams,
	const char* tag )
{
	std::cout << "Testing auto_spectral_rasterizer probe routing: " << label << std::endl;
	const std::string scene = MakeAutoSpectralProbeScene( corpusPath, specParams, /*samples*/ 4, /*dim*/ 96 );
	if( scene.empty() ) {
		Check( false, std::string("corpus scene readable + has a rasterizer chunk: ") + label );
		return;
	}
	const std::string p = WriteSceneToTempFile( scene.c_str(), tag );
	if( p.empty() ) { Check( false, std::string("temp scene written: ") + label ); return; }
	const ImageStats a = RenderAndComputeStats( p.c_str() );
	std::remove( p.c_str() );

	PrintStats( "probe", a );
	Check( a.valid,   std::string("probe render produced output: ") + label );
	if( !a.valid ) return;
	Check( a.wasAuto, std::string("active rasterizer is an AutoRasterizer: ") + label );
	// Query API consistency: the reported name must track whatever it
	// actually resolved to (expected-independent — the string API is wired
	// to the real decision, whichever way the marginal tail lands).
	Check( a.queryApiOK && a.resolvedName == ChoiceName(a.resolved),
		std::string("query API (name+reason) consistent with resolved choice: ") + label );
	// The locked invariant: the reach gate keeps VCM out.
	Check( a.resolved != AutoIntegratorChoice::VCM,
		std::string("probe kept VCM out (reach gate rejects VCM-spectral; resolved '")
		+ ChoiceName(a.resolved) + "'): " + label );
}

//////////////////////////////////////////////////////////////////////
// CountingProgressCallback -- records which progress callback the
// delegate actually drove.  The macOS GUI crash was a dangling
// pProgressFunc whose SetTitle() jumped through a zeroed vtable slot.
//////////////////////////////////////////////////////////////////////
class CountingProgressCallback : public virtual IProgressCallback
{
public:
	int titleCalls;
	int progressCalls;
	CountingProgressCallback() : titleCalls(0), progressCalls(0) {}
	virtual bool Progress( const double, const double ) override { progressCalls++; return true; }
	virtual void SetTitle( const char* ) override { titleCalls++; }
};

//////////////////////////////////////////////////////////////////////
// TestDelegateStateForwarding -- the AutoRasterizer is a delegating
// wrapper; the host (Job::Rasterize + the GUI bridge) re-sets the
// progress callback and swaps output sinks on EVERY render, so those
// mutators must reach the resolved DELEGATE, not just the wrapper.
// Pre-fix the delegate froze whatever it was seeded with at the
// one-time resolve; on a GUI Merge-load (no clearAll) the bridge
// deleted + recreated its progress callback, leaving the delegate's
// pointer dangling -> the next render crashed in
// PixelBasedRasterizerHelper::RasterizeScene at
// pProgressFunc->SetTitle("Rasterizing Scene: ") (PC=0).
//
// Render once (resolving + seeding the delegate with O1/P1), then swap
// in O2/P2 exactly as the host does and render again.  With the fix the
// delegate drives O2/P2 and drops O1/P1; pre-fix it kept driving O1/P1.
//////////////////////////////////////////////////////////////////////
static void TestDelegateStateForwarding()
{
	const std::string label = "delegate state forwarding survives a post-resolve swap";
	std::cout << "Testing " << label << std::endl;

	const std::string scene =
		std::string("RISE ASCII SCENE 7\n") + kShader + kAutoPT + kSceneCommon;
	const std::string p = WriteSceneToTempFile( scene.c_str(), "fwd" );
	if( p.empty() ) { Check( false, "temp scene written: " + label ); return; }

	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) {
		Check( false, "job created: " + label );
		std::remove( p.c_str() );
		return;
	}
	if( !pJob->LoadAsciiSceneViaCst( p.c_str() ) ) {
		Check( false, "scene loaded: " + label );
		safe_release( pJob );
		std::remove( p.c_str() );
		return;
	}

	IRasterizer* pRast = pJob->GetRasterizer();
	Check( dynamic_cast<AutoRasterizer*>( pRast ) != nullptr,
		"active rasterizer is the auto dispatcher: " + label );

	// First render: seed O1 + P1.  This is the one-time resolve that
	// wires the delegate.  We hold our OWN ref on O1 so we can inspect it
	// after the (forwarded) FreeRasterizerOutputs drops the wrapper's +
	// delegate's refs.
	pJob->RemoveRasterizerOutputs();
	CapturingRasterizerOutput* O1 = new CapturingRasterizerOutput();
	O1->addref();
	pRast->AddRasterizerOutput( O1 );
	CountingProgressCallback P1;
	pJob->SetProgress( &P1 );
	pJob->Rasterize();

	Check( O1->width > 0 && P1.titleCalls > 0,
		"first render wired the initial sink + callback (delegate resolved): " + label );

	// Swap render state AFTER resolution -- exactly what the host bridge
	// does every render (the path the GUI crash came from).
	pRast->FreeRasterizerOutputs();
	CapturingRasterizerOutput* O2 = new CapturingRasterizerOutput();
	O2->addref();
	pRast->AddRasterizerOutput( O2 );
	CountingProgressCallback P2;
	pJob->SetProgress( &P2 );

	// Clear O1 so a stale delegate write to it is detectable (P1 is a
	// stack object -- always safe to inspect).
	O1->width = 0;
	O1->height = 0;
	O1->pixels.clear();
	P1.titleCalls = 0;
	P1.progressCalls = 0;

	pJob->Rasterize();

	// With the forwarding fix the delegate tracks the wrapper's new state:
	Check( O2->width > 0,
		"post-swap render reached the NEW output sink: " + label );
	Check( P2.titleCalls > 0,
		"post-swap render drove the NEW progress callback (no dangling pointer): " + label );
	// ...and no longer touches the superseded (would-be-freed) state:
	Check( O1->width == 0,
		"post-swap render did NOT write the superseded output sink: " + label );
	Check( P1.titleCalls == 0,
		"post-swap render did NOT drive the superseded progress callback: " + label );

	safe_release( O1 );
	safe_release( O2 );
	safe_release( pJob );
	std::remove( p.c_str() );
}

//////////////////////////////////////////////////////////////////////
// TestAutoParamPassThrough -- UI parameter edits on the auto dispatcher
// must reach the (rebuilt) rasterizer + its delegate.  Before the fix,
// RebuildRasterizer had no auto_rasterizer case (every edit returned false
// and was silently dropped) and ApplyRasterizerParam/FormatRasterizerParam
// didn't map the `integrator` pin.  Drives SetRasterizerParameter /
// GetRasterizerParameter exactly as the GUI param panel does.
//////////////////////////////////////////////////////////////////////
static void TestAutoParamPassThrough()
{
	const std::string label = "auto-rasterizer UI param edits pass through";
	std::cout << "Testing " << label << std::endl;

	const std::string scene =
		std::string("RISE ASCII SCENE 7\n") + kShader + kAutoAuto + kSceneCommon;
	const std::string p = WriteSceneToTempFile( scene.c_str(), "autoparam" );
	if( p.empty() ) { Check( false, "temp scene written: " + label ); return; }

	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) {
		Check( false, "job created: " + label );
		std::remove( p.c_str() );
		return;
	}
	if( !pJob->LoadAsciiSceneViaCst( p.c_str() ) ) {
		Check( false, "scene loaded: " + label );
		safe_release( pJob );
		std::remove( p.c_str() );
		return;
	}

	// (1) A common param (samples) must apply + round-trip.  Pre-fix
	//     RebuildRasterizer returned false for auto -> the edit was dropped.
	const bool setSamples = pJob->SetRasterizerParameter( "auto_rasterizer", "samples", "7" );
	Check( setSamples, "SetRasterizerParameter(samples=7) succeeds on auto: " + label );
	Check( pJob->GetRasterizerParameter( "auto_rasterizer", "samples" ) == "7",
		"samples round-trips through Get/SetRasterizerParameter: " + label );

	// (2) The auto-specific integrator pin must apply + round-trip.  Pre-fix
	//     ApplyRasterizerParam / FormatRasterizerParam didn't map "integrator".
	const bool setInt = pJob->SetRasterizerParameter( "auto_rasterizer", "integrator", "vcm" );
	Check( setInt, "SetRasterizerParameter(integrator=vcm) succeeds on auto: " + label );
	Check( pJob->GetRasterizerParameter( "auto_rasterizer", "integrator" ) == "vcm",
		"integrator pin round-trips through Get/SetRasterizerParameter: " + label );

	// End-to-end: the rebuilt dispatcher must actually resolve to the pinned
	// integrator at render time (proving the edit reached the live rasterizer).
	pJob->RemoveRasterizerOutputs();
	CapturingRasterizerOutput* cap = new CapturingRasterizerOutput();
	cap->addref();
	pJob->GetRasterizer()->AddRasterizerOutput( cap );
	IRasterizer* pRast = pJob->GetRasterizer();
	pJob->Rasterize();
	AutoRasterizer* pAuto = dynamic_cast<AutoRasterizer*>( pRast );
	Check( pAuto && pAuto->ResolvedIntegrator() == AutoIntegratorChoice::VCM,
		"the pinned integrator edit reached the live dispatcher (resolved VCM): " + label );

	safe_release( cap );
	safe_release( pJob );
	std::remove( p.c_str() );
}

int main()
{
	// Phase-4: enable the Tier-2 probe at low spp for the routing tests by
	// pointing GlobalOptions at a temp file that drops the activation gate
	// to 1 and sets a cheap probe (spp 4, half-res).  MUST be set before any
	// GlobalOptions() access (it is a lazy singleton read once) — i.e. before
	// the first render.  Phase-1/2 scenes carry no `probe` line, so the
	// lowered activation gate never reaches them (probe defaults off).
	{
		char optPath[256];
		std::snprintf( optPath, sizeof(optPath),
			"/tmp/auto_probe_test_opts_%d.txt", static_cast<int>(::getpid()) );
		std::ofstream ofs( optPath );
		ofs << "auto_probe_activation_spp 1\n"
		    << "auto_probe_spp 4\n"
		    << "auto_probe_scale 4\n"          // §6.2 default (quarter-res)
		    << "auto_probe_tau_caustic 1.30\n"
		    << "auto_probe_tau_reach 1.50\n"   // §6.2 transport-reach gate (jewel_vault over-fire fix)
		    << "auto_probe_reach_winsor_pct 0.99\n" // §6.2 firefly-robust reach: winsorize VCM mean (jewel_vault flake fix)
		    << "auto_probe_tau_bdpt 1.35\n"
		    << "auto_probe_variance_renders 2\n";
		ofs.close();
#ifdef _WIN32
		_putenv_s( "RISE_OPTIONS_FILE", optPath );
#else
		setenv( "RISE_OPTIONS_FILE", optPath, 1 );
#endif
	}

	std::cout << "=== AutoRasterizerTest ===" << std::endl;
	std::cout << std::endl;
	std::cout << "--- Fire Pel preview consistency ---" << std::endl;
	TestFirePelPreviewDivergence();

	// Pinned delegations: auto(X) must resolve to X and match X_pel_rasterizer.
	CheckDelegation( "pin pt   -> pathtracing_pel", kAutoPT,   "auto_pt",   kRefPT,   "ref_pt",   AutoIntegratorChoice::PT );
	CheckDelegation( "pin bdpt -> bdpt_pel",        kAutoBDPT, "auto_bdpt", kRefBDPT, "ref_bdpt", AutoIntegratorChoice::BDPT );
	CheckDelegation( "pin vcm  -> vcm_pel",         kAutoVCM,  "auto_vcm",  kRefVCM,  "ref_vcm",  AutoIntegratorChoice::VCM );

	// `integrator auto` and an omitted `integrator` both fall back to PT
	// in Phase 1 (Tier-0 selection), so both must match pathtracing_pel.
	CheckDelegation( "integrator auto  -> PT", kAutoAuto,  "auto_auto",  kRefPT, "ref_pt2", AutoIntegratorChoice::PT );
	CheckDelegation( "integrator unset -> PT", kAutoUnset, "auto_unset", kRefPT, "ref_pt3", AutoIntegratorChoice::PT );

	// --- Lifecycle regression: delegate state forwarding (the macOS GUI
	//     Merge-load crash, 2026-06-06).  The wrapper MUST forward a
	//     post-resolution SetProgressCallback / FreeRasterizerOutputs /
	//     AddRasterizerOutput to its delegate, else the delegate keeps a
	//     frozen (soon-dangling) progress callback / output set -> PC=0.
	std::cout << std::endl;
	std::cout << "--- Lifecycle: delegate state forwarding ---" << std::endl;
	TestDelegateStateForwarding();

	std::cout << std::endl;
	std::cout << "--- Lifecycle: auto-rasterizer UI param pass-through ---" << std::endl;
	TestAutoParamPassThrough();

	// --- Phase 2: Tier-1 static best-guess (integrator auto, no pin) ---
	// The dispatcher introspects the assembled scene: any heterogeneous or
	// emissive medium forces PT (fire/smoke G10); otherwise a transmissive
	// material + a positional point/spot light -> VCM (caustic-prone), and
	// everything else -> PT.  See docs/AUTO_RASTERIZER_DESIGN.md §5 and
	// docs/FIRE_SMOKE_DESIGN.md §5.2 / §7.1 step 5.
	std::cout << std::endl;
	std::cout << "--- Phase 2: static best-guess routing ---" << std::endl;

	// (a) Plain diffuse (point + area lit, no dielectric) -> PT.
	CheckStaticRoute( "diffuse + point light -> PT",
		kAutoAuto, kSceneCommon, "p2_diffuse", AutoIntegratorChoice::PT );

	// (b) Dielectric ball + positional point light -> caustic-prone -> VCM.
	CheckStaticRoute( "dielectric + point light -> VCM",
		kAutoAuto, std::string(kSceneCommon) + kGlassSphere, "p2_vcm", AutoIntegratorChoice::VCM );

	// (c) Dielectric but lit only by an area emitter (no positional light)
	//     -> PT.  Guards that the positional-light requirement spares
	//     glass-but-not-caustic scenes (the jewel_vault / cloister /
	//     alchemists class, which carry glass yet have no point light)
	//     from VCM's cost.
	CheckStaticRoute( "dielectric, area-lit only -> PT",
		kAutoAuto, std::string(kSceneAreaLitOnly) + kGlassSphere, "p2_diel_nopos", AutoIntegratorChoice::PT );

	// (d) Strong-indirect, purely area-lit (a gi_spheres analog) -> PT in
	//     the static tier.  BDPT is NOT statically separable from the
	//     PT-efficient glossy/diffuse bulk (design §5: gi_spheres is
	//     byte-identical in cheap signals to the PT-winning ggx_showcase);
	//     the Phase-4 probe is what would pick BDPT.  Asserting PT
	//     locks the conservative behavior.  (BDPT via an explicit pin is
	//     covered by the delegation test above.)
	CheckStaticRoute( "strong-indirect area-lit -> PT (probe picks BDPT)",
		kAutoAuto, kSceneAreaLitOnly, "p2_strongind", AutoIntegratorChoice::PT );

	// (e) A non-emissive heterogeneous medium overrides the otherwise-valid
	//     dielectric + point-light VCM signal and forces PT.
	CheckStaticRoute( "heterogeneous medium + caustic signal -> PT",
		kAutoAuto,
		std::string(kSceneCommon) + kGlassSphere +
			kHeterogeneousMediumDefinition + kGlobalHeterogeneousMedium,
		"p2_heterogeneous", AutoIntegratorChoice::PT );

	// (f) The emissive half is independent: a homogeneous emitting medium
	//     also overrides the competing VCM signal.  The scene language does
	//     not author homogeneous emission, so this fixture uses the public
	//     IJob medium API before the lazy render-time auto selection.
	CheckStaticRoute( "emissive homogeneous medium + caustic signal -> PT",
		kAutoAuto, std::string(kSceneCommon) + kGlassSphere,
		"p2_emissive", AutoIntegratorChoice::PT, true );

	// (g) Object-interior media use a separate scene scan from the global
	//     slot.  Without that scan, this dielectric + point-light fixture
	//     resolves VCM.
	CheckStaticRoute( "object-interior heterogeneous medium + caustic signal -> PT",
		kAutoAuto,
		std::string(kSceneCommon) + kHeterogeneousMediumDefinition +
			kGlassSphereWithInteriorMedium,
		"p2_object_medium", AutoIntegratorChoice::PT );

	// (h) Probe precedence: G10 is a capability gate, so it must return PT
	//     before Tier 2 renders any unsupported candidate.  The test options
	//     set activation_spp=1, making this probe live without the guard.
	CheckStaticRoute( "probe-enabled heterogeneous medium short-circuits to PT",
		kAutoProbe,
		std::string(kSceneCommon) + kGlassSphere +
			kHeterogeneousMediumDefinition + kGlobalHeterogeneousMedium,
		"p2_probe_medium", AutoIntegratorChoice::PT, false, true );

	// --- Phase 4: Tier-2 render-time probe (active) on REAL scenes ---
	// The probe corrects the static tier on the cases it provably can't see.
	// Each scene's rasterizer chunk is swapped for `auto_rasterizer{probe
	// true}` at a shrunk film; the probe (spp 4, half-res) renders candidate
	// integrators in-process and routes per-scene.  Decisions verified by the
	// real in-process probe (see docs/AUTO_RASTERIZER_DESIGN.md §6.2):
	//   gi_spheres     -> BDPT  (σ²·T ~480× @128px — the diffuse-GI blind spot)
	//   ggx_showcase   -> PT    (σ²·T ~0.26× — the glossy blind-spot partner)
	//   glass_pavilion -> VCM   (median-lum ~2.3× AND reach ~20-32× — a real refractive caustic)
	//   jewel_vault    -> PT    (median-lum ~2.6-3.1× fires, but reach ~1.0× < 1.50 -> NOT a
	//                            caustic; the over-fire fixed by the transport-reach gate, §6.2)
	//   env_only       -> PT    (env-IBL gate kills the +63% VCM env-bias confound)
	//   corridor       -> PT    (non-dielectric many-light -> BDPT check -> PT)
	std::cout << std::endl;
	std::cout << "--- Phase 4: render-time probe routing (real scenes) ---" << std::endl;

	CheckProbeRoute( "gi_spheres -> BDPT (diffuse-GI blind spot)",
		"scenes/FeatureBased/Combined/gi_spheres.RISEscene", "p4_gi", AutoIntegratorChoice::BDPT );
	CheckProbeRoute( "ggx_showcase -> PT (glossy blind-spot partner)",
		"scenes/FeatureBased/Materials/ggx_showcase.RISEscene", "p4_ggx", AutoIntegratorChoice::PT );
	CheckProbeRoute( "glass_pavilion -> VCM (refractive caustic: median AND reach gates)",
		"scenes/FeatureBased/Combined/glass_pavilion.RISEscene", "p4_glass", AutoIntegratorChoice::VCM );
	// Regression lock for the §6.2 jewel_vault over-fire: a dielectric + area-lit
	// scene whose caustic MEDIAN gate fires at probe spp (PT's hard indirect is
	// transiently under-converged) but whose transport-reach (mean-lum) gate does
	// NOT — PT reaches the same energy VCM does, so it is NOT a real caustic and
	// must NOT route VCM.  Pre-fix this resolved to VCM.  The reach uses a
	// firefly-ROBUST VCM mean (upper-tail winsorized, p99): at the cheap probe
	// spp VCM's sparse merge fireflies spiked the raw VCM mean past 1.50 ~2.6%
	// of the time -> a flaky false VCM route.  Winsorizing the VCM numerator (PT
	// denominator stays raw) drops jewel_vault's reach to a stable ~0.6-0.9
	// (< 1.50) while glass_pavilion's BROAD caustic survives at ~25x — see
	// AutoRasterizer.cpp WinsorizedMeanLuminance.
	//
	// Assertion is "never VCM", not "exactly PT": after the reach gate rejects
	// VCM the decision falls through to the σ²·T PT-vs-BDPT gate, and
	// jewel_vault (documented as the NOISIEST PT at probe spp, §6.2) reads a
	// σ²·T ratio that straddles τ_bdpt=1.35 run-to-run (measured 0.07..1.07,
	// spikes past τ in the wild -> a genuine BDPT resolution), because that
	// signal is multi-thread float-accumulation nondeterminism.  Asserting
	// exact PT was a coin flip; "never VCM" is the stable over-fire lock.  See
	// CheckProbeRouteNotVCM's header.
	CheckProbeRouteNotVCM( "jewel_vault -> not VCM (over-fire rejected by transport-reach gate)",
		"scenes/FeatureBased/PathTracing/pt_jewel_vault.RISEscene", "p4_jewel" );
	CheckProbeRoute( "env_only -> PT (env-IBL gate rejects VCM env-bias)",
		"scenes/Tests/UnifiedLighting/envmap_nee_test_pt.RISEscene", "p4_env", AutoIntegratorChoice::PT );
	CheckProbeRoute( "corridor_100lights -> PT (non-dielectric many-light)",
		"scenes/Tests/LightBVH/corridor_100lights_bvh.RISEscene", "p4_corridor", AutoIntegratorChoice::PT );

	// --- Phase 1b: spectral sibling (auto_spectral_rasterizer) ---
	// Same dispatcher class via the domain flag; the decision logic is shared
	// verbatim with the Pel path.  Pins prove delegation to the *_spectral_
	// concrete rasterizers; the static routes prove the spectral VCM delegate
	// is reachable; the probe locks the documented spectral_caustic limitation.
	std::cout << std::endl;
	std::cout << "--- Phase 1b: auto_spectral_rasterizer ---" << std::endl;

	// Pin equivalence: auto_spectral(X) resolves to X and matches X_spectral_rasterizer.
	CheckDelegation( "spectral pin pt   -> pathtracing_spectral", kAutoSpecPT,   "as_pt",   kRefPTSpec,   "rs_pt",   AutoIntegratorChoice::PT );
	CheckDelegation( "spectral pin bdpt -> bdpt_spectral",        kAutoSpecBDPT, "as_bdpt", kRefBDPTSpec, "rs_bdpt", AutoIntegratorChoice::BDPT );
	CheckDelegation( "spectral pin vcm  -> vcm_spectral",         kAutoSpecVCM,  "as_vcm",  kRefVCMSpec,  "rs_vcm",  AutoIntegratorChoice::VCM );

	// Static routing (probe off): same two signals as Pel, in the spectral domain.
	CheckStaticRoute( "spectral diffuse + point light -> PT",
		kAutoSpecAuto, kSceneCommon, "as_diffuse", AutoIntegratorChoice::PT );
	// dielectric + positional -> VCM proves the spectral VCM delegate is reachable
	// (the probe can't select it for spectral_caustic; this static route can).
	CheckStaticRoute( "spectral dielectric + point light -> VCM",
		kAutoSpecAuto, std::string(kSceneCommon) + kGlassSphere, "as_vcm_static", AutoIntegratorChoice::VCM );
	CheckStaticRoute( "spectral dielectric, area-lit only -> PT",
		kAutoSpecAuto, std::string(kSceneAreaLitOnly) + kGlassSphere, "as_diel_nopos", AutoIntegratorChoice::PT );
	CheckStaticRoute( "spectral heterogeneous medium + caustic signal -> PT",
		kAutoSpecAuto,
		std::string(kSceneCommon) + kGlassSphere +
			kHeterogeneousMediumDefinition + kGlobalHeterogeneousMedium,
		"as_heterogeneous", AutoIntegratorChoice::PT );

	// Probe routing (probe on): the spectral dispersive caustic.  DOCUMENTED
	// LIMITATION (design doc §6.2.2): the median gate fires (~2.9x) but the
	// transport-reach gate FAILS (~0.7x) because VCM-spectral's luminance-proxy
	// merge loses dispersion energy (SPECTRAL_PARITY_AUDIT §3), so VCM's
	// RGB-projected mean does NOT exceed PT's -> the reach gate rejects VCM.
	// This LOCKS that behavior (a regression guard); closing it to VCM needs
	// per-wavelength VCM photons (out of scope).
	//
	// The assertion is "never VCM", not "exactly PT": once the reach gate
	// rejects VCM the decision falls through to the general σ²·T PT-vs-BDPT
	// gate, whose reading for this scene sits in the marginal band the probe
	// is DESIGNED to treat as noise (RunProbe's τ_bdpt comment), so at the
	// cheap test probe config it straddles τ_bdpt run-to-run (the σ² signal
	// is multi-thread float-accumulation nondeterminism — there is no seed to
	// pin).  Asserting exact PT was a coin flip; "never VCM" is the real,
	// stable invariant.  See CheckSpectralProbeRouteNotVCM's header.
	CheckSpectralProbeRouteNotVCM( "spectral_caustic -> not VCM (reach gate defeats VCM-spectral merge; documented)",
		"scenes/Tests/Spectral/spectral_dispersive_caustic.RISEscene",
		"\tnmbegin 405\n\tnmend 705\n\tnum_wavelengths 8\n", "as_probe_caustic" );

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;

	return failCount == 0 ? 0 : 1;
}
