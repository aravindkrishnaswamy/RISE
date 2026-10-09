//////////////////////////////////////////////////////////////////////
//
//  WeaveGapShadowTransmittanceTest.cpp - DL-05 (docs/DEBT_LEDGER.md;
//    docs/CLOTH_FABRIC_DESIGN.md section 15 item 27): a NEE shadow ray
//    toward a DELTA light (omni / spot / directional) must see through
//    a `transmission thin` weave's delta GAP lobe with transmittance
//    `gap`, exactly as the SPF's own gap draw (probability `gap`,
//    `kray = 1`) carries a sampled path through it.
//
//    Before DL-05 the shadow ray treated every weave as opaque, so the
//    path  delta light -> straight through a weave gap -> receiver  had
//    NO estimator in PT at all (NEE blocked; a BSDF-sampled ray can
//    never hit a delta light) while BDPT/VCM reached it by light
//    tracing -- a two-layer weave box lit from outside read PT 1.28-1.55x
//    UNDER BDPT/VCM.
//
//  SECTIONS (WEAVE_GAP_FILTER env var selects by keyword substring;
//  unset runs every gated section -- the CI invocation):
//
//    query    THE CONSISTENCY CONDITION, at the C++ level: for every
//             material that reports IMaterial::HasDeltaPassThrough, the
//             SPF's ISPF::DeltaPassThroughTransmittance{,NM} must equal
//             the Monte-Carlo expectation of what its own Scatter{,NM}
//             emits along the incoming direction as a delta ray -- bare
//             weave, fabric over it (with a weave rotation), coated over
//             it (clear and tinted/absorbing), coated over fabric over
//             it, and a luminaire wrapper.  Plus the capability flags:
//             true exactly where the gap exists.
//    closed   CLOSED FORM.  A Lambertian receiver patch under a single
//             `gap g` weave sheet (no diffuse transmission:
//             `fabric custom`'s warp/weft_transmit default 0), lit by an
//             omni light directly overhead.  The patch's only light
//             arrives through the gap, so its radiance is exactly
//             `g * L0`, where L0 is the same render WITHOUT the sheet
//             (itself checked against rho/pi * P/d^2).  Rows: PT RGB,
//             PT spectral (hwss off and on), BDPT, VCM.  PT reads 0
//             pre-fix; BDPT/VCM read g*L0 before and after.
//    composite  The same receiver under a `composite_material` of two
//             gapped weaves: g^2 * L0 (CompositeSPF's straight exit is
//             gap -> gap).  PT read 0 before the composite override.
//    directional  Same receiver under a `directional_light` -- the
//             Step-1 zero-exitance path in PT AND BDPT's deterministic
//             zero-exitance sweep (both route through the light's own
//             ComputeDirectLighting -> RayCaster::CastShadowRayAuto),
//             the only estimator either integrator has for it.  Both
//             read 0 pre-fix.
//    area     PARTITION GUARD.  Same receiver under a small AREA
//             emitter.  PT already reaches it through the gap by BSDF
//             sampling at MIS weight 1 (the gap is a delta vertex, so
//             the emitter hit has no NEE partner); the fix must NOT let
//             the area-light NEE arm see through the gap too, or the
//             path is counted twice.  g*L0 before AND after.
//    sms      DL-295.  The same receiver (and a camera looking UP at the
//             emitter through the sheet, and a closed black-weave box
//             under a uniform environment) with `sms_enabled TRUE`: a
//             BLACK-yarn gapped sheet in front of a 4 x 4 area emitter
//             reads g*L0, a composite of two such weaves g^2*L0, the env
//             box g*L0 -- SMS cannot represent a chain through a weave
//             (GetSpecularInfo reports no caster), so SMS on must equal
//             SMS off.  Every row but the env box read 0 before the fix
//             (PT dropped the emitter hit after the gap).  HWSS rows and
//             a composite(dielectric over weave) sibling are SMS-on vs
//             SMS-off parity, as are the review-round rows (a smooth
//             SSS / polished caster seen with no SMS anchor, the HWSS
//             SSS and no-BSDF hand-offs with and without an anchor); an
//             anchored no-gap caster reflection is parity under DL-372
//             (the historical DL-339 (a) suppression is removed); the ior-1.0 perfect refractor plane is
//             printed and the ior-1.5 open sheet (DL-339 (b)) is SMS-on
//             vs SMS-off parity since DL-345.  Renders here are
//             Sobol'-salted per (seed base, index) but NOT reproducible
//             run to run -- see the band note in the section.
//    castsshadows  P2-2 (external review): the transparent-shadow walk
//             (WalkShadowSegment, shared with DL-05's pass-through
//             walk) must STEP OVER a `casts_shadows FALSE` object, not
//             block on it -- the binary any-hit test already ignores
//             one.  NO weave anywhere in this section; a general
//             RayCaster regression found while building the DL-05 walk,
//             not a DL-05 mechanism.
//    layers   The design-doc topology: a closed two-layer weave box
//             (and its free-standing two-plane twin) with the omni
//             light OUTSIDE.  PT/BDPT/VCM within 8%.
//
//    fovsweep DL-294 (docs/DL294_NARROW_FOV_SPLAT.md): the spot rows
//             at fov 1/2/3/5/10 deg on the 2 x 2 patch, BDPT, VCM, VCM
//             with merging off and BDPT under the default gaussian
//             filter, every render referenced to PT's no-sheet render
//             at the same fov, plus the gap render's edge-row/column
//             fingerprint at fov 2.  The gap path reaches the
//             bidirectional integrators only by t = 1 light tracing, so
//             this isolates the splat: pre-fix BDPT read -6.05 % at
//             fov 2 with column 0 and row 0 at half radiance.
//    dl294    (opt-in only; WEAVE_GAP_FILTER=dl294; argv[2] = n,
//             default 4)  The same sweep with n repeats, mean +/- sd,
//             plus VCM-without-merging and the no-weave control (the
//             0.2 x 0.2 patch at fov 2, spot, no sheet, vs the analytic
//             value).  No assertions -- a measurement aid.
//    scenehash (opt-in only; WEAVE_GAP_FILTER=scenehash)  Pixel hash
//             of every scene in WEAVE_GAP_SCENES, fixed seed, for a
//             pre/post bit-identity check (see HashScenes).
//    table    (opt-in only; WEAVE_GAP_FILTER=table)  Re-measures
//             docs/CLOTH_FABRIC_DESIGN.md section 15 item 27's table at
//             its own setup (24x24, 512 spp) with n repeats (argv[2],
//             default 4); prints mean +/- sd per integrator and the
//             ratios.  No assertions -- a measurement aid.
//
//  SEEDING: argv[1] is an optional seed base (default 1000); every
//  render calls std::srand( seedBase + renderIndex ) and applies a
//  Sobol value salt. RenderSalted preserves its caller-supplied salt;
//  ordinary renders derive one from the seed base and render index.
//
//  OIDN is disabled on every rasterizer (the capture only overrides
//  OutputImage, which the default OutputDenoisedImage would feed with
//  denoised pixels).
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Utilities/Math3D/VectorsOps.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Utilities/IORStack.h"
#include "../src/Library/Utilities/SobolSampler.h"
#include "../src/Library/Materials/FabricMaterial.h"
#include "../src/Library/Materials/CoatedMaterial.h"
#include "../src/Library/Materials/LambertianMaterial.h"
#include "../src/Library/Materials/LambertianLuminaireMaterial.h"
#include "../src/Library/Materials/CompositeMaterial.h"
#include "WeaveTestFixture.h"

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

static unsigned int g_seedBase = 1000;
static unsigned int g_renderIndex = 0;
//! DL-355: every render uses its own randomized-QMC value salt.
//! libc rand() alone leaves BDPT/VCM on one frozen Sobol point set.

//! WEAVE_GAP_SPP_SCALE (env, measurement aid only): multiplies every
//! gated row's sample count, to separate a structured (QMC) residual from
//! a bias.  Unset -> 1.
static unsigned int SppScale()
{
	const char* s = std::getenv( "WEAVE_GAP_SPP_SCALE" );
	const long v = s ? std::strtol( s, nullptr, 10 ) : 1;
	return v > 0 ? (unsigned int)v : 1u;
}

//////////////////////////////////////////////////////////////////////
// Capture + statistics.
//////////////////////////////////////////////////////////////////////
class CapturingRasterizerOutput
	: public virtual IRasterizerOutput
	, public virtual Reference
{
public:
	std::vector<RISEColor> pixels;
	CapturingRasterizerOutput() {}
protected:
	virtual ~CapturingRasterizerOutput() {}
public:
	virtual void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}
	virtual void OutputImage( const IRasterImage& img, const Rect*, const unsigned int ) override
	{
		const unsigned int w = img.GetWidth(), h = img.GetHeight();
		pixels.resize( w * h );
		for( unsigned int y = 0; y < h; y++ ) {
			for( unsigned int x = 0; x < w; x++ ) {
				pixels[y * w + x] = img.GetPEL( x, y );
			}
		}
	}
};

//! Mean Rec.709 luminance of the composited-over-black radiance
//! (base * alpha), the convention-independent quantity
//! BDPTStrategyBalanceTest's ComputeStats documents.  -1 when the render
//! failed or produced a nonfinite pixel.
static double MeanLuminance( const CapturingRasterizerOutput& cap )
{
	if( cap.pixels.empty() ) return -1.0;
	double sum = 0;
	for( const RISEColor& c : cap.pixels ) {
		const double r = c.base.r * c.a, g = c.base.g * c.a, b = c.base.b * c.a;
		if( !std::isfinite( r ) || !std::isfinite( g ) || !std::isfinite( b ) ) return -1.0;
		sum += 0.2126 * r + 0.7152 * g + 0.0722 * b;
	}
	return sum / double( cap.pixels.size() );
}

//! Per-pixel Rec.709 luminance of the composited-over-black radiance,
//! row-major (image row 0 = top), for the DL-294 edge fingerprint.
static void PixelLuminance( const CapturingRasterizerOutput& cap, std::vector<double>& out )
{
	out.resize( cap.pixels.size() );
	for( size_t i = 0; i < cap.pixels.size(); i++ ) {
		const RISEColor& c = cap.pixels[i];
		out[i] = 0.2126 * c.base.r * c.a + 0.7152 * c.base.g * c.a + 0.0722 * c.base.b * c.a;
	}
}

//! FNV-1a over the captured pixels' double bytes (the scenehash section).
static unsigned long long PixelHash( const CapturingRasterizerOutput& cap )
{
	unsigned long long h = 1469598103934665603ull;
	for( const RISEColor& c : cap.pixels ) {
		const double v[4] = { c.base.r, c.base.g, c.base.b, c.a };
		const unsigned char* b = reinterpret_cast<const unsigned char*>( v );
		for( size_t i = 0; i < sizeof( v ); i++ ) {
			h ^= b[i];
			h *= 1099511628211ull;
		}
	}
	return h;
}

static unsigned long long g_lastPixelHash = 0;
static std::vector<RISEColor> g_lastPixels;

static double Render( const std::string& sceneText, const char* tag, std::vector<double>* pPixels = nullptr, unsigned int explicitSalt = 0u )
{
	char path[512];
	std::snprintf( path, sizeof(path), "/tmp/weave_gap_shadow_%s_%d.RISEscene",
		tag, static_cast<int>( ::getpid() ) );
	{
		std::ofstream ofs( path );
		if( !ofs.is_open() ) return -1.0;
		ofs << sceneText;
	}

	std::srand( g_seedBase + g_renderIndex );
	SobolSamplerTestHooks::ValueSalt().store( explicitSalt ? explicitSalt
		: SobolSequence::HashCombine( 0xD355u + g_seedBase, g_renderIndex ) );
	g_renderIndex++;

	double result = -1.0;
	IJobPriv* pJob = nullptr;
	if( RISE_CreateJobPriv( &pJob ) && pJob )
	{
		if( pJob->LoadAsciiSceneViaCst( path ) )
		{
			pJob->RemoveRasterizerOutputs();
			CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
			GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "test capture output" );
			pJob->GetRasterizer()->AddRasterizerOutput( pCap );
			if( pJob->Rasterize() ) {
				result = MeanLuminance( *pCap );
				if( pPixels ) PixelLuminance( *pCap, *pPixels );
				g_lastPixelHash = PixelHash( *pCap );
				g_lastPixels = pCap->pixels;
			}
			safe_release( pCap );
		}
		safe_release( pJob );
	}
	SobolSamplerTestHooks::ValueSalt().store( 0u );
	std::remove( path );
	return result;
}

//! Render with an EXPLICIT Sobol' salt (P2-2, DL-294): BDPT's and VCM's
//! Sobol' streams are keyed by pixel/sample index, not by libc `rand()`,
//! so libc seed increments alone would leave them BIT-IDENTICAL.
//! `Render` salts every call; this helper selects an explicit salt. `salt` should
//! come from `SobolSequence::HashCombine` over a caller-chosen base so
//! repeats are independent draws; reset to 0 after so this function's
//! callers cannot leak a salt into unrelated `Render()` calls elsewhere
//! in this file.
static double RenderSalted( const std::string& sceneText, const char* tag, unsigned int salt,
	std::vector<double>* pPixels = nullptr )
{
	const double result = Render( sceneText, tag, pPixels, SobolSequence::HashCombine( g_seedBase, salt ) );
	SobolSamplerTestHooks::ValueSalt().store( 0u );
	return result;
}

//! Independent salted renders; no averaging of captures used by ROI tests.
static double RenderMeanN( const std::string& sceneText, const char* tag, unsigned int n = 8 )
{
    double sum = 0, sum2 = 0;
    for( unsigned int i = 0; i < n; ++i ) {
        const double v = Render( sceneText, tag );
        if( !( v >= 0 ) ) return -1;
        sum += v;
        sum2 += v*v;
    }
    const double mean=sum/n;
    const double se=std::sqrt(std::max(0.0,(sum2-sum*mean)/(n*(n-1))));
    std::cout << "  salted average " << tag << " n=" << n << " mean=" << mean << " SE=" << se << std::endl;
    return mean;
}

//////////////////////////////////////////////////////////////////////
// Rasterizer chunks.  All with oidn off and a box filter.
//////////////////////////////////////////////////////////////////////
static const char* kOutputChunk =
	"file_rasterizeroutput\n{\n\tpattern rendered/weave_gap_shadow_unused\n\ttype EXR\n\tbpp 32\n\tcolor_space Rec709RGB_Linear\n}\n";

static std::string RastPT( unsigned int spp )
{
	std::ostringstream ss;
	ss << "pathtracing_pel_rasterizer\n{\n\tsamples " << spp * SppScale()
	   << "\n\trr_min_depth 8\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n" << kOutputChunk;
	return ss.str();
}

//! RastPT with the opt-in `transparent_shadows` walk turned on -- P2-2's
//! `casts_shadows FALSE` step-over test needs this walk specifically
//! (WalkShadowSegment, shared with the DL-05 pass-through walk), not the
//! binary default.
static std::string RastPTTransparentShadows( unsigned int spp )
{
	std::ostringstream ss;
	ss << "pathtracing_pel_rasterizer\n{\n\tsamples " << spp * SppScale()
	   << "\n\trr_min_depth 8\n\tpixel_filter box\n\toidn_denoise FALSE\n\ttransparent_shadows TRUE\n}\n\n" << kOutputChunk;
	return ss.str();
}

static std::string RastPTSpectral( unsigned int spp, bool hwss )
{
	std::ostringstream ss;
	ss << "pathtracing_spectral_rasterizer\n{\n\tsamples " << spp * SppScale()
	   << "\n\trr_min_depth 8\n\tpixel_filter box\n\toidn_denoise FALSE\n"
	   << "\tnmbegin 380\n\tnmend 720\n\tnum_wavelengths 8\n\tspectral_samples 1\n"
	   << "\thwss " << ( hwss ? "true" : "false" ) << "\n}\n\n" << kOutputChunk;
	return ss.str();
}

static std::string RastBDPT( unsigned int spp )
{
	std::ostringstream ss;
	ss << "bdpt_pel_rasterizer\n{\n\tsamples " << spp * SppScale()
	   << "\n\tmax_eye_depth 8\n\tmax_light_depth 8\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n" << kOutputChunk;
	return ss.str();
}

static std::string RastVCM( unsigned int spp )
{
	std::ostringstream ss;
	ss << "vcm_pel_rasterizer\n{\n\tsamples " << spp * SppScale()
	   << "\n\tmax_eye_depth 8\n\tmax_light_depth 8\n\tmerge_radius 0.0\n\tvc_enabled true\n\tvm_enabled true\n"
	   << "\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n" << kOutputChunk;
	return ss.str();
}

//! RastBDPT with the DEFAULT reconstruction filter (gaussian, 2-pixel
//! support): the filtered splat path, where DL-294 showed up not as a
//! lost mean but as a half-pixel MISREGISTRATION of the splat layer
//! (edge column 0 / row 0 at ~0.49 of the interior, the opposite
//! edges at ~1.49).
static std::string RastBDPTDefaultFilter( unsigned int spp )
{
	std::ostringstream ss;
	ss << "bdpt_pel_rasterizer\n{\n\tsamples " << spp * SppScale()
	   << "\n\tmax_eye_depth 8\n\tmax_light_depth 8\n\toidn_denoise FALSE\n}\n\n" << kOutputChunk;
	return ss.str();
}

//! RastVCM with merging OFF (vertex connection only): separates the
//! splat from VCM's merge-radius blur in the DL-294 measurement.
static std::string RastVCMNoMerge( unsigned int spp )
{
	std::ostringstream ss;
	ss << "vcm_pel_rasterizer\n{\n\tsamples " << spp * SppScale()
	   << "\n\tmax_eye_depth 8\n\tmax_light_depth 8\n\tmerge_radius 0.0\n\tvc_enabled true\n\tvm_enabled false\n"
	   << "\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n" << kOutputChunk;
	return ss.str();
}

//! DL-295: the PT rasterizers with `sms_enabled` set either way.  The
//! SMS section compares the two on scenes that contain NO SMS caster
//! (no material whose GetSpecularInfo reports isSpecular), where SMS can
//! contribute nothing and so must change nothing.  @a envPainter, when
//! true, prepends a uniform L = 1 `pnt_env` painter and binds it as the
//! global radiance map (the env-box rows).
static std::string EnvPainterChunk()
{
	return "uniformcolor_painter\n{\n\tname pnt_env\n\tcolor 1 1 1\n\tcolorspace Rec709RGB_Linear\n}\n\n";
}

static std::string RastPTSMS( unsigned int spp, bool sms, bool envPainter = false )
{
	std::ostringstream ss;
	if( envPainter ) ss << EnvPainterChunk();
	ss << "pathtracing_pel_rasterizer\n{\n\tsamples " << spp * SppScale()
	   << "\n\trr_min_depth 8\n\tpixel_filter box\n\toidn_denoise FALSE\n"
	   << "\tsms_enabled " << ( sms ? "TRUE" : "FALSE" ) << "\n";
	if( envPainter ) ss << "\tradiance_map pnt_env\n\tradiance_scale 1.0\n";
	ss << "}\n\n" << kOutputChunk;
	return ss.str();
}

static std::string RastPTSpectralSMS( unsigned int spp, bool hwss, bool sms, bool envPainter = false )
{
	std::ostringstream ss;
	if( envPainter ) ss << EnvPainterChunk();
	ss << "pathtracing_spectral_rasterizer\n{\n\tsamples " << spp * SppScale()
	   << "\n\trr_min_depth 8\n\tpixel_filter box\n\toidn_denoise FALSE\n"
	   << "\tnmbegin 380\n\tnmend 720\n\tnum_wavelengths 8\n\tspectral_samples 1\n"
	   << "\thwss " << ( hwss ? "true" : "false" ) << "\n"
	   << "\tsms_enabled " << ( sms ? "TRUE" : "FALSE" ) << "\n";
	if( envPainter ) ss << "\tradiance_map pnt_env\n\tradiance_scale 1.0\n";
	ss << "}\n\n" << kOutputChunk;
	return ss.str();
}

static std::string Assemble( const std::string& rasterizer, const std::string& body )
{
	// The standard_shader is ignored by the modern rasterizers (they drive
	// their integrators directly) and is only here so every scene string
	// has the same shape.
	return std::string( "RISE ASCII SCENE 7\n" )
		+ "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		+ rasterizer + body;
}

//////////////////////////////////////////////////////////////////////
// CLOSED-FORM RECEIVER SCENE (y up).
//
//   receiver  0.2 x 0.2 Lambertian patch at y = 0, rho = 0.5 (linear)
//   sheet     8 x 8 `weave_material { fabric custom transmission thin
//             gap g }` at y = kSheetY, double-sided -- `custom` has
//             zero warp/weft diffuse transmission, so light reaches the
//             underside ONLY through the gap
//   camera    at (0, 1, 1.2), below the sheet, fov 2 deg on the patch
//             centre, so every pixel sees the patch within +/-0.03 of
//             its centre (the 1/d^2 and cos variation over that
//             footprint is < 1e-4) and no camera ray crosses the sheet
//
// With the sheet absent the patch radiance is rho/pi * E, and with it
// present exactly g * that (plus a receiver <-> sheet-underside
// interreflection of relative size ~rho * rho_sheet * A_patch /
// (pi d_sheet^2) < 0.1%).
//////////////////////////////////////////////////////////////////////
static const double kSheetY  = 2.0;
static const double kLightY  = 4.0;
static const double kRho     = 0.5;
static const double kOmniPow = 16.0;	// I = color * power = 16 W/sr, E(0) = I / 4^2 = 1

enum LightKind { kOmni, kSpot, kDirectional, kArea, kAreaLarge };

//! Camera framing.  kTight: fov 2 deg on a 0.2 x 0.2 patch -- the
//! footprint over which the omni's 1/d^2 and cosine are constant to
//! < 1e-4, so the no-sheet render can be checked against rho/pi * I/d^2
//! absolutely.  kWide: fov 10 deg on a 2 x 2 patch, for the
//! BIDIRECTIONAL rows (chosen before DL-294 was fixed, when BDPT's and
//! VCM's t = 1 splat read up to 6 % low whenever the frame was lit edge
//! to edge -- the `fovsweep` section gates that now).  Every kWide row
//! is a RATIO against the same framing's no-sheet render, so it needs no
//! absolute closed form.
//! kLookUp (DL-295): the camera BELOW the sheet looking straight UP at
//! the area luminaire through it (fov 4 deg: every pixel sees the 0.5 x
//! 0.5 emitter 3 units away), so the ONLY vertex on the path is the
//! sheet itself -- a gap draw at depth 0 with no SMS anchor anywhere.
enum CamKind { kTight, kWide, kLookUp };

//! DL-329: when true, ReceiverScene builds the sheet as a double-sided
//! `indexedmesh_geometry` instead of a double-sided clipped plane.  A
//! file-scope switch (default false) so no existing call site changes.
static bool g_meshSheet = false;

//! DL-330 review: when true, ReceiverScene's receiver is a CLOSED sphere
//! (radius 0.5, centred (0, -0.5, 0), top at the origin) instead of the
//! open patch -- a random walk needs a closed body (DL-409).
static bool g_recvSphere = false;

