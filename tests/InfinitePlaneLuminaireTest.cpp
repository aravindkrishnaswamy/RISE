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
//    under PT / BDPT / VCM / MLT pel and PT / BDPT spectral; four renders
//    each (PT/BDPT/VCM: four Sobol' salts; MLT: PSSMLT does not read
//    SobolSamplerTestHooks::ValueSalt -- repeats are bit-identical,
//    measured -- so its four repeats vary `chains`, which reallocates its
//    random streams).  Every image must be finite; scale 2 must be twice
//    scale 1 (per integrator); the whole-image means must agree.
//
//    SPHERE METRIC.  The whole-image mean is dominated by camera-visible
//    backdrop pixels, which every integrator weights 1 by construction, so
//    it cannot see a few-percent MIS error on BSDF-sampled plane hits.  The
//    sphere-only metric can: over pixels fully covered by the sphere, the
//    mean is compared against a CLOSED FORM.  The plane is infinite and
//    faces +z, so from any sphere point every direction with d.z < 0 hits
//    it (radiance Lb, read from the fully-backdrop pixels), every other
//    direction escapes to black, and the convex sphere never shadows
//    itself: E(n) = pi Lb (1 - n.z) / 2 and the outgoing radiance of the
//    rho = 0.5 Lambertian is rho E / pi = 0.25 Lb (1 - n.z), averaged
//    over each pixel with 8x8 sub-pixel camera rays.  The sphere's
//    illumination arrives ONLY through BSDF-sampled plane hits (the plane
//    is not a light-table entry), so a weight below 1 there shows up
//    directly.
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
	std::vector<double> lum;
protected:
	virtual ~LumaCapture() {}
