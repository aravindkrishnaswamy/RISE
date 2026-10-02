//////////////////////////////////////////////////////////////////////
//
//  PixelCenterConventionTest.cpp - DL-368: every rasterizer places its
//  pixels on its camera's NOMINAL film.
//
//  Screen space is the camera's film, x in [0, W), y in [0, H): every
//  camera translates it by -W/2, -H/2 before projecting, so the optical
//  axis passes through screen (W/2, H/2) -- the CORNER shared by pixels
//  (W/2 - 1, W/2) x (rows H/2 - 1, H/2).  Before DL-368 every rasterizer
//  drew pixel (x, row y) at (x + u - 0.5, H - y + v - 0.5), so pixel
//  (W/2, row H/2) was CENTRED on the axis instead: the image sat half a
//  pixel off its own camera on both axes, for every camera.
//
//  ROWS
//    A  Quadrant probe.  Four tiny emitting squares, one in each quadrant
//       of the plane the camera looks straight down at, each touching the
//       optical axis and each imaging into well under one pixel.  On the
//       nominal film they land in the four pixels around the axis, a
//       quarter of the energy each; under the old convention all four
//       land in the ONE pixel centred on the axis.  Rendered through
//       every rasterizer (PT, PT spectral hero / HWSS, pixelpel,
//       pixelintegratingspectral, BDPT, BDPT spectral, VCM, VCM spectral,
//       MLT, MLT spectral) and every camera (orthographic, pinhole, thin
//       lens focused on the plane, fisheye).  Orientation-agnostic: the
//       check is "a 2 x 2 block around the axis holds the energy, a
//       quarter per pixel", which no mirror or transposition can change.
//    B  The row's own closed form: a floor under an orthographic camera
//       (view 1, 32 px) lit by ONE small face-down quad off to the side
//       at x = +2 and then x = -2.  The irradiance gradient across the
//       footprint turns the half-pixel shift into a mean bias, measured
//       pre-fix at 0.9794 / 1.0208 of the closed form under PT and BDPT.
//
//  Box filter, OIDN off, salted Sobol' renders.  The splat side (a t = 1
//  light-traced splat landing in the same pixel an eye hit would) is
//  BDPTStrategyBalanceTest's TestNarrowFovStripeGuard (`--narrow-fov-only`).
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
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
#include "../src/Library/Utilities/SobolSampler.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const std::string& name )
{
	if( condition ) {
		passCount++;
	} else {
		failCount++;
		std::cout << "  FAIL: " << name << std::endl;
	}
}

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

	virtual void OutputImage( const IRasterImage& img, const Rect*, const unsigned int ) override
	{
		width = img.GetWidth();
		height = img.GetHeight();
		pixels.resize( width * height );
		for( unsigned int y = 0; y < height; y++ ) {
			for( unsigned int x = 0; x < width; x++ ) {
				pixels[y * width + x] = img.GetPEL( x, y );
			}
		}
	}
};

static std::string Fmt( const char* f, ... )
{
	char buf[8192];
	va_list ap;
	va_start( ap, f );
	std::vsnprintf( buf, sizeof(buf), f, ap );
	va_end( ap );
	return std::string( buf );
}

//! Renders `scene` once per salt and returns the per-pixel achromatic
//! mean over `n` salted renders (empty on any failure).
static std::vector<double> RenderMean( const std::string& scene, const char* tag, int n, unsigned saltBase,
	unsigned& w, unsigned& h )
{
	const std::string path = Fmt( "/tmp/pixel_center_convention_%s_%d.RISEscene", tag, int( getpid() ) );
	{
		std::ofstream ofs( path.c_str() );
		ofs << scene;
	}
	std::vector<double> sum;
	bool ok = true;
	for( int i = 0; i < n && ok; i++ ) {
		SobolSamplerTestHooks::ValueSalt().store( SobolSequence::HashCombine( saltBase, unsigned( i ) ) );
		IJobPriv* pJob = nullptr;
		ok = RISE_CreateJobPriv( &pJob ) && pJob;
		if( !ok ) break;
		ok = pJob->LoadAsciiSceneViaCst( path.c_str() );
		if( ok ) {
			pJob->RemoveRasterizerOutputs();
			CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
			GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "test capture output" );
			pJob->GetRasterizer()->AddRasterizerOutput( pCap );
			std::srand( 0x368u + unsigned( i ) );
			ok = pJob->Rasterize() && pCap->width > 0 && pCap->height > 0;
			if( ok ) {
				w = pCap->width;
				h = pCap->height;
				if( sum.empty() ) sum.assign( size_t( w ) * h, 0.0 );
				for( size_t k = 0; k < sum.size(); k++ ) {
					const RISEColor& c = pCap->pixels[k];
					sum[k] += ( c.base.r + c.base.g + c.base.b ) / 3.0 * c.a;
				}
			}
			safe_release( pCap );
		}
		safe_release( pJob );
	}
	SobolSamplerTestHooks::ValueSalt().store( 0u );
	std::remove( path.c_str() );
	if( !ok ) return std::vector<double>();
	for( double& v : sum ) v /= double( n );
	return sum;
}

