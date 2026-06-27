//////////////////////////////////////////////////////////////////////
//
//  FinalGatherSpectralTest.cpp - Regression for the spectral-black
//    FinalGatherShaderOp stub.
//
//    THE BUG: FinalGatherShaderOp::PerformOperationNM was a TODO stub
//    ("We do nothing for spectral rendering, yet") that returned 0.
//    StandardShader/AdvancedShader::ShadeNM call every shader op's
//    PerformOperationNM in the spectral path (and ShadeHWSS routes
//    through the default PerformOperationHWSS), so any material using
//    final-gather GI rendered through a *_spectral_* rasterizer
//    (pixelintegratingspectral / pathtracing_spectral / bdpt_spectral /
//    vcm_spectral / mlt_spectral) got ZERO final-gather contribution --
//    final-gather indirect illumination rendered BLACK -- even though the
//    RGB path (PerformOperation) works.  Same spectral-stub bug family as
//    DataDrivenBSDF::valueNM and the two SSS shader ops.
//
//    THE FIX: PerformOperationNM evaluates the full RGB final-gather
//    result (the same global-photon-map gather / irradiance-cache
//    interpolation the RGB path does), clamps it non-negative
//    (ColorMath::EnsurePositve -- defense-in-depth at the projection
//    boundary: PerformOperation already clamps, but c flows through signed
//    irradiance gradients and a negative RGB would flip RGBUnboundedSpectrum's
//    max-channel scale, a catastrophe unique to the spectral path), and
//    uplifts it to wavelength nm via RGBUnboundedSpectrum::FromRGB(c).Eval(nm)
//    -- the chroma-preserving JH uplift the RGB painters use for GetColorNM.
//
//    APPROACH (mirrors the SSS spectral test): a small final-gather-ONLY
//    Cornell box (the `global` shader is the finalgather op alone, so
//    every surface's visible shade is purely the gathered indirect
//    radiance) lit by a warm area light, with a global photon map +
//    irradiance cache.  Render through the spectral integrator AND the
//    pel integrator, capture the linear framebuffer, and assert:
//      A. GI PRESENT (the spectral-black fix): a large FRACTION of pixels
//         are lit.  Pre-fix only the directly-visible light source is lit
//         (~1% of the frame); post-fix the whole box is lit by gathered
//         GI (~99%).  This is the assertion with teeth -- it FAILS on the
//         pre-fix stub (which renders the box black).
//      B. WARM CHROMA preserved: the gathered GI carries the warm light's
//         hue (mean R > 1.1 * mean B).  A luminance / flat-spectrum
//         fallback would desaturate to gray (R == B) and FAIL B -- so B
//         pins the chroma-preserving uplift choice.
//      C. HUE matches the RGB render: spectral and RGB agree on the red
//         fraction R/(R+G+B).  Both root in the SAME RGB photon-map
//         gather (PerformOperation); the spectral path merely uplifts the
//         RGB result, so it must reconstruct the RGB render's hue.  (Like
//         the SSS test's C, this is a hue-CONSISTENCY cross-check, not a
//         spectrum-class discriminator.)
//
//    Final gather is far more deterministic run-to-run than SSS (the
//    irradiance cache is populated across many records in a dedicated
//    pass, not one lazy single-thread build), so the means are stable to
//    a few % MC noise -- the generous tolerances below absorb that.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
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
		std::cout << "  pass: " << testName << std::endl;
	} else {
		failCount++;
		std::cout << "  FAIL: " << testName << std::endl;
	}
}

//////////////////////////////////////////////////////////////////////
// CapturingRasterizerOutput — same shape as the SSS / EnvLightBalance tests.
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
	double litFraction;			// fraction of pixels with luminance above a small floor
	double maxlum;
	double medianRedFraction;	// MEDIAN per-pixel R/(R+G+B) over lit pixels -- firefly-robust hue
	bool   valid;
};

static double Lum( const RISEColor& c )
{
	return 0.2126 * c.base.r + 0.7152 * c.base.g + 0.0722 * c.base.b;
}