//! @a compositeSheet: the sheet is a `composite_material` of two such
//! weaves (zero thickness, no extinction), whose only straight exit is
//! gap -> gap, so the closed form becomes g^2.
//! @a sheetMaterialChunks / @a recvMaterialChunks (DL-295's SMS
//! section, empty = the default): verbatim material chunks that define
//! `mat_sheet` / `mat_recv` in place of the built-in ones.
//! @a fovDeg > 0 overrides the framing's field of view (the DL-294
//! sweep; the patch size still follows @a cam).
static std::string ReceiverScene( LightKind light, bool withSheet, double gap, CamKind cam = kTight, bool compositeSheet = false,
	const std::string& sheetMaterialChunks = std::string(), const std::string& recvMaterialChunks = std::string(),
	double fovDeg = 0.0 )
{
	const bool wide = ( cam != kTight );
	const double fov = fovDeg > 0.0 ? fovDeg : ( cam == kLookUp ? 4.0 : ( wide ? 10.0 : 2.0 ) );
	std::ostringstream camSs;
	if( cam == kLookUp ) {
		camSs << "pinhole_camera\n{\n\tlocation 0 1 0\n\tlookat 0 4 0\n\tup 0 0 1\n\tfov " << fov << "\n}\n\n";
	} else {
		camSs << "pinhole_camera\n{\n\tlocation 0 1 1.2\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov " << fov << "\n}\n\n";
	}
	const std::string camChunk = camSs.str();
	std::ostringstream ss;
	ss <<
		"film\n{\n\twidth 16\n\theight 16\n}\n\n"
		<< camChunk <<
		"uniformcolor_painter\n{\n\tname pnt_recv\n\tcolor " << kRho << " " << kRho << " " << kRho
			<< "\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		<< ( recvMaterialChunks.empty()
			? std::string( "lambertian_material\n{\n\tname mat_recv\n\treflectance pnt_recv\n}\n\n" )
			: recvMaterialChunks )
		<< ( g_recvSphere
			? std::string( "sphere_geometry\n{\n\tname geo_recv\n\tradius 0.5\n}\n\n"
				"standard_object\n{\n\tname obj_recv\n\tgeometry geo_recv\n\tmaterial mat_recv\n\tposition 0 -0.5 0\n}\n\n" )
			: std::string( "clippedplane_geometry\n{\n\tname geo_recv\n" )
				+ ( wide
					? "\tpta -1 0 1\n\tptb 1 0 1\n\tptc 1 0 -1\n\tptd -1 0 -1\n"
					: "\tpta -0.1 0 0.1\n\tptb 0.1 0 0.1\n\tptc 0.1 0 -0.1\n\tptd -0.1 0 -0.1\n" )
				+ "\tdoublesided TRUE\n}\n\n"
				"standard_object\n{\n\tname obj_recv\n\tgeometry geo_recv\n\tmaterial mat_recv\n}\n\n" );

	if( withSheet ) {
		ss
			<< ( !sheetMaterialChunks.empty() ? sheetMaterialChunks : compositeSheet
				? std::string( "weave_material\n{\n\tname mat_layer\n\tfabric custom\n\ttransmission thin\n\tgap " ) + std::to_string( gap ) + "\n}\n\n"
				  "composite_material\n{\n\tname mat_sheet\n\ttop mat_layer\n\tbottom mat_layer\n}\n\n"
				: std::string( "weave_material\n{\n\tname mat_sheet\n\tfabric custom\n\ttransmission thin\n\tgap " ) + std::to_string( gap ) + "\n}\n\n" ) <<
			( g_meshSheet
				// DL-329 DOUBLE-SIDED rule: the same 8 x 8 sheet as a
				// double-sided `indexedmesh_geometry` (two triangles,
				// shared corners, per-vertex UVs) -- the mesh class
				// flips BOTH normals toward the ray (DL-70), where the
				// clipped plane flips only on a back-face hit.
				? std::string( "indexedmesh_geometry\n{\n\tname geo_sheet\n" )
					+ "\tvertex -4 " + std::to_string( kSheetY ) + " 4\n"
					+ "\tvertex 4 " + std::to_string( kSheetY ) + " 4\n"
					+ "\tvertex 4 " + std::to_string( kSheetY ) + " -4\n"
					+ "\tvertex -4 " + std::to_string( kSheetY ) + " -4\n"
					+ "\tuv 0 0\n\tuv 1 0\n\tuv 1 1\n\tuv 0 1\n"
					+ "\ttriangle 0 1 2\n\ttriangle 0 2 3\n"
					+ "\tdouble_sided TRUE\n\tface_normals TRUE\n}\n\n"
				: std::string( "clippedplane_geometry\n{\n\tname geo_sheet\n" )
					+ "\tpta -4 " + std::to_string( kSheetY ) + " 4\n\tptb 4 " + std::to_string( kSheetY ) + " 4\n"
					+ "\tptc 4 " + std::to_string( kSheetY ) + " -4\n\tptd -4 " + std::to_string( kSheetY ) + " -4\n"
					+ "\tdoublesided TRUE\n}\n\n" ) <<
			"standard_object\n{\n\tname obj_sheet\n\tgeometry geo_sheet\n\tmaterial mat_sheet\n}\n\n";
	}

	switch( light ) {
	case kOmni:
		ss << "omni_light\n{\n\tname lgt\n\tposition 0 " << kLightY << " 0\n\tcolor 1 1 1\n\tpower " << kOmniPow << "\n}\n\n";
		break;
	case kSpot:
		// A narrow spot (full intensity inside 1.5 deg, zero past 2 deg)
		// aimed at the patch, so almost every BDPT/VCM light subpath lands
		// on it -- the omni twin above is PT-cheap but leaves light
		// tracing only a ~1e-4 chance of reaching a 0.2 x 0.2 patch.  In PT
		// a spot is sampled by the SAME delta-light NEE arm as the omni.
		ss << "spot_light\n{\n\tname lgt\n\tposition 0 " << kLightY << " 0\n\ttarget 0 0 0\n"
		      "\tinner 1.5\n\touter 2.0\n\tcolor 1 1 1\n\tpower " << kOmniPow << "\n}\n\n";
		break;
	case kDirectional:
		// `direction` is FROM the surface TO the light
		// (docs/SCENE_CONVENTIONS.md).  Irradiance = color * power = 1.
		ss << "directional_light\n{\n\tname lgt\n\tdirection 0 1 0\n\tcolor 1 1 1\n\tpower 1.0\n}\n\n";
		break;
	case kAreaLarge:
		// DL-295's SMS rows: a 4 x 4 one-sided luminaire at y = 4 facing
		// DOWN.  PT reaches an area light through a gap ONLY by BSDF
		// sampling, so the 0.5 x 0.5 emitter below leaves a ~5 % per-render
		// sd at 1024 spp on the kWide receiver; this one subtends ~50x the
		// solid angle.  Every receiver -> emitter segment still crosses the
		// 8 x 8 sheet.
		ss <<
			"uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1 1 1\n\tcolorspace Rec709RGB_Linear\n}\n\n"
			"lambertian_luminaire_material\n{\n\tname mat_emit\n\texitance pnt_emit\n\tscale 4.0\n\tmaterial none\n}\n\n"
			"clippedplane_geometry\n{\n\tname geo_emit\n"
				"\tpta -2 " << kLightY << " -2\n\tptb 2 " << kLightY << " -2\n"
				"\tptc 2 " << kLightY << " 2\n\tptd -2 " << kLightY << " 2\n}\n\n"
			"standard_object\n{\n\tname obj_emit\n\tgeometry geo_emit\n\tmaterial mat_emit\n}\n\n";
		break;
	case kArea:
		// A 0.5 x 0.5 one-sided luminaire at y = 4 facing DOWN (winding
		// gives normal -Y).
		ss <<
			"uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1 1 1\n\tcolorspace Rec709RGB_Linear\n}\n\n"
			"lambertian_luminaire_material\n{\n\tname mat_emit\n\texitance pnt_emit\n\tscale 40.0\n\tmaterial none\n}\n\n"
			"clippedplane_geometry\n{\n\tname geo_emit\n"
				"\tpta -0.25 " << kLightY << " -0.25\n\tptb 0.25 " << kLightY << " -0.25\n"
				"\tptc 0.25 " << kLightY << " 0.25\n\tptd -0.25 " << kLightY << " 0.25\n}\n\n"
			"standard_object\n{\n\tname obj_emit\n\tgeometry geo_emit\n\tmaterial mat_emit\n}\n\n";
		break;
	}
	return ss.str();
}

struct RowSpec
{
	const char* label;
	std::string rast;
	double tol;			// relative tolerance on (L_sheet / L0) / g - 1; < 0 = print only
	CamKind cam;
};

static void RunReceiverRows( const char* section, LightKind light, const std::vector<RowSpec>& rows, const double gaps[], int nGaps )
{
	for( const RowSpec& row : rows )
	{
		const double L0 = RenderMeanN( Assemble( row.rast, ReceiverScene( light, false, 0.0, row.cam ) ), "l0" );
		Check( L0 > 0, std::string( section ) + " " + row.label + ": no-sheet control renders non-black" );
		if( !( L0 > 0 ) ) continue;
		std::cout << "  " << section << " " << row.label << ": L0 (no sheet) = " << L0 << std::endl;

		for( int k = 0; k < nGaps; k++ )
		{
			const double g = gaps[k];
			const double L = RenderMeanN( Assemble( row.rast, ReceiverScene( light, true, g, row.cam ) ), "lg" );
			Check( L >= 0, std::string( section ) + " " + row.label + ": sheet render produced output" );
			const double ratio = L / L0;
			char buf[256];
			std::snprintf( buf, sizeof(buf), "%s %s gap %.2f: L/L0 = %.5f  (closed form %.5f, rel err %+.3f%%)",
				section, row.label, g, ratio, g, 100.0 * ( ratio / g - 1.0 ) );
			std::cout << "  " << buf << ( row.tol < 0 ? "   [printed, not gated]" : "" ) << std::endl;
			if( row.tol >= 0 ) {
				Check( std::fabs( ratio / g - 1.0 ) <= row.tol, buf );
			}
		}
	}
}

//////////////////////////////////////////////////////////////////////
// query: DeltaPassThroughTransmittance vs the SPF's own sampler.
//////////////////////////////////////////////////////////////////////

//! A fixed shading point at the origin, normal +Z, UV (0.5, 0.5), seen
//! from @a view (CoatedMaterialChunkTest's MakeIntersectionFromView).
static RayIntersectionGeometric MakeHit( const Vector3& view )
{
	Ray inRay( Point3( view.x, view.y, view.z ), -view );
	RasterizerState rs = { 0, 0 };
	RayIntersectionGeometric ri( inRay, rs );
	ri.bHit = true;
	ri.range = 1.0;
	ri.ptIntersection = Point3( 0, 0, 0 );
	ri.vNormal = Vector3( 0, 0, 1 );
	ri.vGeomNormal = Vector3( 0, 0, 1 );
	ri.onb.CreateFromW( Vector3( 0, 0, 1 ) );
	ri.ptCoord = Point2( 0.5, 0.5 );
	return ri;
}

//! E[ kray * 1{emitted ray is a delta continuing along the incoming
//! direction} ] over @a n Scatter (nm < 0) / ScatterNM calls, per
//! channel (NM: all three equal).  Also returns the per-draw standard
//! error of the largest channel.
static RISEPel SampledPassThrough( const ISPF& spf, const RayIntersectionGeometric& ri,
	const Scalar nm, const int n, double& stdErr )
{
	RandomNumberGenerator rng( 90210u );
	IndependentSampler sampler( rng );
	IORStack stack( 1.0 );
	const Vector3 dir = Vector3Ops::Normalize( ri.ray.Dir() );
	RISEPel sum( 0, 0, 0 );
	double sumSqMax = 0;
	for( int i = 0; i < n; ++i )
	{
		ScatteredRayContainer scattered;
		if( nm < 0 ) {
			spf.Scatter( ri, sampler, scattered, stack );
		} else {
			spf.ScatterNM( ri, sampler, nm, scattered, stack );
		}
		RISEPel draw( 0, 0, 0 );
		for( unsigned int j = 0; j < scattered.Count(); ++j )
		{
			const ScatteredRay& sr = scattered[j];
			if( !sr.isDelta ) continue;
			if( Vector3Ops::Dot( Vector3Ops::Normalize( sr.ray.Dir() ), dir ) < 1.0 - 1e-9 ) continue;
			draw = draw + ( nm < 0 ? sr.kray : RISEPel( sr.krayNM, sr.krayNM, sr.krayNM ) );
		}
		sum = sum + draw;
		const double m = ColorMath::MaxValue( draw );
		sumSqMax += m * m;
	}
	const RISEPel mean = sum * ( 1.0 / double( n ) );
	const double mMax = ColorMath::MaxValue( mean );
	stdErr = std::sqrt( std::fmax( 0.0, sumSqMax / double( n ) - mMax * mMax ) / double( n ) );
	return mean;
}

static void CheckQueryMatchesSampler( const char* label, const IMaterial& mat )
{
	const ISPF* pSPF = mat.GetSPF();
	Check( pSPF != 0, std::string( "query " ) + label + ": material has an SPF" );
	if( !pSPF ) return;

	// 140 deg views the sheet from BELOW its normal: the ray-facing
	// frames flip, and a composite walks bottom-first.
	const double angles[] = { 0.0, 40.0, 75.0, 140.0 };
	const int n = 200000;
	for( double a : angles )
	{
		const double r = a * 3.14159265358979323846 / 180.0;
		const RayIntersectionGeometric ri = MakeHit( Vector3( std::sin( r ), 0.0, std::cos( r ) ) );

		double se = 0;
		const RISEPel sampled = SampledPassThrough( *pSPF, ri, -1.0, n, se );
		const RISEPel query = pSPF->DeltaPassThroughTransmittance( ri );
		double worst = 0;
		for( int c = 0; c < 3; ++c ) worst = std::fmax( worst, std::fabs( query[c] - sampled[c] ) );
		char buf[320];
		std::snprintf( buf, sizeof(buf),
			"query %s view %3.0f deg RGB: query (%.5f %.5f %.5f)  sampled (%.5f %.5f %.5f)  |diff| %.2e  (5 se = %.2e)",
			label, a, query[0], query[1], query[2], sampled[0], sampled[1], sampled[2], worst, 5 * se );
		std::cout << "  " << buf << std::endl;
		Check( worst <= 5 * se + 1e-4, buf );

		double seNM = 0;
		const RISEPel sampledNM = SampledPassThrough( *pSPF, ri, 550.0, n, seNM );
		const Scalar queryNM = pSPF->DeltaPassThroughTransmittanceNM( ri, 550.0 );
		std::snprintf( buf, sizeof(buf),
			"query %s view %3.0f deg NM(550): query %.5f  sampled %.5f  |diff| %.2e  (5 se = %.2e)",
			label, a, queryNM, sampledNM[0], std::fabs( queryNM - sampledNM[0] ), 5 * seNM );
		std::cout << "  " << buf << std::endl;
		Check( std::fabs( queryNM - sampledNM[0] ) <= 5 * seNM + 1e-4, buf );
	}
}

//! DL-329: the HWSS companion ladders (PT's IntegrateFromHitHWSS, the
//! BDPT generators, BDPT's RecomputeSubpathThroughputNM) price a DELTA
//! gap ray through `ISPF::EvaluateKrayNM` -- the 6-parameter overload
//! with pdfHero = -1, which forwards to the 5-parameter one.  It must
//! return exactly the krayNM the SPF's own ScatterNM emits on that ray at
//! that wavelength.  Before DL-329 every one of these returned -1 and the
//! integrators fell back to the CONTINUUM BSDF (~0 along the gap).
//! @a gated false: printed only (CompositeSPF, DL-221).
static void CheckCompanionKrayMatchesScatter( const char* label, const IMaterial& mat, bool gated )
{
	const ISPF* pSPF = mat.GetSPF();
	if( !pSPF ) return;
	const double angles[] = { 0.0, 40.0, 75.0, 140.0 };
	const double nms[] = { 450.0, 550.0, 650.0 };
	RandomNumberGenerator rng( 4242u );
	IndependentSampler sampler( rng );
	IORStack stack( 1.0 );
	int found = 0, mismatched = 0;
	double worst = 0;
	for( double a : angles ) {
		const double r = a * 3.14159265358979323846 / 180.0;
		const RayIntersectionGeometric ri = MakeHit( Vector3( std::sin( r ), 0.0, std::cos( r ) ) );
		const Vector3 dir = Vector3Ops::Normalize( ri.ray.Dir() );
		for( double nm : nms ) {
			for( int attempt = 0; attempt < 20000; ++attempt ) {
				ScatteredRayContainer scattered;
				pSPF->ScatterNM( ri, sampler, nm, scattered, stack );
				bool done = false;
				for( unsigned int j = 0; j < scattered.Count(); ++j ) {
					const ScatteredRay& sr = scattered[j];
					if( !sr.isDelta ) continue;
					if( Vector3Ops::Dot( Vector3Ops::Normalize( sr.ray.Dir() ), dir ) < 1.0 - 1e-9 ) continue;
					const Scalar k = pSPF->EvaluateKrayNM( ri, sr.ray.Dir(), sr.type, nm, stack, -1.0 );
					const double rel = std::fabs( k - sr.krayNM ) / std::fmax( 1e-12, std::fabs( sr.krayNM ) );
					found++;
					if( !( rel <= 1e-9 ) ) mismatched++;
					worst = std::fmax( worst, k < 0 ? 1e30 : rel );
					done = true;
				}
				if( done ) break;
			}
		}
	}
	char buf[256];
	std::snprintf( buf, sizeof(buf), "query %s: EvaluateKrayNM == ScatterNM's gap-ray krayNM on %d/%d draws (worst rel %.2e)%s",
		label, found - mismatched, found, worst, gated ? "" : "   [printed, not gated: DL-221]" );
	std::cout << "  " << buf << std::endl;
	if( gated ) {
		Check( found > 0 && mismatched == 0, buf );
	}
}

static void TestQueryMatchesSampler()
{
	std::cout << "=== query: DeltaPassThroughTransmittance == the SPF's own sampled pass-through ===" << std::endl;
	using namespace RISE::Implementation;

	UniformColorPainter*  white = new UniformColorPainter( RISEPel( 1.0, 1.0, 1.0 ) ); white->addref();
	UniformColorPainter*  tint  = new UniformColorPainter( RISEPel( 0.9, 0.5, 0.3 ) ); tint->addref();
	UniformScalarPainter* one   = new UniformScalarPainter( 1.0 );  one->addref();
	UniformScalarPainter* ior   = new UniformScalarPainter( 1.5 );  ior->addref();
	UniformScalarPainter* rgh   = new UniformScalarPainter( 0.05 ); rgh->addref();
	UniformScalarPainter* zed   = new UniformScalarPainter( 0.0 );  zed->addref();
	UniformScalarPainter* thick = new UniformScalarPainter( 0.4 );  thick->addref();
	UniformScalarPainter* absb  = new UniformScalarPainter( 1.2 );  absb->addref();
	UniformScalarPainter* alph  = new UniformScalarPainter( 0.5 );  alph->addref();
	UniformScalarPainter* rot   = new UniformScalarPainter( 0.6 );  rot->addref();

	RISE::WeaveTest::PresetWeave thin( "linen", 0.0, 0.5, /*whiteDyes=*/true,
	                                   /*thin=*/true, 0.25, 0.25, /*gapOverride=*/0.2 );
	RISE::WeaveTest::PresetWeave opaque( "linen", 0.0, 0.5, /*whiteDyes=*/true,
	                                     /*thin=*/false, -1, -1, /*gapOverride=*/0.2 );
	LambertianMaterial* lamb = new LambertianMaterial( *white ); lamb->addref();

	FabricMaterial* fabThin = new FabricMaterial( *thin.Material(), *white, *alph, *rot ); fabThin->addref();
	CoatedMaterial* coatThin = new CoatedMaterial( *thin.Material(), *one, *ior, *rgh, *zed, *zed, *white ); coatThin->addref();
	CoatedMaterial* coatTint = new CoatedMaterial( *thin.Material(), *one, *ior, *rgh, *thick, *absb, *tint ); coatTint->addref();
	CoatedMaterial* coatFab  = new CoatedMaterial( *fabThin, *one, *ior, *rgh, *zed, *zed, *white ); coatFab->addref();
	LambertianLuminaireMaterial* lum = new LambertianLuminaireMaterial( *white, 1.0, *thin.Material() ); lum->addref();
	// Two thin-weave layers 0.1 apart with an absorbing gap: the walk's only
	// straight exit is gap -> Beer crossing -> gap.
	UniformScalarPainter* ext = new UniformScalarPainter( 1.5 ); ext->addref();
	CompositeMaterial* compWW = new CompositeMaterial( *thin.Material(), *fabThin, 4, 2, 2, 2, 2, 0.1, *ext ); compWW->addref();
	CompositeMaterial* compWL = new CompositeMaterial( *thin.Material(), *lamb, 4, 2, 2, 2, 2, 0.1, *ext ); compWL->addref();

	// Capability flags.
	Check( thin.Material()->HasDeltaPassThrough(), "query: a `transmission thin` weave reports HasDeltaPassThrough" );
	Check( !opaque.Material()->HasDeltaPassThrough(), "query: a `transmission none` weave does NOT" );
	Check( !lamb->HasDeltaPassThrough(), "query: a Lambertian does NOT" );
	{
		// `silk` ships `transmission thin` with a uniform `gap 0`: its SPF
		// can never draw the gap lobe, so it must not claim the capability
		// (it would only make every delta-light shadow ray it occludes pay
		// for a walk that finds zero).  Same for an explicit gap 0.
		RISE::WeaveTest::PresetWeave silk( "silk", 0.0, 0.5, false, /*thin=*/true );
		RISE::WeaveTest::PresetWeave zeroGap( "linen", 0.0, 0.5, false, /*thin=*/true, 0.25, 0.25, /*gapOverride=*/0.0 );
		Check( silk.Material()->GetTransmission() == eWeaveTransmissionThin && !silk.Material()->HasDeltaPassThrough(),
			"query: `silk` (thin, uniform gap 0) does NOT report HasDeltaPassThrough" );
		Check( !zeroGap.Material()->HasDeltaPassThrough(), "query: a thin weave with gap 0 does NOT" );
		UniformScalarPainter* g3 = new UniformScalarPainter( 0.3 ); g3->addref();
		zeroGap.Material()->SetGap( *g3 );
		Check( zeroGap.Material()->HasDeltaPassThrough(), "query: ... and does once SetGap rebinds a non-zero gap" );
		safe_release( g3 );
	}
	Check( fabThin->HasDeltaPassThrough() && coatThin->HasDeltaPassThrough() && coatFab->HasDeltaPassThrough(),
		"query: fabric / coated / coated-over-fabric over a thin weave FORWARD it" );
	Check( lum->HasDeltaPassThrough(), "query: a luminaire wrapping a thin weave FORWARDS it" );
	Check( compWW->HasDeltaPassThrough(), "query: a composite of two pass-through layers reports it" );
	Check( !compWL->HasDeltaPassThrough(), "query: a composite over an opaque bottom does NOT (no straight exit exists)" );

	// The bare weave's value is the gap itself, deterministically.
	{
		const RayIntersectionGeometric ri = MakeHit( Vector3( 0.3, 0.0, 0.9539392 ) );
		const RISEPel t = thin.SPF()->DeltaPassThroughTransmittance( ri );
		Check( std::fabs( t[0] - 0.2 ) < 1e-12 && std::fabs( t[1] - 0.2 ) < 1e-12 && std::fabs( t[2] - 0.2 ) < 1e-12,
			"query: bare thin weave DeltaPassThroughTransmittance == gap (0.2) exactly" );
		const RISEPel t0 = opaque.SPF()->DeltaPassThroughTransmittance( ri );
		Check( t0[0] == 0 && t0[1] == 0 && t0[2] == 0, "query: `transmission none` weave reports 0" );
	}

	CheckQueryMatchesSampler( "weave(thin, gap 0.2)", *thin.Material() );
	CheckQueryMatchesSampler( "fabric(rot 0.6) over weave", *fabThin );
	CheckQueryMatchesSampler( "coated(clear) over weave", *coatThin );
	CheckQueryMatchesSampler( "coated(tinted, absorbing) over weave", *coatTint );
	CheckQueryMatchesSampler( "coated over fabric over weave", *coatFab );
	CheckQueryMatchesSampler( "luminaire over weave", *lum );
	CheckQueryMatchesSampler( "composite(weave | fabric-over-weave, gap 0.1 ext 1.5)", *compWW );

	CheckCompanionKrayMatchesScatter( "weave(thin, gap 0.2)", *thin.Material(), true );
	CheckCompanionKrayMatchesScatter( "fabric(rot 0.6) over weave", *fabThin, true );
	CheckCompanionKrayMatchesScatter( "coated(clear) over weave", *coatThin, true );
	CheckCompanionKrayMatchesScatter( "coated(tinted, absorbing) over weave", *coatTint, true );
	CheckCompanionKrayMatchesScatter( "coated over fabric over weave", *coatFab, true );
	CheckCompanionKrayMatchesScatter( "luminaire over weave", *lum, true );
	CheckCompanionKrayMatchesScatter( "composite(weave | fabric-over-weave)", *compWW, false );

	safe_release( compWL ); safe_release( compWW ); safe_release( ext );
	safe_release( lum ); safe_release( coatFab ); safe_release( coatTint ); safe_release( coatThin );
	safe_release( fabThin ); safe_release( lamb );
	safe_release( rot ); safe_release( alph ); safe_release( absb ); safe_release( thick );
	safe_release( zed ); safe_release( rgh ); safe_release( ior ); safe_release( one );
	safe_release( tint ); safe_release( white );
}

