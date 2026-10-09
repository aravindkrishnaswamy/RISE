//////////////////////////////////////////////////////////////////////
//
//  MotionBlurTimeAverageTest.cpp - DL-457: a motion-blurred frame is the
//    time-average of static renders over the shutter interval.
//
//    For each scene the frame is rendered with camera `exposure` E (every
//    pixel sample draws its own time in the shutter, which is CENTRED on
//    the frame time: [t - E/2, t + E/2]) and compared with the mean of K
//    static renders (exposure 0) at the midpoints of K equal sub-intervals
//    of that same shutter.  Frame time 0.5 and E = 1 put the shutter on
//    the timelines' [0, 1] keyframe span, so no sample is clamped to an
//    end key.  (DL-457 was filed comparing a frame at time 0 -- shutter
//    [-0.5, 0.5] -- with statics over [0, 1].)
//
//      a  a luminary sphere whose scale animates 1 -> 0.5
//      b  a black occluder that GROWS 0.1 -> 0.9 past its frame-time
//         bounds, against a white environment
//      c  a moving omni light
//      e  a black occluder moving OUT of its frame-time bounds
//      f  the same occluder carried by an animated (geometry-less) parent
//      d  a moving luminary sphere
//
//    b, d, e, f add forty out-of-view spheres so a top-level BVH is built
//    and the moving object gets its own leaf: a stale leaf box only loses
//    hits for rays that miss it entirely, which a scene-wide floor sharing
//    the leaf would hide (the reason b, e and f have no floor).  Red on the
//    base for b and e (stale nominal-time TLAS: +2 % / +1.7 % bright) and
//    f (child frozen at the frame pose: -1.7 %).
//
//    DL-463 rows (VCM always included):
//      v  a glass ball lens sweeping under a small luminary; the caustic is
//         reached only by vertex merging (VCM, merge_radius forced).  Red on
//         the base (light store traced at the frame time while eye samples
//         moved: -4.4 %, t -19.5).
//      s  a spot light pointing up at the frame time and as authored, down
//         at the shutter ends: a light BVH built from either culls it.  Red
//         on the base through the PT-vs-BDPT cross-check (0.0094 vs 0.0329).
//      p  an omni light dark as authored and at the frame time, lit at the
//         shutter ends: a light table built at attach dropped it from every
//         frame, statics included.  Red on the base through the absolute
//         floor on the time-average (0.0075, dim light only).
//      q  the same light authored lit (a frame-time-only table drops it).
//      m  two moving luminaries under the light BVH (a guard).
//
//    PT and BDPT (VCM too with DL457_VCM set) unless a row names its own.  The gate is 3 combined
//    standard errors (8 salted renders for the blurred frame; 4 per static
//    time for the reference, 2 per time where 32 times keep the time
//    quadrature below the noise).
//
//    Usage: MotionBlurTimeAverageTest [seed] [all|<row letters>|perf|perf2] [spp] [repeats]
//
//  Author: Claude (debt-dl457)
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <iostream>
#include <fstream>
#include <vector>
#include <cmath>
#include <string>
#include <filesystem>
#include <random>
#include <chrono>

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

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

static unsigned int g_seedBase = 4570u;
static unsigned int g_renderIndex = 0;
static int passCount = 0;
static int failCount = 0;
static int g_spp = 256;
static int g_repeats = 4;			//!< static renders per reference time (2 where 32 times)
static int g_blurRepeats = 16;		//!< blurred renders per frame
static int g_comparisons = 1;		//!< family size for the Bonferroni correction
static const double kFamilyAlpha = 0.01;

