//////////////////////////////////////////////////////////////////////
//
//  MLTSpectralIntegralNormalizationTest.cpp - DL-461: mlt_spectral_rasterizer
//    must render at the brightness of every other rasterizer.
//
//    MLTSpectralRasterizer::EvaluateSampleSpectral averaged its XYZ over
//    the wavelength evaluations (1/totalWavelengths) but never applied the
//    spectral INTEGRAL normalization (lambda_end - lambda_begin) / k_y
//    (~3.74 over [380, 780] nm) that every other spectral rasterizer
//    carries as `mYNormalization` (PixelBasedSpectralIntegratingRasterizer;
//    DL-217 is the same omission on the BDPT/VCM splat path).  A uniform
//    sampled wavelength makes (1/N) sum xbar(l) L(l) the AVERAGE of the
//    integrand over the band; the factor turns it into the integral and
//    divides by k_y so a unit spectrum resolves to Y = 1.  Without it MLT
//    spectral read ~0.27x of MLT pel / PT spectral, still and animation.
//
//    Rows (all orthographic, box filter, OIDN off, linear capture):
//      A. Grey Lambertian floor under one off-axis square emitter, against
//         its closed form (the same oracle as PixelCenterConventionTest B):
//         MLT pel, MLT spectral hwss FALSE and hwss TRUE.
//      B. Coloured floor (red / green / blue / white quadrants) under a
//         square emitter plus an omni light: per-quadrant, per-channel
//         means of MLT spectral (hwss FALSE and TRUE) against PT spectral
//         (num_wavelengths 160, so its grid quadrature bias is negligible).
//         The pre-fix per-region ratio is the same ~3.7 everywhere; any
//         residual REGION dependence (the 3.6-5.7 spread DL-458's parity
//         test reported) would fail here.
//
//    MLT is PSSMLT, so the Sobol' value salt does not reach it.  Repeats
//    vary the `chains` count (each count re-draws every chain seed from the
//    bootstrap CDF and re-seeds every proposal RNG), but they SHARE the
//    bootstrap (seed i is sample i), and PSSMLT's whole-image energy is
//    exactly the bootstrap's b -- so the repeat spread understates the error
//    and every gate is max(3 se, floor): row A 1.5 % (bootstrap error at
//    100000 samples), row B achromatic 4 %, per channel 4 % under HWSS and
//    20 % without it (single-wavelength chroma noise, see TestColourRegions).
//    Defaults 100000 bootstrap / 2048 mutations per pixel; override with
//    RISE_MLTSN_BOOTSTRAP / RISE_MLTSN_MUTATIONS for probing.  ~45 s.
//
//    Red on the unfixed library: every MLT spectral cell reads 0.26-0.30
//    (hwss TRUE) / 0.19-0.30 (hwss FALSE).  The 3.6-5.7 per-region spread
//    DL-458 reported is this factor times the hwss FALSE chroma noise at
//    low mutation counts -- no second factor: at 4096 mutations / 200000
//    bootstrap every cell of both modes reads 0.97-1.04.
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
#include "../src/Library/Interfaces/ILog.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/Color/Color_Template.h"
#include "../src/Library/Utilities/SobolSampler.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

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
	char buf[16384];
	va_list ap;
	va_start( ap, f );
	std::vsnprintf( buf, sizeof(buf), f, ap );
	va_end( ap );
	return std::string( buf );
}

static const unsigned kRes = 32;

//! Linear RGB image of one render (empty on failure).
struct Image
{
	std::vector<double> rgb;	// 3 * kRes * kRes
};

static Image RenderOnce( const std::string& scene, const char* tag, unsigned salt )
{
	Image out;
	const std::string path = Fmt( "/tmp/mlt_spectral_norm_%s_%d.RISEscene", tag, int( getpid() ) );
	{
		std::ofstream ofs( path.c_str() );
		ofs << scene;
	}
	SobolSamplerTestHooks::ValueSalt().store( salt );
	IJobPriv* pJob = nullptr;
	if( RISE_CreateJobPriv( &pJob ) && pJob ) {
		if( pJob->LoadAsciiSceneViaCst( path.c_str() ) ) {
			pJob->RemoveRasterizerOutputs();
			CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
			GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "test capture output" );
			pJob->GetRasterizer()->AddRasterizerOutput( pCap );
			std::srand( salt );
			if( pJob->Rasterize() && pCap->width == kRes && pCap->height == kRes ) {
				out.rgb.resize( 3 * kRes * kRes );
				for( size_t k = 0; k < pCap->pixels.size(); k++ ) {
					const RISEColor& c = pCap->pixels[k];
					out.rgb[3*k+0] = c.base.r * c.a;
					out.rgb[3*k+1] = c.base.g * c.a;
					out.rgb[3*k+2] = c.base.b * c.a;
				}
			}
			safe_release( pCap );
		}
		safe_release( pJob );
	}
	SobolSamplerTestHooks::ValueSalt().store( 0u );
	std::remove( path.c_str() );
	return out;
}

