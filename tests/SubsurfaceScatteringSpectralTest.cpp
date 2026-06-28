//////////////////////////////////////////////////////////////////////
//
//  SubsurfaceScatteringSpectralTest.cpp - Regression for the
//    spectral-black SSS shader-op stub.
//
//    THE BUG: SubSurfaceScatteringShaderOp::PerformOperationNM and
//    DonnerJensenSkinSSSShaderOp::PerformOperationNM were TODO stubs
//    that returned 0.  StandardShader/AdvancedShader::ShadeNM call
//    every shader op's PerformOperationNM in the spectral path (and
//    ShadeHWSS routes through it via the default PerformOperationHWSS),
//    so ANY subsurface-scattering material rendered through a
//    *_spectral_* rasterizer (pixelintegratingspectral / bdpt_spectral
//    / vcm_spectral / pathtracing_spectral / mlt_spectral) contributed
//    ZERO subsurface term -- the object rendered FULLY BLACK -- even
//    though its RGB path (PerformOperation) works.  Same bug family as
//    the DataDrivenBSDF::valueNM spectral-black stub.
//
//    THE FIX: PerformOperationNM evaluates the full RGB BSSRDF (reusing
//    the per-object irradiance octree) and uplifts the RGB exitant
//    radiance to wavelength nm via RGBUnboundedSpectrum::FromRGB(c).Eval(nm)
//    -- the same chroma-preserving JH uplift the RGB painters use for
//    GetColorNM.  The spectral render then reconstructs the RGB SSS
//    appearance instead of rendering black.
//
//    APPROACH (mirrors EnvLightBalanceTest's render harness): render a
//    tiny scene -- a single sphere whose ONLY shader is the SSS op, lit
//    by a RED omni light -- through the spectral integrator, capture the
//    linear radiance buffer, and assert:
//      A. LIT (the spectral-black fix): the spectral SSS render is not
//         black (PEAK luminance above a small floor).  Pre-fix this is
//         EXACTLY 0 (the SSS op is the sphere's only shader and its
//         PerformOperationNM returned 0).  This is the assertion with
//         teeth: it FAILS on the pre-fix stub.
//      B. CHROMA preserved: a RED-lit SSS sphere stays red in the
//         spectral render (mean R > 1.3 * mean G and > 1.3 * mean B).  A
//         luminance / flat-spectrum fallback would desaturate to gray
//         and FAIL B -- so B pins the chroma-preserving uplift choice.
//      C. HUE matches the RGB render: the spectral and RGB renders agree
//         on the red FRACTION R/(R+G+B).  The SSS transport is identical
//         RGB in both (PerformOperation builds the same irradiance octree);
//         the spectral path merely uplifts the RGB result, so it must
//         reconstruct the RGB render's hue.  C is a hue-CONSISTENCY cross-
//         check (it catches a gross hue error, or a desaturating fallback
//         that slips past B's per-channel ratios).  It is NOT a spectrum-
//         class discriminator: on this saturated-red, sub-1.0 scene
//         RGBAlbedo/RGBIlluminant share the same JH sigmoid LUT and move the
//         red fraction only ~0.02 (well inside the 0.10 tolerance), so an
//         accidental swap to one of those would still PASS A, B and C
//         (measured).  Catching that subtle class distinction would need an
//         HDR (>1.0) scene for Albedo's clamp plus a deterministic magnitude
//         check.  It is out of scope here (this test targets the spectral-
//         black bug, not the spectrum-class subtlety); the magnitude was
//         verified manually -- spectral/RGB mean-luminance ratio 0.97.
//
//    ROBUST BY DESIGN (the build non-determinism it guarded against is now
//    FIXED).  The SSS lazy build (PerformOperation) once captured each sample
//    point's irradiance using the triggering thread's scheduling-dependent
//    RNG and the triggering pixel's geometric frame, so the image landed on
//    one of a few discrete brightness levels run-to-run -- the MEAN cratered
//    to ~1/6 of nominal while the PEAK barely moved.  That was FIXED: the
//    build now uses a dedicated fixed-seed RNG and sets the SAMPLE POINT's
//    own surface frame (the reproducibility regression is SSSBuildDetermin-
//    ismTest).  These assertions were originally chosen to survive that
//    non-determinism -- B and C are scale-invariant (ratios / red fraction),
//    A floors on the then-build-stable PEAK luminance, not the mean -- and
//    are retained as-is: still valid, and now also magnitude-stable.
//    Single-threaded the renders were always bit-exact; the spectral/RGB
//    mean-luminance ratio is ~0.97 (the magnitude IS correct).
//
//    COVERAGE: this renders the simple_sss_shaderop path directly.  The
//    diffusion_approximation path is the SAME class (SubSurfaceScattering-
//    ShaderOp, differing only in the bound extinction function), so it runs
//    the identical PerformOperationNM body.  DonnerJensenSkinSSSShaderOp is a
//    SEPARATE class whose PerformOperationNM is byte-identical to this one's
//    (RISEPel c; PerformOperation(...); return RGBUnboundedSpectrum::FromRGB(
//    c).Eval(nm);) but is NOT rendered here -- a direct skin render needs
//    chromophore data and is heavier; the identical one-liner + the shared
//    uplift make the single-path coverage adequate for the spectral-stub fix.
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
// CapturingRasterizerOutput — same shape as EnvLightBalanceTest.
// Stores the linear radiance buffer for in-test pixel analysis.
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
	double maxlum;
	bool   valid;
};

