//////////////////////////////////////////////////////////////////////
//
//  VolumeAbsorptionAttenuationTest.cpp - End-to-end correctness check
//    that a HOMOGENEOUS absorbing medium attenuates background radiance
//    by EXACTLY Beer-Lambert (exp(-sigma_a * d)) along a no-scatter
//    eye path — NOT exp(-2*sigma_a*d).
//
//    THE BUG THIS GUARDS AGAINST (analog volume estimator double-count):
//      HomogeneousMedium::SampleDistance is an ANALOG estimator.
//      Reaching the surface / escaping through the medium without a
//      scatter event is a stochastic SURVIVAL event whose probability is
//      exp(-sigma_t_max * d) — that probability already carries the
//      Beer-Lambert factor.  The no-scatter branches of the PT integrator
//      used to multiply throughput by Tr = exp(-sigma_t * d) AGAIN with
//      no compensating division, so a pure absorber rendered
//      exp(-2*sigma_a*d) (the slab read ~2x too thick / too dark).
//
//      The correct no-scatter weight is the per-channel chromatic ratio
//      Tr / PTTrReduced(Tr): the survival pdf (the max-sigma_t channel of
//      Tr) cancels, leaving 1 on that channel and >= 1 on the others so a
//      COLOURED medium keeps its tint.  For monochrome / NM this reduces
//      to exactly 1.  Note that simply DELETING the `* Tr` would be wrong:
//      it desaturates a coloured medium.  This test's COLOURED case (B)
//      is the one that distinguishes the correct Tr/PTTrReduced fix from
//      the naive "delete the Tr".
//
//    SETUP: a uniform-radiance background (env IBL, L = 1.0) behind a
//    HOMOGENEOUS absorbing slab (sigma_s = 0) of known thickness d and
//    known sigma_a, between the camera and the background.  The eye ray
//    crosses the slab and escapes to the env; the central pixel's
//    radiance must equal background * exp(-sigma_a * d) per channel.
//
//    The slab is a box bounded by a perfect refractor at IOR = 1.0 — no
//    bending and zero Fresnel reflectance, so the ray passes straight
//    through and only the interior medium attenuates it.
//
//    Three absorber cases:
//      A. RGB GRAY absorber   — equal sigma_a per channel.
//      B. RGB COLOURED absorber — distinct sigma_a per channel; asserts
//         per-channel exp(-sigma_a[c]*d) (the desaturation-vs-double-count
//         discriminator).
//      C. NM / spectral rasterizer (non-HWSS) — the spectral path; for
//         monochrome/NM Tr/PTTrReduced reduces to 1.
//
//    The non-HWSS path is exercised throughout (spectral case sets
//    `hwss false`).
//
//    TOLERANCE: chosen tight enough to catch a 2x optical-depth error
//    (exp(-2*tau) vs exp(-tau) differ by a factor exp(-tau) — at tau ~ 1
//    that's ~2.7x, far outside the band) but loose enough for MC noise at
//    the sample count used.
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

// Mean radiance over the central 4x4 block — the slab fully covers the
// frame, so any interior block is the on-slab background path.  Averaging
// a small block instead of a single pixel suppresses MC noise.
struct PixelRGB { double r, g, b; bool valid; };

static PixelRGB CentralBlockMean( const CapturingRasterizerOutput& cap )
{
	PixelRGB out{ 0, 0, 0, false };
	if( cap.pixels.empty() || cap.width < 4 || cap.height < 4 ) {
		return out;
	}
	const unsigned int cx = cap.width / 2;
	const unsigned int cy = cap.height / 2;
	double sr = 0, sg = 0, sb = 0;
	int n = 0;
	for( int dy = -2; dy < 2; dy++ ) {
		for( int dx = -2; dx < 2; dx++ ) {
			const unsigned int x = cx + dx;
			const unsigned int y = cy + dy;
			if( x >= cap.width || y >= cap.height ) continue;
			const RISEColor& c = cap.pixels[y * cap.width + x];
			sr += c.base.r; sg += c.base.g; sb += c.base.b;
			n++;
		}
	}
	if( n == 0 ) return out;
	out.r = sr / n; out.g = sg / n; out.b = sb / n;
	out.valid = true;
	return out;
}