//! Mean of channel `ch` (0..2, or 3 = achromatic) over pixels [x0,x1) x [y0,y1).
static double RegionMean( const Image& img, unsigned x0, unsigned x1, unsigned y0, unsigned y1, int ch )
{
	double s = 0;
	for( unsigned y = y0; y < y1; y++ ) {
		for( unsigned x = x0; x < x1; x++ ) {
			const size_t k = size_t( y ) * kRes + x;
			s += ch == 3 ? ( img.rgb[3*k] + img.rgb[3*k+1] + img.rgb[3*k+2] ) / 3.0 : img.rgb[3*k+ch];
		}
	}
	return s / double( ( x1 - x0 ) * ( y1 - y0 ) );
}

static void MeanSd( const std::vector<double>& v, double& mean, double& se )
{
	mean = 0;
	for( double x : v ) mean += x;
	mean /= double( v.size() );
	double ss = 0;
	for( double x : v ) ss += ( x - mean ) * ( x - mean );
	se = v.size() > 1 ? std::sqrt( ss / double( v.size() - 1 ) / double( v.size() ) ) : 0.0;
}

static const char* kShader = "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n";

static unsigned Mutations()
{
	const char* e = std::getenv( "RISE_MLTSN_MUTATIONS" );
	return e ? unsigned( std::atoi( e ) ) : 2048u;
}

static unsigned Bootstrap()
{
	const char* e = std::getenv( "RISE_MLTSN_BOOTSTRAP" );
	return e ? unsigned( std::atoi( e ) ) : 100000u;
}

static std::string MLT( bool spectral, bool hwss, unsigned chains )
{
	if( !spectral ) {
		return std::string( kShader ) + Fmt( "mlt_rasterizer\n{\n\tbootstrap_samples %u\n\tchains %u\n\tmutations_per_pixel %u\n\tlarge_step_prob 0.3\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n", Bootstrap(), chains, Mutations() );
	}
	return std::string( kShader ) + Fmt( "mlt_spectral_rasterizer\n{\n\tbootstrap_samples %u\n\tchains %u\n\tmutations_per_pixel %u\n\tlarge_step_prob 0.3\n\thwss %s\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n",
		Bootstrap(), chains, Mutations(), hwss ? "TRUE" : "FALSE" );
}

//////////////////////////////////////////////////////////////////////
// Row A: grey floor vs its closed form
//////////////////////////////////////////////////////////////////////
static const double kPi = 3.14159265358979323846;
static const double kRho = 0.5, kH = 1.4, kHalf = 0.4, kScale = 6.0, kCx = 0.6, kView = 1.0;

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

static double ClosedFormMean()
{
	const int N = 384;
	double sum = 0;
	for( int j = 0; j < N; j++ ) {
		for( int i = 0; i < N; i++ ) {
			const double x = -kView / 2 + ( i + 0.5 ) * kView / N;
			const double y = -kView / 2 + ( j + 0.5 ) * kView / N;
			sum += kScale * RectF( x, y, kCx - kHalf, kCx + kHalf, -kHalf, kHalf, kH );
		}
	}
	return kRho / kPi * sum / double( N * N );
}

static std::string Camera()
{
	return Fmt( "film\n{\n\twidth %u\n\theight %u\n}\n\n", kRes, kRes ) +
		Fmt( "orthographic_camera\n{\n\tlocation 0 0 1\n\tlookat 0 0 0\n\tup 0 1 0\n\tviewport_scale %g %g\n}\n\n", kView, kView );
}