//////////////////////////////////////////////////////////////////////
// Row A: quadrant probe
//////////////////////////////////////////////////////////////////////
static const unsigned kRes = 32;

static std::string Emitter( const char* name, double x0, double x1, double y0, double y1, double scale )
{
	// Normal +Z (toward the camera); double-sided anyway.
	return Fmt(
		"uniformcolor_painter\n{\n\tname pnt_%s\n\tcolor 1 1 1\n}\n\n"
		"lambertian_luminaire_material\n{\n\tname mat_%s\n\texitance pnt_%s\n\tscale %g\n\tmaterial none\n}\n\n"
		"clippedplane_geometry\n{\n\tname geo_%s\n\tpta %g %g 0\n\tptb %g %g 0\n\tptc %g %g 0\n\tptd %g %g 0\n\tdoublesided TRUE\n}\n\n"
		"standard_object\n{\n\tname %s\n\tgeometry geo_%s\n\tmaterial mat_%s\n}\n\n",
		name, name, name, scale, name, x0, y0, x1, y0, x1, y1, x0, y1, name, name, name );
}

struct CameraCase { const char* name; std::string text; };

static std::vector<CameraCase> Cameras()
{
	// Every camera looks straight down -Z at the z = 0 plane from z = 4,
	// so the plane is parallel to the film and the axis hits its origin.
	return {
		{ "orthographic", "orthographic_camera\n{\n\tlocation 0 0 4\n\tlookat 0 0 0\n\tup 0 1 0\n\tviewport_scale 4 4\n}\n\n" },
		{ "pinhole",      "pinhole_camera\n{\n\tlocation 0 0 4\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 53.13\n}\n\n" },
		{ "thin lens",    "thinlens_camera\n{\n\tlocation 0 0 4\n\tlookat 0 0 0\n\tup 0 1 0\n\tsensor_size 36\n\tfocal_length 35\n\tfstop 2.8\n\tfocus_distance 4\n}\n\n" },
		{ "fisheye",      "fisheye_camera\n{\n\tlocation 0 0 4\n\tlookat 0 0 0\n\tup 0 1 0\n\tscale 1\n}\n\n" },
	};
}

struct RasterCase { const char* name; std::string text; int reps; };

static std::vector<RasterCase> Rasterizers()
{
	const char* pt = "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n";
	const char* em = "standard_shader\n{\n\tname global\n\tshaderop DefaultEmission\n}\n\n";
	return {
		{ "PT",                      std::string( pt ) + "pathtracing_pel_rasterizer\n{\n\tsamples 256\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n", 2 },
		{ "PT spectral",             std::string( pt ) + "pathtracing_spectral_rasterizer\n{\n\tsamples 256\n\thwss FALSE\n\tnum_wavelengths 16\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n", 2 },
		{ "PT spectral HWSS",        std::string( pt ) + "pathtracing_spectral_rasterizer\n{\n\tsamples 256\n\thwss TRUE\n\tnum_wavelengths 16\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n", 2 },
		{ "pixelpel",                std::string( em ) + "pixelpel_rasterizer\n{\n\tsamples 256\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n", 2 },
		{ "pixelintegratingspectral", std::string( em ) + "pixelintegratingspectral_rasterizer\n{\n\tsamples 256\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n", 2 },
		{ "BDPT",                    std::string( pt ) + "bdpt_pel_rasterizer\n{\n\tsamples 256\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n", 2 },
		{ "BDPT spectral HWSS",      std::string( pt ) + "bdpt_spectral_rasterizer\n{\n\tsamples 256\n\thwss TRUE\n\tnum_wavelengths 16\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n", 2 },
		{ "VCM",                     std::string( pt ) + "vcm_pel_rasterizer\n{\n\tsamples 256\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n", 2 },
		{ "VCM spectral",            std::string( pt ) + "vcm_spectral_rasterizer\n{\n\tsamples 256\n\tnum_wavelengths 16\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n", 2 },
		{ "MLT",                     std::string( pt ) + "mlt_rasterizer\n{\n\tbootstrap_samples 20000\n\tchains 64\n\tmutations_per_pixel 256\n\tlarge_step_prob 0.3\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n", 1 },
		{ "MLT spectral",            std::string( pt ) + "mlt_spectral_rasterizer\n{\n\tbootstrap_samples 20000\n\tchains 64\n\tmutations_per_pixel 256\n\tlarge_step_prob 0.3\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n", 1 },
	};
}