//////////////////////////////////////////////////////////////////////
// closed: omni light overhead.
//////////////////////////////////////////////////////////////////////
static std::string RastBDPTSpectral( unsigned int spp, bool hwss );	// defined below
static std::string RastVCMSpectral( unsigned int spp, bool hwss );	// defined below

static void TestClosedFormOmni()
{
	std::cout << "=== closed: receiver under ONE gapped weave sheet, omni light overhead ===" << std::endl;

	// Harness self-check: the no-sheet PT render against the analytic
	// point-light closed form rho/pi * I / d^2 (cos = 1 at the patch
	// centre; I = color * power -- PointLight.cpp's own convention).
	{
		const double L0 = Render( Assemble( RastPT( 16 ), ReceiverScene( kOmni, false, 0.0 ) ), "harness" );
		const double expected = kRho / 3.14159265358979323846 * kOmniPow / ( kLightY * kLightY );
		std::cout << "  harness: PT L0 = " << L0 << "   rho/pi * I/d^2 = " << expected << std::endl;
		Check( std::fabs( L0 / expected - 1.0 ) <= 0.01,
			"closed: harness -- no-sheet PT patch radiance matches rho/pi * I/d^2 within 1%" );
	}

	const double gaps[] = { 0.3, 0.1 };
	std::vector<RowSpec> rows;
	rows.push_back( { "PT RGB", RastPT( 16 ), 0.02, kTight } );
	rows.push_back( { "PT spectral hwss=false", RastPTSpectral( 64, false ), 0.03, kTight } );
	rows.push_back( { "PT spectral hwss=true", RastPTSpectral( 64, true ), 0.03, kTight } );
	// DL-425: the curtain case.  The see-through connection (DL-330) used
	// to take MIS weight 0 here -- light tracing covers the path -- so the
	// directly seen receiver was left to t = 1 splats alone: 0.0001 /
	// 2.20 (sd 4.07) / 0.0003 of g*L0 at 16 / 64 / 256 spp (n = 8,
	// salted).  MIS-weighted, it reads g*L0 to 0.04 % at 64 spp.
	rows.push_back( { "BDPT RGB (DL-425)", RastBDPT( 64 ), 0.02, kTight } );
	rows.push_back( { "BDPT spectral hwss=true (DL-425)", RastBDPTSpectral( 64, true ), 0.03, kTight } );
	// DL-424: VCM's own see-through NEE (merging and splats alone read
	// 2.02 (sd 3.78) / 1.21 (sd 1.30) of g*L0 at 64 / 256 spp).
	rows.push_back( { "VCM RGB (DL-424)", RastVCM( 64 ), 0.02, kTight } );
	rows.push_back( { "VCM spectral hwss=true (DL-424)", RastVCMSpectral( 64, true ), 0.03, kTight } );
	RunReceiverRows( "closed omni", kOmni, rows, gaps, 2 );

	// The bidirectional rows under the spot twin (see ReceiverScene's
	// kSpot and CamKind comments), plus PT once more through the same arm.
	std::vector<RowSpec> spotRows;
	spotRows.push_back( { "PT RGB", RastPT( 64 ), 0.02, kWide } );
	spotRows.push_back( { "BDPT RGB", RastBDPT( 1024 ), 0.02, kWide } );
	spotRows.push_back( { "VCM RGB", RastVCM( 1024 ), 0.02, kWide } );
	RunReceiverRows( "closed spot", kSpot, spotRows, gaps, 2 );
}

//////////////////////////////////////////////////////////////////////
// composite: a `composite_material` of two gapped thin weaves, spot light.
// Before DL-05's CompositeSPF override the composite reported no
// pass-through and PT read exactly 0 while BDPT read g^2 * L0.
//////////////////////////////////////////////////////////////////////
static void MeasureComposite( unsigned int n )
{
    std::vector<double> ratios;
    for( unsigned int i = 0; i < n; ++i ) {
        const double base = Render( Assemble( RastBDPT( 1024 ), ReceiverScene( kSpot, false, 0.0, kWide ) ), "composite_base" );
        const double gap = Render( Assemble( RastBDPT( 1024 ), ReceiverScene( kSpot, true, 0.3, kWide, true ) ), "composite_gap" );
        ratios.push_back( gap / base / 0.09 );
        std::cout << "composite salted ratio " << ratios.back() << std::endl;
    }
    double mean = 0, ss = 0;
    for( double q : ratios ) mean += q;
    mean /= n;
    for( double q : ratios ) ss += (q-mean)*(q-mean);
    std::cout << "composite calibration: mean=" << mean << " sd=" << std::sqrt(ss/(n-1)) << " n=" << n << std::endl;
}

static void TestClosedFormComposite()
{
	std::cout << "=== composite: receiver under a composite of two gapped weaves (gap 0.3 each), spot light ===" << std::endl;
	struct R { const char* label; std::string rast; double tol; };
	const R rows[] = { { "PT RGB", RastPT( 64 ), 0.02 }, { "BDPT RGB", RastBDPT( 1024 ), 0.06 } };
	// n=16 salted single-render relative ratio sd=0.088761; n=32
	// gives SE=0.01569, so the BDPT 0.06 band is 3.82 SE.
	const double g = 0.3, expected = g * g;
	for( const R& r : rows )
	{
		const double L0 = RenderMeanN( Assemble( r.rast, ReceiverScene( kSpot, false, 0.0, kWide ) ), "c_l0", 32 );
		const double L  = RenderMeanN( Assemble( r.rast, ReceiverScene( kSpot, true, g, kWide, true ) ), "c_lg", 32 );
		char buf[256];
		std::snprintf( buf, sizeof(buf), "composite %s: L/L0 = %.5f  (closed form g^2 = %.5f, rel err %+.3f%%)",
			r.label, L / L0, expected, 100.0 * ( L / L0 / expected - 1.0 ) );
		std::cout << "  " << buf << std::endl;
		Check( L0 > 0 && std::fabs( L / L0 / expected - 1.0 ) <= r.tol, buf );
	}
}

//////////////////////////////////////////////////////////////////////
// directional: Step-1 zero-exitance path (PT) and BDPT's deterministic
// zero-exitance sweep.  VCM has no directional-light sampling at all
// (CLAUDE.md; a separate, documented gap), so it has no row here.
//////////////////////////////////////////////////////////////////////
static void TestClosedFormDirectional()
{
	std::cout << "=== directional: receiver under ONE gapped weave sheet, directional light ===" << std::endl;
	const double gaps[] = { 0.3 };
	std::vector<RowSpec> rows;
	rows.push_back( { "PT RGB", RastPT( 16 ), 0.02, kTight } );
	rows.push_back( { "PT spectral hwss=true", RastPTSpectral( 64, true ), 0.03, kTight } );
	rows.push_back( { "BDPT RGB", RastBDPT( 64 ), 0.02, kTight } );
	RunReceiverRows( "directional", kDirectional, rows, gaps, 1 );
}

//////////////////////////////////////////////////////////////////////
// area: partition guard (see the file header).
//////////////////////////////////////////////////////////////////////
static void TestAreaPartitionGuard()
{
	std::cout << "=== area: receiver under ONE gapped weave sheet, small AREA emitter (partition guard) ===" << std::endl;
	const double gaps[] = { 0.3 };
	std::vector<RowSpec> rows;
	// PT, 5 %: six runs (seed bases 1000-4000, before and after DL-05)
	// put it at -1.42 .. +1.04 % (the pre-fix runs inside the post-fix
	// spread -- this light kind's shadow path is untouched); the failure
	// this row exists for, an area-light NEE arm that sees through the gap
	// while PT's BSDF-sampled continuation still reaches the emitter
	// through it at MIS weight 1, reads +103 % (measured by forcing that
	// arm).  BDPT is PRINTED, NOT GATED: over nine runs it reads -0.20 ..
	// -5.35 % (mean ~-2.2 %), before and after DL-05 alike -- a
	// pre-existing BDPT residual on this fixture that DL-05 does not touch
	// (docs/DL05_WEAVE_GAP_SHADOW_TRANSMITTANCE.md section 8, filed as
	// DL-330 at merge), and a band wide enough to hold it would say
	// nothing.
	rows.push_back( { "PT RGB", RastPT( 1024 ), 0.05, kWide } );
	rows.push_back( { "BDPT RGB", RastBDPT( 512 ), -1.0, kWide } );
	RunReceiverRows( "area", kArea, rows, gaps, 1 );
}

//////////////////////////////////////////////////////////////////////
// hwssgap: DL-329 / DL-330.  The area closed form (g * L0) again, now
// through the SPECTRAL rasterizers.  The path receiver -> gap -> emitter
// is reached ONLY by a Scatter()-sampled CONTINUATION through the delta
// gap lobe (the area NEE arm keeps its binary shadow, DL-05 section 2),
// so every HWSS companion lane is priced by the companion ladder at the
// gap vertex -- PT's `IntegrateFromHitHWSS` (DL-329: WeaveSPF had no
// `EvaluateKrayNM`, so a companion fell back to the CONTINUUM weave BSDF
// at the gap's undeviated direction and read ~0, dropping 3 of 4 lanes)
// and BDPT's `RecomputeSubpathThroughputNM`.
//
// Each row: n salted repeats (WEAVE_GAP_HWSS_N, default 4) of the
// (L0, L) pair through the SAME rasterizer, the ratio of the repeat
// means against g, and the per-repeat ratio's sd.  Rows run on the
// clipped-plane sheet AND (DOUBLE-SIDED rule) on a double-sided
// `indexedmesh_geometry` sheet.
//////////////////////////////////////////////////////////////////////
static void MeanSd( const std::vector<double>& v, double& mean, double& sd );
static std::string BlackWeaveSheet( double gap );	// defined with the sms section below
static std::string PerfectRefractorSheet( const char* ior );
static std::string BlackPainterChunk();
static std::string BlackWeaveChunk( const char* name, double gap );

static std::string RastBDPTSpectral( unsigned int spp, bool hwss )
{
	std::ostringstream ss;
	ss << "bdpt_spectral_rasterizer\n{\n\tsamples " << spp * SppScale()
	   << "\n\tmax_eye_depth 8\n\tmax_light_depth 8\n\tpixel_filter box\n\toidn_denoise FALSE\n"
	   << "\tnmbegin 380\n\tnmend 720\n\tnum_wavelengths 8\n\tspectral_samples 1\n"
	   << "\thwss " << ( hwss ? "true" : "false" ) << "\n}\n\n" << kOutputChunk;
	return ss.str();
}

//! DL-424: VCM spectral twin of RastBDPTSpectral (merging on).
static std::string RastVCMSpectral( unsigned int spp, bool hwss )
{
	std::ostringstream ss;
	ss << "vcm_spectral_rasterizer\n{\n\tsamples " << spp * SppScale()
	   << "\n\tmax_eye_depth 8\n\tmax_light_depth 8\n\tmerge_radius 0.0\n\tvc_enabled true\n\tvm_enabled true\n"
	   << "\tpixel_filter box\n\toidn_denoise FALSE\n"
	   << "\tnmbegin 380\n\tnmend 720\n\tnum_wavelengths 8\n\tspectral_samples 1\n"
	   << "\thwss " << ( hwss ? "true" : "false" ) << "\n}\n\n" << kOutputChunk;
	return ss.str();
}

static unsigned int HwssRepeats()
{
	const char* s = std::getenv( "WEAVE_GAP_HWSS_N" );
	const long v = s ? std::strtol( s, nullptr, 10 ) : 4;
	return v > 1 ? (unsigned int)v : 4u;
}

struct HwssRow
{
	const char* label;
	std::string rast;
	bool mesh;
	double tol;		// two-sided band on mean(L)/mean(L0)/g - 1; < 0 = print only
	int sheet;		// 0 black-yarn weave (exact g * L0); 1 DL-05's own `fabric custom` sheet (reflective
					// yarn, NOT an exact closed form); 2 an ior-1.0 perfect refractor (exact 1 * L0)
};

//! Renders @a row's (L0, L) pair @a n times with independent Sobol' salts
//! and returns mean(L)/mean(L0); @a sdRatio receives the per-repeat
//! ratio's sample sd.
static double HwssRatio( const HwssRow& row, LightKind light, double g, unsigned int n, unsigned int saltBase,
	double& sdRatio, double& meanL0 )
{
	std::vector<double> l0s, ls, rs;
	const bool savedMesh = g_meshSheet;
	g_meshSheet = row.mesh;
	for( unsigned int r = 0; r < n; r++ )
	{
		const unsigned int salt = SobolSequence::HashCombine( saltBase + g_seedBase, r );
		const double L0 = RenderSalted( Assemble( row.rast, ReceiverScene( light, false, 0.0, kWide ) ), "h_l0", salt );
		const double L  = RenderSalted( Assemble( row.rast, ReceiverScene( light, true, g, kWide, false,
			row.sheet == 1 ? std::string() : row.sheet == 2 ? PerfectRefractorSheet( "1.0" ) : BlackWeaveSheet( g ) ) ), "h_lg",
			SobolSequence::HashCombine( salt, 0x51u ) );
		l0s.push_back( L0 ); ls.push_back( L );
		if( L0 > 0 ) rs.push_back( L / L0 );
	}
	g_meshSheet = savedMesh;
	double mL0 = 0, sdL0 = 0, mL = 0, sdL = 0, mR = 0;
	MeanSd( l0s, mL0, sdL0 );
	MeanSd( ls, mL, sdL );
	MeanSd( rs, mR, sdRatio );
	meanL0 = mL0;
	return mL0 > 0 ? mL / mL0 : -1.0;
}

static void TestHWSSGapContinuation()
{
	std::cout << "=== hwssgap: AREA closed form through the spectral rasterizers (DL-329 / DL-330) ===" << std::endl;
	// Measurement aids: WEAVE_GAP_HWSS_G overrides the gap (gating then
	// still applies against the overridden g); WEAVE_GAP_HWSS_ROWS keeps
	// only rows whose label contains that substring.
	const char* gEnv = std::getenv( "WEAVE_GAP_HWSS_G" );
	const double g = gEnv ? std::strtod( gEnv, nullptr ) : 0.3;
	const char* rowFilter = std::getenv( "WEAVE_GAP_HWSS_ROWS" );
	// WEAVE_GAP_HWSS_INDEPENDENT=1: SobolSamplerTestHooks::Independent --
	// i.i.d. draws through the identical code path, the unbiased
	// reference no Sobol' dimension correlation can reach.
	const bool independent = std::getenv( "WEAVE_GAP_HWSS_INDEPENDENT" ) != nullptr;
	SobolSamplerTestHooks::Independent().store( independent );
	if( independent ) std::cout << "  (SobolSamplerTestHooks::Independent ON)" << std::endl;
	const unsigned int n = HwssRepeats();
	// BLACK-yarn sheet (BlackWeaveSheet: no fibre albedo, no Fresnel):
	// nothing reflects between the receiver and the sheet's underside, so
	// L = g * L0 EXACTLY.  The `custom` rows are DL-05's own fixture,
	// whose reflective yarn adds a receiver <-> sheet interreflection
	// that is NOT negligible on this 2 x 2 patch (printed only).
	std::vector<HwssRow> rows;
	// Gated: the two PT hwss=true rows (DL-329 read 0.0765 / 0.0742, -75 %,
	// here).  Their per-repeat sd at 2048 spp is ~1-2.5 % (a BSDF sample
	// must find the 0.5 x 0.5 emitter through the gap), so 6 % is >= 4.8
	// se at n = 4.  PT RGB and hwss=false are not DL-329 targets and carry
	// 4-6 % per-repeat sd at 1024 spp: printed.
	rows.push_back( { "PT RGB",                          RastPT( 1024 ),                  false, -1.0, 0 } );
	rows.push_back( { "PT spectral hwss=false",          RastPTSpectral( 1024, false ),   false, -1.0, 0 } );
	rows.push_back( { "PT spectral hwss=true",           RastPTSpectral( 2048, true ),    false, 0.06, 0 } );
	rows.push_back( { "PT spectral hwss=true  (mesh)",   RastPTSpectral( 2048, true ),    true,  0.06, 0 } );
	rows.push_back( { "BDPT RGB",                        RastBDPT( 512 ),                 false, -1.0, 0 } );
	rows.push_back( { "BDPT spectral hwss=false",        RastBDPTSpectral( 512, false ),  false, -1.0, 0 } );
	rows.push_back( { "BDPT spectral hwss=true",         RastBDPTSpectral( 512, true ),   false, -1.0, 0 } );
	rows.push_back( { "BDPT spectral hwss=true  (mesh)", RastBDPTSpectral( 512, true ),   true,  -1.0, 0 } );
	rows.push_back( { "VCM RGB",                         RastVCM( 512 ),                  false, -1.0, 0 } );
	rows.push_back( { "PT RGB            (custom)",      RastPT( 1024 ),                  false, -1.0, 1 } );
	rows.push_back( { "PT spectral hwss=true (custom)",  RastPTSpectral( 1024, true ),    false, -1.0, 1 } );
	rows.push_back( { "BDPT RGB          (custom)",      RastBDPT( 512 ),                 false, -1.0, 1 } );
	rows.push_back( { "PT RGB            (refractor 1.0)", RastPT( 1024 ),                 false, -1.0, 2 } );
	rows.push_back( { "BDPT RGB          (refractor 1.0)", RastBDPT( 512 ),                false, -1.0, 2 } );
	unsigned int k = 0;
	for( const HwssRow& row : rows )
	{
		const unsigned int rowSalt = 0xD329u + 0x100u * k++;
		if( rowFilter && !std::strstr( row.label, rowFilter ) ) continue;
		double sd = 0, L0 = 0;
		const double ratio = HwssRatio( row, kArea, g, n, rowSalt, sd, L0 );
		const double cf = ( row.sheet == 2 ) ? 1.0 : g;
		char buf[320];
		std::snprintf( buf, sizeof(buf),
			"hwssgap %s gap %.2f: L/L0 = %.5f +/- %.5f (sd, n = %u)  (closed form %.5f, rel err %+.3f%%)  L0 = %.6g",
			row.label, g, ratio, sd, n, cf, 100.0 * ( ratio / cf - 1.0 ), L0 );
		std::cout << "  " << buf << ( row.tol < 0 ? "   [printed, not gated]" : "" ) << std::endl;
		if( row.tol >= 0 ) {
			Check( ratio > 0 && std::fabs( ratio / cf - 1.0 ) <= row.tol, buf );
		}
	}
	SobolSamplerTestHooks::Independent().store( false );
}

//////////////////////////////////////////////////////////////////////
// seethrough: DL-330.  The closed-form receiver (kTight, omni overhead)
// with a SECOND black-yarn gapped weave hung VERTICALLY between the
// camera and the patch (z = 0.6, x in [-1, 1], y in [-0.5, 1.6]: every
// camera ray crosses it, no light ray from the omni to the patch does).
// The only light path is  L - S(horizontal gap) - D(patch) - S(vertical
// gap) - E:  every edge has a delta end, so before DL-330 NO BDPT
// strategy generated it (BDPT read 0; no BSDF sample can hit a point
// light) while PT reads it through DL-05's see-through NEE and VCM by
// merging.  Closed form: L = g * g * L0 exactly (black yarn: nothing
// reflects; L0 is the same rasterizer's render with neither sheet).
// The horizontal sheet is also run as the double-sided mesh.
//////////////////////////////////////////////////////////////////////
static std::string VerticalBlackSheetChunks()
{
	return "clippedplane_geometry\n{\n\tname geo_vsheet\n"
		"\tpta -1 -0.5 0.6\n\tptb 1 -0.5 0.6\n\tptc 1 1.6 0.6\n\tptd -1 1.6 0.6\n"
		"\tdoublesided TRUE\n}\n\n"
		"standard_object\n{\n\tname obj_vsheet\n\tgeometry geo_vsheet\n\tmaterial mat_sheet\n}\n\n";
}

static void TestBDPTSeeThroughDeltaLight()
{
	std::cout << "=== seethrough: L - gap - patch - gap - camera, omni light (DL-330) ===" << std::endl;
	const double g = 0.3, cf = g * g;
	const unsigned int n = 4;
	struct R { const char* label; std::string rast; bool mesh; double tol; };
	const R rows[] = {
		{ "PT RGB",                          RastPT( 16 ),                    false, 0.02 },
		{ "BDPT RGB",                        RastBDPT( 64 ),                  false, 0.03 },
		{ "BDPT RGB         (mesh)",         RastBDPT( 64 ),                  true,  0.03 },
		{ "BDPT spectral hwss=true",         RastBDPTSpectral( 512, true ),   false, 0.04 },
		{ "BDPT spectral hwss=true  (mesh)", RastBDPTSpectral( 512, true ),   true,  0.04 },
		// DL-424: printed only before VCM had the see-through NEE (merging
		// alone: 0.137 +/- 0.27 sd); now 1.000 of g^2 at 64 spp.
		{ "VCM RGB",                         RastVCM( 256 ),                  false, 0.03 },
	};
	unsigned int k = 0;
	for( const R& r : rows )
	{
		std::vector<double> l0s, ls, rs;
		g_meshSheet = r.mesh;
		for( unsigned int rep = 0; rep < n; rep++ ) {
			const unsigned int salt = SobolSequence::HashCombine( 0xD330u + 0x100u * k + g_seedBase, rep );
			const double L0 = RenderSalted( Assemble( r.rast, ReceiverScene( kOmni, false, 0.0, kTight ) ), "st_l0", salt );
			const double L  = RenderSalted( Assemble( r.rast, ReceiverScene( kOmni, true, g, kTight, false,
				BlackWeaveSheet( g ) + VerticalBlackSheetChunks() ) ), "st_lg", SobolSequence::HashCombine( salt, 0x51u ) );
			l0s.push_back( L0 ); ls.push_back( L );
			if( L0 > 0 ) rs.push_back( L / L0 );
		}
		g_meshSheet = false;
		k++;
		double m0, s0, m1, s1, mr, sr;
		MeanSd( l0s, m0, s0 ); MeanSd( ls, m1, s1 ); MeanSd( rs, mr, sr );
		const double ratio = m0 > 0 ? m1 / m0 : -1.0;
		char buf[320];
		std::snprintf( buf, sizeof(buf),
			"seethrough %s: L/L0 = %.5f +/- %.5f (sd, n = %u)  (closed form g^2 = %.5f, rel err %+.3f%%)",
			r.label, ratio, sr, n, cf, 100.0 * ( ratio / cf - 1.0 ) );
		std::cout << "  " << buf << ( r.tol < 0 ? "   [printed, not gated]" : "" ) << std::endl;
		if( r.tol >= 0 ) {
			Check( ratio > 0 && std::fabs( ratio / cf - 1.0 ) <= r.tol, buf );
		}
	}
}

//////////////////////////////////////////////////////////////////////
// sssgap: DL-330 review P1.  An SSS receiver (the kWide patch as
// `subsurfacescattering_material` or `randomwalk_sss_material`) under the
// black-yarn gapped sheet, a DELTA light above it, the camera seeing the
// receiver directly.  BDPT's light family samples  L - gap - B ~jump~ A
// and splats / connects from A (BDPTUtilities::LightJumpPartition keeps
// it: no plain split covers [L, gap, B]), while the eye family samples
// E - A ~jump~ B and reaches L from B only through the see-through
// connection -- so the partition must hand the path to exactly one of
// them.  Before the fix both counted it (BDPT 2.06x the closed form).
// Closed form: L = g * L0 exactly (the sheet attenuates every light path
// to the receiver by g; whatever the receiver does with the light, it
// does with and without the sheet).  The random-walk rows use a closed
// sphere receiver (g_recvSphere).  Default caps, a shallow eye cap
// (max_eye_depth 1: the eye walk still reaches A and jumps to B, so the
// eye family owns the path) and a shallow light cap (max_light_depth 1:
// the light walk cannot reach B behind the gap, the eye family alone).
//////////////////////////////////////////////////////////////////////
static std::string RastBDPTDepth( unsigned int spp, unsigned int eyeDepth, unsigned int lightDepth )
{
	std::ostringstream ss;
	ss << "bdpt_pel_rasterizer\n{\n\tsamples " << spp * SppScale()
	   << "\n\tmax_eye_depth " << eyeDepth << "\n\tmax_light_depth " << lightDepth
	   << "\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n" << kOutputChunk;
	return ss.str();
}