//! Regularized incomplete beta I_x(a, b) (Numerical Recipes' continued fraction).
static double BetaCF( const double a, const double b, const double x )
{
	const double tiny = 1e-300;
	double c = 1, d = 1 - ( a + b ) * x / ( a + 1 );
	if( std::fabs( d ) < tiny ) d = tiny;
	d = 1 / d;
	double h = d;
	for( int m = 1; m <= 300; m++ ) {
		const int m2 = 2 * m;
		double aa = m * ( b - m ) * x / ( ( a + m2 - 1 ) * ( a + m2 ) );
		d = 1 + aa * d; if( std::fabs( d ) < tiny ) d = tiny;
		c = 1 + aa / c; if( std::fabs( c ) < tiny ) c = tiny;
		d = 1 / d; h *= d * c;
		aa = -( a + m ) * ( a + b + m ) * x / ( ( a + m2 ) * ( a + m2 + 1 ) );
		d = 1 + aa * d; if( std::fabs( d ) < tiny ) d = tiny;
		c = 1 + aa / c; if( std::fabs( c ) < tiny ) c = tiny;
		d = 1 / d;
		const double del = d * c;
		h *= del;
		if( std::fabs( del - 1 ) < 1e-14 ) break;
	}
	return h;
}
static double IncompleteBeta( const double a, const double b, const double x )
{
	if( x <= 0 ) return 0;
	if( x >= 1 ) return 1;
	const double bt = std::exp( std::lgamma( a + b ) - std::lgamma( a ) - std::lgamma( b ) + a * std::log( x ) + b * std::log( 1 - x ) );
	return x < ( a + 1 ) / ( a + b + 2 ) ? bt * BetaCF( a, b, x ) / a : 1 - bt * BetaCF( b, a, 1 - x ) / b;
}
//! Two-sided p-value of Student's t at `dof` degrees of freedom.
static double TwoSidedP( const double t, const double dof )
{
	return IncompleteBeta( 0.5 * dof, 0.5, dof / ( dof + t * t ) );
}
//! The two-sided critical |t| at level `alpha` (bisection on TwoSidedP).
static double CriticalT( const double alpha, const double dof )
{
	double lo = 0, hi = 1000;
	for( int i = 0; i < 200; i++ ) {
		const double mid = 0.5 * ( lo + hi );
		if( TwoSidedP( mid, dof ) > alpha ) lo = mid; else hi = mid;
	}
	return 0.5 * ( lo + hi );
}

static void Check( bool condition, const std::string& testName )
{
	if( condition ) {
		passCount++;
	} else {
		failCount++;
		std::cout << "  FAIL: " << testName << std::endl;
	}
}

static std::string Fmt( const char* f, ... )
{
	char buf[8192];
	va_list ap;
	va_start( ap, f );
	std::vsnprintf( buf, sizeof(buf), f, ap );
	va_end( ap );
	return std::string( buf );
}

class CapturingRasterizerOutput : public virtual IRasterizerOutput, public virtual Reference
{
public:
	std::vector<RISEColor> pixels;
protected:
	virtual ~CapturingRasterizerOutput() {}
public:
	virtual void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}
	virtual void OutputImage( const IRasterImage& img, const Rect*, const unsigned int ) override
	{
		pixels.resize( img.GetWidth() * img.GetHeight() );
		for( unsigned int y = 0; y < img.GetHeight(); y++ )
			for( unsigned int x = 0; x < img.GetWidth(); x++ )
				pixels[y * img.GetWidth() + x] = img.GetPEL( x, y );
	}
};

//! Renders the single frame at `frameTime` through RasterizeAnimation.
static bool RenderOnce( const std::string& sceneText, const double frameTime, double& mean )
{
	static const std::string path = ( std::filesystem::temp_directory_path() /
		Fmt( "dl457_render_%08x.RISEscene", static_cast<unsigned int>( std::random_device()() ) ) ).string();
	{ std::ofstream ofs( path ); ofs << sceneText; }
	bool ok = false;
	IJobPriv* pJob = nullptr;
	if( RISE_CreateJobPriv( &pJob ) && pJob ) {
		if( pJob->LoadAsciiSceneViaCst( path.c_str() ) ) {
			pJob->RemoveRasterizerOutputs();
			CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
			GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "capture" );
			pJob->GetRasterizer()->AddRasterizerOutput( pCap );
			SobolSamplerTestHooks::ValueSalt().store( SobolSequence::HashCombine( g_seedBase + g_renderIndex, 0x457u ) );
			std::srand( g_seedBase + g_renderIndex++ );
			const bool bRendered = pJob->RasterizeAnimation( frameTime, frameTime + 1.0, 1, false, false );
			SobolSamplerTestHooks::ValueSalt().store( 0u );
			if( bRendered && !pCap->pixels.empty() ) {
				double sum = 0; ok = true;
				for( const RISEColor& c : pCap->pixels ) {
					const double v = ( c.base.r + c.base.g + c.base.b ) * c.a / 3.0;
					if( !std::isfinite( v ) ) { ok = false; break; }
					sum += v;
				}
				mean = sum / double( pCap->pixels.size() );
			}
			safe_release( pCap );
		}
		safe_release( pJob );
	}
	std::remove( path.c_str() );
	return ok;
}