static std::string Emitter( double cx, double cy, double half, double h, double scale )
{
	const double x1 = cx - half, x2 = cx + half, y1 = cy - half, y2 = cy + half;
	return Fmt( "uniformcolor_painter\n{\n\tname pnt_e\n\tcolor 1 1 1\n}\n\n"
		"lambertian_luminaire_material\n{\n\tname mat_e\n\texitance pnt_e\n\tscale %g\n\tmaterial none\n}\n\n"
		"clippedplane_geometry\n{\n\tname geo_e\n\tpta %g %g %g\n\tptb %g %g %g\n\tptc %g %g %g\n\tptd %g %g %g\n\tdoublesided FALSE\n}\n\n"
		"standard_object\n{\n\tname emit\n\tgeometry geo_e\n\tmaterial mat_e\n}\n\n",
		scale, x1, y2, h, x2, y2, h, x2, y1, h, x1, y1, h );
}

static std::string FloorQuad( const char* name, double x0, double x1, double y0, double y1, double r, double g, double b )
{
	return Fmt( "uniformcolor_painter\n{\n\tname pnt_%s\n\tcolor %g %g %g\n}\n\n"
		"lambertian_material\n{\n\tname mat_%s\n\treflectance pnt_%s\n}\n\n"
		"clippedplane_geometry\n{\n\tname geo_%s\n\tpta %g %g 0\n\tptb %g %g 0\n\tptc %g %g 0\n\tptd %g %g 0\n\tdoublesided FALSE\n}\n\n"
		"standard_object\n{\n\tname %s\n\tgeometry geo_%s\n\tmaterial mat_%s\n}\n\n",
		name, r, g, b, name, name, name, x0, y0, x1, y0, x1, y1, x0, y1, name, name, name );
}

static std::string GreyScene( const std::string& ras )
{
	return std::string( "RISE ASCII SCENE 7\n\n" ) + ras + Camera() +
		FloorQuad( "floor", -200, 200, -200, 200, kRho, kRho, kRho ) +
		Emitter( kCx, 0, kHalf, kH, kScale );
}

static const int kRepeats = 4;

static void TestGreyClosedForm()
{
	std::cout << "\n-- A: grey Lambertian floor vs closed form --" << std::endl;
	const double cf = ClosedFormMean();
	struct Case { const char* name; bool spectral; bool hwss; };
	const Case cases[] = { { "MLT pel", false, false }, { "MLT spectral", true, false }, { "MLT spectral HWSS", true, true } };
	for( const Case& c : cases ) {
		std::vector<double> ratios;
		for( int i = 0; i < kRepeats; i++ ) {
			const Image img = RenderOnce( GreyScene( MLT( c.spectral, c.hwss, 61 + 2 * i ) ), "grey", 0x461A0u + i );
			if( img.rgb.empty() ) {
				Check( false, std::string( "A " ) + c.name + ": render produced output" );
				return;
			}
			ratios.push_back( RegionMean( img, 0, kRes, 0, kRes, 3 ) / cf );
		}
		double m, se;
		MeanSd( ratios, m, se );
		const double band = std::max( 3.0 * se, 0.015 );
		std::printf( "    %-20s mean/closed form %.4f +/- %.4f (n = %d, band %.4f)\n", c.name, m, se, kRepeats, band );
		Check( std::fabs( m - 1.0 ) < band, std::string( "A " ) + c.name + " matches the closed form" );
	}
}

//////////////////////////////////////////////////////////////////////
// Row B: coloured quadrants, region-wise per channel vs PT spectral
//////////////////////////////////////////////////////////////////////
static std::string ColourScene( const std::string& ras )
{
	return std::string( "RISE ASCII SCENE 7\n\n" ) + ras + Camera() +
		FloorQuad( "red",   -200, 0, 0, 200,    0.80, 0.10, 0.10 ) +
		FloorQuad( "green",  0, 200, 0, 200,    0.10, 0.70, 0.10 ) +
		FloorQuad( "blue",  -200, 0, -200, 0,   0.10, 0.10, 0.80 ) +
		FloorQuad( "white",  0, 200, -200, 0,   0.70, 0.70, 0.70 ) +
		Emitter( 0.3, 0.2, 0.3, 1.2, 5.0 ) +
		"omni_light\n{\n\tname omni\n\tpower 3\n\tcolor 1 0.8 0.6\n\tposition -0.4 -0.3 0.8\n}\n\n";
}

