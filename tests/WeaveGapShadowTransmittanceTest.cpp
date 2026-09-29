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
//             SMS-off parity; a perfect refractor at ior 1 (an SMS
//             caster SMS cannot solve, DL-339) is printed.  Renders in
//             this section are Sobol'-salted per (seed base, index).
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
//    dl294    (opt-in only; WEAVE_GAP_FILTER=dl294)  Prints, does not
//             gate, the pre-existing narrow-fov light-tracing residual
//             DL-294: the `closed spot` rows re-run at fov 2 deg, where
//             BDPT reads ~6 % and VCM ~1 % off the closed form before
//             AND after DL-05 (the gap path reaches them only by t = 1
//             light tracing, so this fixture isolates the splat).
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
//  render calls std::srand( seedBase + renderIndex ) first -- the
//  tests/FabricRenderTest.cpp convention (RISE renders are seeded from
//  unsynchronized libc rand(), not the wall clock).
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
//! DL-295's section salts every render's Sobol' VALUE scramble with
//! (seed base, render index) -- `SobolSamplerTestHooks::ValueSalt` -- so
//! re-running the suite at another seed base is an INDEPENDENT
//! randomized-QMC replicate (unsalted, every seed base reuses the
//! identical Sobol' points and a seed sweep omits the QMC error).  Off
//! (salt 0, the sampler's own points) everywhere else.
static bool g_saltRenders = false;

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

//! FNV-1a over the captured pixels' float bytes (the scenehash section).
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

static double Render( const std::string& sceneText, const char* tag )
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
	SobolSamplerTestHooks::ValueSalt().store( g_saltRenders
		? SobolSequence::HashCombine( 0xD295u + g_seedBase, g_renderIndex ) : 0u );
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
				g_lastPixelHash = PixelHash( *pCap );
				g_lastPixels = pCap->pixels;
			}
			safe_release( pCap );
		}
		safe_release( pJob );
	}
	std::remove( path );
	return result;
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
//! BIDIRECTIONAL rows: BDPT's and VCM's light-tracing (t = 1) splat reads
//! up to 6 % low on this fixture at fov 1-3 deg and exact from 5 deg up,
//! identically before and after DL-05 and independent of the pixel
//! sampler -- a pre-existing narrow-fov splat residual recorded as DL-294
//! (WEAVE_GAP_FILTER=dl294 prints it), not a property of the gap.  Every
//! kWide row is a RATIO against the same framing's no-sheet render, so it
//! needs no absolute closed form.
//! kLookUp (DL-295): the camera BELOW the sheet looking straight UP at
//! the area luminaire through it (fov 4 deg: every pixel sees the 0.5 x
//! 0.5 emitter 3 units away), so the ONLY vertex on the path is the
//! sheet itself -- a gap draw at depth 0 with no SMS anchor anywhere.
enum CamKind { kTight, kWide, kLookUp };