struct Stat { double mean, se; bool ok; };

static Stat RenderN( const std::string& text, const double frameTime, const int n )
{
	Stat s{ 0, 0, true };
	std::vector<double> v;
	for( int i = 0; i < n; i++ ) {
		double m = 0;
		if( !RenderOnce( text, frameTime, m ) ) { s.ok = false; return s; }
		v.push_back( m );
	}
	for( double x : v ) s.mean += x;
	s.mean /= n;
	double ss = 0;
	for( double x : v ) ss += ( x - s.mean ) * ( x - s.mean );
	s.se = std::sqrt( ss / ( n - 1 ) / n );
	return s;
}

//! Orthographic camera at z = 1 looking down at a floor (rho 0.5, z = 0,
//! +-2.5) when `floor` is set.  `body` adds the emitters / occluders /
//! timelines.
static std::string Scene( const std::string& body, const std::string& rasterizer, const double exposure, const bool floor )
{
	return std::string( "RISE ASCII SCENE 7\n\n" ) +
		"film\n{\n\twidth 24\n\theight 24\n}\n\n"
		+ Fmt( "orthographic_camera\n{\n\tlocation 0 0 1\n\tlookat 0 0 0\n\tup 0 1 0\n\tviewport_scale 4 4\n\texposure %g\n}\n\n", exposure ) +
		"uniformcolor_painter\n{\n\tname white\n\tcolor 1 1 1\n}\n\n"
		"uniformcolor_painter\n{\n\tname grey\n\tcolor 0.5 0.5 0.5\n}\n\n"
		"lambertian_material\n{\n\tname floor_mat\n\treflectance grey\n}\n\n"
		"uniformcolor_painter\n{\n\tname black\n\tcolor 0 0 0\n}\n\n"
		"lambertian_material\n{\n\tname black_mat\n\treflectance black\n}\n\n"
		"lambertian_luminaire_material\n{\n\tname lum\n\texitance white\n\tscale 10\n\tmaterial none\n}\n\n"
		"clippedplane_geometry\n{\n\tname geo_floor\n\tpta -2.5 -2.5 0\n\tptb 2.5 -2.5 0\n\tptc 2.5 2.5 0\n\tptd -2.5 2.5 0\n\tdoublesided FALSE\n}\n\n"
		+ std::string( floor ? "standard_object\n{\n\tname floor\n\tgeometry geo_floor\n\tmaterial floor_mat\n}\n\n" : "" ) +
		"sphere_geometry\n{\n\tname geo_unit\n\tradius 1\n}\n\n"
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		+ body + rasterizer;
}

static std::string Sphere( const char* name, const char* material, const char* position, const char* scale )
{
	return Fmt( "standard_object\n{\n\tname %s\n\tgeometry geo_unit\n\tmaterial %s\n\tposition %s\n\tscale %s\n}\n\n", name, material, position, scale );
}

static std::string Timeline( const char* type, const char* element, const char* param, const char* v0, const char* v1 )
{
	return Fmt( "timeline\n{\n\telement_type %s\n\telement %s\n\tparam %s\n\tinterpolator linear\n\ttime 0\n\tvalue %s\n\ttime 1\n\tvalue %s\n}\n\n",
		type, element, param, v0, v1 );
}

