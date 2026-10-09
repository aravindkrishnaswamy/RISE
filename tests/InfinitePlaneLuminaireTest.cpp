//////////////////////////////////////////////////////////////////////
//
//  InfinitePlaneLuminaireTest.cpp - DL-311 regression: an
//  `infiniteplane_geometry` carrying an emissive material.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-10-08
//  Tabs: 4
//  Comments:
//
//    THE DEFECT (pre-fix).  `InfinitePlaneGeometry::GetArea()` returns
//    RISE_INFINITY (DBL_MAX), and an infinite plane was admitted to the
//    luminaries list as an ordinary area light.  `LightSampler`'s power
//    table multiplied `averageRadiantExitance() * GetArea()`: finite (but
//    ~DBL_MAX, so the plane monopolised light selection with a pdfPosition
//    of 1/DBL_MAX) at exitance scale 1, +inf above it -- which poisoned the
//    alias table: PT pel and BDPT pel rendered NaN, BDPT spectral rendered a
//    silently black image.
//
//    THE FIX.  An infinite plane has no finite uniform area density, so it
//    cannot honour the area-light sampling contract.  It now reports
//    `CanBeAreaLight() == false`, which keeps it off the luminaries list
//    (LuminaryManager) and routes it through the existing, already-tested
//    non-NEE-sampleable-emitter path: PT gives its BSDF-sampled / camera
//    hits MIS weight 1 (no partner strategy), BDPT flags the s=0 vertex
//    `lightSamplingStrategyAbsent`, VCM zeroes the competing light pdf.
//
//    WHAT THIS TEST PINS.  Grey Lambertian sphere in front of an infinite
//    emissive plane (the row's minimal repro), at exitance scale 1 and 2,
//    under PT / BDPT / VCM / MLT pel and PT / BDPT spectral; four salted
//    renders each.  Every image must be finite; scale 2 must be twice
//    scale 1 (per integrator); PT, BDPT and VCM must agree within 3 sigma
//    (plus a 1 % floor), and PT / BDPT spectral likewise.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#if defined( _WIN32 )
	#include <process.h>
	#define RISE_TEST_GETPID _getpid
#else
	#include <unistd.h>
	#define RISE_TEST_GETPID getpid
#endif

#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Interfaces/ILog.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/SobolSampler.h"
#include "../src/Library/Utilities/Color/Color_Template.h"

using namespace RISE;
using namespace RISE::Implementation;

static int g_failures = 0;

static void Check( bool cond, const std::string& what )
{
	std::cout << ( cond ? "  [ok] " : "  [FAIL] " ) << what << "\n";
	if( !cond ) ++g_failures;
}

class LumaCapture : public virtual IRasterizerOutput, public virtual Reference
{
public:
	double meanLum = 0;
	unsigned nonFinite = 0;
protected:
	virtual ~LumaCapture() {}
public:
	virtual void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}
	virtual void OutputImage( const IRasterImage& img, const Rect*, const unsigned int ) override
	{
		const unsigned w = img.GetWidth(), h = img.GetHeight();
		double sum = 0;
		nonFinite = 0;
		for( unsigned y = 0; y < h; ++y ) {
			for( unsigned x = 0; x < w; ++x ) {
				const RISEColor c = img.GetPEL( x, y );
				const double l = ( c.base.r + c.base.g + c.base.b ) / 3.0;
				if( !std::isfinite( l ) ) { ++nonFinite; continue; }
				sum += l;
			}
		}
		meanLum = ( w && h ) ? sum / double( w * h ) : 0;
	}
};

static std::string WriteScene( const std::string& text, const std::string& tag )
{
	const char* tmp = std::getenv( "TMPDIR" );
	std::string path = ( tmp && *tmp ) ? tmp : ".";
	if( path.back() != '/' ) path += '/';
	char name[128];
	std::snprintf( name, sizeof( name ), "dl311_%s_%d.RISEscene", tag.c_str(), int( RISE_TEST_GETPID() ) );
	path += name;
	std::ofstream ofs( path.c_str() );
	ofs << text;
	return path;
}

