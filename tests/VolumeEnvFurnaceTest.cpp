//////////////////////////////////////////////////////////////////////
//
//  VolumeEnvFurnaceTest.cpp - Regression guard for the environment
//    MIS partition on the CAMERA-RAY MEDIUM-SCATTER path of the
//    path tracer (slice F2 of the PT env-MIS arc).
//
//  WHAT IS GUARDED.  PathTracingIntegrator::IntegrateRayTemplated
//    handles a camera ray that scatters in a participating medium
//    before reaching any surface inline: it evaluates env-NEE at the
//    scatter point (MediumTransport::EvaluateInScattering ->
//    LightSampler::EvaluateDirectLighting with isVolumeScatter=true,
//    which MIS-weights the env sample against the PHASE pdf), then
//    samples the phase function for a continuation.  When that
//    continuation escapes the scene it reads the environment.  That
//    read used to be added at MIS weight 1 -- the exact same defect
//    the preceding commit fixed on the SURFACE escape path -- so the
//    two env strategies summed to 1 + w_nee instead of 1.  For an
//    isotropic phase function under a uniform environment,
//    envPdf == phasePdf == 1/(4 pi), so w_nee = 0.5 under the power-2
//    heuristic and the single-scatter env term was 50 % too bright.
//    The HWSS twin (IntegrateRayHWSS) carried the identical defect.
//
//  HARNESS PATTERN: EnvLightBalanceTest.cpp / HairRenderTest.cpp --
//    scene text assembled as ordinary `RISE ASCII SCENE 7` chunks in
//    C++ string literals, written to a temp file, loaded through
//    IJobPriv::LoadAsciiSceneViaCst (the real CST path `bin/rise`
//    uses), rendered, and captured in memory via a
//    CapturingRasterizerOutput.  No scene file is added to
//    scenes/Tests, so no CST golden regeneration is needed.
//    `oidn_denoise FALSE` everywhere: OutputDenoisedImage's default
//    forwards POST-denoise pixels to OutputImage, which a capture
//    that only overrides OutputImage would silently pick up.
//
//  THE SCENE, AND WHY IT IS SHAPED LIKE THIS.  A closed furnace needs
//    the estimator to be energy-conserving end to end, and the camera
//    medium-scatter path is NOT: after its ONE inline scatter, a
//    continuation that misses all geometry is terminated with a
//    deterministic Beer-Lambert factor (`TrEsc`) and the environment
//    -- the "scatter again" branch is simply dropped.  (The main loop
//    in IntegrateFromHitTemplated does re-sample the medium and is
//    unbiased; this truncation is specific to the camera-ray inline
//    scatter, is PRE-EXISTING, and is out of scope for this test.)
//    A naive box-shaped fog furnace therefore does NOT read 1.0 after
//    the fix, and -- worse for a regression guard -- at moderate
//    optical depth the truncation deficit and the MIS over-count very
//    nearly CANCEL, so such a scene reads ~1.0 with the BUG PRESENT
//    and low WITHOUT it.  A "must read 1.0" assertion on that scene
//    would have the sign of the test backwards.
//
//    The scene below removes the truncation from the answer instead of
//    modelling it: the medium is a long, thin COLUMN along the view
//    axis (bbox 3 x 3 x 101), and the camera sits inside its near end
//    looking down its length through a 1-degree field of view.  The
//    VIEW ray therefore accumulates a substantial optical depth
//    (sigma_t = 0.004 over ~100 units, tau ~= 0.40, so ~33 % of camera
//    rays scatter), while a scattered ray -- isotropic, so essentially
//    never within the ~0.9-degree cone that would send it back down
//    the column -- leaves through a side wall after only ~1-3 units,
//    i.e. an escape optical depth of ~0.01.  The dropped
//    multiple-scatter branch is thus worth ~0.4 % instead of the
//    several percent a cubical fog box would cost, and the closed-form
//    furnace target of 1.0 is restored to within that residual:
//
//      correct   = e^-tau + (1 - e^-tau) * E[Tr_escape]  ~= 0.996
//      with bug  = e^-tau + (1 - e^-tau) * 1.5 * E[Tr_escape] ~= 1.16
//
//    sigma_a = 0 also makes the single-scatter albedo exactly 1, so the
//    scatter branch carries weight 1 and the estimator stays close to
//    unity sample by sample -- which is why 4096 spp on a 24x24 frame
//    (about 5 s for all three variants together) already pins the mean
//    to within about a part in a thousand.
//
//  MEASURED, THIS MACHINE, POST-FIX.  Four SAMPLES (n = 4 repeat runs
//    of this binary), not measured bounds -- much of RISE's sampling is
//    deterministic per pixel at a fixed sample count, so repeat runs
//    under-sample the true run-to-run range and their min/max must not
//    be read as one.  The tolerances below are sized off these values
//    with explicit headroom, never off their spread:
//
//      RGB PT              0.99509 / 0.99543 / 0.99534 / 0.99570
//      spectral hwss=false 0.99915 / 1.00014 / 0.99984 / 0.99997
//      spectral hwss=true  0.95326 / 0.95396 / 0.95405 / 0.95348
//
//    Per-pixel spread at that sample count: RGB [0.969, 1.021],
//    hwss=false [0.950, 1.064], hwss=true [0.933, 0.975].
//
//    RGB sits ~0.46 % under and hwss=false ~0.03 % under.  Both are
//    within the ~0.4 % truncation residual described above; the small
//    RGB-vs-NM difference (the two take different distance-sampling
//    entry points, PTSampleMediumDistance<PelTag> vs the NM twin) was
//    NOT separately diagnosed and is not claimed to be understood.
//
//  THE hwss=true DEFICIT IS PRE-EXISTING AND IS NOT THIS FIX.  The
//    hero-wavelength bundle reads 4.6 % under unity here.  That is the
//    same spectral-bundle env deficit CLAUDE.md's "High-Value Facts"
//    already records for hwss=true env-IBL (18 % under PT on the
//    EnvLightBalanceTest uniform env-only topology, at the disc-area
//    baseline, independent of any MIS migration).  The red-prove below
//    settles it as pre-existing rather than introduced: reverting the
//    fix moves hwss=true from -4.67 % to +10.90 %, a shift of 15.5 pp
//    -- the SAME shift the other two variants show (RGB 15.7 pp,
//    hwss=false 16.4 pp).  The fix therefore does exactly one thing to
//    the bundle path, and the residual sits underneath it untouched.
//    hwss=true accordingly gets its own asymmetric band (below) rather
//    than being dropped or having the shared band widened to hide it.
//
//  RED-PROVE.  With Fix 1 reverted (the MIS weight dropped again at
//    both camera-ray medium-escape sites, i.e. the two
//    `if( pLS && phasePdf > 0 )` guards forced false), rebuilt and
//    re-run, this binary reports:
//
//      RGB PT              1.15731 / 1.15732   (+15.73 %)
//      spectral hwss=false 1.16410 / 1.16395   (+16.39 %)
//      spectral hwss=true  1.10869 / 1.10904   (+10.90 %)
//
//    against a predicted +0.5 * (1 - e^-tau) * E[Tr_escape] ~= +16 %.
//    Six of this file's nine checks fail in that state; all nine pass
//    with the fix in place.
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
// CapturingRasterizerOutput -- same shape as EnvLightBalanceTest's /
// HairRenderTest's.  Deliberately does NOT override
// OutputPreDenoisedImage / OutputDenoisedImage; see the file header.
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
	double luminance;	// mean(mean[0..2])
	double minLum;
	double maxLum;
	bool   valid;
};