static ImageStats ComputeStats( const CapturingRasterizerOutput& cap )
{
	ImageStats s{};
	if( cap.pixels.empty() ) {
		return s;
	}

	double sum[3] = { 0, 0, 0 };
	double maxlum = 0;
	for( const RISEColor& c : cap.pixels ) {
		sum[0] += c.base.r;
		sum[1] += c.base.g;
		sum[2] += c.base.b;
		const double lum = 0.2126 * c.base.r + 0.7152 * c.base.g + 0.0722 * c.base.b;
		if( lum > maxlum ) maxlum = lum;
	}
	const double n = double( cap.pixels.size() );
	s.mean[0] = sum[0] / n;
	s.mean[1] = sum[1] / n;
	s.mean[2] = sum[2] / n;
	s.maxlum  = maxlum;
	s.valid   = true;
	return s;
}

static double MeanLum( const ImageStats& s )
{
	return 0.2126 * s.mean[0] + 0.7152 * s.mean[1] + 0.0722 * s.mean[2];
}

// Red fraction of the mean colour, R/(R+G+B).  Scale-invariant: a global
// scale on the image leaves it unchanged.  Used to compare hue across the
// spectral and RGB renders without depending on their absolute magnitude
// (a scale-invariant hue check).
static double RedFraction( const ImageStats& s )
{
	const double sum = s.mean[0] + s.mean[1] + s.mean[2];
	return ( sum > 1e-12 ) ? ( s.mean[0] / sum ) : 0.0;
}

