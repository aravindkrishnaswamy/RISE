#ifndef ALPHA_TRANSPORT_FIXTURE_H
#define ALPHA_TRANSPORT_FIXTURE_H
// DL-214: alpha coverage must attenuate direct irradiance under every integrator.
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
inline unsigned int SppScale()
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
inline double MeanLuminance( const CapturingRasterizerOutput& cap )
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

inline double Render( const std::string& sceneText, const char* tag )
{
	char path[512];
	std::snprintf( path, sizeof(path), "/tmp/alpha_intersection_%s_%d.RISEscene",
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

inline std::string RastPT( unsigned int spp )
{
	std::ostringstream ss;
	ss << "pathtracing_pel_rasterizer\n{\n\tsamples " << spp * SppScale()
	   << "\n\trr_min_depth 8\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n" << kOutputChunk;
	return ss.str();
}

inline std::string RastBDPT( unsigned int spp )
{
	std::ostringstream ss;
	ss << "bdpt_pel_rasterizer\n{\n\tsamples " << spp * SppScale()
	   << "\n\tmax_eye_depth 8\n\tmax_light_depth 8\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n" << kOutputChunk;
	return ss.str();
}

inline std::string RastVCM( unsigned int spp )
{
	std::ostringstream ss;
	ss << "vcm_pel_rasterizer\n{\n\tsamples " << spp * SppScale()
	   << "\n\tmax_eye_depth 8\n\tmax_light_depth 8\n\tmerge_radius 0.0\n\tvc_enabled true\n\tvm_enabled true\n"
	   << "\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n" << kOutputChunk;
	return ss.str();
}

inline std::string Assemble( const std::string& rasterizer, const std::string& body )
{
	// The standard_shader is ignored by the modern rasterizers (they drive
	// their integrators directly) and is only here so every scene string
	// has the same shape.
	return std::string( "RISE ASCII SCENE 7\n" )
		+ "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		+ rasterizer + body;
}

//////////////////////////////////////////////////////////////////////
// Closed-form lit receiver: an alpha sheet removes a known fraction of light.
// A small receiver emission supplies a finite MLT bootstrap control; subtract
// that independently known glow before measuring shadow transmission.
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

inline std::string ReceiverScene( LightKind light, bool withSheet, double gap, CamKind cam = kTight, bool compositeSheet = false )
{
	const bool wide = ( cam == kWide );
	std::ostringstream ss;
	ss <<
		"film\n{\n\twidth 16\n\theight 16\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 1 1.2\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov " << ( wide ? "10.0" : "2.0" ) << "\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_recv\n\tcolor " << kRho << " " << kRho << " " << kRho
			<< "\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		"lambertian_material\n{\n\tname mat_recv_base\n\treflectance pnt_recv\n}\n\n"
		"uniformcolor_painter\n{\n name floor_glow\n color 0.01 0.01 0.01\n colorspace Rec709RGB_Linear\n}\n"
		"lambertian_luminaire_material\n{\n name mat_recv\n material mat_recv_base\n exitance floor_glow\n scale 1\n}\n"
		"clippedplane_geometry\n{\n\tname geo_recv\n"
		<< ( wide
			? "\tpta -1 0 1\n\tptb 1 0 1\n\tptc 1 0 -1\n\tptd -1 0 -1\n"
			: "\tpta -0.1 0 0.1\n\tptb 0.1 0 0.1\n\tptc 0.1 0 -0.1\n\tptd -0.1 0 -0.1\n" ) <<
			"\tdoublesided TRUE\n}\n\n"
		"standard_object\n{\n\tname obj_recv\n\tgeometry geo_recv\n\tmaterial mat_recv\n}\n\n";

	if( withSheet ) {
		(void)compositeSheet;
        ss << "uniformcolor_painter\n{\n name black\n color 0 0 0\n colorspace Rec709RGB_Linear\n}\n"
           << "uniformcolor_painter\n{\n name white\n color 1 1 1\n colorspace Rec709RGB_Linear\n}\n"
           << "lambertian_material\n{\n name mat_sheet\n reflectance black\n}\n"
           << "checker_painter\n{\n name mask\n colora black\n colorb white\n size 0.001\n}\n"
           << "uniformcolor_painter\n{\n name transparency\n color 0.7 0.7 0.7\n colorspace Rec709RGB_Linear\n}\n";
        ss << "scalar_painter\n{\n name coverage\n painter mask\n channel R\n}\n";
        // The material is defined after its scalar dependency below.
        ss << "lambertian_material\n{\n name mat_alpha_sheet\n reflectance black\n alpha_mode "
           << (gap == 0.5 ? "mask" : "blend") << "\n alpha_coverage "
           << (gap == 0.5 ? "coverage" : "0.3") << "\n}\n"

			"clippedplane_geometry\n{\n\tname geo_sheet\n"
				"\tpta -4 " << kSheetY << " 4\n\tptb 4 " << kSheetY << " 4\n"
				"\tptc 4 " << kSheetY << " -4\n\tptd -4 " << kSheetY << " -4\n"
				"\tdoublesided TRUE\n}\n\n"
			"standard_object\n{\n\tname obj_sheet\n\tgeometry geo_sheet\n\tmaterial mat_alpha_sheet\n}\n\n";
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


#endif