//! @a bootstrap: PSSMLT never reads the Sobol' salt and renders
//! bit-identically run to run, so a caller that wants INDEPENDENT MLT
//! draws varies the bootstrap sample count (which reseeds every chain).
static std::string RastMLT( unsigned int mutations, unsigned int eyeDepth, unsigned int lightDepth,
	unsigned int bootstrap = 200000 )
{
	std::ostringstream ss;
	ss << "mlt_rasterizer\n{\n\tmax_eye_depth " << eyeDepth << "\n\tmax_light_depth " << lightDepth
	   << "\n\tbootstrap_samples " << bootstrap << "\n\tchains 256\n\tmutations_per_pixel " << mutations * SppScale()
	   << "\n\tlarge_step_prob 0.3\n\toidn_denoise FALSE\n}\n\n" << kOutputChunk;
	return ss.str();
}

static std::string SSSReceiverChunks( bool randomWalk )
{
	return randomWalk
		? "randomwalk_sss_material\n{\n\tname mat_recv\n\tior 1.3\n\tabsorption 0.1\n\tscattering 10.0\n\tg 0.0\n\troughness 0.3\n}\n\n"
		: "subsurfacescattering_material\n{\n\tname mat_recv\n\tior 1.3\n\tabsorption 0.1\n\tscattering 1.0\n\tg 0.0\n\troughness 0.3\n}\n\n";
}

static void TestSSSBehindGap()
{
	std::cout << "=== sssgap: SSS receiver behind a gapped weave, delta light (DL-330 review P1) ===" << std::endl;
	const double g = 0.3;
	const unsigned int n = 4;
	struct R { const char* label; LightKind light; bool rw; std::string rast; double tol; };
	const R rows[] = {
		{ "spot, diffusion SSS, PT",                 kSpot, false, RastPT( 256 ),              0.06 },
		{ "spot, diffusion SSS, BDPT 8/8",           kSpot, false, RastBDPTDepth( 256, 8, 8 ), 0.06 },
		{ "spot, diffusion SSS, BDPT eye 1",         kSpot, false, RastBDPTDepth( 256, 1, 8 ), 0.06 },
		{ "spot, diffusion SSS, BDPT light 1",       kSpot, false, RastBDPTDepth( 256, 8, 1 ), 0.06 },
		{ "spot, diffusion SSS, MLT 8/8",            kSpot, false, RastMLT( 256, 8, 8 ),       0.10 },
		{ "spot, random-walk SSS sphere, PT",        kSpot, true,  RastPT( 256 ),              0.06 },
		{ "spot, random-walk SSS sphere, BDPT 8/8",  kSpot, true,  RastBDPTDepth( 256, 8, 8 ), 0.06 },
		{ "spot, random-walk SSS sphere, BDPT eye 1",kSpot, true,  RastBDPTDepth( 256, 1, 8 ), 0.06 },
		{ "spot, random-walk SSS sphere, MLT 8/8",   kSpot, true,  RastMLT( 256, 8, 8 ),       0.10 },
		// Omni: measured per-repeat sd 0.3-1.2 % at 1024 spp (the
		// see-through connection is an NEE estimator), so 6 % is >= 10 se.
		{ "omni, diffusion SSS, PT",                 kOmni, false, RastPT( 256 ),               0.06 },
		{ "omni, diffusion SSS, BDPT 8/8",           kOmni, false, RastBDPTDepth( 1024, 8, 8 ), 0.06 },
		{ "omni, random-walk SSS sphere, BDPT 8/8",  kOmni, true,  RastBDPTDepth( 1024, 8, 8 ), 0.06 },
	};
	unsigned int k = 0;
	for( const R& r : rows )
	{
		std::vector<double> l0s, ls, rs;
		g_recvSphere = r.rw;
		const bool isMLT = r.rast.find( "mlt_rasterizer" ) != std::string::npos;
		for( unsigned int rep = 0; rep < n; rep++ ) {
			const unsigned int salt = SobolSequence::HashCombine( 0xD330Bu + 0x100u * k + g_seedBase, rep );
			// MLT: an independent draw per render through its bootstrap count.
			const std::string rast0 = isMLT ? RastMLT( 256, 8, 8, 200000 + 1000 * rep ) : r.rast;
			const std::string rastG = isMLT ? RastMLT( 256, 8, 8, 200500 + 1000 * rep ) : r.rast;
			const double L0 = RenderSalted( Assemble( rast0, ReceiverScene( r.light, false, 0.0, kWide, false,
				std::string(), SSSReceiverChunks( r.rw ) ) ), "sg_l0", salt );
			const double L  = RenderSalted( Assemble( rastG, ReceiverScene( r.light, true, g, kWide, false,
				BlackWeaveSheet( g ), SSSReceiverChunks( r.rw ) ) ), "sg_lg", SobolSequence::HashCombine( salt, 0x51u ) );
			l0s.push_back( L0 ); ls.push_back( L );
			if( L0 > 0 ) rs.push_back( L / L0 );
		}
		g_recvSphere = false;
		k++;
		double m0, s0, m1, s1, mr, sr;
		MeanSd( l0s, m0, s0 ); MeanSd( ls, m1, s1 ); MeanSd( rs, mr, sr );
		const double ratio = m0 > 0 ? m1 / m0 : -1.0;
		char buf[320];
		std::snprintf( buf, sizeof(buf),
			"sssgap %s: L/L0 = %.5f +/- %.5f (sd, n = %u)  (closed form %.5f, rel err %+.3f%%)  L0 = %.6g",
			r.label, ratio, sr, n, g, 100.0 * ( ratio / g - 1.0 ), m0 );
		std::cout << "  " << buf << ( r.tol < 0 ? "   [printed, not gated]" : "" ) << std::endl;
		if( r.tol >= 0 ) {
			Check( ratio > 0 && std::fabs( ratio / g - 1.0 ) <= r.tol, buf );
		}
	}
}