//! Three keys at t = 0, 0.5, 1: the frame time (0.5) sits on the middle one.
static std::string Timeline3( const char* type, const char* element, const char* param, const char* v0, const char* vMid, const char* v1 )
{
	return Fmt( "timeline\n{\n\telement_type %s\n\telement %s\n\tparam %s\n\tinterpolator linear\n\ttime 0\n\tvalue %s\n\ttime 0.5\n\tvalue %s\n\ttime 1\n\tvalue %s\n}\n\n",
		type, element, param, v0, vMid, v1 );
}

//! Forty small spheres out of view: they push the object count past
//! ObjectManager's linear-loop threshold (4) so a top-level BVH is built,
//! and keep the moving object out of their leaves.
static std::string Padding()
{
	std::string s;
	for( int i = 0; i < 40; i++ ) {
		const std::string n = Fmt( "pad%d", i );
		const std::string pos = Fmt( "%g %g -5", -40.0 + 2.0 * ( i % 8 ), -40.0 + 2.0 * ( i / 8 ) );
		s += Sphere( n.c_str(), "floor_mat", pos.c_str(), "0.5 0.5 0.5" );
	}
	return s;
}

//! A white environment seen directly (radiance_background), so a black
//! occluder in front of it is visible without a floor whose scene-wide box
//! would share the occluder's BVH leaf (any ray through that leaf box
//! tests the occluder at its live position, masking a stale box).
static std::string Env( const bool env ) { return env ? "\tradiance_map white\n\tradiance_scale 1.0\n\tradiance_background TRUE\n" : ""; }
static std::string RasPT( const bool env ) { return "pathtracing_pel_rasterizer\n{\n\tsamples " + std::to_string( g_spp ) + "\n\toidn_denoise FALSE\n\tpixel_filter box\n" + Env( env ) + "}\n\n"; }
static std::string RasBDPT( const bool env ) { return "bdpt_pel_rasterizer\n{\n\tmax_eye_depth 4\n\tmax_light_depth 4\n\tsamples " + std::to_string( g_spp ) + "\n\toidn_denoise FALSE\n\tpixel_filter box\n" + Env( env ) + "}\n\n"; }
//! `mergeRadius` > 0 forces vertex merging on (DL-463 a) with the same
//! radius schedule for the blurred frame and every static reference.
static std::string RasVCM( const bool env, const double mergeRadius ) { return "vcm_pel_rasterizer\n{\n\tmax_eye_depth 4\n\tmax_light_depth 4\n\tsamples " + std::to_string( g_spp ) + "\n\toidn_denoise FALSE\n\tpixel_filter box\n" + ( mergeRadius > 0 ? Fmt( "\tmerge_radius %g\n", mergeRadius ) : std::string() ) + Env( env ) + "}\n\n"; }

enum { kPT = 1, kBDPT = 2, kVCM = 4 };

//! `floor` FALSE replaces the floor by a white environment background.
static int IntegratorMask( const int mask )
{
	return mask ? mask : ( kPT | kBDPT | ( std::getenv( "DL457_VCM" ) ? kVCM : 0 ) );
}

