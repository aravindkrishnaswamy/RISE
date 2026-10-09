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
//    PT and BDPT (VCM too with DL457_VCM set).  The gate is 3 combined
//    standard errors (8 salted renders for the blurred frame; 4 per static
//    time for the reference, 2 per time where 32 times keep the time
//    quadrature below the noise).
//
//    Usage: MotionBlurTimeAverageTest [seed] [a|b|c|d|e|f|all] [spp] [repeats]
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
#include <unistd.h>

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
static int g_repeats = 4;

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
	const std::string path = Fmt( "/tmp/dl457_render_%d.RISEscene", static_cast<int>( ::getpid() ) );
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

//! `floor` FALSE replaces the floor by a white environment background.
static void RunCase( const char* label, const std::string& body, const bool floor = true, const int times = 8 )
{
	const double kFrame = 0.5, kExposure = 1.0;
	struct Integrator { const char* name; std::string ras; };
	std::vector<Integrator> integrators = { { "PT", RasPT( !floor ) }, { "BDPT", RasBDPT( !floor ) } };
	if( std::getenv( "DL457_VCM" ) ) {
		integrators.push_back( { "VCM", "vcm_pel_rasterizer\n{\n\tmax_eye_depth 4\n\tmax_light_depth 4\n\tsamples " + std::to_string( g_spp ) + "\n\toidn_denoise FALSE\n\tpixel_filter box\n" + Env( !floor ) + "}\n\n" } );
	}
	for( const auto& ig : integrators ) {
		const Stat blur = RenderN( Scene( body, ig.ras, kExposure, floor ), kFrame, 2 * g_repeats );
		double refMean = 0, refVar = 0;
		bool ok = blur.ok;
		for( int k = 0; k < times && ok; k++ ) {
			const double t = kFrame - 0.5 * kExposure + ( k + 0.5 ) * kExposure / times;
			const Stat s = RenderN( Scene( body, ig.ras, 0.0, floor ), t, times >= 32 ? 2 : g_repeats );
			ok = ok && s.ok;
			refMean += s.mean / times;
			refVar += s.se * s.se / ( double( times ) * times );
		}
		const double se = std::sqrt( blur.se * blur.se + refVar );
		const double z = se > 0 ? ( blur.mean - refMean ) / se : 0;
		std::printf( "  %-4s %-48s blurred %.6f +/- %.6f  time-average %.6f +/- %.6f  ratio %.4f  z %+.2f\n",
			ig.name, label, blur.mean, blur.se, refMean, std::sqrt( refVar ), refMean > 0 ? blur.mean / refMean : 0.0, z );
		Check( ok && blur.mean > 0, std::string( "DL-457 renders complete: " ) + ig.name + " " + label );
		Check( ok && std::fabs( blur.mean - refMean ) <= 3 * se + 1e-12,
			std::string( "DL-457 motion blur = time average of static renders: " ) + ig.name + " " + label );
	}
}

int main( int argc, char** argv )
{
	std::setvbuf( stdout, nullptr, _IONBF, 0 );
	if( argc > 1 ) g_seedBase = unsigned( std::strtoul( argv[1], nullptr, 10 ) );
	const std::string only = argc > 2 ? argv[2] : "all";
	if( argc > 3 ) g_spp = std::atoi( argv[3] );
	if( argc > 4 ) g_repeats = std::atoi( argv[4] );
	std::cout << "MotionBlurTimeAverageTest (DL-457)\n";

	if( only == "all" || only == "a" ) {
		RunCase( "a: luminary sphere scale 1 -> 0.5",
			Sphere( "e", "lum", "0.3 0 2.6", "1 1 1" ) +
			Timeline( "object", "e", "scale", "1 1 1", "0.5 0.5 0.5" ) );
	}
	if( only == "all" || only == "b" ) {
		RunCase( "b: occluder grows 0.1 -> 0.9 past its bounds",
			Sphere( "occ", "black_mat", "0 0 0", "0.1 0.1 0.1" ) +
			Timeline( "object", "occ", "scale", "0.1 0.1 0.1", "0.9 0.9 0.9" ) + Padding(), false, 32 );
	}
	if( only == "all" || only == "c" ) {
		RunCase( "c: moving omni light",
			"omni_light\n{\n\tname l_omni\n\tpower 20.0\n\tcolor 1.0 1.0 1.0\n\tposition -1.5 0 2\n}\n\n" +
			Timeline( "light", "l_omni", "position", "-1.5 0 2", "1.5 0 2" ), true, 32 );
	}
	if( only == "all" || only == "e" ) {
		RunCase( "e: moving occluder",
			Sphere( "occ", "black_mat", "-3 0 0", "0.5 0.5 0.5" ) +
			Timeline( "object", "occ", "position", "-3 0 0", "0 0 0" ) + Padding(), false, 32 );
	}
	if( only == "all" || only == "f" ) {
		RunCase( "f: occluder carried by a moving parent",
			std::string( "standard_object\n{\n\tname rig\n\tposition -3 0 0\n}\n\n" ) +
			"standard_object\n{\n\tname occ\n\tparent rig\n\tgeometry geo_unit\n\tmaterial black_mat\n\tposition 0 0 0\n\tscale 0.5 0.5 0.5\n}\n\n" +
			Timeline( "object", "rig", "position", "-3 0 0", "0 0 0" ) + Padding(), false, 32 );
	}
	if( only == "all" || only == "d" ) {
		RunCase( "d: moving luminary sphere",
			Sphere( "e", "lum", "-1.5 0 2.6", "0.5 0.5 0.5" ) +
			Timeline( "object", "e", "position", "-1.5 0 2.6", "1.5 0 2.6" ) + Padding(), true, 32 );
	}
	std::cout << "\n" << passCount << " passed, " << failCount << " failed\n";
	return failCount == 0 ? 0 : 1;
}