//! Fraction of the image's energy in each pixel of the 2 x 2 block around
//! the screen centre, and the largest single-pixel fraction anywhere.
static void QuadrantFractions( const std::vector<double>& img, unsigned w, unsigned h,
	double block[4], double& maxAny, double& blockTotal )
{
	double total = 0;
	maxAny = 0;
	for( double v : img ) total += v;
	for( double v : img ) maxAny = std::max( maxAny, total > 0 ? v / total : 0.0 );
	const unsigned cx = w / 2, cy = h / 2;
	const unsigned px[4] = { cx - 1, cx, cx - 1, cx };
	const unsigned py[4] = { cy - 1, cy - 1, cy, cy };
	blockTotal = 0;
	for( int k = 0; k < 4; k++ ) {
		block[k] = total > 0 ? img[size_t( py[k] ) * w + px[k]] / total : 0.0;
		blockTotal += block[k];
	}
}

static void TestQuadrantProbe()
{
	std::cout << "\n-- A: quadrant probe (four sub-pixel emitters around the optical axis) --" << std::endl;
	// Each square spans [0, 0.03] from the axis: 0.24 px under the 4-unit
	// orthographic / pinhole footprint, ~0.08 px through the fisheye.
	const double s = 0.03;
	std::string emitters =
		Emitter( "q1",  0, s,  0, s, 10 ) + Emitter( "q2", -s, 0,  0, s, 10 ) +
		Emitter( "q3", -s, 0, -s, 0, 10 ) + Emitter( "q4",  0, s, -s, 0, 10 );
	const std::string film = Fmt( "film\n{\n\twidth %u\n\theight %u\n}\n\n", kRes, kRes );
	unsigned salt = 0x368A0u;
	for( const CameraCase& c : Cameras() ) {
		for( const RasterCase& r : Rasterizers() ) {
			unsigned w = 0, h = 0;
			const std::string scene = std::string( "RISE ASCII SCENE 7\n\n" ) + r.text + film + c.text + emitters;
			const std::vector<double> img = RenderMean( scene, "quad", r.reps, salt++, w, h );
			const std::string label = std::string( c.name ) + " / " + r.name;
			if( img.empty() || w != kRes || h != kRes ) {
				Check( false, "A " + label + ": render produced output" );
				continue;
			}
			double block[4], maxAny = 0, blockTotal = 0;
			QuadrantFractions( img, w, h, block, maxAny, blockTotal );
			double minBlock = 1;
			for( int k = 0; k < 4; k++ ) minBlock = std::min( minBlock, block[k] );
			std::printf( "    %-40s 2x2 block %.4f %.4f %.4f %.4f (sum %.4f)  max pixel %.4f\n",
				label.c_str(), block[0], block[1], block[2], block[3], blockTotal, maxAny );
			// Each pixel holds one square's energy: 0.25 up to MC noise (a
			// square covers ~6 % of a pixel, so an eye-hit estimator sees
			// few hits; measured block fractions 0.10 .. 0.42 at 64 spp).
			// The old convention puts all four in ONE pixel: max 1.0 and
			// three empty block pixels.
			Check( blockTotal > 0.98 && maxAny < 0.6 && minBlock > 0.08,
				"A " + label + ": the four quadrant emitters land one per pixel around the axis" );
		}
	}
}

//////////////////////////////////////////////////////////////////////
// Row B: the off-axis lone emitter vs its closed form
//////////////////////////////////////////////////////////////////////
static const double kPi = 3.14159265358979323846;
static const double kRho = 0.5;
static const double kH = 1.4, kHalf = 0.4, kScale = 6.0, kCx = 2.0, kView = 1.0;

static double CornerF( const double a, const double b, const double h )
{
	const double A = a / h, B = b / h;
	const double sA = std::sqrt( 1.0 + A * A ), sB = std::sqrt( 1.0 + B * B );
	return ( A / sA * std::atan( B / sA ) + B / sB * std::atan( A / sB ) ) / ( 2.0 * kPi );
}

static double RectF( double x, double y, double x1, double x2, double y1, double y2, double h )
{
	return CornerF( x2 - x, y2 - y, h ) - CornerF( x1 - x, y2 - y, h )
		- CornerF( x2 - x, y1 - y, h ) + CornerF( x1 - x, y1 - y, h );
}