public:
	virtual void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}
	virtual void OutputImage( const IRasterImage& img, const Rect*, const unsigned int ) override
	{
		const unsigned w = img.GetWidth(), h = img.GetHeight();
		double sum = 0;
		nonFinite = 0;
		lum.assign( size_t( w ) * h, 0.0 );
		for( unsigned y = 0; y < h; ++y ) {
			for( unsigned x = 0; x < w; ++x ) {
				const RISEColor c = img.GetPEL( x, y );
				const double l = ( c.base.r + c.base.g + c.base.b ) / 3.0;
				if( !std::isfinite( l ) ) { ++nonFinite; continue; }
				lum[ size_t( y ) * w + x ] = l;
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

static bool RenderOnce( const std::string& path, double& mean, unsigned& nonFinite, std::vector<double>& lum )
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
			lum = pCap->lum;
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

//-----------------------------------------------------------------------------
// Closed-form sphere geometry.  Film 32x32, pinhole at (0,0,5) looking down
// -z, vertical fov 40 deg (full angle), unit sphere at the origin.  Pixel
// (x, row y) covers screen [x, x+1) x [H-1-y, H-y) (ICamera.h
// RasterConvention), i.e. row 0 at the top.  For each pixel: the fraction of
// 8x8 sub-pixel rays that hit the sphere, and the mean of (1 - n.z) / 4 over
// those hits.
//-----------------------------------------------------------------------------
static const int kW = 32, kH = 32;
struct PixelGeom { double coverage; double k; };

static std::vector<PixelGeom> SphereGeometry()
{
	std::vector<PixelGeom> g( size_t( kW ) * kH );
	const double t = std::tan( 20.0 * 3.14159265358979323846 / 180.0 );
	const int N = 8;
	for( int y = 0; y < kH; ++y ) {
		for( int x = 0; x < kW; ++x ) {
			int hits = 0; double k = 0;
			for( int j = 0; j < N; ++j ) {
				for( int i = 0; i < N; ++i ) {
					const double sx = 2.0 * ( x + ( i + 0.5 ) / N ) / kW - 1.0;
					const double sy = 1.0 - 2.0 * ( y + ( j + 0.5 ) / N ) / kH;
					double dx = sx * t, dy = sy * t, dz = -1.0;
					const double inv = 1.0 / std::sqrt( dx * dx + dy * dy + dz * dz );
					dx *= inv; dy *= inv; dz *= inv;
					// origin o = (0,0,5): |o + s d|^2 = 1
					const double b = 5.0 * dz;
					const double disc = b * b - ( 25.0 - 1.0 );
					if( disc < 0 ) continue;
					const double s0 = -b - std::sqrt( disc );
					const double nz = 5.0 + s0 * dz;
					++hits;
					k += 0.25 * ( 1.0 - nz );
				}
			}
			g[ size_t( y ) * kW + x ] = { hits / double( N * N ), hits ? k / hits : 0.0 };
		}
	}
	return g;
}

struct Stat {
	bool ok; unsigned nonFinite; double mean, sd;
	double sphere, sphereSd;		// sphere-ROI mean / closed form, over the repeats
	bool geomOk;					// rendered backdrop/sphere masks match the analytic geometry
};

static Stat Measure( const std::string& chunk, const std::string& mltChunk, double scale, const std::string& tag, unsigned saltBase, double backdropTol )
{
	static const std::vector<PixelGeom> geom = SphereGeometry();
	const int n = 4;
	Stat s{ true, 0, 0, 0, 0, 0, true };
	double sum = 0, sumSq = 0, rs = 0, rsSq = 0;
	for( int i = 0; i < n; ++i ) {
		std::string r = chunk;
		if( !mltChunk.empty() ) {
			char c[64];
			std::snprintf( c, sizeof( c ), "\tchains %d\n", 448 + 64 * i );
			r = mltChunk + c + "}\n\n";
		}
		const std::string path = WriteScene( Scene( r, scale ), tag );
		SobolSamplerTestHooks::ValueSalt().store( SobolSequence::HashCombine( saltBase, unsigned( i ) ) );
		double m = 0; unsigned nf = 0; std::vector<double> lum;
		s.ok = RenderOnce( path, m, nf, lum ) && s.ok;
		std::remove( path.c_str() );
		s.nonFinite += nf;
		sum += m; sumSq += m * m;

		// Sphere ROI vs closed form.
		double lb = 0; int nb = 0;
		for( size_t p = 0; p < geom.size() && p < lum.size(); ++p ) {
			if( geom[p].coverage == 0.0 ) { lb += lum[p]; ++nb; }
		}
		lb = nb ? lb / nb : 0;
		double sphereSum = 0, cfSum = 0;
		for( size_t p = 0; p < geom.size() && p < lum.size(); ++p ) {
			if( geom[p].coverage == 0.0 && std::fabs( lum[p] - lb ) > backdropTol * lb ) s.geomOk = false;
			if( geom[p].coverage == 1.0 ) {
				if( lum[p] > 0.6 * lb ) s.geomOk = false;
				sphereSum += lum[p];
				cfSum += lb * geom[p].k;
			}
		}
		const double ratio = cfSum > 0 ? sphereSum / cfSum : 0;
		rs += ratio; rsSq += ratio * ratio;
	}
	SobolSamplerTestHooks::ValueSalt().store( 0u );
	s.mean = sum / n;
	s.sd = std::sqrt( std::max( 0.0, ( sumSq - n * s.mean * s.mean ) / ( n - 1 ) ) );
	s.sphere = rs / n;
	s.sphereSd = std::sqrt( std::max( 0.0, ( rsSq - n * s.sphere * s.sphere ) / ( n - 1 ) ) );
	std::printf( "  %-14s scale %.0f  mean %.6f  sd %.6f  sphere/closed-form %.5f  sd %.5f  nonFinite %u\n",
		tag.c_str(), scale, s.mean, s.sd, s.sphere, s.sphereSd, s.nonFinite );
	return s;
}

static const std::string kCommon = "\tpixel_filter box\n\toidn_denoise FALSE\n\tshow_luminaires TRUE\n";
static const std::string kSpectral = "\tnum_wavelengths 160\n";

//! backdropTol: how far a fully-backdrop pixel may sit from the backdrop
//! mean.  The pel PT/BDPT/VCM camera hits are exact (1 %); spectral pixels
//! carry wavelength noise and MLT pixels its mutation noise (20 %).  The
//! geometry check only has to separate backdrop (Lb) from sphere (< 0.5 Lb).
struct Integrator { const char* tag; std::string chunk; std::string mltChunk; double backdropTol; };

//! Agreement within 3 sigma of the difference of two 4-repeat means plus a
//! relative floor.
static void CheckAgree( const std::string& what, double a, double sa, double b, double sb, double floorRel )
{
	const double sigma = std::sqrt( sa * sa + sb * sb ) / 2.0;
	const double tol = 3.0 * sigma + floorRel * std::fabs( a );
	char msg[256];
	std::snprintf( msg, sizeof( msg ), "%s: %.5f vs %.5f (|diff| %.5f <= %.5f)",
		what.c_str(), a, b, std::fabs( a - b ), tol );
	Check( std::fabs( a - b ) <= tol, msg );
}

int main()
{
	const std::string spp = "\tsamples 1024\n";
	std::vector<Integrator> ints = {
		{ "pt_pel",        "pathtracing_pel_rasterizer\n{\n" + spp + kCommon + "}\n\n", "", 0.01 },
		{ "bdpt_pel",      "bdpt_pel_rasterizer\n{\n" + spp + kCommon + "}\n\n", "", 0.01 },
		{ "vcm_pel",       "vcm_pel_rasterizer\n{\n" + spp + kCommon + "}\n\n", "", 0.01 },
		{ "mlt_pel",       "", "mlt_rasterizer\n{\n\tmutations_per_pixel 1024\n\tbootstrap_samples 20000\n\tshow_luminaires TRUE\n\toidn_denoise FALSE\n\tpixel_filter box\n", 0.20 },
		{ "pt_spectral",   "pathtracing_spectral_rasterizer\n{\n" + spp + kCommon + kSpectral + "}\n\n", "", 0.20 },
		{ "bdpt_spectral", "bdpt_spectral_rasterizer\n{\n" + spp + kCommon + kSpectral + "}\n\n", "", 0.20 },
	};

	std::vector<Stat> s1, s2;
	unsigned salt = 0x311u;
	for( const Integrator& in : ints ) {
		s1.push_back( Measure( in.chunk, in.mltChunk, 1.0, in.tag, salt++, in.backdropTol ) );
		s2.push_back( Measure( in.chunk, in.mltChunk, 2.0, in.tag, salt++, in.backdropTol ) );
	}

	for( size_t i = 0; i < ints.size(); ++i ) {
		const std::string t = ints[i].tag;
		Check( s1[i].ok && s2[i].ok, t + ": rendered" );
		Check( s1[i].nonFinite == 0 && s2[i].nonFinite == 0, t + ": no non-finite pixels at scale 1 and 2" );
		Check( s1[i].mean > 0.1, t + ": scale-1 image is lit (not black)" );
		Check( s1[i].geomOk && s2[i].geomOk, t + ": rendered backdrop / sphere masks match the analytic camera geometry" );
		// 3 sigma of the ratio (sd of each 4-repeat mean is sd/2) plus a
		// 0.5 % floor.  MLT's repeats differ only in `chains`, and each
		// configuration is deterministic, so its scale-2/scale-1 ratio is
		// exact to rounding.
		const double ratio = s1[i].mean > 0 ? s2[i].mean / s1[i].mean : 0;
		const double rel = ( s1[i].mean > 0 && s2[i].mean > 0 )
			? 0.5 * std::sqrt( std::pow( s1[i].sd / s1[i].mean, 2 ) + std::pow( s2[i].sd / s2[i].mean, 2 ) ) : 0;
		const double band = 3.0 * rel + 0.005;
		char msg[128];
		std::snprintf( msg, sizeof( msg ), "%s: scale 2 / scale 1 = %.5f (expected 2 +/- %.2f%%)", t.c_str(), ratio, band * 100 );
		Check( std::fabs( ratio - 2.0 ) <= 2.0 * band, msg );

		// Sphere ROI vs the closed form (scale 2).  Floor 0.5 % for the
		// unbiased integrators (at 1024 spp x 4 repeats the 3-sigma term is
		// ~1 %, so a ~3 % weight error on BSDF-sampled plane hits fails); MLT carries its bootstrap normalization and
		// the start-up bias of 448-640 chains on a 32x32 film, so it gets
		// its measured 4-repeat spread plus 3 %.
		CheckAgree( t + ": sphere ROI / closed form (scale 2) is 1", s2[i].sphere, s2[i].sphereSd, 1.0, 0.0,
			ints[i].mltChunk.empty() ? 0.005 : 0.03 );
	}

	// Cross-integrator agreement (scale 2): whole image and sphere ROI.
	CheckAgree( "PT pel / BDPT pel whole image",           s2[0].mean, s2[0].sd, s2[1].mean, s2[1].sd, 0.01 );
	CheckAgree( "PT pel / VCM pel whole image",            s2[0].mean, s2[0].sd, s2[2].mean, s2[2].sd, 0.01 );
	CheckAgree( "PT pel / MLT pel whole image",            s2[0].mean, s2[0].sd, s2[3].mean, s2[3].sd, 0.01 );
	CheckAgree( "PT spectral / BDPT spectral whole image", s2[4].mean, s2[4].sd, s2[5].mean, s2[5].sd, 0.01 );
	CheckAgree( "PT pel / BDPT pel sphere",                s2[0].sphere, s2[0].sphereSd, s2[1].sphere, s2[1].sphereSd, 0.005 );
	CheckAgree( "PT pel / VCM pel sphere",                 s2[0].sphere, s2[0].sphereSd, s2[2].sphere, s2[2].sphereSd, 0.005 );
	CheckAgree( "PT spectral / BDPT spectral sphere",      s2[4].sphere, s2[4].sphereSd, s2[5].sphere, s2[5].sphereSd, 0.005 );

	if( g_failures == 0 ) { std::cout << "All InfinitePlaneLuminaire tests passed.\n"; return 0; }
	std::cout << g_failures << " InfinitePlaneLuminaire check(s) FAILED.\n";
	return 1;
}