static ImageStats ComputeStats( const CapturingRasterizerOutput& cap )
{
	ImageStats s{};
	if( cap.pixels.empty() ) {
		return s;
	}

	double sum[3] = { 0, 0, 0 };
	double maxlum = 0;
	const double kLitFloor = 0.01;	// linear radiance; GI-lit box pixels are ~0.1-1, black is ~0
	// Per-pixel red fractions over LIT pixels, for a firefly-robust MEDIAN hue.
	// A whole-frame MEAN red fraction is dominated by the few very bright,
	// hue-unstable firefly / light-source pixels in the low-sample spectral
	// path (maxLum swings run-to-run), making it flap +/-0.15; the median over
	// the lit box pixels ignores that tail and is stable to ~0.01.
	std::vector<double> redFracs;
	redFracs.reserve( cap.pixels.size() );
	for( const RISEColor& c : cap.pixels ) {
		sum[0] += c.base.r;
		sum[1] += c.base.g;
		sum[2] += c.base.b;
		const double lum = Lum( c );
		if( lum > maxlum ) maxlum = lum;
		if( lum > kLitFloor ) {
			const double psum = double(c.base.r) + double(c.base.g) + double(c.base.b);
			if( psum > 1e-12 ) redFracs.push_back( double(c.base.r) / psum );
		}
	}
	const double n = double( cap.pixels.size() );
	s.mean[0]     = sum[0] / n;
	s.mean[1]     = sum[1] / n;
	s.mean[2]     = sum[2] / n;
	s.litFraction = double( redFracs.size() ) / n;
	s.maxlum      = maxlum;
	if( !redFracs.empty() ) {
		std::sort( redFracs.begin(), redFracs.end() );
		s.medianRedFraction = redFracs[ redFracs.size() / 2 ];
	} else {
		s.medianRedFraction = 0.0;
	}
	s.valid       = true;
	return s;
}

static double RedFraction( const ImageStats& s )
{
	const double sum = s.mean[0] + s.mean[1] + s.mean[2];
	return ( sum > 1e-12 ) ? ( s.mean[0] / sum ) : 0.0;
}

static std::string WriteSceneToTempFile( const char* sceneText, const char* tag )
{
	char path[512];
	std::snprintf( path, sizeof(path),
		"%s/fg_spectral_%s_%d.RISEscene",
		(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp"),
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

	const std::string scenePath = WriteSceneToTempFile( sceneText.c_str(), tag );
	if( scenePath.empty() ) {
		std::cout << "  (could not write temp scene for " << tag << ")" << std::endl;
		return result;
	}

	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) {
		std::remove( scenePath.c_str() );
		return result;
	}

	const bool bLoaded = pJob->LoadAsciiScene( scenePath.c_str() );
	// The parser has consumed the file; remove it now so it is cleaned up on
	// every exit path (success, render failure, or early return below).
	std::remove( scenePath.c_str() );
	if( !bLoaded ) {
		std::cout << "  (LoadAsciiScene failed for " << tag << ")" << std::endl;
		safe_release( pJob );
		return result;
	}

	pJob->RemoveRasterizerOutputs();

	CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "test capture output" );
	pJob->GetRasterizer()->AddRasterizerOutput( pCap );

	const bool bRendered = pJob->Rasterize();
	if( !bRendered ) {
		std::cout << "  (Rasterize failed for " << tag << ")" << std::endl;
		safe_release( pCap );
		safe_release( pJob );
		return result;
	}

	result = ComputeStats( *pCap );

	safe_release( pCap );
	safe_release( pJob );
	return result;
}

