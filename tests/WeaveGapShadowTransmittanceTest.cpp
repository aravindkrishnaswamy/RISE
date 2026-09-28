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
//! BIDIRECTIONAL rows: BDPT's and VCM's light-tracing (t = 1) splat reads
//! up to 6 % low on this fixture at fov 1-3 deg and exact from 5 deg up,
//! identically before and after DL-05 and independent of the pixel
//! sampler -- a pre-existing narrow-fov splat residual recorded as DL-294
//! (WEAVE_GAP_FILTER=dl294 prints it), not a property of the gap.  Every
//! kWide row is a RATIO against the same framing's no-sheet render, so it
//! needs no absolute closed form.
enum CamKind { kTight, kWide };

//! @a compositeSheet: the sheet is a `composite_material` of two such
//! weaves (zero thickness, no extinction), whose only straight exit is
//! gap -> gap, so the closed form becomes g^2.
static std::string ReceiverScene( LightKind light, bool withSheet, double gap, CamKind cam = kTight, bool compositeSheet = false )
{
	const bool wide = ( cam == kWide );
	std::ostringstream ss;
	ss <<
		"film\n{\n\twidth 16\n\theight 16\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 1 1.2\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov " << ( wide ? "10.0" : "2.0" ) << "\n}\n\n"
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
	double tol;			// relative tolerance on (L_sheet / L0) / g - 1
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
			std::cout << "  " << buf << std::endl;
			Check( std::fabs( ratio / g - 1.0 ) <= row.tol, buf );
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
	// 5 %: n = 5 seed bases put PT at -0.16 .. +1.04 % and BDPT at
	// -2.77 .. -0.71 % (identical spread before and after DL-05 -- this
	// light kind's shadow path is untouched); the failure this row
	// exists for, an area-light NEE arm that sees through the gap while
	// PT's BSDF-sampled continuation still reaches the emitter through it
	// at MIS weight 1, reads +103 % (measured by forcing that arm).
	rows.push_back( { "PT RGB", RastPT( 1024 ), 0.05, kWide } );
	rows.push_back( { "BDPT RGB", RastBDPT( 512 ), 0.05, kWide } );
	RunReceiverRows( "area", kArea, rows, gaps, 1 );
}

//////////////////////////////////////////////////////////////////////
// The design-doc two-layer topology (docs/CLOTH_FABRIC_DESIGN.md
// section 15 debt 25 / item 27's measurement family): a
// `weave_material { fabric custom transmission thin gap g
// warp_transmit 0.25 weft_transmit 0.25 }` closed box 2.8 x 2.8 x 1.0,
// or the same surfaces as free-standing double-sided planes; omni light
// at (0,0,lightZ) power 6; camera (0,0,3.2) fov 34.
//////////////////////////////////////////////////////////////////////
enum LayerGeom { kBox, kSixPlanes, kTwoPlanes, kOnePlane };

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
	};
	for( const T& r : rows )
	{
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
		PrintNarrowFovSplatResidual();
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
	if( !filter || std::strstr( filter, "layers" ) )      TestTwoLayerLightOutside();

	std::cout << "Passed: " << passCount << "   Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