static std::string WriteSceneToTempFile( const std::string& sceneText, const char* tag )
{
	char path[512];
	std::snprintf( path, sizeof(path),
		"/tmp/volume_absorption_%s_%d.RISEscene",
		tag, static_cast<int>(::getpid()) );

	std::ofstream ofs( path );
	if( !ofs.is_open() ) {
		return std::string();
	}
	ofs << sceneText;
	ofs.close();
	return std::string( path );
}

static PixelRGB RenderCentralBlock( const std::string& sceneText, const char* tag )
{
	PixelRGB result{ 0, 0, 0, false };

	const std::string scenePath = WriteSceneToTempFile( sceneText, tag );
	if( scenePath.empty() ) {
		return result;
	}

	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) {
		return result;
	}

	if( !pJob->LoadAsciiScene( scenePath.c_str() ) ) {
		safe_release( pJob );
		return result;
	}

	pJob->RemoveRasterizerOutputs();

	CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "test capture output" );
	pJob->GetRasterizer()->AddRasterizerOutput( pCap );

	const bool bRendered = pJob->Rasterize();
	if( !bRendered ) {
		safe_release( pCap );
		safe_release( pJob );
		return result;
	}

	result = CentralBlockMean( *pCap );

	safe_release( pCap );
	safe_release( pJob );
	std::remove( scenePath.c_str() );
	return result;
}

//////////////////////////////////////////////////////////////////////
// Scene construction.
//
// The slab box has its faces perpendicular to the camera's +Z axis;
// the camera looks straight down +Z at normal incidence, so the optical
// path through the slab equals its `depth` (= thickness d).  The box is
// 4x4 in X/Y — far wider than the 10-degree FOV view ray bundle at the
// slab — so the whole frame is "on slab".
//////////////////////////////////////////////////////////////////////

static const double kSlabDepth = 2.0;   // thickness d

static std::string BuildRGBScene(
	double sa_r, double sa_g, double sa_b )
{
	std::ostringstream ss;
	ss <<
		"RISE ASCII SCENE 6\n"
		"\n"
		"uniformcolor_painter\n"
		"{\n"
		"\tname pnt_env\n"
		"\tcolor 1.0 1.0 1.0\n"
		"}\n"
		"\n"
		"standard_shader\n"
		"{\n"
		"\tname global\n"
		"\tshaderop DefaultDirectLighting\n"
		"}\n"
		"\n"
		"pathtracing_pel_rasterizer\n"
		"{\n"
		"\tsamples 256\n"
		"\tmax_volume_bounce 16\n"
		"\tpixel_filter box\n"
		"\tradiance_map pnt_env\n"
		"\tradiance_scale 1.0\n"
		"\tradiance_background TRUE\n"
		"}\n"
		"\n"
		"file_rasterizeroutput\n"
		"{\n"
		"\tpattern /tmp/volume_absorption_rgb_unused\n"
		"\ttype PNG\n"
		"\tbpp 8\n"
		"\tcolor_space sRGB\n"
		"}\n"
		"\n"
		"film\n"
		"{\n"
		"\twidth 16\n"
		"\theight 16\n"
		"}\n"
		"\n"
		"pinhole_camera\n"
		"{\n"
		"\tlocation 0 0 -5\n"
		"\tlookat 0 0 0\n"
		"\tup 0 1 0\n"
		"\tfov 10.0\n"
		"}\n"
		"\n"
		"homogeneous_medium\n"
		"{\n"
		"\tname slab_abs\n"
		"\tabsorption " << sa_r << " " << sa_g << " " << sa_b << "\n"
		"\tscattering 0.0 0.0 0.0\n"
		"\tphase isotropic\n"
		"}\n"
		"\n"
		"perfectrefractor_material\n"
		"{\n"
		"\tname clear\n"
		"\trefractance pnt_env\n"
		"\tior 1.0\n"
		"}\n"
		"\n"
		"box_geometry\n"
		"{\n"
		"\tname slabgeom\n"
		"\twidth 4.0\n"
		"\theight 4.0\n"
		"\tdepth " << kSlabDepth << "\n"
		"}\n"
		"\n"
		"standard_object\n"
		"{\n"
		"\tname slab\n"
		"\tgeometry slabgeom\n"
		"\tposition 0 0 0\n"
		"\tmaterial clear\n"
		"\tinterior_medium slab_abs\n"
		"}\n";
	return ss.str();
}