static double ClosedFormMean( double cx )
{
	const int N = 512;
	double sum = 0;
	for( int j = 0; j < N; j++ ) {
		for( int i = 0; i < N; i++ ) {
			const double x = -kView / 2 + ( i + 0.5 ) * kView / N;
			const double y = -kView / 2 + ( j + 0.5 ) * kView / N;
			sum += kScale * RectF( x, y, cx - kHalf, cx + kHalf, -kHalf, kHalf, kH );
		}
	}
	return kRho / kPi * sum / double( N * N );
}

static std::string OffAxisScene( const std::string& ras, double cx )
{
	const double x1 = cx - kHalf, x2 = cx + kHalf, y1 = -kHalf, y2 = kHalf;
	return std::string( "RISE ASCII SCENE 7\n\n" ) + ras +
		Fmt( "film\n{\n\twidth %u\n\theight %u\n}\n\n", kRes, kRes ) +
		Fmt( "orthographic_camera\n{\n\tlocation 0 0 1\n\tlookat 0 0 0\n\tup 0 1 0\n\tviewport_scale %g %g\n}\n\n", kView, kView ) +
		Fmt( "uniformcolor_painter\n{\n\tname pnt_floor\n\tcolor %g %g %g\n}\n\n", kRho, kRho, kRho ) +
		"lambertian_material\n{\n\tname mat_floor\n\treflectance pnt_floor\n}\n\n"
		"clippedplane_geometry\n{\n\tname geo_floor\n\tpta -200 -200 0\n\tptb 200 -200 0\n\tptc 200 200 0\n\tptd -200 200 0\n\tdoublesided FALSE\n}\n\n"
		"standard_object\n{\n\tname floor\n\tgeometry geo_floor\n\tmaterial mat_floor\n}\n\n" +
		Fmt( "uniformcolor_painter\n{\n\tname pnt_e\n\tcolor 1 1 1\n}\n\n"
			"lambertian_luminaire_material\n{\n\tname mat_e\n\texitance pnt_e\n\tscale %g\n\tmaterial none\n}\n\n"
			"clippedplane_geometry\n{\n\tname geo_e\n\tpta %g %g %g\n\tptb %g %g %g\n\tptc %g %g %g\n\tptd %g %g %g\n\tdoublesided FALSE\n}\n\n"
			"standard_object\n{\n\tname emit\n\tgeometry geo_e\n\tmaterial mat_e\n}\n\n",
			kScale, x1, y2, kH, x2, y2, kH, x2, y1, kH, x1, y1, kH );
}

static void TestOffAxisEmitter()
{
	std::cout << "\n-- B: lone off-axis emitter vs its closed form (orthographic, view 1, 32 px) --" << std::endl;
	const char* pt = "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n";
	const std::vector<RasterCase> rs = {
		{ "PT",   std::string( pt ) + "pathtracing_pel_rasterizer\n{\n\tsamples 64\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n", 3 },
		{ "BDPT", std::string( pt ) + "bdpt_pel_rasterizer\n{\n\tmax_eye_depth 4\n\tmax_light_depth 4\n\tsamples 64\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n", 3 },
	};
	unsigned salt = 0x368B0u;
	for( const RasterCase& r : rs ) {
		for( const double cx : { kCx, -kCx } ) {
			const double cf = ClosedFormMean( cx );
			unsigned w = 0, h = 0;
			const std::vector<double> img = RenderMean( OffAxisScene( r.text, cx ), "offaxis", r.reps, salt++, w, h );
			double mean = 0;
			for( double v : img ) mean += v;
			mean = img.empty() ? 0.0 : mean / double( img.size() );
			const double ratio = cf > 0 ? mean / cf : 0.0;
			std::printf( "    %-6s emitter at x = %+g: mean %.6f closed form %.6f ratio %.4f\n", r.name, cx, mean, cf, ratio );
			// Pre-fix 0.9794 / 1.0208; the noise on this mean is ~0.1 %.
			Check( !img.empty() && std::fabs( ratio - 1.0 ) < 0.006,
				Fmt( "B %s emitter at x = %+g matches its closed form", r.name, cx ) );
		}
	}
}

int main( int argc, char** argv )
{
	std::cout << "=== PixelCenterConventionTest (DL-368) ===" << std::endl;
	const std::string only = argc > 1 ? argv[1] : "";
	if( only.empty() || only == "A" ) TestQuadrantProbe();
	if( only.empty() || only == "B" ) TestOffAxisEmitter();
	std::cout << "\nPassed: " << passCount << "\nFailed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