//////////////////////////////////////////////////////////////////////
// hwsstint: DL-329's CHROMATIC half.  A RED-tinted, absorbing
// `coated_material` over the black-yarn gapped weave: the gap ray's
// companion kray is the coat's two-crossing transmittance AT THE
// COMPANION WAVELENGTH (CoatedSPF::EvaluateKrayNM; BDPT's companion
// ladder reads its ratio at the gap vertex,
// RecomputeSubpathThroughputNM).  Reference-free invariant: hwss TRUE
// must reproduce hwss FALSE's per-channel transmittance L/L0 (the
// hero-only render prices every wavelength by its own Scatter).  The
// 4 x 4 emitter (kAreaLarge) keeps the BSDF-sampled gap path cheap.
//////////////////////////////////////////////////////////////////////
static std::string TintedCoatOverWeaveSheet()
{
	return BlackPainterChunk() + BlackWeaveChunk( "mat_layer", 0.3 )
		+ "uniformcolor_painter\n{\n\tname pnt_tint\n\tcolor 0.9 0.3 0.1\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		  "coated_material\n{\n\tname mat_sheet\n\tbase mat_layer\n\tcoat_weight 1.0\n\tcoat_ior 1.5\n"
		  "\tcoat_roughness 0.05\n\tcoat_thickness 0.2\n\tcoat_absorption 0.5\n\tcoat_tint pnt_tint\n}\n\n";
}

static void ChannelMeans( const std::vector<RISEColor>& px, double out[3] )
{
	out[0] = out[1] = out[2] = 0;
	for( const RISEColor& c : px ) {
		out[0] += c.base.r * c.a; out[1] += c.base.g * c.a; out[2] += c.base.b * c.a;
	}
	for( int i = 0; i < 3; i++ ) out[i] /= px.empty() ? 1.0 : double( px.size() );
}

//! Per-channel mean(L)/mean(L0) over @a n salted (L0, L) repeats of the
//! tinted-coat sheet under @a rast; @a sd receives the per-repeat ratio sd.
static void TintedChannelTransmittance( const std::string& rast, unsigned int n, unsigned int saltBase,
	double tr[3], double sd[3] )
{
	std::vector<double> l0[3], ls[3], rr[3];
	for( unsigned int rep = 0; rep < n; rep++ ) {
		const unsigned int salt = SobolSequence::HashCombine( saltBase + g_seedBase, rep );
		double a[3], b[3];
		RenderSalted( Assemble( rast, ReceiverScene( kAreaLarge, false, 0.0, kWide ) ), "t_l0", salt );
		ChannelMeans( g_lastPixels, a );
		RenderSalted( Assemble( rast, ReceiverScene( kAreaLarge, true, 0.3, kWide, false, TintedCoatOverWeaveSheet() ) ),
			"t_lg", SobolSequence::HashCombine( salt, 0x51u ) );
		ChannelMeans( g_lastPixels, b );
		for( int c = 0; c < 3; c++ ) {
			l0[c].push_back( a[c] ); ls[c].push_back( b[c] );
			if( a[c] > 0 ) rr[c].push_back( b[c] / a[c] );
		}
	}
	for( int c = 0; c < 3; c++ ) {
		double m0, s0, m1, s1, mr;
		MeanSd( l0[c], m0, s0 ); MeanSd( ls[c], m1, s1 ); MeanSd( rr[c], mr, sd[c] );
		tr[c] = m0 > 0 ? m1 / m0 : -1.0;
	}
}

static void TestHWSSTintedCoatGap()
{
	std::cout << "=== hwsstint: tinted coat over a gapped weave, BDPT HWSS vs PT HWSS per channel (DL-329) ===" << std::endl;
	const char* nEnv = std::getenv( "WEAVE_GAP_HWSS_N" );
	const unsigned int n = nEnv ? HwssRepeats() : 8u;
	// The REFERENCE is PT's own HWSS render: its companion lanes are the
	// CoatedSPF::EvaluateKrayNM numbers the `query` section pins to
	// ScatterNM exactly.  The hwss=false renders are printed, not used:
	// a single-wavelength sample of this saturated red is far outside
	// Rec.709 and the per-sample conversion does not average to the
	// bundle's (its blue channel reads > 0 where the bundle's reads 0).
	double pt[3], ptSd[3], bd[3], bdSd[3], ptOff[3], bdOff[3], tmp[3];
	TintedChannelTransmittance( RastPTSpectral( 256, true ),    n, 0xD32A0u, pt, ptSd );
	TintedChannelTransmittance( RastBDPTSpectral( 128, true ),  n, 0xD32A1u, bd, bdSd );
	TintedChannelTransmittance( RastPTSpectral( 256, false ),   n, 0xD32A2u, ptOff, tmp );
	TintedChannelTransmittance( RastBDPTSpectral( 128, false ), n, 0xD32A3u, bdOff, tmp );
	char buf[480];
	std::snprintf( buf, sizeof(buf),
		"hwsstint: L/L0 (R, G)  PT hwss=true (%.5f +/- %.5f, %.5f +/- %.5f)  BDPT hwss=true (%.5f +/- %.5f, %.5f +/- %.5f)  "
		"BDPT/PT (%.4f, %.4f)  [hwss=false: PT (%.5f, %.5f) BDPT (%.5f, %.5f)]  n = %u",
		pt[0], ptSd[0], pt[1], ptSd[1], bd[0], bdSd[0], bd[1], bdSd[1], bd[0] / pt[0], bd[1] / pt[1],
		ptOff[0], ptOff[1], bdOff[0], bdOff[1], n );
	std::cout << "  " << buf << std::endl;
	Check( std::fabs( bd[0] / pt[0] - 1.0 ) <= 0.04 && std::fabs( bd[1] / pt[1] - 1.0 ) <= 0.06, buf );
}

//////////////////////////////////////////////////////////////////////
// sms: DL-295.  PT with `sms_enabled TRUE` used to drop EVERY emitter
// hit reached through a delta lobe of a material SMS does not treat as
// a specular caster -- PART 3 set `considerEmission = false` after any
// delta scatter, and PART 1's `smsSuppressEmission` latch suppressed
// the emitter after any delta scatter that followed a diffuse vertex,
// both on the premise that SMS covers that specular chain.  SMS builds
// and validates its chains from `IMaterial::GetSpecularInfo().isSpecular`
// alone (ManifoldSolver's seed trace stops at, and its chain-visibility
// test is blocked by, any hit that reports false), and a weave -- like a
// composite, a fabric or coated wrapper over one, a luminaire wrapper --
// reports false, so a chain through a weave gap has NO estimator under
// SMS.  Every scene here contains no SMS caster at all, so SMS on must
// equal SMS off; the closed forms are DL-05's own (g * L0, g^2 * L0).
//
// HWSS rows are SMS-on vs SMS-off PARITY, not closed forms: PT-HWSS
// prices a Scatter()-sampled gap continuation with the continuum BSDF
// on its companion lanes (DL-329, open), which moves SMS on and off
// identically.
//////////////////////////////////////////////////////////////////////

//! A closed box of BLACK-yarn gapped weave around the kWide receiver and
//! camera, lit by the uniform L = 1 environment only.  The yarn is black
//! (`warp/weft_color` 0, `warp/weft_ior` 1 -- no volume albedo, no fibre
//! Fresnel), so every direction the receiver sees is the gap's `g` times
//! the environment and nothing is reflected back: L = g * rho * L_env
//! exactly, L0 (no box) = rho * L_env.  PT reaches it ONLY by a
//! BSDF-sampled continuation through the gap and the ENV-ESCAPE branch
//! (the env NEE arm keeps its binary shadow, DL-05 section 2).
static std::string PlaneChunk( const char* name, const char* pts );	// defined with the design-doc topology below

static std::string EnvBoxScene( bool withBox, double gap )
{
	std::ostringstream ss;
	ss <<
		"film\n{\n\twidth 16\n\theight 16\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 1 1.2\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 10.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_recv\n\tcolor " << kRho << " " << kRho << " " << kRho
			<< "\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		"lambertian_material\n{\n\tname mat_recv\n\treflectance pnt_recv\n}\n\n"
		"clippedplane_geometry\n{\n\tname geo_recv\n"
			"\tpta -1 0 1\n\tptb 1 0 1\n\tptc 1 0 -1\n\tptd -1 0 -1\n\tdoublesided TRUE\n}\n\n"
		"standard_object\n{\n\tname obj_recv\n\tgeometry geo_recv\n\tmaterial mat_recv\n}\n\n";
	if( withBox ) {
		ss <<
			"uniformcolor_painter\n{\n\tname pnt_black\n\tcolor 0 0 0\n\tcolorspace Rec709RGB_Linear\n}\n\n"
			"weave_material\n{\n\tname mat_box\n\tfabric custom\n\ttransmission thin\n\tgap " << gap << "\n"
			"\twarp_color pnt_black\n\tweft_color pnt_black\n\twarp_ior 1.0\n\tweft_ior 1.0\n}\n\n"
			<< PlaneChunk( "bx_top", "\tpta -3 3 3\n\tptb 3 3 3\n\tptc 3 3 -3\n\tptd -3 3 -3\n" )
			<< PlaneChunk( "bx_bot", "\tpta -3 -1 3\n\tptb 3 -1 3\n\tptc 3 -1 -3\n\tptd -3 -1 -3\n" )
			<< PlaneChunk( "bx_px", "\tpta 3 -1 3\n\tptb 3 3 3\n\tptc 3 3 -3\n\tptd 3 -1 -3\n" )
			<< PlaneChunk( "bx_nx", "\tpta -3 -1 3\n\tptb -3 3 3\n\tptc -3 3 -3\n\tptd -3 -1 -3\n" )
			<< PlaneChunk( "bx_pz", "\tpta -3 -1 3\n\tptb 3 -1 3\n\tptc 3 3 3\n\tptd -3 3 3\n" )
			<< PlaneChunk( "bx_nz", "\tpta -3 -1 -3\n\tptb 3 -1 -3\n\tptc 3 3 -3\n\tptd -3 3 -3\n" );
	}
	return ss.str();
}

//! One ratio row: L(scene) / L(reference scene) against @a expected,
//! gated at @a tol (relative) when tol >= 0.
static void RatioRow( const char* label, const std::string& rast, const std::string& refScene,
	const std::string& scene, double expected, double tol )
{
	const double L0 = Render( Assemble( rast, refScene ), "sms_l0" );
	const double L  = Render( Assemble( rast, scene ), "sms_l" );
	char buf[320];
	std::snprintf( buf, sizeof(buf), "sms %s: L/L0 = %.5f  (closed form %.5f, rel err %+.3f%%)  [L0 %.6f]",
		label, L / L0, expected, 100.0 * ( L / L0 / expected - 1.0 ), L0 );
	std::cout << "  " << buf << ( tol < 0 ? "   [printed, not gated]" : "" ) << std::endl;
	if( tol >= 0 ) {
		Check( L0 > 0 && L >= 0 && std::fabs( L / L0 / expected - 1.0 ) <= tol, buf );
	}
}

//! SMS-on vs SMS-off on the SAME scene (no SMS caster in it): must agree.
static void ParityRow( const char* label, const std::string& rastOn, const std::string& rastOff,
	const std::string& scene, double tol )
{
	const double Lon  = Render( Assemble( rastOn, scene ), "sms_on" );
	const double Loff = Render( Assemble( rastOff, scene ), "sms_off" );
	char buf[320];
	std::snprintf( buf, sizeof(buf), "sms parity %s: SMS on %.6f / off %.6f = %.5f (rel %+.3f%%)",
		label, Lon, Loff, Lon / Loff, 100.0 * ( Lon / Loff - 1.0 ) );
	std::cout << "  " << buf << ( tol < 0 ? "   [printed, not gated]" : "" ) << std::endl;
	if( tol >= 0 ) {
		Check( Loff > 0 && Lon >= 0 && std::fabs( Lon / Loff - 1.0 ) <= tol, buf );
	}
}

//! SMS on / SMS off must fall in [lo, hi] (the DL-372 split rows).
static void RatioBandRow( const char* label, const std::string& rastOn, const std::string& rastOff,
	const std::string& scene, double lo, double hi )
{
	const double Lon  = Render( Assemble( rastOn, scene ), "sms_band_on" );
	const double Loff = Render( Assemble( rastOff, scene ), "sms_band_off" );
	char buf[320];
	std::snprintf( buf, sizeof(buf), "sms split %s: SMS on %.6f / off %.6f = %.5f (band [%.3f, %.3f])",
		label, Lon, Loff, Lon / Loff, lo, hi );
	std::cout << "  " << buf << std::endl;
	Check( Loff > 0 && Lon >= 0 && Lon / Loff >= lo && Lon / Loff <= hi, buf );
}

//! Sibling materials for the parity rows.
//!  * composite(dielectric over gapped weave): the CompositeSPF walker's
//!    delta-TAGGED exits, which leave in a REFRACTED (not incoming)
//!    direction -- delta, not a pass-through, and not an SMS caster.
//!  * a perfect refractor at ior 1: a non-bending delta that IS an SMS
//!    caster (GetSpecularInfo isSpecular), so SMS's premise holds and the
//!    suppression is correct there -- the control for the rule.
//! A BLACK-yarn gapped weave named @a name (`warp/weft_color` 0 and
//! `warp/weft_ior` 1: no volume albedo, no fibre Fresnel), so the sheet
//! transmits exactly its gap and reflects nothing -- no receiver <->
//! sheet interreflection, no CompositeSPF interreflected walker exit, and
//! every closed form below is exact.
static std::string BlackWeaveChunk( const char* name, double gap )
{
	std::ostringstream ss;
	ss << "weave_material\n{\n\tname " << name << "\n\tfabric custom\n\ttransmission thin\n\tgap " << gap << "\n"
	      "\twarp_color pnt_black\n\tweft_color pnt_black\n\twarp_ior 1.0\n\tweft_ior 1.0\n}\n\n";
	return ss.str();
}

static std::string BlackPainterChunk()
{
	return "uniformcolor_painter\n{\n\tname pnt_black\n\tcolor 0 0 0\n\tcolorspace Rec709RGB_Linear\n}\n\n";
}

static std::string BlackWeaveSheet( double gap )
{
	return BlackPainterChunk() + BlackWeaveChunk( "mat_sheet", gap );
}

//! A `composite_material` of two black-yarn gapped weaves: its only
//! transmission is CompositeSPF's straight gap -> gap walker exit, g^2.
static std::string CompositeTwoBlackWeavesSheet( double gap )
{
	return BlackPainterChunk() + BlackWeaveChunk( "mat_layer", gap )
		+ "composite_material\n{\n\tname mat_sheet\n\ttop mat_layer\n\tbottom mat_layer\n}\n\n";
}

static std::string CompositeDielectricOverWeaveSheet()
{
	return BlackPainterChunk() + BlackWeaveChunk( "mat_layer", 0.3 )
		+ "dielectric_material\n{\n\tname mat_glass\n\ttau 1.0\n\tior 1.5\n\tscattering 1000000\n}\n\n"
		  "composite_material\n{\n\tname mat_sheet\n\ttop mat_glass\n\tbottom mat_layer\n}\n\n";
}

//! The DL-05 forwarding wrappers over a black-yarn gapped weave: the
//! gap reaches the continuation as the wrapper's own delta ray, and
//! neither wrapper reports an SMS caster.
static std::string FabricOverWeaveSheet()
{
	return BlackPainterChunk() + BlackWeaveChunk( "mat_layer", 0.3 )
		+ "fabric_material\n{\n\tname mat_sheet\n\tfabric custom\n\tbase mat_layer\n}\n\n";
}

static std::string CoatedOverWeaveSheet()
{
	return BlackPainterChunk() + BlackWeaveChunk( "mat_layer", 0.3 )
		+ "coated_material\n{\n\tname mat_sheet\n\tbase mat_layer\n\tcoat_weight 1.0\n\tcoat_ior 1.5\n\tcoat_roughness 0.05\n}\n\n";
}

static std::string PerfectRefractorSheet( const char* ior )
{
	return
		"uniformcolor_painter\n{\n\tname pnt_refr\n\tcolor 1 1 1\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		"perfectrefractor_material\n{\n\tname mat_sheet\n\trefractance pnt_refr\n\tior " + std::string( ior ) + "\n}\n\n";
}

//////////////////////////////////////////////////////////////////////
// DL-295 review round 1: scenes for the HWSS hand-offs (P1-1) and for a
// BSDF-carrying caster's own delta reflection with no SMS anchor (P2-2).
//
// The CASTER is a smooth `randomwalk_sss_material` (roughness 0 -> an
// SMS caster, `GetSpecularInfo` isSpecular; absorption 50 -> the medium
// is black, so all it returns is its delta Fresnel reflection) or a
// `polished_material` at scattering 1e6 (delta coat, black substrate).
// Both carry a BSDF, so their delta lobe goes through PART 3, and the
// SSS one makes the HWSS body hand off to IntegrateFromHitNM.
//////////////////////////////////////////////////////////////////////
//! @a kind: 0 smooth random-walk SSS, 1 polished, 2 a clear dielectric
//! (BSDF-less: the SPF-only branch; only the dl295probe section uses it).
static std::string CasterChunk( int kind )
{
	if( kind == 2 ) {
		return "dielectric_material\n{\n\tname mat_caster\n\ttau 1.0\n\tior 1.5\n\tscattering 1000000\n}\n\n";
	}
	const bool polished = ( kind == 1 );
	return polished
		? BlackPainterChunk() + "polished_material\n{\n\tname mat_caster\n\treflectance pnt_black\n\ttau 1.0\n\tior 1.5\n\tscattering 1000000\n}\n\n"
		: std::string( "randomwalk_sss_material\n{\n\tname mat_caster\n\tior 1.5\n\tabsorption 50 50 50\n\tscattering 0.01 0.01 0.01\n\tg 0.0\n\troughness 0\n}\n\n" );
}

//! A ONE-SIDED emitter: `clippedplane_geometry`'s `doublesided` defaults
//! to TRUE, whose ray-facing normal makes a BSDF-sampled hit on the back
//! of a luminaire emit while NEE (true normal) sees nothing there.
static std::string EmitterChunks( const char* pts, double scale )
{
	std::ostringstream ss;
	ss << "uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1 1 1\n\tcolorspace Rec709RGB_Linear\n}\n\n"
	      "lambertian_luminaire_material\n{\n\tname mat_emit\n\texitance pnt_emit\n\tscale " << scale << "\n\tmaterial none\n}\n\n"
	      "clippedplane_geometry\n{\n\tname geo_emit\n" << pts << "\tdoublesided FALSE\n}\n\n"
	      "standard_object\n{\n\tname obj_emit\n\tgeometry geo_emit\n\tmaterial mat_emit\n}\n\n";
	return ss.str();
}

//! Camera looking DOWN at a caster floor that mirrors a 1 x 1 emitter
//! hanging above it (the reviewer's S1 / S4 / S5).  @a sheet: a black
//! gap-0.3 weave at y 2 between the camera and the floor.  @a slab: a
//! 0.05-thick ior-1.5 perfect-refractor slab at y 1 (a BSDF-less
//! vertex: the HWSS body's no-BSDF hand-off), with the emitter below it.
//! No row has a non-delta vertex before the caster: no SMS anchor.
static std::string CasterFloorScene( bool polished, bool sheet, bool slab )
{
	std::ostringstream ss;
	ss << "film\n{\n\twidth 16\n\theight 16\n}\n\n"
	      "pinhole_camera\n{\n\tlocation 0 3 3\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 6.0\n}\n\n"
	   << CasterChunk( polished ? 1 : 0 )
	   << ( polished
			? "clippedplane_geometry\n{\n\tname geo_floor\n\tpta -3 0 -3\n\tptb -3 0 3\n\tptc 3 0 3\n\tptd 3 0 -3\n\tdoublesided FALSE\n}\n\n"
			  "standard_object\n{\n\tname floor\n\tgeometry geo_floor\n\tmaterial mat_caster\n}\n\n"
			: "box_geometry\n{\n\tname geo_floor\n\twidth 6\n\theight 1\n\tdepth 6\n}\n\n"
			  "standard_object\n{\n\tname floor\n\tgeometry geo_floor\n\tposition 0 -0.5 0\n\tmaterial mat_caster\n}\n\n" )
	   << EmitterChunks( slab ? "\tpta -0.5 0.5 -1\n\tptb 0.5 0.5 -1\n\tptc 0.5 0.5 0\n\tptd -0.5 0.5 0\n"
			: "\tpta -0.5 1.5 -2\n\tptb 0.5 1.5 -2\n\tptc 0.5 1.5 -1\n\tptd -0.5 1.5 -1\n", 20.0 );
	if( slab ) {
		ss << "uniformcolor_painter\n{\n\tname pnt_refr\n\tcolor 1 1 1\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		      "perfectrefractor_material\n{\n\tname mat_refr\n\trefractance pnt_refr\n\tior 1.5\n}\n\n"
		      "box_geometry\n{\n\tname geo_slab\n\twidth 8\n\theight 0.05\n\tdepth 8\n}\n\n"
		      "standard_object\n{\n\tname slab\n\tgeometry geo_slab\n\tposition 0 1 0\n\tmaterial mat_refr\n}\n\n";
	}
	if( sheet ) {
		ss << ( polished ? "" : BlackPainterChunk().c_str() ) << BlackWeaveChunk( "mat_sheet", 0.3 )
		   << "clippedplane_geometry\n{\n\tname geo_sheet\n\tpta -4 2 4\n\tptb 4 2 4\n\tptc 4 2 -4\n\tptd -4 2 -4\n\tdoublesided TRUE\n}\n\n"
		      "standard_object\n{\n\tname sheet\n\tgeometry geo_sheet\n\tmaterial mat_sheet\n}\n\n";
	}
	return ss.str();
}

//! WITH an SMS anchor before the gap (P1-1's real target): the kWide
//! Lambertian receiver at y 0 looks UP through a black gap-0.3 weave at
//! y 2 at a caster CEILING (smooth SSS box, bottom face y 3.5) that
//! mirrors an UP-facing 2 x 2 emitter at y 3 (its black back hides it
//! from the receiver's NEE).  @a slab puts an ior-1.5 slab at y 2.6.
//! receiver (anchor) -> gap -> [slab] -> caster -> emitter: SMS's seed
//! trace from the receiver stops at the weave, so SMS holds no estimate
//! and the path must be counted.  @a sheet false: the same chain with no
//! gap, which SMS DOES own (the must-stay-suppressed control).
static std::string CasterCeilingScene( int kind, bool sheet, bool slab )
{
	const bool polished = ( kind == 1 );
	std::ostringstream ss;
	ss << "film\n{\n\twidth 16\n\theight 16\n}\n\n"
	      "pinhole_camera\n{\n\tlocation 0 1 1.2\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 10.0\n}\n\n"
	      "uniformcolor_painter\n{\n\tname pnt_recv\n\tcolor " << kRho << " " << kRho << " " << kRho << "\n\tcolorspace Rec709RGB_Linear\n}\n\n"
	      "lambertian_material\n{\n\tname mat_recv\n\treflectance pnt_recv\n}\n\n"
	      "clippedplane_geometry\n{\n\tname geo_recv\n\tpta -1 0 1\n\tptb 1 0 1\n\tptc 1 0 -1\n\tptd -1 0 -1\n\tdoublesided TRUE\n}\n\n"
	      "standard_object\n{\n\tname obj_recv\n\tgeometry geo_recv\n\tmaterial mat_recv\n}\n\n"
	   << CasterChunk( kind )
	   << ( polished
			? "clippedplane_geometry\n{\n\tname geo_ceil\n\tpta -4 3.5 -4\n\tptb 4 3.5 -4\n\tptc 4 3.5 4\n\tptd -4 3.5 4\n\tdoublesided FALSE\n}\n\n"
			  "standard_object\n{\n\tname ceil\n\tgeometry geo_ceil\n\tmaterial mat_caster\n}\n\n"
			: "box_geometry\n{\n\tname geo_ceil\n\twidth 8\n\theight 0.5\n\tdepth 8\n}\n\n"
			  "standard_object\n{\n\tname ceil\n\tgeometry geo_ceil\n\tposition 0 3.75 0\n\tmaterial mat_caster\n}\n\n" )
	   << EmitterChunks( "\tpta -1 3 1\n\tptb 1 3 1\n\tptc 1 3 -1\n\tptd -1 3 -1\n", 40.0 );
	if( slab ) {
		ss << "uniformcolor_painter\n{\n\tname pnt_refr\n\tcolor 1 1 1\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		      "perfectrefractor_material\n{\n\tname mat_refr\n\trefractance pnt_refr\n\tior 1.5\n}\n\n"
		      "box_geometry\n{\n\tname geo_slab\n\twidth 8\n\theight 0.05\n\tdepth 8\n}\n\n"
		      "standard_object\n{\n\tname slab\n\tgeometry geo_slab\n\tposition 0 2.6 0\n\tmaterial mat_refr\n}\n\n";
	}
	if( sheet ) {
		ss << ( polished ? "" : BlackPainterChunk().c_str() ) << BlackWeaveChunk( "mat_sheet", 0.3 )
		   << "clippedplane_geometry\n{\n\tname geo_sheet\n\tpta -4 2 4\n\tptb 4 2 4\n\tptc 4 2 -4\n\tptd -4 2 -4\n\tdoublesided TRUE\n}\n\n"
		      "standard_object\n{\n\tname sheet\n\tgeometry geo_sheet\n\tmaterial mat_sheet\n}\n\n";
	}
	return ss.str();
}

//! DL-295 review round 2 (P1-1): a receiver under a CLOSED 0.05-thick
//! ior-1.5 perfect-refractor slab at y 2, the 4 x 4 emitter at y 4.
//! @a recv: 0 Lambertian (an SMS anchor -- SMS owns receiver -> slab ->
//! emitter), 1 `biospec_skin_material` (NO BSDF: the SPF-only branch
//! skips PART 2, so SMS never runs there and it is NOT an anchor), 2
//! rough `randomwalk_sss_material` (BSSRDF; printed only).
static std::string UnderSlabScene( int recv, bool slab )
{
	std::ostringstream ss;
	ss << "film\n{\n\twidth 16\n\theight 16\n}\n\n"
	      "pinhole_camera\n{\n\tlocation 0 1 1.2\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 10.0\n}\n\n";
	if( recv == 1 ) {
		ss << "biospec_skin_material\n{\n\tname mat_recv\n\tmelanosomes_in_epidermis 0.019\n\tfolds_aspect_ratio 0.75\n}\n\n";
	} else if( recv == 2 ) {
		ss << "randomwalk_sss_material\n{\n\tname mat_recv\n\tior 1.3\n\tabsorption 0.5 0.5 0.5\n\tscattering 20 20 20\n\tg 0.0\n\troughness 0.3\n}\n\n";
	} else {
		ss << "uniformcolor_painter\n{\n\tname pnt_recv\n\tcolor " << kRho << " " << kRho << " " << kRho << "\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		      "lambertian_material\n{\n\tname mat_recv\n\treflectance pnt_recv\n}\n\n";
	}
	ss << "clippedplane_geometry\n{\n\tname geo_recv\n\tpta -1 0 1\n\tptb 1 0 1\n\tptc 1 0 -1\n\tptd -1 0 -1\n\tdoublesided TRUE\n}\n\n"
	      "standard_object\n{\n\tname obj_recv\n\tgeometry geo_recv\n\tmaterial mat_recv\n}\n\n"
	   << EmitterChunks( "\tpta -2 4 -2\n\tptb 2 4 -2\n\tptc 2 4 2\n\tptd -2 4 2\n", 4.0 );
	if( slab ) {
		ss << "uniformcolor_painter\n{\n\tname pnt_refr\n\tcolor 1 1 1\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		      "perfectrefractor_material\n{\n\tname mat_refr\n\trefractance pnt_refr\n\tior 1.5\n}\n\n"
		      "box_geometry\n{\n\tname geo_slab\n\twidth 8\n\theight 0.05\n\tdepth 8\n}\n\n"
		      "standard_object\n{\n\tname slab\n\tgeometry geo_slab\n\tposition 0 2 0\n\tmaterial mat_refr\n}\n\n";
	}
	return ss.str();
}

//! DL-340 fixture (external review round 2): Lambertian receiver under a
//! closed ior-1.5 slab at y 2, a global isotropic fog, and a 2 x 2 emitter
//! at y 3 FACING UP over a black blocker (the receiver, its NEE and SMS
//! never see the emitter, so light reaches the receiver only after a
//! MEDIUM scatter).
//! @a slab false: the no-caster control.
//! A 1.2 x 1.2 black Lambertian blocker at height @a y, under an UP-facing
//! emitter: SMS ignores emitter sidedness (DL-347), so without it an SMS
//! chain from below reaches the emitter's black back and adds light that
//! PT and VCM (correctly) do not see.
static std::string BlockerChunks( double y, double half = 0.6 )
{
	std::ostringstream ss;
	ss << BlackPainterChunk()
	   << "lambertian_material\n{\n\tname mat_block\n\treflectance pnt_black\n}\n\n"
	      "clippedplane_geometry\n{\n\tname geo_block\n\tpta " << -half << " " << y << " " << half << "\n\tptb " << half << " " << y << " " << half << "\n"
	      "\tptc " << half << " " << y << " " << -half << "\n\tptd " << -half << " " << y << " " << -half << "\n\tdoublesided TRUE\n}\n\n"
	      "standard_object\n{\n\tname obj_block\n\tgeometry geo_block\n\tmaterial mat_block\n}\n\n";
	return ss.str();
}

static std::string FogSlabScene( bool slab, double sigmaS )
{
	std::ostringstream ss;
	ss << "film\n{\n\twidth 16\n\theight 16\n}\n\n"
	      "pinhole_camera\n{\n\tlocation 0 1 1.2\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 10.0\n}\n\n"
	      "uniformcolor_painter\n{\n\tname pnt_recv\n\tcolor " << kRho << " " << kRho << " " << kRho << "\n\tcolorspace Rec709RGB_Linear\n}\n\n"
	      "lambertian_material\n{\n\tname mat_recv\n\treflectance pnt_recv\n}\n\n"
	      "clippedplane_geometry\n{\n\tname geo_recv\n\tpta -1 0 1\n\tptb 1 0 1\n\tptc 1 0 -1\n\tptd -1 0 -1\n\tdoublesided TRUE\n}\n\n"
	      "standard_object\n{\n\tname obj_recv\n\tgeometry geo_recv\n\tmaterial mat_recv\n}\n\n"
	   << EmitterChunks( "\tpta -1 3 1\n\tptb 1 3 1\n\tptc 1 3 -1\n\tptd -1 3 -1\n", 20.0 )
	   << BlockerChunks( 2.95, 1.1 )
	   << "homogeneous_medium\n{\n\tname fog\n\tabsorption 0 0 0\n\tscattering " << sigmaS << " " << sigmaS << " " << sigmaS << "\n\tphase isotropic\n}\n\n"
	      "global_medium\n{\n\tmedium fog\n}\n\n";
	if( slab ) {
		ss << "uniformcolor_painter\n{\n\tname pnt_refr\n\tcolor 1 1 1\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		      "perfectrefractor_material\n{\n\tname mat_refr\n\trefractance pnt_refr\n\tior 1.5\n}\n\n"
		      "box_geometry\n{\n\tname geo_slab\n\twidth 8\n\theight 0.05\n\tdepth 8\n}\n\n"
		      "standard_object\n{\n\tname slab\n\tgeometry geo_slab\n\tposition 0 2 0\n\tmaterial mat_refr\n}\n\n";
	}
	return ss.str();
}

//! DL-373 fixture (external review round 2, class 4): floor (y 0) -> slab1
//! (y 1) -> slab2 (y 1.75) -> Lambertian diffuser D (y 2.2) -> slab2 -> a
//! 1 x 1 emitter at y 1.5 FACING UP.  @a gap: a black gap-0.3 weave at y
//! 2.0 between slab2 and D; @a slab1 false removes the lower slab.  The
//! camera looks at the floor.
static std::string TwoChainScene( bool gap, bool slab1 )
{
	std::ostringstream ss;
	ss << "film\n{\n\twidth 16\n\theight 16\n}\n\n"
	      "pinhole_camera\n{\n\tlocation 0 0.5 0.6\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 20.0\n}\n\n"
	      "uniformcolor_painter\n{\n\tname pnt_recv\n\tcolor " << kRho << " " << kRho << " " << kRho << "\n\tcolorspace Rec709RGB_Linear\n}\n\n"
	      "lambertian_material\n{\n\tname mat_recv\n\treflectance pnt_recv\n}\n\n"
	      "clippedplane_geometry\n{\n\tname geo_recv\n\tpta -1 0 1\n\tptb 1 0 1\n\tptc 1 0 -1\n\tptd -1 0 -1\n\tdoublesided TRUE\n}\n\n"
	      "standard_object\n{\n\tname obj_recv\n\tgeometry geo_recv\n\tmaterial mat_recv\n}\n\n"
	      "clippedplane_geometry\n{\n\tname geo_diff\n\tpta -4 2.2 4\n\tptb 4 2.2 4\n\tptc 4 2.2 -4\n\tptd -4 2.2 -4\n\tdoublesided TRUE\n}\n\n"
	      "standard_object\n{\n\tname obj_diff\n\tgeometry geo_diff\n\tmaterial mat_recv\n}\n\n"
	   << EmitterChunks( "\tpta -0.5 1.5 0.5\n\tptb 0.5 1.5 0.5\n\tptc 0.5 1.5 -0.5\n\tptd -0.5 1.5 -0.5\n", 20.0 )
	   << BlockerChunks( 1.45 )
	   << "uniformcolor_painter\n{\n\tname pnt_refr\n\tcolor 1 1 1\n\tcolorspace Rec709RGB_Linear\n}\n\n"
	      "perfectrefractor_material\n{\n\tname mat_refr\n\trefractance pnt_refr\n\tior 1.5\n}\n\n";
	auto slabAt = [&ss]( const char* name, double y ) {
		ss << "box_geometry\n{\n\tname geo_" << name << "\n\twidth 8\n\theight 0.05\n\tdepth 8\n}\n\n"
		   << "standard_object\n{\n\tname " << name << "\n\tgeometry geo_" << name << "\n\tposition 0 " << y << " 0\n\tmaterial mat_refr\n}\n\n";
	};
	if( slab1 ) slabAt( "slab1", 1.0 );
	slabAt( "slab2", 1.75 );
	if( gap ) {
		ss << BlackWeaveChunk( "mat_sheet", 0.3 )
		   << "clippedplane_geometry\n{\n\tname geo_sheet\n\tpta -4 2 4\n\tptb 4 2 4\n\tptc 4 2 -4\n\tptd -4 2 -4\n\tdoublesided TRUE\n}\n\n"
		      "standard_object\n{\n\tname sheet\n\tgeometry geo_sheet\n\tmaterial mat_sheet\n}\n\n";
	}
	return ss.str();
}

//! Opt-in probe (WEAVE_GAP_FILTER=dl295probe): print SMS on / off for the
//! round-1 review scenes, to size the gated rows' sample counts.
static void ProbeReviewScenes()
{
	struct P { const char* label; std::string scene; bool hwss; };
	const P rows[] = {
		{ "floor SSS direct pel", CasterFloorScene( false, false, false ), false },
		{ "floor SSS direct hwss", CasterFloorScene( false, false, false ), true },
		{ "floor polished direct pel", CasterFloorScene( true, false, false ), false },
		{ "floor polished direct hwss", CasterFloorScene( true, false, false ), true },
		{ "floor SSS gap hwss (S1)", CasterFloorScene( false, true, false ), true },
		{ "floor SSS gap slab hwss (S4)", CasterFloorScene( false, true, true ), true },
		{ "ceiling SSS gap pel", CasterCeilingScene( false, true, false ), false },
		{ "ceiling SSS gap hwss", CasterCeilingScene( false, true, false ), true },
		{ "ceiling SSS gap slab hwss", CasterCeilingScene( false, true, true ), true },
		{ "ceiling SSS no gap pel (control)", CasterCeilingScene( false, false, false ), false },
		{ "ceiling polished no gap pel (control)", CasterCeilingScene( true, false, false ), false },
		{ "ceiling SSS no gap hwss (control)", CasterCeilingScene( false, false, false ), true },
		{ "ceiling dielectric no gap pel (control)", CasterCeilingScene( 2, false, false ), false },
		{ "Lambertian under slab pel (SMS owns it)", UnderSlabScene( 0, true ), false },
		{ "rough RW-SSS under slab pel", UnderSlabScene( 2, true ), false },
		{ "rough RW-SSS no slab pel", UnderSlabScene( 2, false ), false },
		{ "DL-340 fog + slab pel", FogSlabScene( true, 0.2 ), false },
		{ "DL-340 fog, no slab pel (control)", FogSlabScene( false, 0.2 ), false },
		{ "DL-373 floor view, no gap pel", TwoChainScene( false, true ), false },
		{ "DL-373 floor view, gap pel", TwoChainScene( true, true ), false },
		{ "DL-373 floor view, gap, slab1 removed pel", TwoChainScene( true, false ), false },
	};
	const char* rowFilter = std::getenv( "WEAVE_GAP_PROBE_ROWS" );	// label substring
	for( const P& r : rows ) {
		if( rowFilter && !std::strstr( r.label, rowFilter ) ) continue;
		const char* sppEnv = std::getenv( "WEAVE_GAP_PROBE_SPP" );
		const unsigned int spp = sppEnv ? (unsigned int)std::strtol( sppEnv, nullptr, 10 ) : 256u;
		ParityRow( r.label, r.hwss ? RastPTSpectralSMS( spp, true, true ) : RastPTSMS( spp, true ),
			r.hwss ? RastPTSpectralSMS( spp, true, false ) : RastPTSMS( spp, false ), r.scene, -1.0 );
	}
	SobolSamplerTestHooks::ValueSalt().store( 0u );
}

static void TestSMSEmissionThroughGap()
{
	std::cout << "=== sms: PT with sms_enabled, emission reached through a weave gap (DL-295) ===" << std::endl;
	const double g = 0.3;

	// Closed forms, SMS on, black-yarn sheet (exact).  Pre-fix: every
	// row but the env box reads 0.
	//
	// BANDS.  The suite runs on ONE render worker (see main), so a render
	// is exactly reproducible from its seed base and a seed sweep IS the
	// run-to-run spread.  Every band is >= 3.3 sd of a 12-seed sweep
	// (seed bases 1000-12000) at these sample counts,
	// docs/DL05_WEAVE_GAP_SHADOW_TRANSMITTANCE.md section 9.3.  Relative
	// sd (band / sd): area RGB 0.83 % (3.6), spectral 0.69 % (5.8),
	// look-up 0.08 %, composite area 5.35 % (3.7) / look-up 2.14 % (7.0;
	// 15 % also covers the external review's MULTITHREADED pooled 3.16 %
	// at 4.7), env 0.05 %; HWSS parity area 1.00 % (5.0) / look-up 0.73 %
	// (5.5) / env 0.90 % (4.4); composite(dielectric) 2.29 % (3.5) /
	// 0.93 % (4.3); fabric, coated, direct-view <= 0.17 %; hand-off S1 /
	// S4 0.44 / 0.53 % (>= 11); anchored ceiling RGB 1.26 % (4.8), HWSS
	// SSS hand-off 2.29 % (3.9), no-BSDF hand-off 2.11 % (4.3); skin under
	// a slab RGB 1.18 % (4.2), spectral 1.85 % (4.3).  Round 1 derived
	// bands from MULTITHREADED runs, which are not reproducible (each
	// worker's RNG is seeded from libc rand() in thread-start order and
	// tiles go to workers nondeterministically), and two of them did not
	// hold (external review P2-1).
	const std::string sheet = BlackWeaveSheet( g ), comp2 = CompositeTwoBlackWeavesSheet( g );
	RatioRow( "area PT RGB (g*L0)", RastPTSMS( 512, true ),
		ReceiverScene( kAreaLarge, false, 0.0, kWide ), ReceiverScene( kAreaLarge, true, g, kWide, false, sheet ), g, 0.03 );
	RatioRow( "area PT spectral hwss=false (g*L0)", RastPTSpectralSMS( 1024, false, true ),
		ReceiverScene( kAreaLarge, false, 0.0, kWide ), ReceiverScene( kAreaLarge, true, g, kWide, false, sheet ), g, 0.04 );
	RatioRow( "lookup PT RGB (camera -> gap -> luminaire, g*L0)", RastPTSMS( 64, true ),
		ReceiverScene( kAreaLarge, false, 0.0, kLookUp ), ReceiverScene( kAreaLarge, true, g, kLookUp, false, sheet ), g, 0.03 );
	RatioRow( "area composite-of-two-weaves PT RGB (g^2*L0)", RastPTSMS( 8192, true ),
		ReceiverScene( kAreaLarge, false, 0.0, kWide ), ReceiverScene( kAreaLarge, true, g, kWide, false, comp2 ), g * g, 0.20 );
	RatioRow( "lookup composite-of-two-weaves PT RGB (g^2*L0)", RastPTSMS( 8192, true ),
		ReceiverScene( kAreaLarge, false, 0.0, kLookUp ), ReceiverScene( kAreaLarge, true, g, kLookUp, false, comp2 ), g * g, 0.15 );
	RatioRow( "env box PT RGB (g*L0, env escape)", RastPTSMS( 256, true, true ),
		EnvBoxScene( false, 0.0 ), EnvBoxScene( true, g ), g, 0.03 );

	// HWSS: parity (DL-329 moves on and off identically).
	ParityRow( "area PT HWSS", RastPTSpectralSMS( 2048, true, true ), RastPTSpectralSMS( 2048, true, false ),
		ReceiverScene( kAreaLarge, true, g, kWide, false, sheet ), 0.05 );
	ParityRow( "lookup PT HWSS", RastPTSpectralSMS( 512, true, true ), RastPTSpectralSMS( 512, true, false ),
		ReceiverScene( kAreaLarge, true, g, kLookUp, false, sheet ), 0.04 );
	ParityRow( "env box PT HWSS", RastPTSpectralSMS( 512, true, true, true ), RastPTSpectralSMS( 512, true, false, true ),
		EnvBoxScene( true, g ), 0.04 );

	// Sibling: CompositeSPF's walker exits through a dielectric top leave
	// REFRACTED -- delta-tagged, not a pass-through, not an SMS caster.
	ParityRow( "area composite(dielectric over weave) PT RGB", RastPTSMS( 4096, true ), RastPTSMS( 4096, false ),
		ReceiverScene( kAreaLarge, true, g, kWide, false, CompositeDielectricOverWeaveSheet() ), 0.08 );
	ParityRow( "lookup composite(dielectric over weave) PT RGB", RastPTSMS( 2048, true ), RastPTSMS( 2048, false ),
		ReceiverScene( kAreaLarge, true, g, kLookUp, false, CompositeDielectricOverWeaveSheet() ), 0.04 );
	ParityRow( "lookup fabric over weave PT RGB", RastPTSMS( 256, true ), RastPTSMS( 256, false ),
		ReceiverScene( kAreaLarge, true, g, kLookUp, false, FabricOverWeaveSheet() ), 0.03 );
	ParityRow( "lookup coated over weave PT RGB", RastPTSMS( 256, true ), RastPTSMS( 256, false ),
		ReceiverScene( kAreaLarge, true, g, kLookUp, false, CoatedOverWeaveSheet() ), 0.03 );
	// DL-295 review round 1, P2-2: a BSDF-carrying caster's OWN delta
	// reflection with NO SMS anchor before it (camera -> smooth SSS /
	// polished coat -> emitter).  SMS never estimates it (it seeds from a
	// non-delta vertex THROUGH casters) and NEE cannot sample a delta lobe,
	// yet PART 3 suppressed it: 0.000003 / 0.000000 vs 0.3124, pel and HWSS.
	ParityRow( "direct smooth-SSS reflection PT RGB (no anchor)", RastPTSMS( 256, true ), RastPTSMS( 256, false ),
		CasterFloorScene( false, false, false ), 0.03 );
	ParityRow( "direct polished reflection PT RGB (no anchor)", RastPTSMS( 256, true ), RastPTSMS( 256, false ),
		CasterFloorScene( true, false, false ), 0.03 );
	ParityRow( "direct smooth-SSS reflection PT HWSS (no anchor)", RastPTSpectralSMS( 256, true, true ), RastPTSpectralSMS( 256, true, false ),
		CasterFloorScene( false, false, false ), 0.03 );
	ParityRow( "direct polished reflection PT HWSS (no anchor)", RastPTSpectralSMS( 256, true, true ), RastPTSpectralSMS( 256, true, false ),
		CasterFloorScene( true, false, false ), 0.03 );

	// DL-295 review round 1, P1-1: the HWSS body hands a smooth-SSS vertex
	// (the SSS hand-off) or a BSDF-less slab (the no-BSDF hand-off) to the
	// per-wavelength NM body; the chain's SMS state must go with it.
	// No anchor (the reviewer's S1 / S4: camera -> gap -> [slab] -> SSS):
	ParityRow( "gap -> smooth-SSS reflection PT HWSS (SSS hand-off, S1)", RastPTSpectralSMS( 1024, true, true ), RastPTSpectralSMS( 1024, true, false ),
		CasterFloorScene( false, true, false ), 0.06 );
	ParityRow( "gap -> slab -> smooth-SSS reflection PT HWSS (no-BSDF hand-off, S4)", RastPTSpectralSMS( 1024, true, true ), RastPTSpectralSMS( 1024, true, false ),
		CasterFloorScene( false, true, true ), 0.06 );
	// WITH an anchor (receiver -> gap -> [slab] -> SSS ceiling -> emitter):
	// the anchor gate cannot rescue these, only the forwarded uncovered
	// state does.
	ParityRow( "receiver -> gap -> smooth-SSS ceiling PT RGB", RastPTSMS( 2048, true ), RastPTSMS( 2048, false ),
		CasterCeilingScene( false, true, false ), 0.06 );
	ParityRow( "receiver -> gap -> smooth-SSS ceiling PT HWSS (SSS hand-off)", RastPTSpectralSMS( 2048, true, true ), RastPTSpectralSMS( 2048, true, false ),
		CasterCeilingScene( false, true, false ), 0.09 );
	ParityRow( "receiver -> gap -> slab -> smooth-SSS ceiling PT HWSS (no-BSDF hand-off)", RastPTSpectralSMS( 2048, true, true ), RastPTSpectralSMS( 2048, true, false ),
		CasterCeilingScene( false, true, true ), 0.09 );
	// DL-295 review round 2, P1-1: a surface with NO BSDF is not an SMS
	// anchor.  `biospec_skin_material` goes through the SPF-only branch,
	// which `continue`s before PART 2, so SMS never runs there -- yet it set
	// the anchor flag, and skin -> slab -> emitter was suppressed with no
	// estimator: PT+SMS 0 vs 0.267 (pel) / 0.244 (spectral).
	ParityRow( "skin receiver under a closed slab PT RGB (no BSDF: not an SMS anchor)", RastPTSMS( 256, true ), RastPTSMS( 256, false ),
		UnderSlabScene( 1, true ), 0.05 );
	ParityRow( "skin receiver under a closed slab PT spectral hwss=false", RastPTSpectralSMS( 256, false, true ), RastPTSpectralSMS( 256, false, false ),
		UnderSlabScene( 1, true ), 0.08 );

	// The same anchored chain with no gap: an SMS chain by PT's old
	// accounting (anchor, then a caster), which read ~0 here -- SMS treats
	// these casters as refractors (`canRefract`) and never estimates their
	// REFLECTION chain (DL-339's premise failure).  Since the DL-372 split
	// suppression PT asks SMS's own seed + solve whether it reaches the
	// hit; it does not (it refracts), so PT keeps it: ~1, and ~2 would be a
	// double count.  Measured (MeasureSplitRows, n 8, salted): 1.00268 +/-
	// 0.00668, 0.99680 +/- 0.00789, HWSS 0.99780 +/- 0.00717 (n 4); pre-
	// split 0.00026, 0.00000, 0.00028.  Bands >= 4.4 sd.  The HWSS row
	// reaches the split through the SSS hand-off's record; a BSDF caster
	// whose reflection the HWSS body suppresses itself keeps today's rule
	// (DL-378).
	ParityRow( "receiver -> smooth-SSS ceiling (anchored, no gap) PT RGB", RastPTSMS( 256, true ), RastPTSMS( 256, false ),
		CasterCeilingScene( false, false, false ), 0.035 );
	ParityRow( "receiver -> polished ceiling (anchored, no gap) PT RGB", RastPTSMS( 256, true ), RastPTSMS( 256, false ),
		CasterCeilingScene( true, false, false ), 0.035 );
	ParityRow( "receiver -> smooth-SSS ceiling (anchored, no gap) PT HWSS", RastPTSpectralSMS( 256, true, true ), RastPTSpectralSMS( 256, true, false ),
		CasterCeilingScene( false, false, false ), 0.035 );

	// Control: an SMS CASTER, where the suppression's premise is SMS's to
	// honour and this fix changes nothing.  The ior-1.0 row is printed, not
	// gated: before master's DL-290 the plane read 0 with SMS on; since, it
	// reads ~1.0 (SMS's matched-index seed walk).  The ior-1.5 OPEN plane
	// read 2.2515 in every build until DL-345 (2026-10-02): the material
	// decided the receiver's crossing by the IOR stack (an ENTRY, from
	// below) while SMS decided it by the face (an EXIT); with the sheet
	// crossed by its face everywhere it reads 1.00044 (SMS-on unchanged at
	// 0.20261, PT 0.0900 -> 0.2025).  Gated since DL-345 (DL-339 (b)).
	ParityRow( "area perfectrefractor ior 1 PT RGB (SMS caster -- control, printed)", RastPTSMS( 256, true ), RastPTSMS( 256, false ),
		ReceiverScene( kAreaLarge, true, g, kWide, false, PerfectRefractorSheet( "1.0" ) ), -1.0 );
	ParityRow( "area perfectrefractor ior 1.5 open sheet PT RGB (DL-339 (b) / DL-345)", RastPTSMS( 256, true ), RastPTSMS( 256, false ),
		ReceiverScene( kAreaLarge, true, g, kWide, false, PerfectRefractorSheet( "1.5" ) ), 0.03 );
	SobolSamplerTestHooks::ValueSalt().store( 0u );
}

//////////////////////////////////////////////////////////////////////
// P2-2 (external review of DL-05): the transparent-shadow walk
// (`RayCaster::WalkShadowSegment`, shared by the opt-in
// `transparent_shadows` path and DL-05's new pass-through walk) now
// STEPS OVER an object whose `casts_shadows` is FALSE -- the binary
// any-hit test (`IObjectManager::IntersectShadowRay`) already ignored
// such objects, and a walk entered because the binary test saw a
// DIFFERENT occluder must not then block on one the binary test itself
// would not have blocked on.  Pre-DL-05 it did: `WalkShadowSegment` had
// no such branch, so with `transparent_shadows TRUE` and NO weave
// anywhere in the scene, a `casts_shadows FALSE` opaque Lambertian
// blocker directly between the receiver and an omni light fully
// occluded the transmittance walk while the binary walk (and the
// blocker-free control) read the light through unattenuated.  No
// weave/gap material is involved -- this is a general RayCaster
// regression, found while implementing DL-05's pass-through walk
// because both walks share WalkShadowSegment, not something DL-05's
// own mechanism (IMaterial::HasDeltaPassThrough) has any part in.
//////////////////////////////////////////////////////////////////////

//! Same receiver/light geometry as ReceiverScene(kOmni, ...), but the
//! "sheet" (when present) is an ORDINARY opaque Lambertian plane with
//! `casts_shadows FALSE`, not a weave.
static std::string NonCasterBlockerScene( bool withBlocker )
{
	std::ostringstream ss;
	ss <<
		"film\n{\n\twidth 16\n\theight 16\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 1 1.2\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 2.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_recv\n\tcolor " << kRho << " " << kRho << " " << kRho
			<< "\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		"lambertian_material\n{\n\tname mat_recv\n\treflectance pnt_recv\n}\n\n"
		"clippedplane_geometry\n{\n\tname geo_recv\n"
			"\tpta -0.1 0 0.1\n\tptb 0.1 0 0.1\n\tptc 0.1 0 -0.1\n\tptd -0.1 0 -0.1\n"
			"\tdoublesided TRUE\n}\n\n"
		"standard_object\n{\n\tname obj_recv\n\tgeometry geo_recv\n\tmaterial mat_recv\n}\n\n";

	if( withBlocker ) {
		ss <<
			"uniformcolor_painter\n{\n\tname pnt_block\n\tcolor 0.8 0.8 0.8\n\tcolorspace Rec709RGB_Linear\n}\n\n"
			"lambertian_material\n{\n\tname mat_block\n\treflectance pnt_block\n}\n\n"
			"clippedplane_geometry\n{\n\tname geo_block\n"
				"\tpta -4 " << kSheetY << " 4\n\tptb 4 " << kSheetY << " 4\n"
				"\tptc 4 " << kSheetY << " -4\n\tptd -4 " << kSheetY << " -4\n"
				"\tdoublesided TRUE\n}\n\n"
			"standard_object\n{\n\tname obj_block\n\tgeometry geo_block\n\tmaterial mat_block\n\tcasts_shadows FALSE\n}\n\n";
	}

	ss << "omni_light\n{\n\tname lgt\n\tposition 0 " << kLightY << " 0\n\tcolor 1 1 1\n\tpower " << kOmniPow << "\n}\n\n";
	return ss.str();
}

static void TestCastsShadowsFalseStepOver()
{
	std::cout << "=== casts_shadows FALSE: the transparent-shadow walk must step over it (P2-2) ===" << std::endl;

	const double L0 = Render( Assemble( RastPT( 16 ), NonCasterBlockerScene( false ) ), "noncaster_l0" );
	Check( L0 > 0, "casts_shadows: no-blocker control renders non-black" );
	if( !( L0 > 0 ) ) return;

	// Binary walk: IntersectShadowRay already excludes a casts_shadows
	// FALSE object, so this has always equalled L0 -- the reference the
	// transmittance walk below is held to.
	const double Lbinary = Render( Assemble( RastPT( 16 ), NonCasterBlockerScene( true ) ), "noncaster_binary" );
	// Opt-in transparent-shadow walk: pre-DL-05 this read ~0 (blocked);
	// post-fix it must step over the non-caster exactly like the binary
	// walk and read L0 too.
	const double Ltrans = Render( Assemble( RastPTTransparentShadows( 16 ), NonCasterBlockerScene( true ) ), "noncaster_trans" );

	std::cout << "  L0 (no blocker) = " << L0
		<< ", binary walk (casts_shadows FALSE blocker) = " << Lbinary
		<< ", transparent_shadows walk (casts_shadows FALSE blocker) = " << Ltrans << std::endl;

	Check( std::fabs( Lbinary / L0 - 1.0 ) <= 0.03, "casts_shadows: binary walk == unblocked L0" );
	Check( std::fabs( Ltrans  / L0 - 1.0 ) <= 0.03, "casts_shadows: transparent-shadow walk == unblocked L0" );
	Check( std::fabs( Ltrans  / Lbinary - 1.0 ) <= 0.03, "casts_shadows: transparent-shadow walk == binary walk" );
}

//////////////////////////////////////////////////////////////////////
// The design-doc two-layer topology (docs/CLOTH_FABRIC_DESIGN.md
// section 15 debt 25 / item 27's measurement family): a
// `weave_material { fabric custom transmission thin gap g
// warp_transmit 0.25 weft_transmit 0.25 }` closed box 2.8 x 2.8 x 1.0,
// or the same surfaces as free-standing double-sided planes; omni light
// at (0,0,lightZ) power 6; camera (0,0,3.2) fov 34.
//////////////////////////////////////////////////////////////////////
enum LayerGeom { kBox, kSixPlanes, kTwoPlanes, kOnePlane, kSphere };

static std::string PlaneChunk( const char* name, const char* pts )
{
	return std::string( "clippedplane_geometry\n{\n\tname " ) + name + "\n" + pts + "\tdoublesided TRUE\n}\n\n"
		+ "standard_object\n{\n\tname o_" + name + "\n\tgeometry " + name + "\n\tmaterial mat_box\n}\n\n";
}

static std::string LayerScene( LayerGeom geom, double gap, double lightZ, unsigned int res )
{
	std::ostringstream ss;
	ss <<
		"film\n{\n\twidth " << res << "\n\theight " << res << "\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 0 3.2\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 34.0\n}\n\n"
		"omni_light\n{\n\tname lgt\n\tposition 0 0 " << lightZ << "\n\tcolor 1.0 1.0 1.0\n\tpower 6.0\n}\n\n"
		"weave_material\n{\n\tname mat_box\n\tfabric custom\n\ttransmission thin\n\tgap " << gap << "\n"
			"\twarp_transmit 0.25\n\tweft_transmit 0.25\n}\n\n";

	const char* pz = "\tpta -1.4 -1.4 0.5\n\tptb 1.4 -1.4 0.5\n\tptc 1.4 1.4 0.5\n\tptd -1.4 1.4 0.5\n";
	const char* nz = "\tpta 1.4 -1.4 -0.5\n\tptb -1.4 -1.4 -0.5\n\tptc -1.4 1.4 -0.5\n\tptd 1.4 1.4 -0.5\n";
	switch( geom ) {
	case kBox:
		ss << "box_geometry\n{\n\tname g\n\twidth 2.8\n\theight 2.8\n\tdepth 1.0\n}\n\n"
		      "standard_object\n{\n\tname o\n\tgeometry g\n\tmaterial mat_box\n\tposition 0 0 0\n}\n\n";
		break;
	case kSixPlanes:
		ss << PlaneChunk( "px", "\tpta 1.4 -1.4 -0.5\n\tptb 1.4 -1.4 0.5\n\tptc 1.4 1.4 0.5\n\tptd 1.4 1.4 -0.5\n" )
		   << PlaneChunk( "nx", "\tpta -1.4 -1.4 0.5\n\tptb -1.4 -1.4 -0.5\n\tptc -1.4 1.4 -0.5\n\tptd -1.4 1.4 0.5\n" )
		   << PlaneChunk( "py", "\tpta -1.4 1.4 -0.5\n\tptb 1.4 1.4 -0.5\n\tptc 1.4 1.4 0.5\n\tptd -1.4 1.4 0.5\n" )
		   << PlaneChunk( "ny", "\tpta -1.4 -1.4 0.5\n\tptb 1.4 -1.4 0.5\n\tptc 1.4 -1.4 -0.5\n\tptd -1.4 -1.4 -0.5\n" )
		   << PlaneChunk( "pz", pz ) << PlaneChunk( "nz", nz );
		break;
	case kTwoPlanes:
		ss << PlaneChunk( "pz", pz ) << PlaneChunk( "nz", nz );
		break;
	case kOnePlane:
		ss << PlaneChunk( "z0", "\tpta -1.4 -1.4 0\n\tptb 1.4 -1.4 0\n\tptc 1.4 1.4 0\n\tptd -1.4 1.4 0\n" );
		break;
	case kSphere:
		// A closed analytic shell: every NEE shadow ray from the lit-from-
		// behind front hemisphere leaves its OWN surface and crosses the
		// SAME object's far side -- the self-root topology debt 25's
		// sibling audit fixed, so a walk that re-found the ray's own
		// origin surface would multiply in a spurious extra `gap`.
		ss << "sphere_geometry\n{\n\tname g\n\tradius 1.0\n}\n\n"
		      "standard_object\n{\n\tname o\n\tgeometry g\n\tmaterial mat_box\n\tposition 0 0 0\n}\n\n";
		break;
	}
	return ss.str();
}

static void TestTwoLayerLightOutside()
{
	std::cout << "=== layers: two-layer weave, omni light OUTSIDE (design-doc topology) ===" << std::endl;
	struct L { LayerGeom geom; double gap; const char* label; };
	const L rows[] = {
		{ kBox,       0.3, "box gap 0.3" },
		{ kTwoPlanes, 0.1, "two planes gap 0.1" },
		// The closed SPHERE is deliberately NOT gated here, only printed by
		// the `table` section: BDPT reads it ~6.5 % under PT with gap 0.3
		// (0.01653 vs 0.01770) while VCM at 4096 spp reads 0.0180 +/-
		// 0.0004, i.e. with PT -- a BDPT closed-shell residual that exists
		// independently of DL-05 (the same sphere at gap 0.0, where no
		// pass-through exists, already reads BDPT/PT 1.041 before and after)
		// and that this 8 % band would sit too close to.
	};
	// 512 spp, the table's own count: at 256 spp BDPT reads the box row
	// ~3 % lower than at 512 with a run-to-run sd of ~0.03 %, i.e. a
	// sample-count-dependent QMC structure rather than noise, so the band
	// is checked where the table itself converged.
	const unsigned int res = 24, spp = 512;
	for( const L& r : rows )
	{
		const std::string body = LayerScene( r.geom, r.gap, -3.0, res );
		const double pt   = RenderMeanN( Assemble( RastPT( spp ),   body ), "lay_pt" );
		const double bdpt = RenderMeanN( Assemble( RastBDPT( spp ), body ), "lay_bdpt" );
		const double vcm  = RenderMeanN( Assemble( RastVCM( spp ),  body ), "lay_vcm" );
		Check( pt > 0 && bdpt > 0 && vcm > 0, std::string( "layers " ) + r.label + ": all three renders non-black" );
		if( !( pt > 0 && bdpt > 0 && vcm > 0 ) ) continue;
		char buf[256];
		std::snprintf( buf, sizeof(buf), "layers %s: PT %.5f  BDPT/PT %.4f  VCM/PT %.4f", r.label, pt, bdpt / pt, vcm / pt );
		std::cout << "  " << buf << std::endl;
		Check( std::fabs( bdpt / pt - 1.0 ) <= 0.08, std::string( buf ) + " -- BDPT/PT within 8%" );
		Check( std::fabs( vcm / pt - 1.0 ) <= 0.08, std::string( buf ) + " -- VCM/PT within 8%" );
	}
}

//////////////////////////////////////////////////////////////////////
// fovsweep (gated) and dl294 (measurement aid): DL-294.
//
// The t = 1 light-tracing splat must cover EXACTLY the film the eye
// subpaths sample.  Before DL-294 it did not: the camera's
// world-to-raster inverse (BDPTCameraUtilities::Rasterize) accepted
// raster x in [0, W) and y in [0, H) -- the camera's NOMINAL film --
// while every rasterizer samples pixel (x, row y) at screen
// (x + u - 0.5, H - y + v - 0.5) (the convention before DL-368, which
// later moved every rasterizer to the camera's nominal film), i.e. the film is x in [-0.5, W - 0.5),
// y in [0.5, H + 0.5), and SplatFilm rounds a splat to the nearest
// pixel CENTRE in that same convention.  The camera rejected the
// half-pixel strips x in [-0.5, 0) and y in [H, H + 0.5) -- ON that
// film (the strips it accepted in their place are off it, and the film
// rightly dropped them) -- so the splat covered 15.5 x 15.5 of a 16 x 16 film,
// 1 - (15.5/16)^2 = 6.15 % of a uniformly lit frame, with image
// column 0 and row 0 at exactly HALF their radiance.  Only a frame
// that is lit edge to edge sees it, which is why it looked like a
// narrow-FIELD-OF-VIEW defect on this fixture (the spot fills a 1-3
// degree frame and leaves a 5-10 degree frame's edges dark).
//
// Every quantity is referenced to PT's no-sheet render at the SAME
// fov (PT's estimator is NEE and never splats), so no closed form is
// needed across the spot's penumbra: L_int(gap g) / (g * L0_PT) and
// L0_int / L0_PT.
//////////////////////////////////////////////////////////////////////
static void MeanSd( const std::vector<double>& v, double& mean, double& sd );

static const double kSweepFovs[] = { 1.0, 2.0, 3.0, 5.0, 10.0 };
static const int kNumSweepFovs = 5;
static const double kSweepGap = 0.3;

//! Edge fingerprint of a pure-splat render: mean of image column 0 /
//! row 0 / column W-1 / row H-1 over the mean of the interior pixels.
struct EdgeRatios { double col0, row0, colLast, rowLast; };
static EdgeRatios EdgeFingerprint( const std::vector<double>& px, unsigned int w, unsigned int h )
{
	EdgeRatios e = { -1, -1, -1, -1 };
	if( px.size() != size_t( w ) * h || w < 3 || h < 3 ) return e;
	double interior = 0, c0 = 0, r0 = 0, cl = 0, rl = 0;
	for( unsigned int y = 1; y + 1 < h; y++ )
		for( unsigned int x = 1; x + 1 < w; x++ ) interior += px[y * w + x];
	interior /= double( ( w - 2 ) * ( h - 2 ) );
	for( unsigned int y = 1; y + 1 < h; y++ ) { c0 += px[y * w]; cl += px[y * w + w - 1]; }
	for( unsigned int x = 1; x + 1 < w; x++ ) { r0 += px[x]; rl += px[( h - 1 ) * w + x]; }
	if( !( interior > 0 ) ) return e;
	e.col0 = c0 / double( h - 2 ) / interior;
	e.colLast = cl / double( h - 2 ) / interior;
	e.row0 = r0 / double( w - 2 ) / interior;
	e.rowLast = rl / double( w - 2 ) / interior;
	return e;
}

static void TestNarrowFovSplat()
{
	std::cout << "=== fovsweep: t = 1 splat vs PT at fov 1-10 deg, 2 x 2 patch, spot light (DL-294) ===" << std::endl;
	for( int k = 0; k < kNumSweepFovs; k++ )
	{
		const double fov = kSweepFovs[k];
		const unsigned int fovSaltBase = SobolSequence::HashCombine( 0xD1294Au, unsigned( k ) );
		const double ptL0 = RenderSalted( Assemble( RastPT( 64 ), ReceiverScene( kSpot, false, 0.0, kWide, false, std::string(), std::string(), fov ) ), "fs_pt",
			SobolSequence::HashCombine( fovSaltBase, 0x1u ) );
		Check( ptL0 > 0, "fovsweep: PT reference renders non-black" );
		if( !( ptL0 > 0 ) ) continue;

		// DL-390 corrected-salt audit (n=4, seed 1000): fov-1 gap
		// sample sd is 0.946% BDPT, 0.394% VCM, 0.274% VCM-no-merge.
		// Their 3%/2%/3% bands are 3.17/5.08/10.95 sample sd wide.
		// At fov 2, BDPT/VCM/no-merge sd is 0.128/0.073/0.186%;
		// at fov 3 it is 0.068/0.075/0.090%. Full VCM retains its
		// ~1.1% merge-radius penumbra deficit at fov 3, inside 2%.
		// Earlier helper-swallowed salt measurements in DL294's closure
		// are historical, not the calibration for this corrected helper.
		// Gaussian and remaining fovs: batch validation record.
		struct R { const char* label; std::string rast; double tolGap; double tolGapFov1; double tolL0; bool edge; };
		const R rows[] = {
			{ "BDPT RGB",              RastBDPT( 1024 ),       0.01, 0.03, 0.005, true },
			{ "VCM RGB",               RastVCM( 1024 ),        0.02, 0.02, 0.005, false },
			{ "VCM RGB merging OFF",   RastVCMNoMerge( 1024 ), 0.01, 0.03, 0.005, true },
			{ "BDPT RGB gaussian filter", RastBDPTDefaultFilter( 1024 ), 0.01, 0.03, 0.005, true },
		};
		for( int ri = 0; ri < 4; ri++ )
		{
			const R& r = rows[ri];
			const unsigned int rowSaltBase = SobolSequence::HashCombine( fovSaltBase, unsigned( ri ) + 0x10u );
			const double L0 = RenderSalted( Assemble( r.rast, ReceiverScene( kSpot, false, 0.0, kWide, false, std::string(), std::string(), fov ) ), "fs_l0",
				SobolSequence::HashCombine( rowSaltBase, 0x2u ) );
			std::vector<double> px;
			const double Lg = RenderSalted( Assemble( r.rast, ReceiverScene( kSpot, true, kSweepGap, kWide, false, std::string(), std::string(), fov ) ), "fs_lg",
				SobolSequence::HashCombine( rowSaltBase, 0x3u ), &px );
			const double tolGap = ( fov == 1.0 ) ? r.tolGapFov1 : r.tolGap;
			char buf[256];
			std::snprintf( buf, sizeof(buf), "fovsweep fov %4.1f %s: L0/L0_PT = %.5f (%+.3f%%)",
				fov, r.label, L0 / ptL0, 100.0 * ( L0 / ptL0 - 1.0 ) );
			std::cout << "  " << buf << std::endl;
			Check( std::fabs( L0 / ptL0 - 1.0 ) <= r.tolL0, buf );
			std::snprintf( buf, sizeof(buf), "fovsweep fov %4.1f %s: L(gap %.1f)/(g*L0_PT) = %.5f (%+.3f%%)",
				fov, r.label, kSweepGap, Lg / ( kSweepGap * ptL0 ), 100.0 * ( Lg / ( kSweepGap * ptL0 ) - 1.0 ) );
			std::cout << "  " << buf << std::endl;
			Check( std::fabs( Lg / ( kSweepGap * ptL0 ) - 1.0 ) <= tolGap, buf );

			// The fingerprint: at fov 2 the frame is lit edge to edge and
			// the gap render IS the splat, so every edge row / column must
			// read like the interior (pre-fix, box filter: column 0 and
			// row 0 at 0.49 / 0.52; gaussian filter: 0.49 / 0.51 with
			// the last column / row at 1.48 / 1.51 -- the filtered path
			// renormalised the out-of-film strip onto them).  Not at fov 1, where the per-pixel splat count
			// is low enough that a deterministic QMC pattern moves the
			// edge means by up to 12 % in BOTH builds.
			if( fov == 2.0 && r.edge ) {
				const EdgeRatios e = EdgeFingerprint( px, 16, 16 );
				std::snprintf( buf, sizeof(buf), "fovsweep fov %4.1f %s gap render edges / interior: col0 %.4f row0 %.4f colLast %.4f rowLast %.4f",
					fov, r.label, e.col0, e.row0, e.colLast, e.rowLast );
				std::cout << "  " << buf << std::endl;
				const double tolEdge = 0.10;
				Check( std::fabs( e.col0 - 1.0 ) <= tolEdge && std::fabs( e.row0 - 1.0 ) <= tolEdge &&
				       std::fabs( e.colLast - 1.0 ) <= tolEdge && std::fabs( e.rowLast - 1.0 ) <= tolEdge, buf );
			}
		}
	}
}

//! dl294 (measurement aid, no assertions): the same sweep with n SALTED
//! repeats (argv[2], default 4; P2-2, external review 2026-09-29 --
//! BDPT/VCM's Sobol' streams are keyed by pixel/sample index, so an
//! UNSALTED repeat is bit-identical and its printed "sd" was always
//! 0.000, not an error bar), mean +/- sample sd per cell, plus the
//! NO-WEAVE control (the kTight 0.2 x 0.2 patch at fov 2 under the
//! spot, no sheet, against the analytic rho/pi * I/d^2) and the edge
//! fingerprint of each gap render.
static void MeasureNarrowFovSplat( unsigned int n )
{
	std::cout << "=== dl294: t = 1 splat fov sweep, n = " << n << " SALTED (mean +/- sd; printed, NOT gated) ===" << std::endl;
	for( int k = 0; k < kNumSweepFovs; k++ )
	{
		const double fov = kSweepFovs[k];
		const unsigned int fovBase = SobolSequence::HashCombine( 0xD1294Eu, unsigned( k ) );
		std::vector<double> pt, bL0, bG, vL0, vG, bc0, br0, vc0, vr0, vnG;
		for( unsigned int i = 0; i < n; i++ ) {
			const unsigned int s = SobolSequence::HashCombine( fovBase, i );
			pt.push_back( RenderSalted( Assemble( RastPT( 64 ), ReceiverScene( kSpot, false, 0.0, kWide, false, std::string(), std::string(), fov ) ), "m_pt",
				SobolSequence::HashCombine( s, 0x1u ) ) );
			std::vector<double> px;
			bL0.push_back( RenderSalted( Assemble( RastBDPT( 1024 ), ReceiverScene( kSpot, false, 0.0, kWide, false, std::string(), std::string(), fov ) ), "m_bl0",
				SobolSequence::HashCombine( s, 0x2u ) ) );
			bG.push_back( RenderSalted( Assemble( RastBDPT( 1024 ), ReceiverScene( kSpot, true, kSweepGap, kWide, false, std::string(), std::string(), fov ) ), "m_bg",
				SobolSequence::HashCombine( s, 0x3u ), &px ) );
			EdgeRatios e = EdgeFingerprint( px, 16, 16 ); bc0.push_back( e.col0 ); br0.push_back( e.row0 );
			vL0.push_back( RenderSalted( Assemble( RastVCM( 1024 ), ReceiverScene( kSpot, false, 0.0, kWide, false, std::string(), std::string(), fov ) ), "m_vl0",
				SobolSequence::HashCombine( s, 0x4u ) ) );
			vG.push_back( RenderSalted( Assemble( RastVCM( 1024 ), ReceiverScene( kSpot, true, kSweepGap, kWide, false, std::string(), std::string(), fov ) ), "m_vg",
				SobolSequence::HashCombine( s, 0x5u ), &px ) );
			e = EdgeFingerprint( px, 16, 16 ); vc0.push_back( e.col0 ); vr0.push_back( e.row0 );
			vnG.push_back( RenderSalted( Assemble( RastVCMNoMerge( 1024 ), ReceiverScene( kSpot, true, kSweepGap, kWide, false, std::string(), std::string(), fov ) ), "m_vng",
				SobolSequence::HashCombine( s, 0x6u ) ) );
		}
		double mp, sp; MeanSd( pt, mp, sp );
		auto rel = [&]( const std::vector<double>& v, double scale ) {
			std::vector<double> r; for( double x : v ) r.push_back( 100.0 * ( x / ( scale * mp ) - 1.0 ) ); return r; };
		double m1, s1, m2, s2, m3, s3, m4, s4, e1, f1, e2, f2, e3, f3, e4, f4;
		MeanSd( rel( bL0, 1.0 ), m1, s1 ); MeanSd( rel( bG, kSweepGap ), m2, s2 );
		MeanSd( rel( vL0, 1.0 ), m3, s3 ); MeanSd( rel( vG, kSweepGap ), m4, s4 );
		MeanSd( bc0, e1, f1 ); MeanSd( br0, e2, f2 ); MeanSd( vc0, e3, f3 ); MeanSd( vr0, e4, f4 );
		std::printf( "  dl294 fov %4.1f | PT L0 %.6f +/- %.6f | BDPT L0 %+.3f +/- %.3f %% gap %+.3f +/- %.3f %% | VCM L0 %+.3f +/- %.3f %% gap %+.3f +/- %.3f %%\n",
			fov, mp, sp, m1, s1, m2, s2, m3, s3, m4, s4 );
		double m5, s5; MeanSd( rel( vnG, kSweepGap ), m5, s5 );
		std::printf( "  dl294 fov %4.1f | VCM merging OFF (vc only) gap %+.3f +/- %.3f %%\n", fov, m5, s5 );
		std::printf( "  dl294 fov %4.1f | edges/interior: BDPT gap col0 %.4f +/- %.4f row0 %.4f +/- %.4f | VCM gap col0 %.4f +/- %.4f row0 %.4f +/- %.4f\n",
			fov, e1, f1, e2, f2, e3, f3, e4, f4 );
	}

	// NO-WEAVE control: the tight 0.2 x 0.2 patch, fov 2, spot, no sheet.
	const double analytic = kRho / 3.14159265358979323846 * kOmniPow / ( kLightY * kLightY );
	struct R { const char* label; std::string rast; };
	const R rows[] = { { "PT RGB", RastPT( 64 ) }, { "BDPT RGB", RastBDPT( 1024 ) }, { "VCM RGB", RastVCM( 1024 ) } };
	for( const R& r : rows ) {
		std::vector<double> v;
		for( unsigned int i = 0; i < n; i++ )
			v.push_back( 100.0 * ( Render( Assemble( r.rast, ReceiverScene( kSpot, false, 0.0, kTight ) ), "m_nw" ) / analytic - 1.0 ) );
		double m, sd; MeanSd( v, m, sd );
		std::printf( "  dl294 no-weave control (0.2 patch, fov 2, spot, NO sheet) %s: L0 vs analytic %+.3f +/- %.3f %%\n", r.label, m, sd );
	}
}

//////////////////////////////////////////////////////////////////////
// table: the design doc's item-27 table, n repeats, mean +/- sd.
//////////////////////////////////////////////////////////////////////
static void MeanSd( const std::vector<double>& v, double& mean, double& sd )
{
	mean = 0; sd = 0;
	if( v.empty() ) return;
	for( double x : v ) mean += x;
	mean /= double( v.size() );
	if( v.size() > 1 ) {
		for( double x : v ) sd += ( x - mean ) * ( x - mean );
		sd = std::sqrt( sd / double( v.size() - 1 ) );
	}
}

// Separate corrected-salt audit for the Gaussian-filter fov row.
static void MeasureGaussianSweep()
{
    for(int k=0;k<kNumSweepFovs;++k) {
        const double fov=kSweepFovs[k];
        std::vector<double> ratios;
        for(unsigned int t=0;t<4;++t) {
            const unsigned int salt=SobolSequence::HashCombine(390u+unsigned(k),t);
            const double base=RenderSalted(Assemble(RastPT(64),ReceiverScene(kSpot,false,0.0,kWide,false,std::string(),std::string(),fov)),"gm_base",salt);
            const double gap=RenderSalted(Assemble(RastBDPTDefaultFilter(1024),ReceiverScene(kSpot,true,kSweepGap,kWide,false,std::string(),std::string(),fov)),"gm_gap",SobolSequence::HashCombine(salt,1u));
            Check(base>0 && gap>=0,"Gaussian calibration finite and lit");
            ratios.push_back(gap/(kSweepGap*base));
        }
        double mean,sd; MeanSd(ratios,mean,sd);
        std::cout << "Gaussian fov=" << fov << " ratio=" << mean << " sd=" << sd << " n=4" << std::endl;
    }
}

//! Opt-in (WEAVE_GAP_FILTER=dl330, argv[2] = n): the closed weave sphere
//! (gap 0.3 and 0.0) lit from outside by the omni light (a DELTA light)
//! and by a small spherical AREA emitter at the same place, PT / BDPT /
//! VCM, salted repeats.  Discriminates a delta-light-specific BDPT
//! coverage gap (light -> gap -> diffuse -> gap -> eye has no BDPT
//! strategy when the light is a point) from an MIS / contribution bias,
//! which would not care what kind of light it is.
static void MeasureDL330( unsigned int nRepeats )
{
	std::cout << "=== dl330: closed weave sphere, omni vs small area light (24x24, 512 spp, n = "
		<< nRepeats << ") ===" << std::endl;

	for( int light = 0; light < 2; light++ ) {
		for( int gi = 0; gi < 2; gi++ ) {
			const double gap = gi == 0 ? 0.3 : 0.0;
			std::string body = LayerScene( kSphere, gap, -3.0, 24 );
			// Off-axis (3, 0, -2): behind the sphere but OUTSIDE the
			// frustum, so the area emitter is never seen directly
			// through two gaps (that term would swamp the comparison).
			{
				const std::string onAxis = "\tposition 0 0 -3\n";
				const size_t p = body.find( onAxis );
				if( p != std::string::npos ) body.replace( p, onAxis.size(), "\tposition 3 0 -2\n" );
			}
			if( light == 1 ) {
				// Swap the omni for a radius-0.05 sphere emitter of the same
				// total power: Phi = 4 pi I = 4 pi * 6; a Lambertian sphere
				// of radius r and exitance M emits 4 pi r^2 M, so
				// M = 6 / r^2 = 2400 (exitance 1 * scale 2400).
				const std::string omni = "omni_light\n{\n\tname lgt\n\tposition 3 0 -2\n\tcolor 1.0 1.0 1.0\n\tpower 6.0\n}\n\n";
				const size_t at = body.find( omni );
				if( at == std::string::npos ) { std::cout << "  omni chunk not found" << std::endl; return; }
				body.replace( at, omni.size(),
					"uniformcolor_painter\n{\n\tname pnt_e\n\tcolor 1 1 1\n\tcolorspace Rec709RGB_Linear\n}\n\n"
					"lambertian_luminaire_material\n{\n\tname mat_e\n\texitance pnt_e\n\tscale 2400.0\n\tmaterial none\n}\n\n"
					"sphere_geometry\n{\n\tname g_e\n\tradius 0.05\n}\n\n"
					"standard_object\n{\n\tname o_e\n\tgeometry g_e\n\tmaterial mat_e\n\tposition 3 0 -2\n}\n\n" );
			}
			std::vector<double> pt, bd, vc;
			for( unsigned int i = 0; i < nRepeats; i++ ) {
				pt.push_back( Render( Assemble( RastPT( 512 ),   body ), "d330_pt" ) );
				bd.push_back( Render( Assemble( RastBDPT( 512 ), body ), "d330_bdpt" ) );
				vc.push_back( Render( Assemble( RastVCM( 512 ),  body ), "d330_vcm" ) );
			}
			double mp, sp, mb, sb, mv, sv;
			MeanSd( pt, mp, sp ); MeanSd( bd, mb, sb ); MeanSd( vc, mv, sv );
			std::printf( "  | %-6s gap %.1f | PT %.6f +/- %.6f | BDPT %.6f +/- %.6f | VCM %.6f +/- %.6f | BDPT/PT %.4f | VCM/PT %.4f |\n",
				light == 0 ? "omni" : "area", gap, mp, sp, mb, sb, mv, sv, mb / mp, mv / mp );
		}
	}

	SobolSamplerTestHooks::ValueSalt().store( 0u );
}

//! Opt-in (WEAVE_GAP_FILTER=dl425, argv[2] = n): DL-425's curtain case --
//! the `closed` receiver under ONE gapped sheet (gap 0.3, kTight camera)
//! lit by the OMNI light, seen directly.  BDPT RGB at several spp, n
//! salted renders each: mean L/L0, per-render sd of L/L0, wall seconds
//! per render.  L0 is the no-sheet PT render (n-averaged).  Before DL-425
//! the see-through connection took weight 0 here (light tracing covers
//! the path), leaving the receiver to t = 1 splats alone.
static void MeasureDL425( unsigned int n )
{
	const double g = 0.3;
	std::cout << "=== dl425: curtain, omni, gap 0.3, BDPT RGB (n = " << n << ") ===" << std::endl;
	std::vector<double> l0v;
	for( unsigned int i = 0; i < n; i++ ) {
		l0v.push_back( RenderSalted( Assemble( RastPT( 64 ), ReceiverScene( kOmni, false, 0.0, kTight ) ),
			"d425_l0", SobolSequence::HashCombine( 0xD425u, i ) ) );
	}
	double L0, sL0;
	MeanSd( l0v, L0, sL0 );
	std::cout << "  PT L0 = " << L0 << " +/- " << sL0 << " (sd)" << std::endl;
	const unsigned int spps[] = { 16, 64, 256 };
	for( unsigned int spp : spps ) {
		std::vector<double> rs, secs;
		for( unsigned int i = 0; i < n; i++ ) {
			const auto t0 = std::chrono::steady_clock::now();
			const double L = RenderSalted( Assemble( RastBDPT( spp ), ReceiverScene( kOmni, true, g, kTight ) ),
				"d425_l", SobolSequence::HashCombine( 0xD4251u + spp, i ) );
			const auto t1 = std::chrono::steady_clock::now();
			secs.push_back( std::chrono::duration<double>( t1 - t0 ).count() );
			rs.push_back( L / L0 / g );
		}
		double m, sd, ms, ss;
		MeanSd( rs, m, sd ); MeanSd( secs, ms, ss );
		std::printf( "  BDPT %4u spp: (L/L0)/g = %.4f  per-render sd %.4f  se %.4f  (%.2f s/render)\n",
			spp, m, sd, sd / std::sqrt( double( n ) ), ms );
	}
}

//! Opt-in (WEAVE_GAP_FILTER=dl424, argv[2] = n): VCM RGB on the
//! `seethrough` scene (L - gap - patch - gap - camera, omni, gap 0.3,
//! closed form g^2) and on the curtain (`closed` omni, gap 0.3, closed
//! form g), n salted renders each: mean, per-render sd, s/render.
static void MeasureDL424( unsigned int n )
{
	const double g = 0.3;
	std::cout << "=== dl424: VCM RGB, omni, gap 0.3 (n = " << n << ") ===" << std::endl;
	for( int scene = 0; scene < 2; scene++ ) {
		const double cf = scene == 0 ? g * g : g;
		const std::string extra = scene == 0 ? BlackWeaveSheet( g ) + VerticalBlackSheetChunks() : std::string();
		const unsigned int spps[] = { 64, 256 };
		for( unsigned int spp : spps ) {
			std::vector<double> rs, secs;
			for( unsigned int i = 0; i < n; i++ ) {
				const unsigned int salt = SobolSequence::HashCombine( 0xD424u + 0x100u * scene + spp, i );
				const double L0 = RenderSalted( Assemble( RastPT( 64 ), ReceiverScene( kOmni, false, 0.0, kTight ) ), "d424_l0", salt );
				const auto t0 = std::chrono::steady_clock::now();
				const double L = RenderSalted( Assemble( RastVCM( spp ), ReceiverScene( kOmni, true, g, kTight, false, extra ) ),
					"d424_l", SobolSequence::HashCombine( salt, 0x51u ) );
				const auto t1 = std::chrono::steady_clock::now();
				secs.push_back( std::chrono::duration<double>( t1 - t0 ).count() );
				rs.push_back( L / L0 / cf );
			}
			double m, sd, ms, ss;
			MeanSd( rs, m, sd ); MeanSd( secs, ms, ss );
			std::printf( "  VCM %-10s %4u spp: (L/L0)/cf = %.4f  per-render sd %.4f  se %.4f  (%.2f s/render)\n",
				scene == 0 ? "seethrough" : "curtain", spp, m, sd, sd / std::sqrt( double( n ) ), ms );
		}
	}
}

static void MeasureDesignDocTable( unsigned int nRepeats )
{
	std::cout << "=== table: docs/CLOTH_FABRIC_DESIGN.md section 15 item 27 (24x24, 512 spp, n = "
		<< nRepeats << ") ===" << std::endl;
	struct T { LayerGeom geom; double gap; double lightZ; const char* label; };
	const T rows[] = {
		{ kBox,       0.3, -3.0, "box gap 0.3, light outside" },
		{ kSixPlanes, 0.3, -3.0, "six planes gap 0.3, light outside" },
		{ kBox,       0.1, -3.0, "box gap 0.1, light outside" },
		{ kSixPlanes, 0.1, -3.0, "six planes gap 0.1, light outside" },
		{ kTwoPlanes, 0.1, -3.0, "two planes gap 0.1, light outside" },
		{ kOnePlane,  0.3, -3.0, "single plane gap 0.3" },
		{ kBox,       0.3, -0.2, "box gap 0.3, light INSIDE" },
		// Gap-0 controls: no pass-through exists, so DL-05 cannot move
		// them -- they carry the separate, pre-existing ~3 % BDPT/VCM-
		// under-PT closed-box residual docs/CLOTH_FABRIC_DESIGN.md
		// section 15 debt 25 records.
		{ kBox,       0.0, -3.0, "box gap 0.0, light outside (control)" },
		{ kTwoPlanes, 0.0, -3.0, "two planes gap 0.0 (control)" },
		{ kSphere,    0.3, -3.0, "sphere gap 0.3, light outside" },
		{ kSphere,    0.0, -3.0, "sphere gap 0.0, light outside (control)" },
	};
	// WEAVE_GAP_TABLE_ROWS (optional): only rows whose label contains it.
	const char* rowFilter = std::getenv( "WEAVE_GAP_TABLE_ROWS" );
	// SALTED (DL-330 slice, 2026-10-02): every repeat is an independent
	// randomized-QMC replicate.  Before, the repeats differed only in libc
	// `rand()` -- which no Sobol' stream reads -- so the "+/- sd" columns
	// of section 5's table were ~0 by construction and every BDPT/PT and
	// VCM/PT ratio there is ONE Sobol' point set, not a distribution.

	for( const T& r : rows )
	{
		if( rowFilter && !std::strstr( r.label, rowFilter ) ) continue;
		const std::string body = LayerScene( r.geom, r.gap, r.lightZ, 24 );
		std::vector<double> pt, bd, vc;
		for( unsigned int i = 0; i < nRepeats; i++ ) {
			pt.push_back( Render( Assemble( RastPT( 512 ),   body ), "tab_pt" ) );
			bd.push_back( Render( Assemble( RastBDPT( 512 ), body ), "tab_bdpt" ) );
			vc.push_back( Render( Assemble( RastVCM( 512 ),  body ), "tab_vcm" ) );
		}
		double mp, sp, mb, sb, mv, sv;
		MeanSd( pt, mp, sp ); MeanSd( bd, mb, sb ); MeanSd( vc, mv, sv );
		std::printf( "  | %-34s | PT %.5f +/- %.5f | BDPT %.5f +/- %.5f | VCM %.5f +/- %.5f | BDPT/PT %.4f | VCM/PT %.4f |\n",
			r.label, mp, sp, mb, sb, mv, sv, mb / mp, mv / mp );
	}

	SobolSamplerTestHooks::ValueSalt().store( 0u );
}

//////////////////////////////////////////////////////////////////////
// scenehash (opt-in only; WEAVE_GAP_FILTER=scenehash): DL-295's
// "nothing without a non-caster delta vertex moves" proof.  Renders every
// scene file named in WEAVE_GAP_SCENES (whitespace-separated; default:
// every shipped scene whose ACTIVE rasterizer sets `sms_enabled TRUE`,
// kSMSScenes below) with
// std::srand(4242) and the Sobol' salt 0, every `samples` line capped at
// WEAVE_GAP_SCENE_SPP (default 8), and prints the mean luminance and an
// FNV-1a hash of the captured pixels.  Deterministic because main() forces
// a single render worker (a multithreaded render seeds each worker's RNG
// from libc rand() in thread-start order).  Compare two separately built
// binaries' output line by line.  `pool_caustics_vcm` is NOT in the list:
// its SMS rasterizer chunk is commented out.
//////////////////////////////////////////////////////////////////////
static const char* kSMSScenes =
	"scenes/Tests/Caustics/diacaustic_pt_sms.RISEscene "
	"scenes/Tests/Caustics/triplecaustic_pt_sms.RISEscene "
	"scenes/Tests/SMS/sms_k1_botonly.RISEscene "
	"scenes/Tests/SMS/sms_k1_refract.RISEscene "
	"scenes/Tests/SMS/sms_k2_flatslab.RISEscene "
	"scenes/Tests/SMS/sms_k2_glassblock.RISEscene "
	"scenes/Tests/SMS/sms_k2_glasssphere.RISEscene "
	"scenes/Tests/SMS/sms_k2_glasssphere_tess.RISEscene "
	"scenes/Tests/SMS/sms_k2_glasssphere_tess_disp.RISEscene "
	"scenes/Tests/SMS/sms_k2_torus_cross.RISEscene "
	"scenes/Tests/SMS/sms_luminous_orb.RISEscene "
	"scenes/Tests/SMS/sms_slab_close_pt_sms_hispp.RISEscene "
	"scenes/Tests/SMS/sms_slab_close_sms.RISEscene "
	"scenes/Tests/SMS/sms_teapot_close_sms.RISEscene "
	"scenes/Tests/SMS/sms_veach_egg.RISEscene "
	"scenes/Tests/SMS/sms_veach_egg_bumpmap.RISEscene "
	"scenes/Tests/SMS/sms_veach_egg_displaced.RISEscene "
	"scenes/Tests/SMS/sms_visibility_occluded.RISEscene "
	"scenes/Tests/SMS/sms_visibility_unoccluded.RISEscene "
	"scenes/Tests/Spectral/sms_through_glass_emitter_pt_sms.RISEscene "
	"scenes/Tests/Spectral/spectral_dispersive_caustic_pt_sms.RISEscene";
//////////////////////////////////////////////////////////////////////
static void HashScenes()
{
	const char* listEnv = std::getenv( "WEAVE_GAP_SCENES" );
	const char* list = listEnv ? listEnv : kSMSScenes;
	const char* sppEnv = std::getenv( "WEAVE_GAP_SCENE_SPP" );
	const long cap = sppEnv ? std::strtol( sppEnv, nullptr, 10 ) : 8;
	std::istringstream names( list );
	std::string path;
	while( names >> path )
	{
		std::ifstream ifs( path );
		if( !ifs.is_open() ) {
			std::cout << "scenehash " << path << ": cannot open" << std::endl;
			continue;
		}
		std::ostringstream text;
		std::string line;
		while( std::getline( ifs, line ) ) {
			const size_t k = line.find_first_not_of( " \t" );
			if( k != std::string::npos && line.compare( k, 7, "samples" ) == 0 &&
			    ( line.size() == k + 7 || line[k + 7] == ' ' || line[k + 7] == '\t' ) ) {
				const long n = std::strtol( line.c_str() + k + 7, nullptr, 10 );
				if( n > cap && cap > 0 ) {
					line = line.substr( 0, k ) + "samples " + std::to_string( cap );
				}
			}
			text << line << "\n";
		}
		g_seedBase = 4242;
		g_renderIndex = 0;
		const double L = Render( text.str(), "scenehash" );
		std::printf( "scenehash %s: mean %.9f hash %016llx\n", path.c_str(), L, g_lastPixelHash );
	}
}

//////////////////////////////////////////////////////////////////////
// dl295audit (opt-in only; WEAVE_GAP_FILTER=dl295audit, argv[2] = n):
// the DOUBLE-COUNT audit.  sms_k2_glasssphere's caster (a perfect
// refractor sphere, ior 1.5, r 0.3 at y 0.6, an SMS caster) over a
// Lambertian floor, a 1 x 1 area emitter at y 1.8, and a BLACK-yarn gap
// 0.3 weave sheet at y 1.2 between the emitter and the sphere -- so the
// caustic path is  light -> weave gap -> glass -> glass -> floor.
// `cover`: 0 no sheet (control), 1 sheet over the whole emitter, 2 sheet
// over the x < 0 half only (SMS still reaches the x > 0 half directly).
// Two sibling probes, no sheet: 3 a global scattering medium (a medium
// vertex after the glass -- does PART 1's latch survive it?), 4 the
// sphere at ior 1.0 (an SMS caster whose refraction constraint is
// degenerate -- does SMS still solve it?).
// Prints PT+SMS, PT without SMS and VCM, mean +/- sd over n salted
// replicates, whole image and a caustic ROI.  No assertions.
//////////////////////////////////////////////////////////////////////
static std::string CausticAuditScene( int cover )
{
	std::ostringstream ss;
	ss <<
		"film\n{\n\twidth 48\n\theight 36\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 2.0 3\n\tlookat 0 0.2 0\n\tup 0 1 0\n\tfov 45.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_floor\n\tcolor 0.8 0.75 0.65\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1 1 1\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_glass\n\tcolor 1 1 1\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		"lambertian_material\n{\n\tname floor_mat\n\treflectance pnt_floor\n}\n\n"
		"lambertian_luminaire_material\n{\n\tname light_mat\n\texitance pnt_emit\n\tscale 20.0\n\tmaterial none\n}\n\n"
		"perfectrefractor_material\n{\n\tname glass_mat\n\trefractance pnt_glass\n\tior " << ( cover == 4 ? "1.0" : "1.5" ) << "\n}\n\n"
		"sphere_geometry\n{\n\tname sphere_geom\n\tradius 0.3\n}\n\n"
		"clippedplane_geometry\n{\n\tname floor_geom\n\tpta -4 0 -4\n\tptb -4 0 4\n\tptc 4 0 4\n\tptd 4 0 -4\n}\n\n"
		"clippedplane_geometry\n{\n\tname light_geom\n\tpta -0.5 1.8 -0.5\n\tptb 0.5 1.8 -0.5\n\tptc 0.5 1.8 0.5\n\tptd -0.5 1.8 0.5\n}\n\n"
		"standard_object\n{\n\tname floor\n\tgeometry floor_geom\n\tmaterial floor_mat\n}\n\n"
		"standard_object\n{\n\tname glass_ball\n\tgeometry sphere_geom\n\tposition 0 0.6 0\n\tmaterial glass_mat\n}\n\n"
		"standard_object\n{\n\tname area_light\n\tgeometry light_geom\n\tmaterial light_mat\n}\n\n";
	if( cover == 3 ) {
		ss << "homogeneous_medium\n{\n\tname fog\n\tabsorption 0 0 0\n\tscattering 0.25 0.25 0.25\n\tphase isotropic\n}\n\n"
		      "global_medium\n{\n\tmedium fog\n}\n\n";
	}
	if( cover == 1 || cover == 2 ) {
		ss << BlackPainterChunk() << BlackWeaveChunk( "mat_sheet", 0.3 )
		   << "clippedplane_geometry\n{\n\tname geo_sheet\n"
		   << ( cover == 1
				? "\tpta -3 1.2 3\n\tptb 3 1.2 3\n\tptc 3 1.2 -3\n\tptd -3 1.2 -3\n"
				: "\tpta -3 1.2 3\n\tptb 0 1.2 3\n\tptc 0 1.2 -3\n\tptd -3 1.2 -3\n" )
		   << "\tdoublesided TRUE\n}\n\n"
		   "standard_object\n{\n\tname obj_sheet\n\tgeometry geo_sheet\n\tmaterial mat_sheet\n}\n\n";
	}
	return ss.str();
}

static void AuditMeans( double& whole, double& roi )
{
	whole = 0; roi = 0;
	const unsigned int w = 48, h = 36;
	if( g_lastPixels.size() != w * h ) { whole = roi = -1; return; }
	unsigned int nr = 0;
	for( unsigned int y = 0; y < h; y++ ) {
		for( unsigned int x = 0; x < w; x++ ) {
			const RISEColor& c = g_lastPixels[y * w + x];
			const double l = 0.2126 * c.base.r * c.a + 0.7152 * c.base.g * c.a + 0.0722 * c.base.b * c.a;
			whole += l;
			// The caustic under the sphere: rows 60-85 %, columns 35-65 %.
			if( y >= h * 60 / 100 && y < h * 85 / 100 && x >= w * 35 / 100 && x < w * 65 / 100 ) {
				roi += l;
				nr++;
			}
		}
	}
	whole /= double( w * h );
	roi /= double( nr ? nr : 1 );
}

static void MeasureCausticAudit( unsigned int n, unsigned int ptSpp, unsigned int vcmSpp )
{
	const char* coverName[5] = { "no sheet (control)", "sheet over the whole emitter", "sheet over the x<0 half",
		"no sheet, global fog", "no sheet, sphere ior 1.0" };
	const char* only = std::getenv( "WEAVE_GAP_AUDIT_COVERS" );	// e.g. "34"; default "012"
	for( int cover = 0; cover < 5; cover++ )
	{
		if( !std::strchr( only ? only : "012", char( '0' + cover ) ) ) continue;
		struct R { const char* label; std::string rast; };
		const R rows[] = {
			{ "PT+SMS", RastPTSMS( ptSpp, true ) },
			{ "PT (no SMS)", RastPTSMS( ptSpp, false ) },
			{ "VCM", RastVCM( vcmSpp ) } };
		double m[3][2] = {}, sd[3][2] = {};
		for( int r = 0; r < 3; r++ ) {
			std::vector<double> wv, rv;
			for( unsigned int i = 0; i < n; i++ ) {
				double wm = 0, rm = 0;
				Render( Assemble( rows[r].rast, CausticAuditScene( cover ) ), "audit" );
				AuditMeans( wm, rm );
				wv.push_back( wm );
				rv.push_back( rm );
			}
			MeanSd( wv, m[r][0], sd[r][0] );
			MeanSd( rv, m[r][1], sd[r][1] );
		}
		std::printf( "  dl295audit %-30s | whole: PT+SMS %.5f +/- %.5f  PT %.5f +/- %.5f  VCM %.5f +/- %.5f  | SMS/PT %.4f  SMS/VCM %.4f\n",
			coverName[cover], m[0][0], sd[0][0], m[1][0], sd[1][0], m[2][0], sd[2][0], m[0][0] / m[1][0], m[0][0] / m[2][0] );
		std::printf( "  dl295audit %-30s | ROI:   PT+SMS %.5f +/- %.5f  PT %.5f +/- %.5f  VCM %.5f +/- %.5f  | SMS/PT %.4f  SMS/VCM %.4f\n",
			coverName[cover], m[0][1], sd[0][1], m[1][1], sd[1][1], m[2][1], sd[2][1], m[0][1] / m[1][1], m[0][1] / m[2][1] );
	}
	SobolSamplerTestHooks::ValueSalt().store( 0u );
}

//////////////////////////////////////////////////////////////////////
// split (DL-372 / DL-336 split suppression).  PT with `sms_enabled` used
// to drop every BSDF-sampled emitter hit behind an SMS anchor and an
// all-caster chain; snell-mode SMS seeds ONE deterministic chain per
// light sample, so at a multi-root caustic the roots the seed misses were
// dropped by PT and estimated by nobody.  PT now suppresses a hit only if
// SMS's own seed + solve from the anchor reaches it
// (ManifoldSolver::ClassifyEmitterHitCoverage).
//
// The fixture is `sms_visibility_unoccluded`'s ball lens (r 1 at y 1.5,
// the floor at its paraxial focus) with the camera framing the caustic
// under it (24 x 24, fov 6): pre-fix PT+SMS/PT reads ~0.37 there (single
// render) where the shipped 200 x 200 view averages it to 0.917.  The
// same lens as a perfect refractor (every PT chain is exactly a root) and
// as the shipped dielectric (`scattering 100000`, a warped lobe: PT's
// chain is assigned to the root Newton reaches from it).  Bands: see
// TestSMSSplitSuppression.
//////////////////////////////////////////////////////////////////////
static std::string BallLensCausticScene( bool dielectric )
{
	std::ostringstream ss;
	ss << "film\n{\n\twidth 24\n\theight 24\n}\n\n"
	      "pinhole_camera\n{\n\tlocation 0 4 8\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 6.0\n}\n\n"
	      "uniformcolor_painter\n{\n\tname pnt_floor\n\tcolor 0.7 0.7 0.7\n}\n\n"
	      "uniformcolor_painter\n{\n\tname pnt_light\n\tcolor 1.0 1.0 1.0\n}\n\n"
	      "uniformcolor_painter\n{\n\tname pnt_glass_tau\n\tcolor 0.999 0.999 0.999\n}\n\n"
	      "lambertian_material\n{\n\tname floor_mat\n\treflectance pnt_floor\n}\n\n"
	      "lambertian_luminaire_material\n{\n\tname light_mat\n\texitance pnt_light\n\tscale 500.0\n\tmaterial none\n}\n\n";
	if( dielectric ) {
		ss << "dielectric_material\n{\n\tname glass_mat\n\ttau 0.999\n\tior 1.5\n\tscattering 100000\n}\n\n";
	} else {
		ss << "perfectrefractor_material\n{\n\tname glass_mat\n\trefractance pnt_glass_tau\n\tior 1.5\n}\n\n";
	}
	ss << "sphere_geometry\n{\n\tname sphere_geom\n\tradius 1.0\n}\n\n"
	      "clippedplane_geometry\n{\n\tname floor_geom\n\tpta -5.0 0.0 -5.0\n\tptb -5.0 0.0 5.0\n\tptc 5.0 0.0 5.0\n\tptd 5.0 0.0 -5.0\n}\n\n"
	      "clippedplane_geometry\n{\n\tname light_geom\n\tpta -1.5 0.0 -1.5\n\tptb 1.5 0.0 -1.5\n\tptc 1.5 0.0 1.5\n\tptd -1.5 0.0 1.5\n}\n\n"
	      "standard_object\n{\n\tname floor\n\tgeometry floor_geom\n\tmaterial floor_mat\n}\n\n"
	      "standard_object\n{\n\tname glass_sphere\n\tgeometry sphere_geom\n\tposition 0 1.5 0\n\tmaterial glass_mat\n}\n\n"
	      "standard_object\n{\n\tname area_light\n\tgeometry light_geom\n\tposition 0 5.0 0\n\tmaterial light_mat\n}\n\n";
	return ss.str();
}

static const unsigned int kSplitSpp = 256;

//! Opt-in band measurement (WEAVE_GAP_FILTER=splitmeasure, argv[2] = n):
//! n salted replicates of each gated split row, mean +/- sd of SMS on/off.
static void MeasureSplitRows( unsigned int n )
{
	struct M { const char* label; std::string scene; bool hwss; };
	const M rows[] = {
		{ "ball lens caustic, perfect refractor", BallLensCausticScene( false ), false },
		{ "ball lens caustic, dielectric scattering 1e5", BallLensCausticScene( true ), false },
		{ "receiver -> smooth-SSS ceiling (anchored, no gap)", CasterCeilingScene( false, false, false ), false },
		{ "receiver -> polished ceiling (anchored, no gap)", CasterCeilingScene( true, false, false ), false },
		{ "receiver -> smooth-SSS ceiling (anchored, no gap) HWSS", CasterCeilingScene( false, false, false ), true },
		{ "ball lens caustic, perfect refractor HWSS", BallLensCausticScene( false ), true },
	};
	const char* only = std::getenv( "WEAVE_GAP_SPLIT_ROWS" );	// e.g. "0145"
	for( size_t ri = 0; ri < sizeof( rows ) / sizeof( rows[0] ); ri++ ) {
		const M& r = rows[ri];
		if( only && !std::strchr( only, char( '0' + ri ) ) ) continue;
		std::vector<double> q;
		for( unsigned int i = 0; i < n; i++ ) {
			const double on  = Render( Assemble( r.hwss ? RastPTSpectralSMS( kSplitSpp, true, true ) : RastPTSMS( kSplitSpp, true ), r.scene ), "split_on" );
			const double off = Render( Assemble( r.hwss ? RastPTSpectralSMS( kSplitSpp, true, false ) : RastPTSMS( kSplitSpp, false ), r.scene ), "split_off" );
			q.push_back( off > 0 ? on / off : -1.0 );
		}
		double m = 0, sd = 0;
		MeanSd( q, m, sd );
		std::printf( "  splitmeasure %-52s SMS on/off %.5f +/- %.5f (n %u)\n", r.label, m, sd, n );
	}
	SobolSamplerTestHooks::ValueSalt().store( 0u );
}

static void TestSMSSplitSuppression()
{
	std::cout << "=== split: PT keeps the anchored emitter hits SMS's seed does not reach (DL-372 / DL-336) ===" << std::endl;
	// BANDS from MeasureSplitRows (single worker, seed base 1000, salted),
	// docs/SMS_ENERGY_LOSS_INVESTIGATION.md section 7; each band holds the
	// measured mean by >= 4 sd and excludes the pre-split value by > 40 sd.
	//   perfect refractor      0.99661 +/- 0.00772 (n 8); pre-split 0.38101 +/- 0.00703
	//   dielectric 1e5         1.03378 +/- 0.01055 (n 8); pre-split 0.38278 +/- 0.00850
	//   perfect refractor HWSS 0.99218 +/- 0.00636 (n 4); pre-split 0.38609 +/- 0.01050 (n 8)
	// The dielectric's +3.4 % is the warped-lobe residual: its PT chain is
	// not a root, and is assigned to the root Newton reaches from it -- the
	// delta-limit partition, not an exact one (at the paraxial focus a 3e-3
	// rad warp scatters PT's chains across roots).  The shipped 200 x 200
	// view, where the caustic is a fraction of the image, reads 1.0025.
	RatioBandRow( "ball lens caustic, perfect refractor (DL-372 twin)",
		RastPTSMS( kSplitSpp, true ), RastPTSMS( kSplitSpp, false ), BallLensCausticScene( false ), 0.96, 1.035 );
	RatioBandRow( "ball lens caustic, dielectric scattering 1e5 (shipped DL-372)",
		RastPTSMS( kSplitSpp, true ), RastPTSMS( kSplitSpp, false ), BallLensCausticScene( true ), 0.99, 1.08 );
	RatioBandRow( "ball lens caustic, perfect refractor HWSS (no-BSDF hand-off carries the record)",
		RastPTSpectralSMS( kSplitSpp, true, true ), RastPTSpectralSMS( kSplitSpp, true, false ), BallLensCausticScene( false ), 0.96, 1.03 );
	SobolSamplerTestHooks::ValueSalt().store( 0u );
}

static char g_optPath[512] = { 0 };

// Exact regression for DL-390: Render used to overwrite RenderSalted's
// caller-supplied salt with zero, making four independent salts identical.
static void TestExplicitSaltContract()
{
    const std::string scene = Assemble( RastBDPT( 64 ), ReceiverScene( kSpot, true, 0.3, kWide, true ) );
    unsigned long long previous = 0;
    const unsigned int startIndex=g_renderIndex;
    const RandomNumberGenerator priorRng=GlobalRNG();
    for( unsigned int i = 0; i < 4; ++i ) {
        // Hold both RNGs fixed so only the explicit value salt changes.
        GlobalRNG()=RandomNumberGenerator(390u);
        g_renderIndex=startIndex;
        const double v = RenderSalted( scene, "salt_contract", SobolSequence::HashCombine( 390u, i ) );
        Check( v >= 0, "salt contract: valid render" );
        if( i ) Check( g_lastPixelHash != previous, "salt contract: distinct value salts change BDPT point set" );
        previous = g_lastPixelHash;
    }
    GlobalRNG()=priorRng;
    g_renderIndex=startIndex+4;
}

int main( int argc, char** argv )
{
	// DL-295 review round 2 (P2-1/P3-3): run every render on ONE worker
	// unless the caller supplies its own options file.  A multithreaded
	// render seeds each worker's RNG from libc rand() in thread-start order
	// (RasterizeDispatchers) and hands tiles to workers nondeterministically,
	// so the same seed base and Sobol' salt read differently run to run;
	// single-threaded rendering draws from GlobalRNG() and is exactly
	// reproducible.  Every fixture here is at most 24 x 24 -- a single
	// 32-pixel tile -- so one worker costs a few percent.  With it a seed
	// sweep IS the run-to-run spread, and the `sms` bands are set from one.
	// (Must precede the first GlobalOptions() read, which caches.)
	if( !std::getenv( "RISE_OPTIONS_FILE" ) ) {
		std::snprintf( g_optPath, sizeof(g_optPath), "/tmp/weave_gap_options_%d.txt", static_cast<int>( ::getpid() ) );
		char* optPath = g_optPath;
		std::atexit( []() { std::remove( g_optPath ); } );
		std::ofstream opt( optPath );
		opt << "force_number_of_threads 1\n";
		opt.close();
#ifdef _WIN32
		_putenv_s( "RISE_OPTIONS_FILE", optPath );
#else
		setenv( "RISE_OPTIONS_FILE", optPath, 1 );
#endif
	}

	if( argc > 1 ) {
		const long v = std::strtol( argv[1], nullptr, 10 );
		if( v > 0 ) g_seedBase = (unsigned int)v;
	}
	std::srand(g_seedBase);
	GlobalRNG()=RandomNumberGenerator(g_seedBase);
	std::cout << "WeaveGapShadowTransmittanceTest (DL-05)   seed base = " << g_seedBase << std::endl;

	const char* filter = std::getenv( "WEAVE_GAP_FILTER" );
	if( filter && std::strstr( filter, "gaussianmeasure" ) ) { MeasureGaussianSweep(); return failCount ? 1 : 0; }
	if( filter && std::strstr( filter, "compositemeasure" ) ) { MeasureComposite( 16 ); return 0; }
	if( filter && std::strstr( filter, "dl294" ) ) {
		unsigned int n = 4;
		if( argc > 2 ) {
			const long v = std::strtol( argv[2], nullptr, 10 );
			if( v > 0 ) n = (unsigned int)v;
		}
		MeasureNarrowFovSplat( n );
		return 0;
	}
	if( filter && std::strstr( filter, "dl295probe" ) ) {
		ProbeReviewScenes();
		return 0;
	}
	if( filter && std::strstr( filter, "dl295audit" ) ) {
		unsigned int n = 3;
		if( argc > 2 ) {
			const long v = std::strtol( argv[2], nullptr, 10 );
			if( v > 0 ) n = (unsigned int)v;
		}
		const char* ptSppEnv = std::getenv( "WEAVE_GAP_AUDIT_PT_SPP" );
		const char* vcmSppEnv = std::getenv( "WEAVE_GAP_AUDIT_VCM_SPP" );
		MeasureCausticAudit( n, ptSppEnv ? (unsigned int)std::strtol( ptSppEnv, nullptr, 10 ) : 1024u,
			vcmSppEnv ? (unsigned int)std::strtol( vcmSppEnv, nullptr, 10 ) : 1024u );
		return 0;
	}
	if( filter && std::strstr( filter, "splitmeasure" ) ) {
		unsigned int n = 8;
		if( argc > 2 ) {
			const long v = std::strtol( argv[2], nullptr, 10 );
			if( v > 0 ) n = (unsigned int)v;
		}
		MeasureSplitRows( n );
		return 0;
	}
	if( filter && std::strstr( filter, "scenehash" ) ) {
		HashScenes();
		return 0;
	}
	if( filter && std::strstr( filter, "dl330" ) ) {
		unsigned int n = 4;
		if( argc > 2 ) {
			const long v = std::strtol( argv[2], nullptr, 10 );
			if( v > 0 ) n = (unsigned int)v;
		}
		MeasureDL330( n );
		return 0;
	}
	if( filter && std::strstr( filter, "dl424" ) ) {
		unsigned int n = 8;
		if( argc > 2 ) {
			const long v = std::strtol( argv[2], nullptr, 10 );
			if( v > 0 ) n = (unsigned int)v;
		}
		MeasureDL424( n );
		return 0;
	}
	if( filter && std::strstr( filter, "dl425" ) ) {
		unsigned int n = 8;
		if( argc > 2 ) {
			const long v = std::strtol( argv[2], nullptr, 10 );
			if( v > 0 ) n = (unsigned int)v;
		}
		MeasureDL425( n );
		return 0;
	}
	if( filter && std::strstr( filter, "table" ) ) {
		unsigned int n = 4;
		if( argc > 2 ) {
			const long v = std::strtol( argv[2], nullptr, 10 );
			if( v > 0 ) n = (unsigned int)v;
		}
		MeasureDesignDocTable( n );
		return 0;
	}

	if( !filter || std::strstr( filter, "saltcontract" ) ) TestExplicitSaltContract();
	if( !filter || std::strstr( filter, "query" ) )       TestQueryMatchesSampler();
	if( !filter || std::strstr( filter, "closed" ) )      TestClosedFormOmni();
	if( !filter || std::strstr( filter, "composite" ) )   TestClosedFormComposite();
	if( !filter || std::strstr( filter, "directional" ) ) TestClosedFormDirectional();
	if( !filter || std::strstr( filter, "area" ) )        TestAreaPartitionGuard();
	if( !filter || std::strstr( filter, "hwssgap" ) )     TestHWSSGapContinuation();
	if( !filter || std::strstr( filter, "hwsstint" ) )    TestHWSSTintedCoatGap();
	if( !filter || std::strstr( filter, "seethrough" ) )  TestBDPTSeeThroughDeltaLight();
	if( !filter || std::strstr( filter, "sssgap" ) )      TestSSSBehindGap();
	if( !filter || std::strstr( filter, "sms" ) )         TestSMSEmissionThroughGap();
	if( !filter || std::strstr( filter, "castsshadows" ) ) TestCastsShadowsFalseStepOver();
	if( !filter || std::strstr( filter, "layers" ) )      TestTwoLayerLightOutside();
	if( !filter || std::strstr( filter, "fovsweep" ) )    TestNarrowFovSplat();
	if( !filter || std::strstr( filter, "split" ) )       TestSMSSplitSuppression();

	std::cout << "Passed: " << passCount << "   Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