static std::string BuildSpectralScene( double sa )
{
	std::ostringstream ss;
	ss <<
		"RISE ASCII SCENE 6\n"
		"\n"
		"uniformcolor_painter\n"
		"{\n"
		"\tname pnt_env\n"
		"\tcolor 1.0 1.0 1.0\n"
		"}\n"
		"\n"
		"standard_shader\n"
		"{\n"
		"\tname global\n"
		"\tshaderop DefaultDirectLighting\n"
		"}\n"
		"\n"
		"pathtracing_spectral_rasterizer\n"
		"{\n"
		"\tsamples 512\n"
		"\tmax_volume_bounce 16\n"
		"\tpixel_filter box\n"
		"\tnmbegin 380\n"
		"\tnmend 720\n"
		"\tnum_wavelengths 8\n"
		"\tspectral_samples 1\n"
		"\thwss false\n"
		"\tmax_diffuse_bounce 3\n"
		"\tradiance_map pnt_env\n"
		"\tradiance_scale 1.0\n"
		"\tradiance_background TRUE\n"
		"}\n"
		"\n"
		"file_rasterizeroutput\n"
		"{\n"
		"\tpattern /tmp/volume_absorption_nm_unused\n"
		"\ttype PNG\n"
		"\tbpp 8\n"
		"\tcolor_space sRGB\n"
		"}\n"
		"\n"
		"film\n"
		"{\n"
		"\twidth 16\n"
		"\theight 16\n"
		"}\n"
		"\n"
		"pinhole_camera\n"
		"{\n"
		"\tlocation 0 0 -5\n"
		"\tlookat 0 0 0\n"
		"\tup 0 1 0\n"
		"\tfov 10.0\n"
		"}\n"
		"\n"
		"homogeneous_medium\n"
		"{\n"
		"\tname slab_abs\n"
		"\tabsorption " << sa << " " << sa << " " << sa << "\n"
		"\tscattering 0.0 0.0 0.0\n"
		"\tphase isotropic\n"
		"}\n"
		"\n"
		"perfectrefractor_material\n"
		"{\n"
		"\tname clear\n"
		"\trefractance pnt_env\n"
		"\tior 1.0\n"
		"}\n"
		"\n"
		"box_geometry\n"
		"{\n"
		"\tname slabgeom\n"
		"\twidth 4.0\n"
		"\theight 4.0\n"
		"\tdepth " << kSlabDepth << "\n"
		"}\n"
		"\n"
		"standard_object\n"
		"{\n"
		"\tname slab\n"
		"\tgeometry slabgeom\n"
		"\tposition 0 0 0\n"
		"\tmaterial clear\n"
		"\tinterior_medium slab_abs\n"
		"}\n";
	return ss.str();
}

// Beer-Lambert reference for the background path: L_env * exp(-sigma_a * d).
static double Expected( double sa )
{
	return std::exp( -sa * kSlabDepth );
}

// Observed optical depth implied by a measured channel value: -ln(measured).
// (background L = 1, so measured = exp(-tau_obs).)  The bug yields
// tau_obs ~ 2 * sigma_a * d (twice the authored optical depth).
static double ObservedTau( double measured )
{
	// Build is -ffast-math (no infinities); clamp a non-positive measurement
	// to a small floor so -ln() yields a large finite "very absorbed" tau for
	// the diagnostic print rather than tripping UB.
	const double m = std::fmax( measured, 1e-12 );
	return -std::log( m );
}

//////////////////////////////////////////////////////////////////////
// Tests
//////////////////////////////////////////////////////////////////////

// Relative tolerance.  The signal-vs-bug gap is large (exp(-tau) vs
// exp(-2tau)); 8% comfortably separates them while absorbing MC noise at
// the sample counts used.
static const double kRelTol = 0.08;

static bool ChannelOk( double measured, double expected, const char* name )
{
	const double denom = std::fmax( std::fabs( expected ), 1e-4 );
	const double rel = std::fabs( measured - expected ) / denom;
	const bool ok = rel <= kRelTol;
	if( !ok ) {
		std::cout << "    " << name
			<< "  measured=" << measured
			<< "  expected=" << expected
			<< "  rel=" << rel
			<< "  (observed optical depth=" << ObservedTau( measured )
			<< " vs authored=" << ObservedTau( expected ) << ")"
			<< std::endl;
	}
	return ok;
}