static std::string WriteSceneToTempFile( const char* sceneText, const char* tag )
{
	char path[512];
	std::snprintf( path, sizeof(path),
		"%s/sss_spectral_%s_%d.RISEscene",
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
// Scene chunks.
//
// A single sphere lit by a RED omni, whose rasterizer-default `global`
// shader is the SSS op ALONE -- no other shader op adds light -- so
// pre-fix the SSS spectral path renders the sphere PURE BLACK.  The
// material is Lambertian only so the irradiance-capture sub-shader
// (DefaultDirectLighting) has a BSDF to gather E with; the visible shade
// is the SSS op.
//
// OIDN is forced OFF: a denoiser on a 48x48 mostly-dark image smears the
// lit / hue signal the assertions read (with OIDN on, the two near-black
// images denoise to wildly different scales -- a measurement artifact,
// not an SSS bug), and it would make even the scale-invariant hue check
// noisy.
//
// `spectral` selects pixelintegratingspectral vs pixelpel; the SSS
// transport itself is identical (PerformOperation builds the same RGB
// irradiance octree), so the spectral render should reconstruct the RGB
// render's hue via the chroma-preserving uplift.
//////////////////////////////////////////////////////////////////////
static std::string BuildScene( bool spectral )
{
	std::string s =
		"RISE ASCII SCENE 6\n"
		"\n"
		"film\n"
		"{\n"
		"\twidth 48\n"
		"\theight 48\n"
		"}\n"
		"\n"
		"pinhole_camera\n"
		"{\n"
		"\tlocation 0 0 4\n"
		"\tlookat 0 0 0\n"
		"\tup 0 1 0\n"
		"\tfov 30.0\n"
		"}\n"
		"\n"
		"standard_shader\n"
		"{\n"
		"\tname sss_irrad\n"
		"\tshaderop DefaultDirectLighting\n"
		"}\n"
		"\n"
		"simple_sss_shaderop\n"
		"{\n"
		"\tname sss_op\n"
		"\tnumpoints 2000\n"
		"\tirrad_scale 6.0\n"
		"\tgeometric_scale 1.0\n"
		"\tshader sss_irrad\n"
		"}\n"
		"\n"
		"standard_shader\n"
		"{\n"
		"\tname global\n"
		"\tshaderop sss_op\n"
		"}\n"
		"\n"
		"uniformcolor_painter\n"
		"{\n"
		"\tname white\n"
		"\tcolor 1.0 1.0 1.0\n"
		"}\n"
		"\n"
		"lambertian_material\n"
		"{\n"
		"\tname sss_mat\n"
		"\treflectance white\n"
		"}\n"
		"\n"
		"sphere_geometry\n"
		"{\n"
		"\tname spheregeom\n"
		"\tradius 1.0\n"
		"}\n"
		"\n"
		"standard_object\n"
		"{\n"
		"\tname sss_sphere\n"
		"\tgeometry spheregeom\n"
		"\tmaterial sss_mat\n"
		"}\n"
		"\n"
		"omni_light\n"
		"{\n"
		"\tname light\n"
		"\tpower 1200.0\n"
		"\tposition 0 4 4\n"
		"\tcolor 1.0 0.3 0.3\n"
		"}\n"
		"\n";

	if( spectral ) {
		s +=
			"pixelintegratingspectral_rasterizer\n"
			"{\n"
			"\tmax_recursion 4\n"
			"\tsamples 24\n"
			"\tlum_samples 1\n"
			"\toidn_denoise FALSE\n"
			"}\n";
	} else {
		s +=
			"pixelpel_rasterizer\n"
			"{\n"
			"\tmax_recursion 4\n"
			"\tsamples 16\n"
			"\tlum_samples 1\n"
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
		<< "  meanLum " << MeanLum( s )
		<< "  maxLum " << s.maxlum << std::endl;
}

int main()
{
	std::cout << "SubsurfaceScatteringSpectralTest" << std::endl;
	std::cout << "================================" << std::endl;

	const ImageStats sssSpec = RenderAndComputeStats( BuildScene( true  ), "sss_spectral" );
	const ImageStats sssRgb  = RenderAndComputeStats( BuildScene( false ), "sss_rgb"      );

	PrintStats( "SSS spectral", sssSpec );
	PrintStats( "SSS rgb     ", sssRgb );

	Check( sssSpec.valid && sssRgb.valid, "both renders produced an image" );

	if( sssSpec.valid )
	{
		// A. LIT — the spectral-black fix.  Floored on the PEAK luminance
		//    (maxLum): EXACTLY 0 on the pre-fix stub (pure black) and ~0.07 when
		//    lit, so the 0.01 floor keeps teeth with a wide margin.  (maxLum was
		//    also the build-STABLE statistic back when the build was non-
		//    deterministic -- the mean cratered, the peak barely moved; the build
		//    is deterministic now, but maxLum remains a fine floor.)
		Check( sssSpec.maxlum > 0.01,
			"spectral SSS render is LIT (not black) -- the PerformOperationNM fix" );

		// B. CHROMA preserved — a RED-lit SSS sphere stays red.  Scale-
		//    invariant.  A luminance / flat-spectrum fallback would gray it
		//    (R == G == B) and FAIL this.
		Check( sssSpec.mean[0] > sssSpec.mean[1] * 1.30 && sssSpec.mean[0] > sssSpec.mean[2] * 1.30,
			"spectral SSS preserves RED chroma (mean R dominates G and B)" );
	}

	// C. HUE matches the RGB render.  Both renders root in the same RGB
	//    irradiance octree (PerformOperation); the spectral path only
	//    uplifts the RGB result, so its red fraction R/(R+G+B) must track
	//    the RGB render's.  Scale-invariant (robust by design; the build
	//    non-determinism that motivated this is now fixed).  A hue-CONSISTENCY
	//    cross-check (catches gross hue
	//    errors / a desaturating fallback), NOT a spectrum-class
	//    discriminator -- Albedo/Illuminant pass C on this scene; see header.
	if( sssSpec.valid && sssRgb.valid )
	{
		const double rfSpec = RedFraction( sssSpec );
		const double rfRgb  = RedFraction( sssRgb );
		std::cout << "  red fraction: spectral=" << rfSpec << "  rgb=" << rfRgb
			<< "  (|diff|=" << std::fabs( rfSpec - rfRgb ) << ")" << std::endl;
		Check( rfSpec > 0.80 && rfRgb > 0.80,
			"both renders are red-dominant (R fraction > 0.80)" );
		Check( std::fabs( rfSpec - rfRgb ) < 0.10,
			"spectral SSS hue matches the RGB render (red fraction within 0.10)" );
	}

	std::cout << "================================" << std::endl;
	std::cout << "Passed: " << passCount << "  Failed: " << failCount << std::endl;
	return ( failCount == 0 ) ? 0 : 1;
}
