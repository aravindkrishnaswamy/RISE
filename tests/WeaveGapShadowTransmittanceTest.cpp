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

static double Render( const std::string& sceneText, const char* tag, std::vector<double>* pPixels = nullptr )
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

enum LightKind { kOmni, kSpot, kDirectional, kArea };

//! Camera framing.  kTight: fov 2 deg on a 0.2 x 0.2 patch -- the
//! footprint over which the omni's 1/d^2 and cosine are constant to
//! < 1e-4, so the no-sheet render can be checked against rho/pi * I/d^2
//! absolutely.  kWide: fov 10 deg on a 2 x 2 patch, for the
//! BIDIRECTIONAL rows (chosen before DL-294 was fixed, when BDPT's and
//! VCM's t = 1 splat read up to 6 % low whenever the frame was lit edge
//! to edge -- the `fovsweep` section gates that now).  Every kWide row
//! is a RATIO against the same framing's no-sheet render, so it needs no
//! absolute closed form.
enum CamKind { kTight, kWide };

//! @a compositeSheet: the sheet is a `composite_material` of two such
//! weaves (zero thickness, no extinction), whose only straight exit is
//! gap -> gap, so the closed form becomes g^2.
//! @a fovDeg > 0 overrides the framing's field of view (the DL-294
//! sweep; the patch size still follows @a cam).
static std::string ReceiverScene( LightKind light, bool withSheet, double gap, CamKind cam = kTight, bool compositeSheet = false, double fovDeg = 0.0 )
{
	const bool wide = ( cam == kWide );
	const double fov = fovDeg > 0.0 ? fovDeg : ( wide ? 10.0 : 2.0 );
	std::ostringstream ss;
	ss <<
		"film\n{\n\twidth 16\n\theight 16\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 1 1.2\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov " << fov << "\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_recv\n\tcolor " << kRho << " " << kRho << " " << kRho
			<< "\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		"lambertian_material\n{\n\tname mat_recv\n\treflectance pnt_recv\n}\n\n"
		"clippedplane_geometry\n{\n\tname geo_recv\n"
		<< ( wide
			? "\tpta -1 0 1\n\tptb 1 0 1\n\tptc 1 0 -1\n\tptd -1 0 -1\n"
			: "\tpta -0.1 0 0.1\n\tptb 0.1 0 0.1\n\tptc 0.1 0 -0.1\n\tptd -0.1 0 -0.1\n" ) <<
			"\tdoublesided TRUE\n}\n\n"
		"standard_object\n{\n\tname obj_recv\n\tgeometry geo_recv\n\tmaterial mat_recv\n}\n\n";

	if( withSheet ) {
		ss
			<< ( compositeSheet
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
// fovsweep (gated) and dl294 (measurement aid): DL-294.
//
// The t = 1 light-tracing splat must cover EXACTLY the film the eye
// subpaths sample.  Before DL-294 it did not: the camera's
// world-to-raster inverse (BDPTCameraUtilities::Rasterize) accepted
// raster x in [0, W) and y in [0, H) -- the camera's NOMINAL film --
// while every rasterizer samples pixel (x, row y) at screen
// (x + u - 0.5, H - y + v - 0.5), i.e. the film is x in [-0.5, W - 0.5),
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
		const double ptL0 = Render( Assemble( RastPT( 64 ), ReceiverScene( kSpot, false, 0.0, kWide, false, fov ) ), "fs_pt" );
		Check( ptL0 > 0, "fovsweep: PT reference renders non-black" );
		if( !( ptL0 > 0 ) ) continue;

		// Tolerances: BDPT and VCM-without-merging are the pure splat
		// on the gap render; their post-fix residuals are QMC-pattern
		// (seed-independent) and read <= 0.18 % across the sweep, so 1 %
		// is a >5x margin while the pre-fix -6.05 % (fov 2) fails by 5x.
		// Full VCM's gap render additionally carries its merge-radius
		// blur of the spot's penumbra (-1.05 +/- 0.07 % at fov 3, n = 4;
		// -0.02 % with merging off, so not the splat), hence 2 %.  Every L0
		// (no-sheet) row is NEE-dominated and reads <= 0.10 %; pre-fix
		// full VCM's L0 read -1.42 % at fov 2 (its balance-heuristic
		// splat share times the 6 % loss), so 0.5 %.
		struct R { const char* label; std::string rast; double tolGap; double tolL0; bool edge; };
		const R rows[] = {
			{ "BDPT RGB",              RastBDPT( 1024 ),       0.01, 0.005, true },
			{ "VCM RGB",               RastVCM( 1024 ),        0.02, 0.005, false },
			{ "VCM RGB merging OFF",   RastVCMNoMerge( 1024 ), 0.01, 0.005, true },
			{ "BDPT RGB gaussian filter", RastBDPTDefaultFilter( 1024 ), 0.01, 0.005, true },
		};
		for( const R& r : rows )
		{
			const double L0 = Render( Assemble( r.rast, ReceiverScene( kSpot, false, 0.0, kWide, false, fov ) ), "fs_l0" );
			std::vector<double> px;
			const double Lg = Render( Assemble( r.rast, ReceiverScene( kSpot, true, kSweepGap, kWide, false, fov ) ), "fs_lg", &px );
			char buf[256];
			std::snprintf( buf, sizeof(buf), "fovsweep fov %4.1f %s: L0/L0_PT = %.5f (%+.3f%%)",
				fov, r.label, L0 / ptL0, 100.0 * ( L0 / ptL0 - 1.0 ) );
			std::cout << "  " << buf << std::endl;
			Check( std::fabs( L0 / ptL0 - 1.0 ) <= r.tolL0, buf );
			std::snprintf( buf, sizeof(buf), "fovsweep fov %4.1f %s: L(gap %.1f)/(g*L0_PT) = %.5f (%+.3f%%)",
				fov, r.label, kSweepGap, Lg / ( kSweepGap * ptL0 ), 100.0 * ( Lg / ( kSweepGap * ptL0 ) - 1.0 ) );
			std::cout << "  " << buf << std::endl;
			Check( std::fabs( Lg / ( kSweepGap * ptL0 ) - 1.0 ) <= r.tolGap, buf );

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

//! dl294 (measurement aid, no assertions): the same sweep with n
//! repeats (argv[2], default 4), mean +/- sample sd per cell, plus the
//! NO-WEAVE control (the kTight 0.2 x 0.2 patch at fov 2 under the
//! spot, no sheet, against the analytic rho/pi * I/d^2) and the edge
//! fingerprint of each gap render.
static void MeasureNarrowFovSplat( unsigned int n )
{
	std::cout << "=== dl294: t = 1 splat fov sweep, n = " << n << " (mean +/- sd; printed, NOT gated) ===" << std::endl;
	for( int k = 0; k < kNumSweepFovs; k++ )
	{
		const double fov = kSweepFovs[k];
		std::vector<double> pt, bL0, bG, vL0, vG, bc0, br0, vc0, vr0, vnG;
		for( unsigned int i = 0; i < n; i++ ) {
			pt.push_back( Render( Assemble( RastPT( 64 ), ReceiverScene( kSpot, false, 0.0, kWide, false, fov ) ), "m_pt" ) );
			std::vector<double> px;
			bL0.push_back( Render( Assemble( RastBDPT( 1024 ), ReceiverScene( kSpot, false, 0.0, kWide, false, fov ) ), "m_bl0" ) );
			bG.push_back( Render( Assemble( RastBDPT( 1024 ), ReceiverScene( kSpot, true, kSweepGap, kWide, false, fov ) ), "m_bg", &px ) );
			EdgeRatios e = EdgeFingerprint( px, 16, 16 ); bc0.push_back( e.col0 ); br0.push_back( e.row0 );
			vL0.push_back( Render( Assemble( RastVCM( 1024 ), ReceiverScene( kSpot, false, 0.0, kWide, false, fov ) ), "m_vl0" ) );
			vG.push_back( Render( Assemble( RastVCM( 1024 ), ReceiverScene( kSpot, true, kSweepGap, kWide, false, fov ) ), "m_vg", &px ) );
			e = EdgeFingerprint( px, 16, 16 ); vc0.push_back( e.col0 ); vr0.push_back( e.row0 );
			vnG.push_back( Render( Assemble( RastVCMNoMerge( 1024 ), ReceiverScene( kSpot, true, kSweepGap, kWide, false, fov ) ), "m_vng" ) );
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

int main( int argc, char** argv )
{
	if( argc > 1 ) {
		const long v = std::strtol( argv[1], nullptr, 10 );
		if( v > 0 ) g_seedBase = (unsigned int)v;
	}
	std::cout << "WeaveGapShadowTransmittanceTest (DL-05)   seed base = " << g_seedBase << std::endl;

	const char* filter = std::getenv( "WEAVE_GAP_FILTER" );
	if( filter && std::strstr( filter, "dl294" ) ) {
		unsigned int n = 4;
		if( argc > 2 ) {
			const long v = std::strtol( argv[2], nullptr, 10 );
			if( v > 0 ) n = (unsigned int)v;
		}
		MeasureNarrowFovSplat( n );
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
	if( !filter || std::strstr( filter, "castsshadows" ) ) TestCastsShadowsFalseStepOver();
	if( !filter || std::strstr( filter, "layers" ) )      TestTwoLayerLightOutside();
	if( !filter || std::strstr( filter, "fovsweep" ) )    TestNarrowFovSplat();

	std::cout << "Passed: " << passCount << "   Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