static void RunCase( const char* label, const std::string& body, const bool floor = true, const int times = 8, const int mask = 0, const double mergeRadius = 0,
	const bool crossCheck = false, const double refFloor = 0 )
{
	std::vector<std::pair<const char*, Stat> > blurred;
	const double kFrame = 0.5, kExposure = 1.0;
	struct Integrator { const char* name; std::string ras; };
	std::vector<Integrator> integrators;
	const int m = IntegratorMask( mask );
	if( m & kPT ) integrators.push_back( { "PT", RasPT( !floor ) } );
	if( m & kBDPT ) integrators.push_back( { "BDPT", RasBDPT( !floor ) } );
	if( m & kVCM ) integrators.push_back( { "VCM", RasVCM( !floor, mergeRadius ) } );
	for( const auto& ig : integrators ) {
		const Stat blur = RenderN( Scene( body, ig.ras, kExposure, floor ), kFrame, g_blurRepeats );
		const int refRepeats = times >= 32 ? 2 : g_repeats;
		double refMean = 0, refVar = 0, refDofDen = 0;
		bool ok = blur.ok;
		for( int k = 0; k < times && ok; k++ ) {
			const double t = kFrame - 0.5 * kExposure + ( k + 0.5 ) * kExposure / times;
			const Stat s = RenderN( Scene( body, ig.ras, 0.0, floor ), t, refRepeats );
			ok = ok && s.ok;
			refMean += s.mean / times;
			const double v = s.se * s.se / ( double( times ) * times );
			refVar += v;
			refDofDen += v * v / ( refRepeats - 1 );
		}
		// Welch-Satterthwaite: the reference variance is a sum of `times`
		// independent per-time variance estimates.
		const double vb = blur.se * blur.se;
		const double var = vb + refVar;
		const double dofDen = vb * vb / ( g_blurRepeats - 1 ) + refDofDen;
		const double dof = dofDen > 0 ? var * var / dofDen : 1e6;
		const double se = std::sqrt( var );
		const double z = se > 0 ? ( blur.mean - refMean ) / se : 0;
		const double crit = CriticalT( kFamilyAlpha / g_comparisons, dof );
		std::printf( "  %-4s %-48s blurred %.6f +/- %.6f  time-average %.6f +/- %.6f  ratio %.4f  t %+.2f (dof %.0f, crit %.2f)\n",
			ig.name, label, blur.mean, blur.se, refMean, std::sqrt( refVar ), refMean > 0 ? blur.mean / refMean : 0.0, z, dof, crit );
		Check( ok && blur.mean > 0, std::string( "DL-457 renders complete: " ) + ig.name + " " + label );
		Check( ok && std::fabs( blur.mean - refMean ) <= crit * se + 1e-12,
			std::string( "DL-457 motion blur = time average of static renders: " ) + ig.name + " " + label );
		if( refFloor > 0 ) {
			// DL-463: a light dark at the frame time (and as authored) but lit
			// during the shutter must reach the statics too (a light table
			// built once, at attach, dropped it from every frame).
			Check( ok && refMean > refFloor,
				Fmt( "DL-463 time-average sees the light lit during the shutter (%.6f > %.6f): %s %s", refMean, refFloor, ig.name, label ) );
		}
		blurred.push_back( std::make_pair( ig.name, blur ) );
	}
	if( crossCheck ) {
		// DL-463: integrators agree on the blurred frame (a reference-free
		// check: the light BVH drives PT's NEE only, so a culled light
		// shows up as PT against BDPT / VCM).
		for( std::size_t i = 1; i < blurred.size(); i++ ) {
			const Stat& a = blurred[0].second;
			const Stat& b = blurred[i].second;
			const double va = a.se * a.se, vb = b.se * b.se;
			const double se = std::sqrt( va + vb );
			const double dofDen = va * va / ( g_blurRepeats - 1 ) + vb * vb / ( g_blurRepeats - 1 );
			const double dof = dofDen > 0 ? ( va + vb ) * ( va + vb ) / dofDen : 1e6;
			const double z = se > 0 ? ( a.mean - b.mean ) / se : 0;
			const double crit = CriticalT( kFamilyAlpha / g_comparisons, dof );
			std::printf( "  %s vs %s %-44s %.6f vs %.6f  ratio %.4f  t %+.2f (crit %.2f)\n", blurred[0].first, blurred[i].first, label,
				a.mean, b.mean, b.mean > 0 ? a.mean / b.mean : 0.0, z, crit );
			Check( a.ok && b.ok && std::fabs( a.mean - b.mean ) <= crit * se + 1e-12,
				std::string( "DL-463 blurred frame agrees across integrators: " ) + blurred[0].first + " vs " + blurred[i].first + " " + label );
		}
	}
}