static void TestColourRegions()
{
	std::cout << "\n-- B: coloured quadrants, per region and channel, vs PT spectral --" << std::endl;
	const unsigned h = kRes / 2;
	// Image row 0 is the TOP of the view (+y), so red (x<0, y>0) is top left.
	struct Region { const char* name; unsigned x0, x1, y0, y1; };
	const Region regions[] = {
		{ "red",   1, h - 1, 1, h - 1 },
		{ "green", h + 1, kRes - 1, 1, h - 1 },
		{ "blue",  1, h - 1, h + 1, kRes - 1 },
		{ "white", h + 1, kRes - 1, h + 1, kRes - 1 },
	};
	const char* chName[4] = { "R", "G", "B", "achromatic" };

	// Reference: PT spectral, 160 wavelengths, salted repeats.
	const std::string pt = std::string( kShader ) +
		"pathtracing_spectral_rasterizer\n{\n\tsamples 256\n\thwss FALSE\n\tnum_wavelengths 160\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";
	std::vector<Image> ref;
	for( int i = 0; i < kRepeats; i++ ) {
		ref.push_back( RenderOnce( ColourScene( pt ), "colpt", 0x461B0u + i ) );
		if( ref.back().rgb.empty() ) {
			Check( false, "B PT spectral reference produced output" );
			return;
		}
	}

	for( const bool hwss : { false, true } ) {
		std::vector<Image> mlt;
		for( int i = 0; i < kRepeats; i++ ) {
			mlt.push_back( RenderOnce( ColourScene( MLT( true, hwss, 61 + 2 * i ) ), "colmlt", 0x461C0u + i ) );
			if( mlt.back().rgb.empty() ) {
				Check( false, "B MLT spectral produced output" );
				return;
			}
		}
		double rmin = 1e30, rmax = -1e30;
		for( const Region& r : regions ) {
			for( int ch = 0; ch < 4; ch++ ) {
				std::vector<double> a, b;
				for( int i = 0; i < kRepeats; i++ ) {
					a.push_back( RegionMean( mlt[i], r.x0, r.x1, r.y0, r.y1, ch ) );
					b.push_back( RegionMean( ref[i], r.x0, r.x1, r.y0, r.y1, ch ) );
				}
				double ma, sa, mb, sb;
				MeanSd( a, ma, sa );
				MeanSd( b, mb, sb );
				const double ratio = mb > 0 ? ma / mb : 0.0;
				const double se = ratio * std::sqrt( ( ma > 0 ? sa * sa / ( ma * ma ) : 0.0 ) + sb * sb / ( mb * mb ) );
				// Floors (see the file header): the achromatic region mean
				// 4 %; a single channel 4 % under HWSS and 20 % without it,
				// where one wavelength per mutation under a Y-only Metropolis
				// target leaves the weak channel of a saturated surface
				// (~0.05 of its strong one) noisy -- measured 0.87 .. 1.04 at
				// these settings across runs, moving both ways with the
				// mutation count (a variance, not a bias).  Pre-fix every
				// cell read ~0.27.
				const double floor = ( ch == 3 || hwss ) ? 0.04 : 0.20;
				const double band = std::max( 3.0 * se, floor );
				rmin = std::min( rmin, ratio );
				rmax = std::max( rmax, ratio );
				std::printf( "    %-17s %-5s %s: MLT %.5f PT %.5f ratio %.4f +/- %.4f (band %.4f)\n",
					hwss ? "MLT spectral HWSS" : "MLT spectral", r.name, chName[ch], ma, mb, ratio, se, band );
				Check( std::fabs( ratio - 1.0 ) < band,
					Fmt( "B %s %s %s matches PT spectral", hwss ? "hwss TRUE" : "hwss FALSE", r.name, chName[ch] ) );
			}
		}
		std::printf( "    ratio spread over regions/channels: %.4f .. %.4f\n", rmin, rmax );
	}
}

int main( int argc, char** argv )
{
	std::cout << "=== MLTSpectralIntegralNormalizationTest (DL-461) ===" << std::endl;
	const std::string only = argc > 1 ? argv[1] : "";
	if( only.empty() || only == "A" ) TestGreyClosedForm();
	if( only.empty() || only == "B" ) TestColourRegions();
	std::cout << "\nPassed: " << passCount << "\nFailed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