//////////////////////////////////////////////////////////////////////
// Scene: a small final-gather-ONLY Cornell box, warm area light, global
// photon map + irradiance cache.  Inline colours (no `> run` dependency,
// since the test runner does not set RISE_MEDIA_PATH).  `spectral` selects
// pixelintegratingspectral vs pixelpel; the photon-map gather is identical
// RGB in both, so the spectral render should reconstruct the RGB hue via
// the uplift.  OIDN off (fair A/B; a denoiser would smear the lit/hue
// signal the assertions read).
//////////////////////////////////////////////////////////////////////
static std::string BuildScene( bool spectral )
{
	std::string s =
		"RISE ASCII SCENE 6\n"
		"\n"
		"finalgather_shaderop\n"
		"{\n"
		"\tname fg\n"
		"\tcachegradients TRUE\n"
		"\tmin_effective_contributors 2\n"
		"\thigh_variation_reuse_scale 0.25\n"
		"\tsamples 36\n"
		"}\n"
		"\n"
		"standard_shader\n"
		"{\n"
		"\tname global\n"
		"\tshaderop fg\n"
		"}\n"
		"\n"
		"film\n"
		"{\n"
		"\twidth 48\n"
		"\theight 48\n"
		"}\n"
		"\n"
		"pinhole_camera\n"
		"{\n"
		"\tlocation 278 273 -800\n"
		"\tlookat 278 273 200\n"
		"\tup 0 1 0\n"
		"\tfov 37.0\n"
		"}\n"
		"\n"
		"uniformcolor_painter\n{\n\tname pnt_white\n\tcolor 0.7 0.7 0.7\n}\n"
		"uniformcolor_painter\n{\n\tname pnt_green\n\tcolor 0.1 0.45 0.1\n}\n"
		"uniformcolor_painter\n{\n\tname pnt_red\n\tcolor 0.5 0.06 0.06\n}\n"
		"uniformcolor_painter\n{\n\tname pnt_light\n\tcolor 1.0 0.85 0.6\n}\n"
		"\n"
		"lambertian_material\n{\n\tname white\n\treflectance pnt_white\n}\n"
		"lambertian_material\n{\n\tname green\n\treflectance pnt_green\n}\n"
		"lambertian_material\n{\n\tname red\n\treflectance pnt_red\n}\n"
		"lambertian_luminaire_material\n{\n\tname white_lum\n\texitance pnt_light\n\tscale 100.0\n\tmaterial none\n}\n"
		"\n"
		"clippedplane_geometry\n{\n\tname floorgeom\n\tpta 552.8 0 0\n\tptb 0 0 0\n\tptc 0 0 559.2\n\tptd 549.6 0 559.2\n}\n"
		"clippedplane_geometry\n{\n\tname ceilinggeom\n\tpta 556.0 548.8 0.0\n\tptb 556.0 548.8 559.2\n\tptc 0 548.8 559.2\n\tptd 0.0 548.8 0.0\n}\n"
		"clippedplane_geometry\n{\n\tname bwallgeom\n\tpta 549.6 0 559.2\n\tptb 0 0 559.2\n\tptc 0 548.8 559.2\n\tptd 556.0 548.8 559.2\n}\n"
		"clippedplane_geometry\n{\n\tname rwallgeom\n\tpta 0 0 559.2\n\tptb 0 0 0\n\tptc 0 548.8 0.0\n\tptd 0 548.8 559.2\n}\n"
		"clippedplane_geometry\n{\n\tname lwallgeom\n\tpta 552.8 0 0\n\tptb 549.6 0 559.2\n\tptc 556.0 548.8 559.2\n\tptd 556.0 548.8 0\n}\n"
		"clippedplane_geometry\n{\n\tname lightgeom\n\tpta 343.0 548.7999 227.0\n\tptb 343.0 548.7999 332.0\n\tptc 213.0 548.7999 332.0\n\tptd 213.0 548.7999 227.0\n}\n"
		"sphere_geometry\n{\n\tname ballgeom\n\tradius 90\n}\n"
		"\n"
		"standard_object\n{\n\tname floor\n\tgeometry floorgeom\n\tmaterial white\n}\n"
		"standard_object\n{\n\tname ceiling\n\tgeometry ceilinggeom\n\tmaterial white\n}\n"
		"standard_object\n{\n\tname bwall\n\tgeometry bwallgeom\n\tmaterial white\n}\n"
		"standard_object\n{\n\tname rwall\n\tgeometry rwallgeom\n\tmaterial green\n}\n"
		"standard_object\n{\n\tname lwall\n\tgeometry lwallgeom\n\tmaterial red\n}\n"
		"standard_object\n{\n\tname ball\n\tgeometry ballgeom\n\tmaterial white\n\tposition 278 90 279\n}\n"
		"standard_object\n{\n\tname light\n\tgeometry lightgeom\n\tmaterial white_lum\n}\n"
		"\n"
		"global_pel_photonmap\n{\n\tnum 12000\n\tmax_recursion 5\n\tmin_importance 0.01\n\tbranch FALSE\n}\n"
		"global_pel_gather\n{\n\tmax_photons 80\n\tradius 50\n\tellipse_ratio 0.05\n}\n"
		"irradiance_cache\n{\n\tsize 2000\n\ttolerance 0.1\n\tmin_spacing 4\n\tmax_spacing 120\n\tquery_threshold_scale 0.5\n\tneighbor_spacing_scale 2.0\n}\n"
		"\n";

	if( spectral ) {
		s +=
			"pixelintegratingspectral_rasterizer\n"
			"{\n"
			"\tmax_recursion 3\n"
			"\tsamples 3\n"
			"\tlum_samples 4\n"
			"\toidn_denoise FALSE\n"
			"}\n";
	} else {
		s +=
			"pixelpel_rasterizer\n"
			"{\n"
			"\tmax_recursion 3\n"
			"\tsamples 3\n"
			"\tlum_samples 4\n"
			"\toidn_denoise FALSE\n"
			"}\n";
	}
	return s;
}