static bool RenderOnce( const std::string& path, double& mean, unsigned& nonFinite )
{
	bool ok = false;
	IJobPriv* pJob = 0;
	if( RISE_CreateJobPriv( &pJob ) && pJob ) {
		if( pJob->LoadAsciiSceneViaCst( path.c_str() ) ) {
			pJob->RemoveRasterizerOutputs();
			LumaCapture* pCap = new LumaCapture();
			GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "dl311 capture" );
			pJob->GetRasterizer()->AddRasterizerOutput( pCap );
			ok = pJob->Rasterize();
			mean = pCap->meanLum;
			nonFinite = pCap->nonFinite;
			safe_release( pCap );
		}
		safe_release( pJob );
	}
	return ok;
}

static std::string Scene( const std::string& rasterizer, double scale )
{
	char lum[512];
	std::snprintf( lum, sizeof( lum ),
		"lambertian_luminaire_material\n{\n\tname mat_glow\n\texitance pnt_white\n\tscale %.3f\n\tmaterial none\n}\n\n",
		scale );
	return std::string( "RISE ASCII SCENE 7\n\n" )
		+ "film\n{\n\twidth 32\n\theight 32\n}\n\n"
		+ "pinhole_camera\n{\n\tlocation 0 0 5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n"
		+ "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		+ rasterizer
		+ "uniformcolor_painter\n{\n\tname pnt_grey\n\tcolor 0.5 0.5 0.5\n}\n\n"
		+ "uniformcolor_painter\n{\n\tname pnt_white\n\tcolor 1 1 1\n}\n\n"
		+ "lambertian_material\n{\n\tname grey\n\treflectance pnt_grey\n}\n\n"
		+ lum
		+ "sphere_geometry\n{\n\tname ball\n\tradius 1.0\n}\n\n"
		+ "standard_object\n{\n\tname obj_ball\n\tgeometry ball\n\tmaterial grey\n}\n\n"
		+ "infiniteplane_geometry\n{\n\tname backdrop\n\txtile 1.0\n\tytile 1.0\n}\n\n"
		+ "standard_object\n{\n\tname obj_backdrop\n\tgeometry backdrop\n\tmaterial mat_glow\n\tposition 0 0 -70\n}\n";
}

struct Stat { bool ok; unsigned nonFinite; double mean, sd; };

static Stat Measure( const std::string& rasterizer, double scale, const std::string& tag, unsigned saltBase )
{
	const std::string path = WriteScene( Scene( rasterizer, scale ), tag );
	const int n = 4;
	Stat s{ true, 0, 0, 0 };
	double sum = 0, sumSq = 0;
	for( int i = 0; i < n; ++i ) {
		SobolSamplerTestHooks::ValueSalt().store( SobolSequence::HashCombine( saltBase, unsigned( i ) ) );
		double m = 0; unsigned nf = 0;
		s.ok = RenderOnce( path, m, nf ) && s.ok;
		s.nonFinite += nf;
		sum += m; sumSq += m * m;
	}
	SobolSamplerTestHooks::ValueSalt().store( 0u );
	std::remove( path.c_str() );
	s.mean = sum / n;
	s.sd = std::sqrt( std::max( 0.0, ( sumSq - n * s.mean * s.mean ) / ( n - 1 ) ) );
	std::printf( "  %-14s scale %.0f  mean %.6f  sd %.6f  nonFinite %u\n", tag.c_str(), scale, s.mean, s.sd, s.nonFinite );
	return s;
}

static const std::string kCommon = "\tpixel_filter box\n\toidn_denoise FALSE\n\tshow_luminaires TRUE\n";
static const std::string kSpectral = "\tnum_wavelengths 160\n";

struct Integrator { const char* tag; std::string chunk; bool spectral; bool mlt; };

