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

static std::string RastPT( unsigned int spp )
{
	std::ostringstream ss;
	ss << "pathtracing_pel_rasterizer\n{\n\tsamples " << spp * SppScale()
	   << "\n\trr_min_depth 8\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n" << kOutputChunk;
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

static std::string ReceiverScene( LightKind light, bool withSheet, double gap, CamKind cam = kTight, bool compositeSheet = false )
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


int main()
{
    const std::string rasters[] = { RastPT(1024), RastBDPT(1024), RastVCM(1024),
        "mlt_rasterizer\n{\n mutations_per_pixel 1024\n bootstrap_samples 20000\n chains 8\n pixel_filter box\n oidn_denoise FALSE\n}\n" + std::string(kOutputChunk) };
    const char* labels[] = { "PT", "BDPT", "VCM", "MLT" };
    for (unsigned r=0; r<4; ++r) {
        double base=Render(Assemble(rasters[r], ReceiverScene(kOmni,false,0,kWide)), labels[r]);
        Check(base>0, std::string(labels[r])+" lit-floor control renders");
        std::cout << labels[r] << " clear-floor " << base << std::endl;
        const double glow = 0.01/3.14159265358979323846;
        Check(std::abs(base-glow-kRho/3.14159265358979323846)<0.01, std::string(labels[r])+" control matches rho/pi");
        for(double transmission : {0.5,0.7}) {
            double value=Render(Assemble(rasters[r], ReceiverScene(kOmni,true,transmission,kWide)), labels[r]);
            std::cout << labels[r] << " transmission " << transmission << " measured " << (value-glow)/(base-glow) << std::endl;
            Check(value>=0 && base>0 && std::abs((value-glow)/(base-glow)-transmission)<0.035,
                std::string(labels[r])+" alpha irradiance matches closed form");
        }
    }


    for (unsigned r=0;r<3;++r) {
        std::string scene=ReceiverScene(kArea,false,0,kWide);
        const double base=Render(Assemble(rasters[r],scene),"area emitter control");
        const auto at=scene.find("name mat_emit\n");
        scene.insert(at+std::string("name mat_emit\n").size()," alpha_mode blend\n alpha_coverage 0.3\n");
        const double alpha=Render(Assemble(rasters[r],scene),"alpha area emitter");
        const double glow=.01/3.14159265358979323846;
        const double ratio=(alpha-glow)/(base-glow);
        std::cout<<labels[r]<<" emitter coverage ratio="<<ratio<<std::endl;
        Check(base>glow+.01,std::string(labels[r])+" area emitter control lights receiver");
        Check(std::fabs(ratio-.3)<.035,std::string(labels[r])+" emitter endpoint coverage counted once across MIS strategies");
    }
    for (const char* kind : {"pathtracing_spectral_rasterizer","bdpt_spectral_rasterizer","vcm_spectral_rasterizer"}) {
        for(bool hwss : {false,true}) {
            const std::string raster=std::string(kind)+"\n{\n samples 1024\n hwss "+(hwss?"true":"false")+"\n pixel_filter box\n oidn_denoise FALSE\n}\n"+kOutputChunk;
            const double base=Render(Assemble(raster,ReceiverScene(kOmni,false,0,kWide)),kind);
            const double alpha=Render(Assemble(raster,ReceiverScene(kOmni,true,.7,kWide)),kind);
            const double ratio=alpha/base;
            // Glow is only 2% of the unobstructed floor and travels the same
            // spectral conversion; the expected ratio including it is known.
            const double expected=(.7*kRho+.01)/(kRho+.01);
            std::cout<<kind<<" hwss="<<hwss<<" ratio="<<ratio<<std::endl;
            Check(base>0,std::string(kind)+" finite spectral control");
            Check(std::fabs(ratio-expected)<.035,std::string(kind)+" spectral alpha has wavelength-independent coverage");
        }
    }

    // A delta mirror cannot receive NEE. The emitter is reached only by a
    // BSDF continuation, crossing the sheet after the first camera hit.
    // Absorbing global medium prices the original full segment after skips.
    for(unsigned r=0;r<3;++r)for(bool medium:{false,true}) {
        auto body=[&](bool sheet){
            std::string scene=ReceiverScene(kArea,sheet,.7,kWide);
            auto rep=[&](const std::string& a,const std::string& b){const auto at=scene.find(a);if(at!=std::string::npos)scene.replace(at,a.size(),b);};
            rep("lambertian_material\n{\n\tname mat_recv_base","perfectreflector_material\n{\n\tname mat_recv_base");
            rep("color 0.01 0.01 0.01","color 0 0 0");
            const auto emitter=scene.find("name geo_emit");
            if(emitter!=std::string::npos){
                std::string tail=scene.substr(emitter);size_t pos=0;
                while((pos=tail.find("0.25",pos))!=std::string::npos){tail.replace(pos,4,"8");++pos;}
                scene.replace(emitter,scene.size()-emitter,tail);
            }
            if(medium)scene+="homogeneous_medium\n{\n name absorber\n absorption 0.15 0.15 0.15\n scattering 0 0 0\n phase hg 0\n}\nglobal_medium\n{\n medium absorber\n}\n";
            return scene;
        };
        const double base=Render(Assemble(rasters[r],body(false)),"mirror continuation control");
        const double alpha=Render(Assemble(rasters[r],body(true)),"mirror alpha continuation");
        std::cout<<labels[r]<<" continuation medium="<<medium<<" ratio="<<alpha/base<<std::endl;
        Check(base>.01,std::string(labels[r])+" specular continuation reaches emitter");
        Check(std::fabs(alpha/base-.7)<.035,std::string(labels[r])+" skipped-alpha continuation retains full medium segment");
    }
    // VM-only has no NEE/VC source and zero glow. A positive control proves
    // the photon merge strategy actually contributes. Coverage at both eye
    // and photon endpoints must leave one (not two) factors of 0.5.
    for (unsigned mode=0;mode<3;++mode) {
        std::string rast=RastVCM(1024);
        auto replace=[](std::string& text,const std::string& a,const std::string& b) {
            const auto at=text.find(a); if(at!=std::string::npos)text.replace(at,a.size(),b);
        };
        replace(rast,"merge_radius 0.0","merge_radius 0.20");
        if(mode==0)replace(rast,"vc_enabled true","vc_enabled false");
        if(mode==1)replace(rast,"vm_enabled true","vm_enabled false");
        std::string scene=ReceiverScene(kSpot,false,0,kWide);
        replace(scene,"color 0.01 0.01 0.01","color 0 0 0");
        replace(scene,"inner 1.5","inner 20"); replace(scene,"outer 2.0","outer 25");
        const double opaque=Render(Assemble(rast,scene),"VCM coverage control");
        replace(scene,"name mat_recv\n","name mat_recv\n alpha_mode blend\n alpha_coverage 0.5\n");
        const double alpha=Render(Assemble(rast,scene),"VCM coverage receiver");
        const char* modeName=mode==0?"VM-only":mode==1?"VC-only":"VC+VM";
        std::cout<<modeName<<" opaque="<<opaque<<" alpha="<<alpha<<" ratio="<<alpha/opaque<<std::endl;
        Check(opaque>.05,std::string(modeName)+" nonzero lit control (VM is active)");
        Check(std::fabs(alpha/opaque-.5)<.04,std::string(modeName)+" receiver coverage has one factor");
    }
    std::cout << passCount << " passed / " << failCount << " failed" << std::endl;
    return failCount ? 1 : 0;
}