static void PrintStats( const char* label, const ImageStats& s )
{
	if( !s.valid ) {
		std::cout << "  " << label << ": <invalid>" << std::endl;
		return;
	}
	std::cout << "  " << label
		<< "  mean (" << s.mean[0] << ", " << s.mean[1] << ", " << s.mean[2] << ")"
		<< "  litFrac " << s.litFraction
		<< "  maxLum " << s.maxlum
		<< "  medianRedFrac " << s.medianRedFraction
		<< "  (meanRedFrac " << RedFraction( s ) << ")" << std::endl;
}

int main()
{
	std::cout << "FinalGatherSpectralTest" << std::endl;
	std::cout << "=======================" << std::endl;

	const ImageStats spec = RenderAndComputeStats( BuildScene( true  ), "spectral" );
	const ImageStats rgb  = RenderAndComputeStats( BuildScene( false ), "rgb"      );

	PrintStats( "FG spectral", spec );
	PrintStats( "FG rgb     ", rgb );

	Check( spec.valid && rgb.valid, "both renders produced an image" );

	if( spec.valid )
	{
		// A. GI PRESENT — the spectral-black fix.  Gathered GI lights the
		//    whole box; pre-fix only the directly-visible light (~1%) is
		//    lit, so the box is black and litFraction is tiny.  A 0.4 floor
		//    cleanly separates the pre-fix stub (~0.01) from the fix (~0.99).
		Check( spec.litFraction > 0.4,
			"spectral final-gather GI lights the box (litFraction > 0.4) -- the PerformOperationNM fix" );

		// B. WARM CHROMA preserved — the gathered GI carries the warm
		//    light's hue.  Scale-invariant.  A luminance / flat fallback
		//    would gray it (R == B) and FAIL this.
		Check( spec.mean[0] > spec.mean[2] * 1.10,
			"spectral final-gather GI keeps the warm hue (mean R dominates B)" );
	}

	// C. HUE matches the RGB render.  Both root in the same RGB photon-map
	//    gather; the spectral path only uplifts the RGB result, so its hue
	//    must track the RGB render's.  Compared on the MEDIAN per-pixel red
	//    fraction over lit pixels, NOT the whole-frame mean: the mean is
	//    dominated by the few very bright, hue-unstable firefly / light-source
	//    pixels in the low-sample spectral path and flaps +/-0.15 run-to-run,
	//    whereas the median over the lit box pixels is stable to ~0.01.
	//    The tolerance (0.10) covers a systematic ~0.03-0.05 offset -- the
	//    spectral GI runs slightly redder than RGB, the expected
	//    RGBUnboundedSpectrum round-trip chroma shift (peak channel preserved,
	//    non-peak shifted) -- plus the residual run-to-run noise.  Scale-
	//    invariant.  (Like the SSS test, this is a hue-consistency cross-check,
	//    not a spectrum-class discriminator.)
	if( spec.valid && rgb.valid )
	{
		const double mrfSpec = spec.medianRedFraction;
		const double mrfRgb  = rgb.medianRedFraction;
		std::cout << "  median red fraction: spectral=" << mrfSpec << "  rgb=" << mrfRgb
			<< "  (|diff|=" << std::fabs( mrfSpec - mrfRgb ) << ")" << std::endl;
		Check( rgb.litFraction > 0.4, "rgb final-gather GI also lights the box (sanity)" );
		Check( std::fabs( mrfSpec - mrfRgb ) < 0.10,
			"spectral final-gather hue matches the RGB render (median red fraction within 0.10)" );
	}

	std::cout << "=======================" << std::endl;
	std::cout << "Passed: " << passCount << "  Failed: " << failCount << std::endl;
	return ( failCount == 0 ) ? 0 : 1;
}