//! @a compositeSheet: the sheet is a `composite_material` of two such
//! weaves (zero thickness, no extinction), whose only straight exit is
//! gap -> gap, so the closed form becomes g^2.
//! @a sheetMaterialChunks / @a recvMaterialChunks (DL-295's SMS
//! section, empty = the default): verbatim material chunks that define
//! `mat_sheet` / `mat_recv` in place of the built-in ones.
static std::string ReceiverScene( LightKind light, bool withSheet, double gap, CamKind cam = kTight, bool compositeSheet = false,
	const std::string& sheetMaterialChunks = std::string(), const std::string& recvMaterialChunks = std::string() )
{
	const bool wide = ( cam != kTight );
	const char* camChunk = ( cam == kLookUp )
		? "pinhole_camera\n{\n\tlocation 0 1 0\n\tlookat 0 4 0\n\tup 0 0 1\n\tfov 4.0\n}\n\n"
		: ( wide
			? "pinhole_camera\n{\n\tlocation 0 1 1.2\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 10.0\n}\n\n"
			: "pinhole_camera\n{\n\tlocation 0 1 1.2\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 2.0\n}\n\n" );
	std::ostringstream ss;
	ss <<
		"film\n{\n\twidth 16\n\theight 16\n}\n\n"
		<< camChunk <<
		"uniformcolor_painter\n{\n\tname pnt_recv\n\tcolor " << kRho << " " << kRho << " " << kRho
			<< "\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		<< ( recvMaterialChunks.empty()
			? std::string( "lambertian_material\n{\n\tname mat_recv\n\treflectance pnt_recv\n}\n\n" )
			: recvMaterialChunks ) <<
		"clippedplane_geometry\n{\n\tname geo_recv\n"
		<< ( wide
			? "\tpta -1 0 1\n\tptb 1 0 1\n\tptc 1 0 -1\n\tptd -1 0 -1\n"
			: "\tpta -0.1 0 0.1\n\tptb 0.1 0 0.1\n\tptc 0.1 0 -0.1\n\tptd -0.1 0 -0.1\n" ) <<
			"\tdoublesided TRUE\n}\n\n"
		"standard_object\n{\n\tname obj_recv\n\tgeometry geo_recv\n\tmaterial mat_recv\n}\n\n";

	if( withSheet ) {
		ss
			<< ( !sheetMaterialChunks.empty() ? sheetMaterialChunks : compositeSheet
				? std::string( "weave_material\n{\n\tname mat_layer\n\tfabric custom\n\ttransmission thin\n\tgap " ) + std::to_string( gap ) + "\n}\n\n"
				  "composite_material\n{\n\tname mat_sheet\n\ttop mat_layer\n\tbottom mat_layer\n}\n\n"
				: std::string( "weave_material\n{\n\tname mat_sheet\n\tfabric custom\n\ttransmission thin\n\tgap " ) + std::to_string( gap ) + "\n}\n\n" ) <<
			"clippedplane_geometry\n{\n\tname geo_sheet\n"
				"\tpta -4 " << kSheetY << " 4\n\tptb 4 " << kSheetY << " 4\n"
				"\tptc 4 " << kSheetY << " -4\n\tptd -4 " << kSheetY << " -4\n"
				"\tdoublesided TRUE\n}\n\n"
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
		const double L0 = Render( Assemble( row.rast, ReceiverScene( light, false, 0.0, row.cam ) ), "l0" );
		Check( L0 > 0, std::string( section ) + " " + row.label + ": no-sheet control renders non-black" );
		if( !( L0 > 0 ) ) continue;
		std::cout << "  " << section << " " << row.label << ": L0 (no sheet) = " << L0 << std::endl;

		for( int k = 0; k < nGaps; k++ )
		{
			const double g = gaps[k];
			const double L = Render( Assemble( row.rast, ReceiverScene( light, true, g, row.cam ) ), "lg" );
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
static void TestClosedFormComposite()
{
	std::cout << "=== composite: receiver under a composite of two gapped weaves (gap 0.3 each), spot light ===" << std::endl;
	struct R { const char* label; std::string rast; double tol; };
	const R rows[] = { { "PT RGB", RastPT( 64 ), 0.02 }, { "BDPT RGB", RastBDPT( 1024 ), 0.03 } };
	const double g = 0.3, expected = g * g;
	for( const R& r : rows )
	{
		const double L0 = Render( Assemble( r.rast, ReceiverScene( kSpot, false, 0.0, kWide ) ), "c_l0" );
		const double L  = Render( Assemble( r.rast, ReceiverScene( kSpot, true, g, kWide, true ) ), "c_lg" );
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

//! An anchored chain PT must leave to SMS: SMS on must read under 5 % of
//! SMS off (see the call site for why it reads ~0 rather than ~1).
static void SuppressedRow( const char* label, const std::string& rastOn, const std::string& rastOff,
	const std::string& scene )
{
	const double Lon  = Render( Assemble( rastOn, scene ), "sms_sup_on" );
	const double Loff = Render( Assemble( rastOff, scene ), "sms_sup_off" );
	char buf[320];
	std::snprintf( buf, sizeof(buf), "sms suppressed %s: SMS on %.6f / off %.6f = %.5f (must stay < 0.05)",
		label, Lon, Loff, Lon / Loff );
	std::cout << "  " << buf << std::endl;
	Check( Loff > 0 && Lon >= 0 && Lon / Loff < 0.05, buf );
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
static std::string CasterChunk( bool polished )
{
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
	   << CasterChunk( polished )
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
static std::string CasterCeilingScene( bool polished, bool sheet, bool slab )
{
	std::ostringstream ss;
	ss << "film\n{\n\twidth 16\n\theight 16\n}\n\n"
	      "pinhole_camera\n{\n\tlocation 0 1 1.2\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 10.0\n}\n\n"
	      "uniformcolor_painter\n{\n\tname pnt_recv\n\tcolor " << kRho << " " << kRho << " " << kRho << "\n\tcolorspace Rec709RGB_Linear\n}\n\n"
	      "lambertian_material\n{\n\tname mat_recv\n\treflectance pnt_recv\n}\n\n"
	      "clippedplane_geometry\n{\n\tname geo_recv\n\tpta -1 0 1\n\tptb 1 0 1\n\tptc 1 0 -1\n\tptd -1 0 -1\n\tdoublesided TRUE\n}\n\n"
	      "standard_object\n{\n\tname obj_recv\n\tgeometry geo_recv\n\tmaterial mat_recv\n}\n\n"
	   << CasterChunk( polished )
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

//! Opt-in probe (WEAVE_GAP_FILTER=dl295probe): print SMS on / off for the
//! round-1 review scenes, to size the gated rows' sample counts.
static void ProbeReviewScenes()
{
	g_saltRenders = true;
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
	};
	for( const P& r : rows ) {
		const unsigned int spp = 256;
		ParityRow( r.label, r.hwss ? RastPTSpectralSMS( spp, true, true ) : RastPTSMS( spp, true ),
			r.hwss ? RastPTSpectralSMS( spp, true, false ) : RastPTSMS( spp, false ), r.scene, -1.0 );
	}
	g_saltRenders = false;
	SobolSamplerTestHooks::ValueSalt().store( 0u );
}

static void TestSMSEmissionThroughGap()
{
	std::cout << "=== sms: PT with sms_enabled, emission reached through a weave gap (DL-295) ===" << std::endl;
	const double g = 0.3;
	g_saltRenders = true;

	// Closed forms, SMS on, black-yarn sheet (exact).  Pre-fix: every
	// row but the env box reads 0.
	//
	// BANDS.  Every band is >= 3 sd of the MEASURED run-to-run spread of
	// that row at these sample counts: n = 8 runs, seed bases 1000-8000
	// (Sobol'-salted per render), docs/DL05_WEAVE_GAP_SHADOW_TRANSMITTANCE.md
	// section 9.3.  The Sobol' salt does NOT make a render reproducible:
	// renders are multithreaded and each render thread's own RNG is seeded
	// from libc rand() in thread-start order (SobolSampling2D's per-pixel
	// film scramble, among others, draws from it), so the same seed AND
	// salt read differently run to run -- measured, and deterministic with
	// `force_number_of_threads 1`.  The spread below includes that.
	// Relative sd (band / sd): area RGB 0.88 % (3.4), spectral 1.14 %
	// (3.5), look-up 0.10 %, composite area 5.1 % (3.5) / look-up 1.9 %
	// (5.3; 3.4 against the external review's pooled 2.9 %), env 0.06 %;
	// HWSS parity 0.57-0.59 % (4.0 on look-up against the review's
	// 1.08 %); composite(dielectric) 2.1 % (3.3) / 0.8 % (3.7); fabric,
	// coated, direct-view 0.01-0.12 %; hand-off rows 0.57-1.38 % (>= 4.3),
	// the slab-and-anchor row 2.4 % (3.7).
	const std::string sheet = BlackWeaveSheet( g ), comp2 = CompositeTwoBlackWeavesSheet( g );
	RatioRow( "area PT RGB (g*L0)", RastPTSMS( 512, true ),
		ReceiverScene( kAreaLarge, false, 0.0, kWide ), ReceiverScene( kAreaLarge, true, g, kWide, false, sheet ), g, 0.03 );
	RatioRow( "area PT spectral hwss=false (g*L0)", RastPTSpectralSMS( 1024, false, true ),
		ReceiverScene( kAreaLarge, false, 0.0, kWide ), ReceiverScene( kAreaLarge, true, g, kWide, false, sheet ), g, 0.04 );
	RatioRow( "lookup PT RGB (camera -> gap -> luminaire, g*L0)", RastPTSMS( 64, true ),
		ReceiverScene( kAreaLarge, false, 0.0, kLookUp ), ReceiverScene( kAreaLarge, true, g, kLookUp, false, sheet ), g, 0.03 );
	RatioRow( "area composite-of-two-weaves PT RGB (g^2*L0)", RastPTSMS( 8192, true ),
		ReceiverScene( kAreaLarge, false, 0.0, kWide ), ReceiverScene( kAreaLarge, true, g, kWide, false, comp2 ), g * g, 0.18 );
	RatioRow( "lookup composite-of-two-weaves PT RGB (g^2*L0)", RastPTSMS( 8192, true ),
		ReceiverScene( kAreaLarge, false, 0.0, kLookUp ), ReceiverScene( kAreaLarge, true, g, kLookUp, false, comp2 ), g * g, 0.10 );
	RatioRow( "env box PT RGB (g*L0, env escape)", RastPTSMS( 256, true, true ),
		EnvBoxScene( false, 0.0 ), EnvBoxScene( true, g ), g, 0.03 );

	// HWSS: parity (DL-329 moves on and off identically).
	ParityRow( "area PT HWSS", RastPTSpectralSMS( 2048, true, true ), RastPTSpectralSMS( 2048, true, false ),
		ReceiverScene( kAreaLarge, true, g, kWide, false, sheet ), 0.05 );
	ParityRow( "lookup PT HWSS", RastPTSpectralSMS( 512, true, true ), RastPTSpectralSMS( 512, true, false ),
		ReceiverScene( kAreaLarge, true, g, kLookUp, false, sheet ), 0.04 );
	ParityRow( "env box PT HWSS", RastPTSpectralSMS( 512, true, true, true ), RastPTSpectralSMS( 512, true, false, true ),
		EnvBoxScene( true, g ), 0.03 );

	// Sibling: CompositeSPF's walker exits through a dielectric top leave
	// REFRACTED -- delta-tagged, not a pass-through, not an SMS caster.
	ParityRow( "area composite(dielectric over weave) PT RGB", RastPTSMS( 4096, true ), RastPTSMS( 4096, false ),
		ReceiverScene( kAreaLarge, true, g, kWide, false, CompositeDielectricOverWeaveSheet() ), 0.07 );
	ParityRow( "lookup composite(dielectric over weave) PT RGB", RastPTSMS( 2048, true ), RastPTSMS( 2048, false ),
		ReceiverScene( kAreaLarge, true, g, kLookUp, false, CompositeDielectricOverWeaveSheet() ), 0.03 );
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
		CasterCeilingScene( false, true, false ), 0.06 );
	ParityRow( "receiver -> gap -> slab -> smooth-SSS ceiling PT HWSS (no-BSDF hand-off)", RastPTSpectralSMS( 2048, true, true ), RastPTSpectralSMS( 2048, true, false ),
		CasterCeilingScene( false, true, true ), 0.09 );
	// MUST STAY SUPPRESSED: the same anchored chain with no gap is an SMS
	// chain by PT's accounting (anchor, then a caster), so PT must not
	// count it -- a regression that un-suppressed it would read PT+SMS/PT
	// ~1 (or ~2 had SMS estimated it).  It reads ~0: SMS treats these
	// casters as refractors (`canRefract`) and never estimates their
	// REFLECTION chain, so PT+SMS loses it entirely, before and after this
	// slice -- a pre-existing premise failure recorded in DL-339, pinned
	// here so a change to it is deliberate.
	SuppressedRow( "receiver -> smooth-SSS ceiling (anchored, no gap) PT RGB", RastPTSMS( 256, true ), RastPTSMS( 256, false ),
		CasterCeilingScene( false, false, false ) );
	SuppressedRow( "receiver -> polished ceiling (anchored, no gap) PT RGB", RastPTSMS( 256, true ), RastPTSMS( 256, false ),
		CasterCeilingScene( true, false, false ) );
	SuppressedRow( "receiver -> smooth-SSS ceiling (anchored, no gap) PT HWSS", RastPTSpectralSMS( 256, true, true ), RastPTSpectralSMS( 256, true, false ),
		CasterCeilingScene( false, false, false ) );

	// Control: an SMS CASTER, where the suppression's premise is SMS's to
	// honour and this fix changes nothing.  Printed, not gated: it reads
	// 0 with SMS on before AND after -- SMS does not solve a chain through
	// a single open refractive plane (DL-339).
	ParityRow( "area perfectrefractor ior 1 PT RGB (SMS caster -- control, printed)", RastPTSMS( 256, true ), RastPTSMS( 256, false ),
		ReceiverScene( kAreaLarge, true, g, kWide, false, PerfectRefractorSheet( "1.0" ) ), -1.0 );
	ParityRow( "area perfectrefractor ior 1.5 PT RGB (SMS caster -- control, printed)", RastPTSMS( 256, true ), RastPTSMS( 256, false ),
		ReceiverScene( kAreaLarge, true, g, kWide, false, PerfectRefractorSheet( "1.5" ) ), -1.0 );
	g_saltRenders = false;
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
		const double pt   = Render( Assemble( RastPT( spp ),   body ), "lay_pt" );
		const double bdpt = Render( Assemble( RastBDPT( spp ), body ), "lay_bdpt" );
		const double vcm  = Render( Assemble( RastVCM( spp ),  body ), "lay_vcm" );
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
// dl294: printed, not gated -- see the file header.
//////////////////////////////////////////////////////////////////////
static void PrintNarrowFovSplatResidual()
{
	std::cout << "=== dl294: bidirectional t=1 splat at fov 2 deg (printed, NOT gated) ===" << std::endl;
	struct R { const char* label; std::string rast; };
	const R rows[] = { { "BDPT RGB", RastBDPT( 1024 ) }, { "VCM RGB", RastVCM( 1024 ) }, { "PT RGB", RastPT( 64 ) } };
	const double gaps[] = { 0.3, 0.1 };
	for( const R& r : rows )
	{
		const double L0 = Render( Assemble( r.rast, ReceiverScene( kSpot, false, 0.0, kTight ) ), "d294_l0" );
		const double analytic = kRho / 3.14159265358979323846 * kOmniPow / ( kLightY * kLightY );
		std::printf( "  dl294 %s: L0 = %.6f (analytic %.6f, %+.3f%%)\n", r.label, L0, analytic, 100.0 * ( L0 / analytic - 1.0 ) );
		for( double g : gaps ) {
			const double L = Render( Assemble( r.rast, ReceiverScene( kSpot, true, g, kTight ) ), "d294_lg" );
			std::printf( "  dl294 %s gap %.2f: L/L0 = %.5f (%+.3f%%)   L/(g*analytic) %+.3f%%\n",
				r.label, g, L / L0, 100.0 * ( L / L0 / g - 1.0 ), 100.0 * ( L / ( g * analytic ) - 1.0 ) );
		}
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
}

//////////////////////////////////////////////////////////////////////
// scenehash (opt-in only; WEAVE_GAP_FILTER=scenehash): DL-295's
// "nothing without a non-caster delta vertex moves" proof.  Renders every
// scene file named in WEAVE_GAP_SCENES (whitespace-separated) with
// std::srand(4242) and the Sobol' salt 0, every `samples` line capped at
// WEAVE_GAP_SCENE_SPP (default 8), and prints the mean luminance and an
// FNV-1a hash of the captured pixels.  Deterministic ONLY with a single
// render thread (RISE_OPTIONS_FILE containing `force_number_of_threads
// 1`): the per-thread RNGs are seeded from libc rand() in thread start
// order.  Compare two separately built binaries' output line by line.
//////////////////////////////////////////////////////////////////////
static void HashScenes()
{
	const char* list = std::getenv( "WEAVE_GAP_SCENES" );
	const char* sppEnv = std::getenv( "WEAVE_GAP_SCENE_SPP" );
	const long cap = sppEnv ? std::strtol( sppEnv, nullptr, 10 ) : 8;
	if( !list ) {
		std::cout << "scenehash: set WEAVE_GAP_SCENES" << std::endl;
		return;
	}
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
	g_saltRenders = true;
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
	g_saltRenders = false;
	SobolSamplerTestHooks::ValueSalt().store( 0u );
}

int main( int argc, char** argv )
{
	if( argc > 1 ) {
		const long v = std::strtol( argv[1], nullptr, 10 );
		if( v > 0 ) g_seedBase = (unsigned int)v;
	}
	std::cout << "WeaveGapShadowTransmittanceTest (DL-05)   seed base = " << g_seedBase << std::endl;

	const char* filter = std::getenv( "WEAVE_GAP_FILTER" );
	if( filter && std::strstr( filter, "dl294" ) ) {
		PrintNarrowFovSplatResidual();
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
	if( filter && std::strstr( filter, "scenehash" ) ) {
		HashScenes();
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

	if( !filter || std::strstr( filter, "query" ) )       TestQueryMatchesSampler();
	if( !filter || std::strstr( filter, "closed" ) )      TestClosedFormOmni();
	if( !filter || std::strstr( filter, "composite" ) )   TestClosedFormComposite();
	if( !filter || std::strstr( filter, "directional" ) ) TestClosedFormDirectional();
	if( !filter || std::strstr( filter, "area" ) )        TestAreaPartitionGuard();
	if( !filter || std::strstr( filter, "sms" ) )         TestSMSEmissionThroughGap();
	if( !filter || std::strstr( filter, "castsshadows" ) ) TestCastsShadowsFalseStepOver();
	if( !filter || std::strstr( filter, "layers" ) )      TestTwoLayerLightOutside();

	std::cout << "Passed: " << passCount << "   Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