static ImageStats ComputeStats( const CapturingRasterizerOutput& cap )
{
	ImageStats s{};
	if( cap.pixels.empty() ) {
		return s;
	}

	double sum[3] = { 0, 0, 0 };
	s.minLum = 1e30;
	s.maxLum = -1e30;
	for( const RISEColor& c : cap.pixels ) {
		sum[0] += c.base.r;
		sum[1] += c.base.g;
		sum[2] += c.base.b;
		const double lum = (c.base.r + c.base.g + c.base.b) / 3.0;
		s.minLum = std::fmin( s.minLum, lum );
		s.maxLum = std::fmax( s.maxLum, lum );
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
		"/tmp/volume_env_furnace_test_%s_%d.RISEscene",
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
// Scene assembly.
//
// The medium is a `painter_heterogeneous_medium` with a CONSTANT
// density painter rather than a `homogeneous_medium`, for one reason
// only: it is the sole medium chunk that carries an explicit AABB.  An
// unbounded global medium would make both the escape transmittance
// (EvalTransmittance over RISE_INFINITY) and every env shadow ray
// evaluate to exp(-sigma_t * DBL_MAX) == 0, and the whole env term
// would vanish rather than being MIS-weighted.  With a constant density
// of 1.0 the majorant equals the local extinction everywhere, so delta
// tracking takes no null collisions and the medium behaves exactly as a
// homogeneous one inside the box.
//
// There is deliberately NO geometry in the scene.  Any object the
// scattered ray could hit would route the continuation into
// IntegrateFromHitForTag (the main loop, whose env MIS was already
// correct) instead of the camera-ray escape branch this test guards.
//////////////////////////////////////////////////////////////////////

// Optical parameters -- see the file header for the derivation.
static const double kSigmaS      = 0.004;	// sigma_s == sigma_t (sigma_a = 0)
static const double kColumnHalfXY = 1.5;	// transverse half-extent of the fog column
static const double kColumnZNear = -1.0;	// camera sits at z = 0, inside
static const double kColumnZFar  = 100.0;
static const double kFovDegrees  = 1.0;		// keeps every ray inside the column

static std::string SceneCommon( unsigned int width, unsigned int height )
{
	std::ostringstream ss;
	ss <<
		"standard_shader\n{\n\tname global\n\tshaderop DefaultDirectLighting\n}\n\n"
		"film\n{\n\twidth " << width << "\n\theight " << height << "\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 0 0\n\tlookat 0 0 1\n\tup 0 1 0\n\tfov "
			<< kFovDegrees << "\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_env\n\tcolor 1.0 1.0 1.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_density\n\tcolor 1.0 1.0 1.0\n}\n\n"
		"painter_heterogeneous_medium\n{\n"
		"\tname furnace_fog\n"
		"\tabsorption 0.0 0.0 0.0\n"
		"\tscattering " << kSigmaS << " " << kSigmaS << " " << kSigmaS << "\n"
		"\tphase isotropic\n"
		"\tdensity_painter pnt_density\n"
		"\tresolution 4\n"
		"\tcolor_to_scalar luminance\n"
		"\tbbox_min " << -kColumnHalfXY << " " << -kColumnHalfXY << " " << kColumnZNear << "\n"
		"\tbbox_max " <<  kColumnHalfXY << " " <<  kColumnHalfXY << " " << kColumnZFar  << "\n"
		"}\n\n"
		"global_medium\n{\n\tmedium furnace_fog\n}\n\n";
	return ss.str();
}

static std::string RasterizerPTRgb( unsigned int samples )
{
	std::ostringstream ss;
	ss <<
		"pathtracing_pel_rasterizer\n{\n"
		"\tsamples " << samples << "\n"
		"\toidn_denoise FALSE\n"
		"\tpixel_filter box\n"
		"\tradiance_map pnt_env\n\tradiance_scale 1.0\n\tradiance_background TRUE\n"
		"}\n\n"
		"file_rasterizeroutput\n{\n\tpattern /tmp/volume_env_furnace_unused\n\ttype PNG\n\tbpp 8\n\tcolor_space sRGB\n}\n";
	return ss.str();
}

static std::string RasterizerPTSpectral( unsigned int samples, bool hwss )
{
	std::ostringstream ss;
	ss <<
		"pathtracing_spectral_rasterizer\n{\n"
		"\tsamples " << samples << "\n"
		"\toidn_denoise FALSE\n"
		"\tpixel_filter box\n"
		"\tnmbegin 380\n\tnmend 720\n\tnum_wavelengths 8\n\tspectral_samples 1\n"
		"\thwss " << (hwss ? "true" : "false") << "\n"
		"\tradiance_map pnt_env\n\tradiance_scale 1.0\n\tradiance_background TRUE\n"
		"}\n\n"
		"file_rasterizeroutput\n{\n\tpattern /tmp/volume_env_furnace_unused\n\ttype PNG\n\tbpp 8\n\tcolor_space sRGB\n}\n";
	return ss.str();
}

static std::string AssembleScene( const std::string& common, const std::string& rasterizer )
{
	return std::string( "RISE ASCII SCENE 7\n" ) + common + rasterizer;
}

//////////////////////////////////////////////////////////////////////
// The furnace assertions.
//
// TOLERANCES.  The mean band is expressed as an asymmetric pair
// (`loTol`, `hiTol`) around the closed-form furnace value of 1.0,
// because the two directions guard different things.
//
//   ABOVE unity is the defect.  Both bands cap over-unity at 2 %.
//   Nothing in this scene can legitimately exceed 1.0: a sigma_a = 0
//   medium in equilibrium with a uniform L = 1 field is exactly 1.0 and
//   the one bias present (the dropped multiple-scatter branch, file
//   header) can only LOSE energy.  The pre-fix build reads +15.7 to
//   +16.4 %, so the guard has ~8x margin and cannot be satisfied by the
//   bug it exists for.
//
//   BELOW unity is the structural residual.  For RGB and hwss=false the
//   floor is also 2 %, which clears the worst measured deficit
//   (0.49 %) by 4x while still failing on the 2 %+ scale a genuinely
//   lost transport term would cost.  hwss=true gets a 7 % floor instead
//   -- it measures 4.6 % under for a documented, pre-existing,
//   separately-red-proved reason (file header) -- which still leaves
//   ~1.5x headroom and still catches an over-unity regression at the
//   same 2 % as the others.
//
// PER-PIXEL BAND.  Deliberately loose (15 %) and deliberately NOT a
// precision check: at these sample counts the per-pixel spread is
// several percent of genuine MC noise (measured extremes in the file
// header), so a tight per-pixel band would be a flakiness generator.
// Its job is to catch a STRUCTURALLY broken frame -- black pixels, a
// wrongly-lit region, a half-frame discontinuity -- that a mean-level
// assertion could average away.
//////////////////////////////////////////////////////////////////////
static const double kFurnaceTolLo     = 0.02;	// how far BELOW 1.0 is allowed
static const double kFurnaceTolHi     = 0.02;	// how far ABOVE 1.0 is allowed
static const double kHwssFurnaceTolLo = 0.07;	// hwss=true only; see above
static const double kPixelSanityBand  = 0.15;

static void RunFurnaceCase(
	const char* label,
	const std::string& sceneText,
	const char* tag,
	const double loTol )
{
	const ImageStats s = RenderAndComputeStats( sceneText, tag );

	std::string validName = std::string( label ) + ": render produced output";
	Check( s.valid, validName.c_str() );
	if( !s.valid ) {
		return;
	}

	std::cout << "  " << label
		<< ": mean = " << s.luminance
		<< "  (min " << s.minLum << ", max " << s.maxLum << ")"
		<< "  deviation from 1.0 = "
		<< ( (s.luminance - 1.0) * 100.0 ) << " %"
		<< "  [band -" << (loTol * 100.0) << " % .. +"
		<< (kFurnaceTolHi * 100.0) << " %]" << std::endl;

	std::string meanName = std::string( label ) + ": mean inside the furnace band around 1.0";
	Check( s.luminance >= 1.0 - loTol && s.luminance <= 1.0 + kFurnaceTolHi,
		   meanName.c_str() );

	std::string bandName = std::string( label ) + ": no pixel structurally off unity";
	Check( std::fabs( s.minLum - 1.0 ) <= kPixelSanityBand &&
		   std::fabs( s.maxLum - 1.0 ) <= kPixelSanityBand,
		   bandName.c_str() );
}

// 24x24 at 4096 spp is ~1.6 s per variant here (~5 s for the file).
// The frame is uniform by construction -- every pixel has the same
// optical depth -- so resolution buys nothing but wall time; the sample
// count is what converges the mean.
static const unsigned int kW = 24, kH = 24, kSamples = 4096;

static void TestVolumeEnvFurnaceRGB()
{
	std::cout << "=== 1. Camera-in-medium env furnace -- RGB PT ===" << std::endl;
	RunFurnaceCase( "RGB PT",
		AssembleScene( SceneCommon( kW, kH ), RasterizerPTRgb( kSamples ) ),
		"rgb", kFurnaceTolLo );
}

static void TestVolumeEnvFurnaceSpectral()
{
	std::cout << "=== 2. Camera-in-medium env furnace -- spectral PT, hwss=false ===" << std::endl;
	RunFurnaceCase( "spectral hwss=false",
		AssembleScene( SceneCommon( kW, kH ), RasterizerPTSpectral( kSamples, false ) ),
		"spec", kFurnaceTolLo );
}

static void TestVolumeEnvFurnaceHwss()
{
	std::cout << "=== 3. Camera-in-medium env furnace -- spectral PT, hwss=true ===" << std::endl;
	RunFurnaceCase( "spectral hwss=true",
		AssembleScene( SceneCommon( kW, kH ), RasterizerPTSpectral( kSamples, true ) ),
		"hwss", kHwssFurnaceTolLo );
}

int main( int /*argc*/, char* /*argv*/[] )
{
	std::cout << "VolumeEnvFurnaceTest -- camera-in-medium environment MIS partition regression" << std::endl;

	TestVolumeEnvFurnaceRGB();
	TestVolumeEnvFurnaceSpectral();
	TestVolumeEnvFurnaceHwss();

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;

	return failCount == 0 ? 0 : 1;
}