static void TestRGBGray()
{
	std::cout << "[A] RGB gray absorber (sigma_a = 0.5 all channels, d = "
		<< kSlabDepth << ")" << std::endl;
	const double sa = 0.5;
	const std::string scene = BuildRGBScene( sa, sa, sa );
	const PixelRGB px = RenderCentralBlock( scene, "rgb_gray" );

	Check( px.valid, "A: render produced a frame" );
	if( !px.valid ) return;

	const double exp = Expected( sa );
	std::cout << "    measured (" << px.r << ", " << px.g << ", " << px.b
		<< ")  expected " << exp << " each" << std::endl;

	Check( ChannelOk( px.r, exp, "A.r" ), "A: red channel == exp(-sigma_a*d)" );
	Check( ChannelOk( px.g, exp, "A.g" ), "A: green channel == exp(-sigma_a*d)" );
	Check( ChannelOk( px.b, exp, "A.b" ), "A: blue channel == exp(-sigma_a*d)" );
}

static void TestRGBColored()
{
	// Distinct per-channel absorption.  A correct Tr/PTTrReduced fix keeps
	// each channel at its own exp(-sigma_a[c]*d); a "delete the * Tr" fix
	// would collapse all channels toward the survival pdf (the max-sigma_t
	// channel) and DESATURATE the medium.
	std::cout << "[B] RGB coloured absorber (sigma_a = 0.2/0.6/1.2, d = "
		<< kSlabDepth << ")" << std::endl;
	const double sar = 0.2, sag = 0.6, sab = 1.2;
	const std::string scene = BuildRGBScene( sar, sag, sab );
	const PixelRGB px = RenderCentralBlock( scene, "rgb_colored" );

	Check( px.valid, "B: render produced a frame" );
	if( !px.valid ) return;

	const double er = Expected( sar );
	const double eg = Expected( sag );
	const double eb = Expected( sab );
	std::cout << "    measured (" << px.r << ", " << px.g << ", " << px.b
		<< ")  expected (" << er << ", " << eg << ", " << eb << ")" << std::endl;

	Check( ChannelOk( px.r, er, "B.r" ), "B: red == exp(-0.2*d)" );
	Check( ChannelOk( px.g, eg, "B.g" ), "B: green == exp(-0.6*d)" );
	Check( ChannelOk( px.b, eb, "B.b" ), "B: blue == exp(-1.2*d)" );

	// Explicit desaturation guard: the channels must stay well separated.
	// (red brighter than green brighter than blue, with the authored ratio.)
	Check( px.r > px.g * 1.2 && px.g > px.b * 1.2,
		"B: medium stays coloured (r > g > b, not desaturated)" );
}

static void TestNMSpectral()
{
	std::cout << "[C] NM / spectral absorber (sigma_a = 0.5, d = "
		<< kSlabDepth << ", hwss off)" << std::endl;
	const double sa = 0.5;
	const std::string scene = BuildSpectralScene( sa );
	const PixelRGB px = RenderCentralBlock( scene, "nm" );

	Check( px.valid, "C: render produced a frame" );
	if( !px.valid ) return;

	// The spectral pipeline integrates a uniform white env over a gray
	// absorber, so resolved RGB is gray at exp(-sigma_a*d).  Use the
	// luminance-ish mean of the three channels for the optical-depth check
	// (each channel should already be ~equal).
	const double meanCh = ( px.r + px.g + px.b ) / 3.0;
	const double exp = Expected( sa );
	std::cout << "    measured (" << px.r << ", " << px.g << ", " << px.b
		<< ")  mean=" << meanCh << "  expected " << exp << std::endl;

	Check( ChannelOk( meanCh, exp, "C.mean" ),
		"C: spectral mean == exp(-sigma_a*d)" );
}

int main( int /*argc*/, char* /*argv*/[] )
{
	std::cout << "VolumeAbsorptionAttenuationTest — Beer-Lambert single-count "
		"through a homogeneous absorbing slab" << std::endl;

	TestRGBGray();
	TestRGBColored();
	TestNMSpectral();

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;

	return failCount == 0 ? 0 : 1;
}