struct Case
{
	const char* key;
	const char* label;
	std::string body;
	bool floor;
	int times;
	int mask;				//!< kPT | kBDPT | kVCM; 0 = PT + BDPT (+ VCM with DL457_VCM)
	double mergeRadius;		//!< VCM merge radius (> 0 forces VM on)
	bool crossCheck = false;	//!< also compare the integrators' blurred frames
	double refFloor = 0;		//!< > 0: the time-average must exceed this
};

int main( int argc, char** argv )
{
	std::setvbuf( stdout, nullptr, _IONBF, 0 );
	if( argc > 1 ) g_seedBase = unsigned( std::strtoul( argv[1], nullptr, 10 ) );
	const std::string only = argc > 2 ? argv[2] : "all";
	if( argc > 3 ) g_spp = std::atoi( argv[3] );
	if( argc > 4 ) g_blurRepeats = std::atoi( argv[4] );
	std::cout << "MotionBlurTimeAverageTest (DL-457)\n";

	const std::string rodOccluder =
		Sphere( "rod", "black_mat", "0 0 0", "1.5 0.1 0.1" ) +
		Timeline( "object", "rod", "orientation", "0 0 0", "0 0 90" );

	const Case cases[] = {
		{ "a", "a: luminary sphere scale 1 -> 0.5",
			Sphere( "e", "lum", "0.3 0 2.6", "1 1 1" ) +
			Timeline( "object", "e", "scale", "1 1 1", "0.5 0.5 0.5" ), true, 8, 0, 0 },
		{ "b", "b: occluder grows 0.1 -> 0.9 past its bounds",
			Sphere( "occ", "black_mat", "0 0 0", "0.1 0.1 0.1" ) +
			Timeline( "object", "occ", "scale", "0.1 0.1 0.1", "0.9 0.9 0.9" ) + Padding(), false, 32, 0, 0 },
		{ "c", "c: moving omni light",
			"omni_light\n{\n\tname l_omni\n\tpower 20.0\n\tcolor 1.0 1.0 1.0\n\tposition -1.5 0 2\n}\n\n" +
			Timeline( "light", "l_omni", "position", "-1.5 0 2", "1.5 0 2" ), true, 32, 0, 0 },
		{ "e", "e: moving occluder",
			Sphere( "occ", "black_mat", "-3 0 0", "0.5 0.5 0.5" ) +
			Timeline( "object", "occ", "position", "-3 0 0", "0 0 0" ) + Padding(), false, 32, 0, 0 },
		{ "f", "f: occluder carried by a moving parent",
			std::string( "standard_object\n{\n\tname rig\n\tposition -3 0 0\n}\n\n" ) +
			"standard_object\n{\n\tname occ\n\tparent rig\n\tgeometry geo_unit\n\tmaterial black_mat\n\tposition 0 0 0\n\tscale 0.5 0.5 0.5\n}\n\n" +
			Timeline( "object", "rig", "position", "-3 0 0", "0 0 0" ) + Padding(), false, 32, 0, 0 },
		{ "d", "d: moving luminary sphere",
			Sphere( "e", "lum", "-1.5 0 2.6", "0.5 0.5 0.5" ) +
			Timeline( "object", "e", "position", "-1.5 0 2.6", "1.5 0 2.6" ) + Padding(), true, 32, 0, 0 },
		// An infinite plane's box is +-DBL_MAX; any rotation overflows it
		// to inf, and a naive sweep turned inf - inf into a NaN box that
		// BoundingBox::Include ignores, dropping the floor from the TLAS.
		{ "g", "g: rotated infinite-plane floor + moving occluder",
			std::string( "infiniteplane_geometry\n{\n\tname geo_inf\n}\n\n" ) +
			"standard_object\n{\n\tname inffloor\n\tgeometry geo_inf\n\tmaterial floor_mat\n\tposition 0 0 -0.6\n\torientation 0 0 90\n}\n\n" +
			Sphere( "occ", "black_mat", "-3 0 0", "0.5 0.5 0.5" ) +
			Timeline( "object", "occ", "position", "-3 0 0", "0 0 0" ) + Padding(), false, 32, 0, 0 },
		{ "h", "h: rotating rod occluder (0 -> 90 deg)", rodOccluder + Padding(), false, 32, 0, 0 },
		// DL-463 (a): a glass ball lens sweeping across the view, focusing
		// a small overhead luminary onto the floor; the caustic is reached
		// only by vertex merging.  The light store must be traced at the
		// time of the eye samples it is merged with.
		{ "v", "v: VCM caustic of a moving glass ball (VM)",
			std::string( "dielectric_material\n{\n\tname glass\n\ttau 1 1 1\n\tior 1.5\n\tscattering 1000000\n}\n\n" ) +
			Sphere( "ball", "glass", "-6 0 0.55", "0.4 0.4 0.4" ) +
			Timeline( "object", "ball", "position", "-6 0 0.55", "6 0 0.55" ) +
			Sphere( "e", "lum", "0 0 3", "0.15 0.15 0.15" ), true, 32, kVCM, 0.05 },
		// DL-463 (b): a spot light that points UP at the frame time (and as
		// authored) and down at both ends of the shutter.  A light BVH built
		// at the frame time culls it (zero orientation importance) at every
		// floor point below it.  A static omni light makes the BVH (2+ lights).
		{ "s", "s: spot light swinging down/up/down (light BVH)",
			std::string( "spot_light\n{\n\tname spot\n\tposition 0 0 2\n\ttarget 0 1 4\n\tcolor 1 1 1\n\tpower 40\n\tinner 30\n\touter 40\n}\n\n" ) +
			Timeline3( "light", "spot", "target", "0 1 0", "0 1 4", "0 1 0" ) +
			"omni_light\n{\n\tname l_dim\n\tpower 2.0\n\tcolor 1.0 1.0 1.0\n\tposition 3 3 3\n}\n\n", true, 32, kPT | kBDPT | kVCM, 0, true },
		// DL-463 (b): an omni light whose energy is ZERO at the frame time
		// AND as authored, positive at both ends of the shutter.  A light
		// table built once at attach (base) dropped it from every frame,
		// statics included; one built at the frame time alone drops it
		// from the blurred frame.
		{ "p", "p: omni light fading 20 -> 0 -> 20 (authored 0)",
			std::string( "omni_light\n{\n\tname l_fade\n\tpower 0.0\n\tcolor 1.0 1.0 1.0\n\tposition 0.5 0 2\n}\n\n" ) +
			Timeline3( "light", "l_fade", "energy", "20", "0", "20" ) +
			"omni_light\n{\n\tname l_dim\n\tpower 2.0\n\tcolor 1.0 1.0 1.0\n\tposition 3 3 3\n}\n\n", true, 32, kPT | kBDPT | kVCM, 0, false, 0.015 },
		// The same light authored ON: the attach-time table keeps it, a
		// frame-time-only table (no shutter sweep) drops it.
		{ "q", "q: omni light fading 20 -> 0 -> 20 (authored 20)",
			std::string( "omni_light\n{\n\tname l_fade\n\tpower 20.0\n\tcolor 1.0 1.0 1.0\n\tposition 0.5 0 2\n}\n\n" ) +
			Timeline3( "light", "l_fade", "energy", "20", "0", "20" ) +
			"omni_light\n{\n\tname l_dim\n\tpower 2.0\n\tcolor 1.0 1.0 1.0\n\tposition 3 3 3\n}\n\n", true, 32, kPT | kBDPT | kVCM, 0, true, 0.015 },
		// DL-463 (b): two luminaries moving in opposite directions under the
		// light BVH (a guard: mesh luminaries carry full-sphere cones).
		{ "m", "m: two moving luminaries (light BVH)",
			Sphere( "e1", "lum", "-1.5 0 2.6", "0.4 0.4 0.4" ) +
			Timeline( "object", "e1", "position", "-1.5 0 2.6", "1.5 0 2.6" ) +
			Sphere( "e2", "lum", "1.5 1 2.6", "0.3 0.3 0.3" ) +
			Timeline( "object", "e2", "position", "1.5 1 2.6", "-1.5 -1 2.6" ), true, 32, kPT | kBDPT | kVCM, 0, true },
	};

	if( only == "perf2" ) {
		// Not a gate (DL-463 d): the same 400 static links PLUS an animated
		// parented subtree (a moving rig carrying the occluder), so every
		// pixel sample re-composes -- only the animated subtree since
		// DL-463, every link before.  Compare across builds.
		std::string body;
		for( int i = 0; i < 400; i++ ) {
			body += Fmt( "standard_object\n{\n\tname rig%d\n\tposition %g %g -5\n}\n\n", i, -40.0 + 0.2 * ( i % 20 ), -40.0 + 0.2 * ( i / 20 ) );
			body += Fmt( "standard_object\n{\n\tname kid%d\n\tparent rig%d\n\tgeometry geo_unit\n\tmaterial floor_mat\n\tscale 0.05 0.05 0.05\n}\n\n", i, i );
		}
		body += std::string( "standard_object\n{\n\tname mover\n\tposition -3 0 0\n}\n\n" ) +
			"standard_object\n{\n\tname occ\n\tparent mover\n\tgeometry geo_unit\n\tmaterial black_mat\n\tscale 0.5 0.5 0.5\n}\n\n" +
			Timeline( "object", "mover", "position", "-3 0 0", "0 0 0" );
		const std::string text = Scene( body, RasPT( true ), 1.0, false );
		for( int r = 0; r < 3; r++ ) {
			double m = 0;
			const auto t0 = std::chrono::steady_clock::now();
			RenderOnce( text, 0.5, m );
			const double sec = std::chrono::duration<double>( std::chrono::steady_clock::now() - t0 ).count();
			std::printf( "  perf2: blurred frame %.3f s (mean %.6f)\n", sec, m );
		}
		return 0;
	}

	if( only == "perf" ) {
		// Not a gate: wall time of a motion-blurred PT frame whose scene has
		// 400 parent links but no animated parented subtree (only an
		// unrelated sphere moves).  Compare across builds.
		std::string body;
		for( int i = 0; i < 400; i++ ) {
			body += Fmt( "standard_object\n{\n\tname rig%d\n\tposition %g %g -5\n}\n\n", i, -40.0 + 0.2 * ( i % 20 ), -40.0 + 0.2 * ( i / 20 ) );
			body += Fmt( "standard_object\n{\n\tname kid%d\n\tparent rig%d\n\tgeometry geo_unit\n\tmaterial floor_mat\n\tscale 0.05 0.05 0.05\n}\n\n", i, i );
		}
		body += Sphere( "occ", "black_mat", "-3 0 0", "0.5 0.5 0.5" ) + Timeline( "object", "occ", "position", "-3 0 0", "0 0 0" );
		const std::string text = Scene( body, RasPT( true ), 1.0, false );
		for( int r = 0; r < 3; r++ ) {
			double m = 0;
			const auto t0 = std::chrono::steady_clock::now();
			RenderOnce( text, 0.5, m );
			const double sec = std::chrono::duration<double>( std::chrono::steady_clock::now() - t0 ).count();
			std::printf( "  perf: blurred frame %.3f s (mean %.6f)\n", sec, m );
		}
		return 0;
	}

	g_comparisons = 0;
	for( const Case& c : cases ) {
		if( only == "all" || only.find( c.key ) != std::string::npos ) {
			const int m = IntegratorMask( c.mask );
			const int n = ( m & kPT ? 1 : 0 ) + ( m & kBDPT ? 1 : 0 ) + ( m & kVCM ? 1 : 0 );
			g_comparisons += n + ( c.crossCheck ? n - 1 : 0 );
		}
	}
	if( g_comparisons == 0 ) g_comparisons = 1;
	for( const Case& c : cases ) {
		if( only == "all" || only.find( c.key ) != std::string::npos ) RunCase( c.label, c.body, c.floor, c.times, c.mask, c.mergeRadius, c.crossCheck, c.refFloor );
	}
	std::cout << "\n" << passCount << " passed, " << failCount << " failed\n";
	return failCount == 0 ? 0 : 1;
}