static void CheckAgree( const char* what, const Stat& a, const Stat& b )
{
	const double sigma = std::sqrt( a.sd * a.sd + b.sd * b.sd ) / 2.0;	// sd of the difference of two 4-render means
	const double tol = 3.0 * sigma + 0.01 * a.mean;
	char msg[256];
	std::snprintf( msg, sizeof( msg ), "%s agree: %.6f vs %.6f (|diff| %.6f <= %.6f)",
		what, a.mean, b.mean, std::fabs( a.mean - b.mean ), tol );
	Check( std::fabs( a.mean - b.mean ) <= tol, msg );
}

int main()
{
	std::vector<Integrator> ints = {
		{ "pt_pel",        "pathtracing_pel_rasterizer\n{\n\tsamples 64\n" + kCommon + "}\n\n", false, false },
		{ "bdpt_pel",      "bdpt_pel_rasterizer\n{\n\tsamples 64\n" + kCommon + "}\n\n", false, false },
		{ "vcm_pel",       "vcm_pel_rasterizer\n{\n\tsamples 64\n" + kCommon + "}\n\n", false, false },
		{ "mlt_pel",       "mlt_rasterizer\n{\n\tmutations_per_pixel 64\n\tbootstrap_samples 20000\n\tshow_luminaires TRUE\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n\n", false, true },
		{ "pt_spectral",   "pathtracing_spectral_rasterizer\n{\n\tsamples 64\n" + kCommon + kSpectral + "}\n\n", true, false },
		{ "bdpt_spectral", "bdpt_spectral_rasterizer\n{\n\tsamples 64\n" + kCommon + kSpectral + "}\n\n", true, false },
	};

	std::vector<Stat> s1, s2;
	unsigned salt = 0x311u;
	for( const Integrator& in : ints ) {
		s1.push_back( Measure( in.chunk, 1.0, in.tag, salt++ ) );
		s2.push_back( Measure( in.chunk, 2.0, in.tag, salt++ ) );
	}

	for( size_t i = 0; i < ints.size(); ++i ) {
		const std::string t = ints[i].tag;
		Check( s1[i].ok && s2[i].ok, t + ": rendered" );
		Check( s1[i].nonFinite == 0 && s2[i].nonFinite == 0, t + ": no non-finite pixels at scale 1 and 2" );
		Check( s1[i].mean > 0.1, t + ": scale-1 image is lit (not black)" );
		const double ratio = s1[i].mean > 0 ? s2[i].mean / s1[i].mean : 0;
		// MLT normalises its whole image by the bootstrap estimate, so its
		// scale-2/scale-1 ratio carries that estimator's noise too.
		// Otherwise: 3 sigma of the ratio (sd of each 4-render mean is sd/2)
		// plus a 0.5 % floor.
		const double rel = ( s1[i].mean > 0 && s2[i].mean > 0 )
			? 0.5 * std::sqrt( std::pow( s1[i].sd / s1[i].mean, 2 ) + std::pow( s2[i].sd / s2[i].mean, 2 ) ) : 0;
		const double band = ints[i].mlt ? 0.05 : ( 3.0 * rel + 0.005 );
		char msg[128];
		std::snprintf( msg, sizeof( msg ), "%s: scale 2 / scale 1 = %.5f (expected 2 +/- %.2f%%)", t.c_str(), ratio, band * 100 );
		Check( std::fabs( ratio - 2.0 ) <= 2.0 * band, msg );
	}

	// Cross-integrator agreement (scale 2).
	CheckAgree( "PT pel / BDPT pel",           s2[0], s2[1] );
	CheckAgree( "PT pel / VCM pel",            s2[0], s2[2] );
	CheckAgree( "PT spectral / BDPT spectral", s2[4], s2[5] );
	{
		const double r = s2[0].mean > 0 ? s2[3].mean / s2[0].mean : 0;
		char msg[128];
		std::snprintf( msg, sizeof( msg ), "MLT pel / PT pel = %.4f (within 5%%)", r );
		Check( std::fabs( r - 1.0 ) <= 0.05, msg );
	}

	if( g_failures == 0 ) { std::cout << "All InfinitePlaneLuminaire tests passed.\n"; return 0; }
	std::cout << g_failures << " InfinitePlaneLuminaire check(s) FAILED.\n";
	return 1;
}
