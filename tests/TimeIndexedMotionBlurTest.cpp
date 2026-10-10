//////////////////////////////////////////////////////////////////////
//
//  TimeIndexedMotionBlurTest.cpp - DL-465: a motion-blurred animation
//    frame rendered MULTI-threaded through per-thread time-indexed scene
//    state (Rendering/TimeIndexedMotionBlur.h) must be the same estimator
//    as the historical single-threaded path that moved the shared scene to
//    every pixel sample's own shutter time.
//
//    Scene "combo" moves everything a sample can see: the camera (pinhole,
//    location keyframed), a luminary sphere (NEE and emitter hits), a black
//    occluder, a geometry-less parent rotating a child (the per-sample
//    hierarchy re-compose), an omni light (position) and a spot light
//    (target), plus forty out-of-view spheres so a top-level BVH is built.
//
//    A  EXACT, one render thread: time-indexed clones (TI) vs the shared-
//       scene path (OLD), same salt -> the same random draws in the same
//       order -> pixel hashes must be identical.  OLD is first checked
//       against itself.
//    B  STATISTICAL, all threads: TI multi-threaded vs OLD (single-
//       threaded by construction), n salted renders each; whole-image mean
//       and 4x4 region means compared by Welch t at a Bonferroni family
//       level of 0.01.  Every TI render must report a time-indexed frame.
//    C  FALLBACK: a keyframed painter cannot be time-indexed; the frame
//       must take the old path (no time-indexed frame) and still render.
//    D  SPEED (not a gate unless asked): TI vs OLD wall time, interleaved.
//
//    Options are process-wide and read once, so every render runs in a
//    child process of this binary (`--child ...`) with its own options
//    file (force_number_of_threads / render_thread_reserve_count 0 /
//    time_indexed_motion_blur).
//
//    S  (sanity) dropping any ONE timeline changes the render.
//
//    Scenes A and S run on combo and ext_combo (thin lens f/1.4, keyframed
//    directional + ambient lights, a non-uniformly scaling luminary, a
//    keyframed matrix-authored object); B on their 192x192 twins.
//
//    Usage: TimeIndexedMotionBlurTest [all|S|A|B|C|D] [spp] [n]
//
//  Author: Claude (debt-dl465)
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cstdint>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <cmath>
#include <string>
#include <filesystem>
#include <random>
#include <chrono>
#include <unistd.h>
#include <sys/resource.h>

#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/SobolSampler.h"
#include "../src/Library/Rendering/TimeIndexedMotionBlur.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

static int passCount = 0;
static int failCount = 0;
static std::string g_self;

static void Check( bool condition, const std::string& testName )
{
	if( condition ) {
		passCount++;
		std::cout << "  PASS: " << testName << "\n";
	} else {
		failCount++;
		std::cout << "  FAIL: " << testName << "\n";
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

// --------------------------------------------------------------------
// Scenes
// --------------------------------------------------------------------

static const int kW = 32, kH = 32;

static std::string Timeline( const char* type, const char* element, const char* param, const char* v0, const char* v1 )
{
	return Fmt( "timeline\n{\n\telement_type %s\n%s\tparam %s\n\tinterpolator linear\n\ttime 0\n\tvalue %s\n\ttime 1\n\tvalue %s\n}\n\n",
		type, element ? Fmt( "\telement %s\n", element ).c_str() : "", param, v0, v1 );
}

static std::string Sphere( const char* name, const char* material, const char* position, const char* scale )
{
	return Fmt( "standard_object\n{\n\tname %s\n\tgeometry geo_unit\n\tmaterial %s\n\tposition %s\n\tscale %s\n}\n\n", name, material, position, scale );
}

static std::string Rasterizer( const std::string& kind, const int spp )
{
	const std::string common = Fmt( "\tsamples %d\n\toidn_denoise FALSE\n\tpixel_filter box\n", spp );
	if( kind == "pt" )   return "pathtracing_pel_rasterizer\n{\n" + common + "}\n\n";
	if( kind == "pts" )  return "pathtracing_spectral_rasterizer\n{\n" + common + "\tnum_wavelengths 4\n}\n\n";
	if( kind == "bdpt" ) return "bdpt_pel_rasterizer\n{\n\tmax_eye_depth 4\n\tmax_light_depth 4\n" + common + "}\n\n";
	if( kind == "bdpts" ) return "bdpt_spectral_rasterizer\n{\n\tmax_eye_depth 4\n\tmax_light_depth 4\n" + common + "\tnum_wavelengths 4\n}\n\n";
	if( kind == "vcm" )  return "vcm_pel_rasterizer\n{\n\tmax_eye_depth 4\n\tmax_light_depth 4\n\tvm_enabled FALSE\n" + common + "}\n\n";
	if( kind == "vcms" ) return "vcm_spectral_rasterizer\n{\n\tmax_eye_depth 4\n\tmax_light_depth 4\n\tvm_enabled FALSE\n" + common + "\tnum_wavelengths 4\n}\n\n";
	return "";
}

//! Scene keys: [ext_]<base>[_big], base one of
//!   combo        everything below moves
//!   no_<x>       combo without timeline <x> (section S)
//!   static       combo with exposure 0 (no blur; the frame-time pose)
//!   fallback     combo plus a keyframed painter (not time-indexable)
//!   matmove      combo plus a MATRIX-authored object whose position is keyframed
//!   matstatic    combo plus a static matrix-authored object
//! "ext_" adds the review's extended variant: a thin-lens camera (f/1.4), a
//! keyframed directional light (direction) and ambient light (energy), a
//! luminary whose scale animates non-uniformly, and a keyframed matrix-
//! authored object.  "_big": a 192x192 film (the 32x32 film is ONE
//! FrameStore tile, so it never spreads across threads).
static std::string Scene( const std::string& keyIn, const std::string& ras, const int spp )
{
	std::string key = keyIn;
	const bool big = key.size() > 4 && key.compare( key.size() - 4, 4, "_big" ) == 0;
	if( big ) key = key.substr( 0, key.size() - 4 );
	const bool ext = key.compare( 0, 4, "ext_" ) == 0;
	if( ext ) key = key.substr( 4 );
	std::string s = "RISE ASCII SCENE 7\n\n";
	s += Fmt( "film\n{\n\twidth %d\n\theight %d\n}\n\n", big ? 192 : kW, big ? 192 : kH );
	const bool still = key == "static";
	if( ext ) {
		s += Fmt( "thinlens_camera\n{\n\tlocation 0 0 5\n\tlookat 0 0 0\n\tup 0 1 0\n\tsensor_size 36\n\tfocal_length 35\n\tfstop 1.4\n\tfocus_distance 4\n\texposure %d\n}\n\n", still ? 0 : 1 );
	} else {
		s += Fmt( "pinhole_camera\n{\n\tlocation 0 0 5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 55\n\texposure %d\n}\n\n", still ? 0 : 1 );
	}
	s += "uniformcolor_painter\n{\n\tname white\n\tcolor 1 1 1\n}\n\n"
		"uniformcolor_painter\n{\n\tname grey\n\tcolor 0.5 0.5 0.5\n}\n\n"
		"uniformcolor_painter\n{\n\tname black\n\tcolor 0 0 0\n}\n\n"
		"lambertian_material\n{\n\tname floor_mat\n\treflectance grey\n}\n\n"
		"lambertian_material\n{\n\tname white_mat\n\treflectance white\n}\n\n"
		"lambertian_material\n{\n\tname black_mat\n\treflectance black\n}\n\n"
		"lambertian_luminaire_material\n{\n\tname lum\n\texitance white\n\tscale 10\n\tmaterial none\n}\n\n"
		"clippedplane_geometry\n{\n\tname geo_floor\n\tpta -3 -3 0\n\tptb 3 -3 0\n\tptc 3 3 0\n\tptd -3 3 0\n\tdoublesided FALSE\n}\n\n"
		"standard_object\n{\n\tname floor\n\tgeometry geo_floor\n\tmaterial floor_mat\n}\n\n"
		"sphere_geometry\n{\n\tname geo_unit\n\tradius 1\n}\n\n"
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n";
	const bool all = key.compare( 0, 3, "no_" ) != 0;
	auto keep = [&]( const char* x ) { return all || key != std::string( "no_" ) + x; };
	s += Sphere( "lum_s", "lum", "-1 1 1.2", "0.25 0.25 0.25" );
	if( keep( "lum" ) ) s += Timeline( "object", "lum_s", "position", "-1 1 1.2", "1 1 1.2" );
	s += Sphere( "occ", "black_mat", "-1.5 -0.5 0.4", "0.4 0.4 0.4" );
	if( keep( "occ" ) ) s += Timeline( "object", "occ", "position", "-1.5 -0.5 0.4", "1.5 -0.5 0.4" );
	s += "standard_object\n{\n\tname rig\n\tposition 0 -1.5 0\n}\n\n"
		"standard_object\n{\n\tname kid\n\tparent rig\n\tgeometry geo_unit\n\tmaterial white_mat\n\tposition 0.8 0 0.3\n\tscale 0.3 0.3 0.3\n}\n\n";
	if( keep( "rig" ) ) s += Timeline( "object", "rig", "orientation", "0 0 0", "0 0 90" );
	s += "omni_light\n{\n\tname l_omni\n\tpower 10.0\n\tcolor 1.0 0.8 0.6\n\tposition -2 0 2\n}\n\n";
	if( keep( "omni" ) ) s += Timeline( "light", "l_omni", "position", "-2 0 2", "2 0 2" );
	s += "spot_light\n{\n\tname spot\n\tposition 0 2 2\n\ttarget 0 2 0\n\tcolor 0.6 0.8 1\n\tpower 30\n\tinner 30\n\touter 40\n}\n\n";
	if( keep( "spot" ) ) s += Timeline( "light", "spot", "target", "0 2 0", "1 0 0" );
	if( keep( "cam" ) ) s += Timeline( "camera", 0, "location", "0 0 5", "1 0.5 5" );
	const bool matObject = ext || key == "matmove" || key == "matstatic";
	const bool matMoves = ext || key == "matmove";
	if( matObject ) {
		// A MATRIX-authored object carries authoritative-matrix metadata, so
		// every keyframe application on it goes through that metadata.
		s += "standard_object\n{\n\tname matobj\n\tgeometry geo_unit\n\tmaterial white_mat\n\tmatrix 0.3 0 0 0 0 0.3 0 0 0 0 0.3 0 0.5 -1.2 1 1\n}\n\n";
		if( matMoves && keep( "mat" ) ) s += Timeline( "object", "matobj", "position", "0.5 -1.2 1", "-0.5 -1.0 1" );
	}
	if( ext ) {
		s += "directional_light\n{\n\tname l_dir\n\tpower 1.5\n\tcolor 1 1 1\n\tdirection 0.3 0.3 1\n}\n\n";
		if( keep( "dir" ) ) s += Timeline( "light", "l_dir", "direction", "0.3 0.3 1", "-0.6 0.1 0.5" );
		s += "ambient_light\n{\n\tname l_amb\n\tpower 0.05\n\tcolor 1 1 1\n}\n\n";
		if( keep( "amb" ) ) s += Timeline( "light", "l_amb", "energy", "0.05", "0.3" );
		s += Sphere( "lum2", "lum", "1 -1 1.0", "0.3 0.3 0.3" );
		if( keep( "lum2" ) ) s += Timeline( "object", "lum2", "scale", "0.3 0.3 0.3", "0.6 0.1 0.3" );
	}
	if( key == "fallback" ) {
		// A keyframed painter: not time-indexable.
		s += "uniformcolor_painter\n{\n\tname pulse\n\tcolor 0.2 0.2 0.2\n}\n\n";
		s += Timeline( "painter", "pulse", "risepel", "0.2 0.2 0.2", "0.9 0.9 0.9" );
	}
	for( int i = 0; i < 40; i++ ) {
		const std::string n = Fmt( "pad%d", i );
		const std::string pos = Fmt( "%g %g -5", -40.0 + 2.0 * ( i % 8 ), -40.0 + 2.0 * ( i / 8 ) );
		s += Sphere( n.c_str(), "floor_mat", pos.c_str(), "0.5 0.5 0.5" );
	}
	s += Rasterizer( ras, spp );
	return s;
}

// --------------------------------------------------------------------
// Child: one render in a fresh process
// --------------------------------------------------------------------

class CapturingRasterizerOutput : public virtual IRasterizerOutput, public virtual Reference
{
public:
	std::vector<RISEColor> pixels;
	unsigned int w = 0, h = 0;
protected:
	virtual ~CapturingRasterizerOutput() {}
public:
	virtual void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}
	virtual void OutputImage( const IRasterImage& img, const Rect*, const unsigned int ) override
	{
		w = img.GetWidth(); h = img.GetHeight();
		pixels.resize( w * h );
		for( unsigned int y = 0; y < h; y++ )
			for( unsigned int x = 0; x < w; x++ )
				pixels[y * w + x] = img.GetPEL( x, y );
	}
};

struct ChildResult
{
	bool ok = false;
	unsigned long long hash = 0;
	double mean = 0;
	double regions[16] = {};
	double seconds = 0;
	double userCpu = 0, sysCpu = 0;	//!< the child's CPU time (getrusage of children)
	unsigned long long frames = 0;
};

static int ChildMain( int argc, char** argv )
{
	// --child <scene> <ras> <threads> <ti> <salt> <spp> <out>
	if( argc < 9 ) return 2;
	const std::string sceneKey = argv[2], ras = argv[3];
	const int threads = std::atoi( argv[4] );
	const int ti = std::atoi( argv[5] );
	const unsigned int salt = unsigned( std::strtoul( argv[6], nullptr, 10 ) );
	const int spp = std::atoi( argv[7] );
	const std::string out = argv[8];

	const std::string opts = out + ".options";
	{
		std::ofstream o( opts );
		o << "render_thread_reserve_count 0\n";
		if( threads > 0 ) o << "force_number_of_threads " << threads << "\n";
		o << "time_indexed_motion_blur " << ( ti ? "true" : "false" ) << "\n";
	}
	setenv( "RISE_OPTIONS_FILE", opts.c_str(), 1 );

	const std::string path = out + ".RISEscene";
	{ std::ofstream ofs( path ); ofs << Scene( sceneKey, ras, spp ); }

	ChildResult r;
	IJobPriv* pJob = nullptr;
	if( RISE_CreateJobPriv( &pJob ) && pJob ) {
		if( pJob->LoadAsciiSceneViaCst( path.c_str() ) ) {
			pJob->RemoveRasterizerOutputs();
			CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
			pJob->GetRasterizer()->AddRasterizerOutput( pCap );
			SobolSamplerTestHooks::ValueSalt().store( SobolSequence::HashCombine( salt, 0x465u ) );
			std::srand( salt );
			const auto t0 = std::chrono::steady_clock::now();
			const bool bRendered = pJob->RasterizeAnimation( 0.5, 1.5, 1, false, false );
			r.seconds = std::chrono::duration<double>( std::chrono::steady_clock::now() - t0 ).count();
			if( bRendered && !pCap->pixels.empty() ) {
				r.ok = true;
				unsigned long long hsh = 1469598103934665603ull;
				double sum = 0;
				double reg[16] = {};
				for( unsigned int y = 0; y < pCap->h; y++ ) {
					for( unsigned int x = 0; x < pCap->w; x++ ) {
						const RISEColor& c = pCap->pixels[y * pCap->w + x];
						const double ch[4] = { c.base.r, c.base.g, c.base.b, c.a };
						const unsigned char* b = reinterpret_cast<const unsigned char*>( ch );
						for( std::size_t k = 0; k < sizeof( ch ); k++ ) { hsh ^= b[k]; hsh *= 1099511628211ull; }
						const double v = ( c.base.r + c.base.g + c.base.b ) * c.a / 3.0;
						if( !std::isfinite( v ) ) r.ok = false;
						sum += v;
						reg[ ( y * 4 / pCap->h ) * 4 + ( x * 4 / pCap->w ) ] += v;
					}
				}
				r.hash = hsh;
				r.mean = sum / double( pCap->pixels.size() );
				for( int k = 0; k < 16; k++ ) r.regions[k] = reg[k] / double( pCap->pixels.size() / 16 );
			}
			safe_release( pCap );
		}
		safe_release( pJob );
	}
	r.frames = Implementation::TimeIndexedFrame::CreatedCount();
	std::remove( path.c_str() );
	std::remove( opts.c_str() );

	std::ofstream o( out );
	o.precision( 17 );
	o << ( r.ok ? 1 : 0 ) << " " << r.hash << " " << r.mean << " " << r.seconds << " " << r.frames;
	for( int k = 0; k < 16; k++ ) o << " " << r.regions[k];
	o << "\n";
	return r.ok ? 0 : 1;
}

static ChildResult RunChild( const std::string& scene, const std::string& ras, const int threads, const bool ti, const unsigned int salt, const int spp )
{
	static int counter = 0;
	const std::string out = ( std::filesystem::temp_directory_path() /
		Fmt( "dl465_%d_%d.txt", int( ::getpid() ), counter++ ) ).string();
	const std::string cmd = Fmt( "\"%s\" --child %s %s %d %d %u %d \"%s\" > /dev/null 2>&1",
		g_self.c_str(), scene.c_str(), ras.c_str(), threads, ti ? 1 : 0, salt, spp, out.c_str() );
	ChildResult r;
	struct rusage before, after;
	getrusage( RUSAGE_CHILDREN, &before );
	const int rc = std::system( cmd.c_str() );
	getrusage( RUSAGE_CHILDREN, &after );
	auto sec = []( const struct timeval& t ) { return double( t.tv_sec ) + 1e-6 * double( t.tv_usec ); };
	r.userCpu = sec( after.ru_utime ) - sec( before.ru_utime );
	r.sysCpu = sec( after.ru_stime ) - sec( before.ru_stime );
	std::ifstream in( out );
	int ok = 0;
	if( in >> ok >> r.hash >> r.mean >> r.seconds >> r.frames ) {
		for( int k = 0; k < 16; k++ ) in >> r.regions[k];
		r.ok = ok == 1 && rc == 0;
	}
	std::remove( out.c_str() );
	return r;
}

// --------------------------------------------------------------------
// Statistics
// --------------------------------------------------------------------

static double BetaCF( const double a, const double b, const double x )
{
	double c = 1, d = 1 - ( a + b ) * x / ( a + 1 );
	if( std::fabs( d ) < 1e-300 ) d = 1e-300;
	d = 1 / d;
	double h = d;
	for( int m = 1; m < 300; m++ ) {
		const int m2 = 2 * m;
		double aa = m * ( b - m ) * x / ( ( a + m2 - 1 ) * ( a + m2 ) );
		d = 1 + aa * d; if( std::fabs( d ) < 1e-300 ) d = 1e-300;
		c = 1 + aa / c; if( std::fabs( c ) < 1e-300 ) c = 1e-300;
		d = 1 / d; h *= d * c;
		aa = -( a + m ) * ( a + b + m ) * x / ( ( a + m2 ) * ( a + m2 + 1 ) );
		d = 1 + aa * d; if( std::fabs( d ) < 1e-300 ) d = 1e-300;
		c = 1 + aa / c; if( std::fabs( c ) < 1e-300 ) c = 1e-300;
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
	if( x < ( a + 1 ) / ( a + b + 2 ) ) return bt * BetaCF( a, b, x ) / a;
	return 1 - bt * BetaCF( b, a, 1 - x ) / b;
}

//! Two-sided p of Student's t with `dof` degrees of freedom.
static double TwoSidedP( const double t, const double dof )
{
	return IncompleteBeta( dof / 2, 0.5, dof / ( dof + t * t ) );
}

struct Welch { double t, dof, p; };

static Welch WelchT( const std::vector<double>& a, const std::vector<double>& b )
{
	auto ms = []( const std::vector<double>& v, double& m, double& var ) {
		m = 0; for( double x : v ) m += x; m /= v.size();
		var = 0; for( double x : v ) var += ( x - m ) * ( x - m ); var /= ( v.size() - 1 );
	};
	double ma, va, mb, vb;
	ms( a, ma, va ); ms( b, mb, vb );
	const double sa = va / a.size(), sb = vb / b.size();
	Welch w;
	const double se = std::sqrt( sa + sb );
	w.t = se > 0 ? ( ma - mb ) / se : 0;
	w.dof = ( sa + sb ) * ( sa + sb ) / ( sa * sa / ( a.size() - 1 ) + sb * sb / ( b.size() - 1 ) + 1e-300 );
	if( !std::isfinite( w.dof ) || w.dof < 1 ) w.dof = 1;
	w.p = se > 0 ? TwoSidedP( w.t, w.dof ) : 1;
	return w;
}

static void MeanSd( const std::vector<double>& v, double& m, double& sd )
{
	m = 0; for( double x : v ) m += x; m /= v.size();
	sd = 0; for( double x : v ) sd += ( x - m ) * ( x - m ); sd = std::sqrt( sd / ( v.size() - 1 ) );
}

// --------------------------------------------------------------------
// Sections
// --------------------------------------------------------------------

static const char* kRasterizers[] = { "pt", "pts", "bdpt", "bdpts", "vcm", "vcms" };

static void SectionA( const int spp )
{
	std::cout << "\nA: one thread, TI clones vs moving the shared scene, same salt (exact)\n";
	for( const char* scene : { "combo", "ext_combo" } ) {
		for( const char* ras : kRasterizers ) {
			const unsigned int salt = 1000u + unsigned( std::strlen( ras ) );
			const ChildResult old1 = RunChild( scene, ras, 1, false, salt, spp );
			const ChildResult old2 = RunChild( scene, ras, 1, false, salt, spp );
			const ChildResult ti = RunChild( scene, ras, 1, true, salt, spp );
			std::printf( "  %-9s %-6s OLD %016llx / %016llx  TI %016llx  (TI frames %llu, OLD frames %llu)  mean %.9f vs %.9f\n",
				scene, ras, old1.hash, old2.hash, ti.hash, ti.frames, old1.frames, old1.mean, ti.mean );
			Check( old1.ok && old2.ok && ti.ok, Fmt( "A %s %s: all three renders succeed", scene, ras ) );
			Check( old1.frames == 0 && ti.frames > 0, Fmt( "A %s %s: OLD took the shared-scene path, TI the time-indexed one", scene, ras ) );
			if( old1.hash != old2.hash ) {
				std::printf( "  %s %s: OLD is not deterministic against itself at one thread; exactness not testable\n", scene, ras );
				continue;
			}
			Check( ti.hash == old1.hash, Fmt( "A %s %s: TI pixel hash == OLD pixel hash", scene, ras ) );
		}
	}
}

//! Not equivalence: proves the scene is not degenerate -- dropping any ONE
//! timeline changes the (single-threaded, same-salt) render, so each moving
//! element really is seen by the samples sections A and B compare.
static void SectionS( const int spp )
{
	std::cout << "\nS: every timeline of the combo / ext_combo scenes is visible in the render\n";
	const ChildResult full = RunChild( "combo", "pt", 1, false, 31337u, spp );
	const ChildResult fullExt = RunChild( "ext_combo", "pt", 1, false, 31337u, spp );
	for( const char* x : { "cam", "lum", "occ", "rig", "omni", "spot", "dir", "amb", "lum2", "mat" } ) {
		const bool extOnly = std::string( x ) == "dir" || std::string( x ) == "amb" || std::string( x ) == "lum2" || std::string( x ) == "mat";
		const ChildResult r = RunChild( std::string( extOnly ? "ext_no_" : "no_" ) + x, "pt", 1, false, 31337u, spp );
		const ChildResult& ref = extOnly ? fullExt : full;
		double maxRel = 0;
		for( int k = 0; k < 16; k++ ) {
			const double d = std::fabs( r.regions[k] - ref.regions[k] ) / ( std::fabs( ref.regions[k] ) + 1e-6 );
			if( d > maxRel ) maxRel = d;
		}
		std::printf( "  without the %-4s timeline: image mean %.6f vs %.6f, largest region change %.1f %%\n", x, r.mean, ref.mean, 100 * maxRel );
		Check( ref.ok && r.ok && maxRel > 0.02, Fmt( "S: the %s timeline changes some region by > 2 %%", x ) );
	}
}

//! On the 192x192 film: the 32x32 film is ONE FrameStore tile, which one
//! worker renders alone, so only the big film exercises concurrent posing.
static void SectionB( const int spp, const int n )
{
	std::cout << "\nB: all threads TI vs OLD (single-threaded), 192x192, " << n << " salted renders each\n";
	static const char* kScenes[] = { "combo_big", "ext_combo_big" };
	const int nRas = int( sizeof( kRasterizers ) / sizeof( kRasterizers[0] ) );
	const int family = 2 * nRas * 17;
	const double alpha = 0.01 / family;
	for( const char* scene : kScenes ) {
		for( const char* ras : kRasterizers ) {
			std::vector<double> tiMean, oldMean;
			std::vector<std::vector<double> > tiReg( 16 ), oldReg( 16 );
			bool ok = true, allTI = true;
			for( int i = 0; i < n; i++ ) {
				const unsigned int salt = 70000u + unsigned( i ) * 7919u + unsigned( std::strlen( ras ) ) + unsigned( std::strlen( scene ) ) * 131u;
				const ChildResult a = RunChild( scene, ras, 0, true, salt, spp );
				const ChildResult b = RunChild( scene, ras, 0, false, salt + 1u, spp );
				ok = ok && a.ok && b.ok;
				allTI = allTI && a.frames > 0 && b.frames == 0;
				tiMean.push_back( a.mean ); oldMean.push_back( b.mean );
				for( int k = 0; k < 16; k++ ) { tiReg[k].push_back( a.regions[k] ); oldReg[k].push_back( b.regions[k] ); }
			}
			Check( ok, Fmt( "B %s %s: all renders succeed", scene, ras ) );
			Check( allTI, Fmt( "B %s %s: TI renders took the time-indexed path, OLD did not", scene, ras ) );
			double mt, st, mo, so;
			MeanSd( tiMean, mt, st ); MeanSd( oldMean, mo, so );
			const Welch w = WelchT( tiMean, oldMean );
			std::printf( "  %-13s %-6s image mean TI %.6f sd %.6f | OLD %.6f sd %.6f | ratio %.5f t %+.2f p %.3g\n",
				scene, ras, mt, st, mo, so, mo > 0 ? mt / mo : 0.0, w.t, w.p );
			Check( w.p > alpha, Fmt( "B %s %s: image mean TI == OLD (Welch p %.3g > %.2g)", scene, ras, w.p, alpha ) );
			double worstP = 1, worstT = 0; int worstK = -1;
			for( int k = 0; k < 16; k++ ) {
				const Welch r = WelchT( tiReg[k], oldReg[k] );
				if( r.p < worstP ) { worstP = r.p; worstT = r.t; worstK = k; }
			}
			std::printf( "  %-13s %-6s worst of 16 regions: region %d t %+.2f p %.3g\n", scene, ras, worstK, worstT, worstP );
			Check( worstP > alpha, Fmt( "B %s %s: every 4x4 region mean TI == OLD (worst p %.3g > %.2g)", scene, ras, worstP, alpha ) );
		}
	}
}

static void SectionC( const int spp )
{
	std::cout << "\nC: a keyframed painter keeps the single-threaded path\n";
	const ChildResult r = RunChild( "fallback", "pt", 0, true, 4242u, spp );
	Check( r.ok, "C: fallback render succeeds" );
	Check( r.frames == 0, "C: no time-indexed frame was created" );
}

static void SectionD( const int spp, const int n )
{
	std::cout << "\nD: wall time, all threads, interleaved (not a gate)\n";
	// The review's three cases (PT, 192x192): no matrix-authored object, a
	// static one, a moving one.  Wall, user and sys seconds of the child.
	for( const char* scene : { "combo_big", "matstatic_big", "matmove_big" } ) {
		std::vector<double> tw, tu, ts, ow, ou, os;
		for( int i = 0; i < n; i++ ) {
			const ChildResult a = RunChild( scene, "pt", 0, true, 8000u + i, spp );
			const ChildResult b = RunChild( scene, "pt", 0, false, 8500u + i, spp );
			tw.push_back( a.seconds ); tu.push_back( a.userCpu ); ts.push_back( a.sysCpu );
			ow.push_back( b.seconds ); ou.push_back( b.userCpu ); os.push_back( b.sysCpu );
		}
		double m1, s1, m2, s2, m3, s3, m4, s4, m5, s5, m6, s6;
		MeanSd( tw, m1, s1 ); MeanSd( tu, m2, s2 ); MeanSd( ts, m3, s3 );
		MeanSd( ow, m4, s4 ); MeanSd( ou, m5, s5 ); MeanSd( os, m6, s6 );
		std::printf( "  pt %-14s TI wall %.3f s sd %.3f (user %.2f sys %.2f) | OLD wall %.3f s sd %.3f (user %.2f sys %.2f) | speedup %.2fx (n=%d)\n",
			scene, m1, s1, m2, m3, m4, s4, m5, m6, m4 / m1, n );
	}
	for( const char* ras : { "pt", "bdpt", "vcm" } ) {
		std::vector<double> ti, old;
		for( int i = 0; i < n; i++ ) {
			ti.push_back( RunChild( "combo_big", ras, 0, true, 9000u + i, spp ).seconds );
			old.push_back( RunChild( "combo_big", ras, 0, false, 9500u + i, spp ).seconds );
		}
		double mt, st, mo, so;
		MeanSd( ti, mt, st ); MeanSd( old, mo, so );
		std::printf( "  %-5s TI %.3f s sd %.3f | OLD %.3f s sd %.3f | speedup %.2fx (n=%d)\n", ras, mt, st, mo, so, mo / mt, n );
		// The ceiling: the same scene without motion blur, all threads vs one.
		std::vector<double> mtS, stS;
		for( int i = 0; i < n; i++ ) {
			mtS.push_back( RunChild( "static_big", ras, 0, true, 9000u + i, spp ).seconds );
			stS.push_back( RunChild( "static_big", ras, 1, true, 9500u + i, spp ).seconds );
		}
		MeanSd( mtS, mt, st ); MeanSd( stS, mo, so );
		std::printf( "  %-5s (no blur: all threads %.3f s sd %.3f | one thread %.3f s sd %.3f | speedup %.2fx)\n", ras, mt, st, mo, so, mo / mt );
	}
}

int main( int argc, char** argv )
{
	std::setvbuf( stdout, nullptr, _IONBF, 0 );
	if( argc > 1 && std::strcmp( argv[1], "--child" ) == 0 ) {
		return ChildMain( argc, argv );
	}
	if( argc > 4 && std::strcmp( argv[1], "--dump" ) == 0 ) {
		// --dump <scene> <ras> <spp>: print a scene (for cross-build checks).
		std::cout << Scene( argv[2], argv[3], std::atoi( argv[4] ) );
		return 0;
	}
	g_self = std::filesystem::absolute( argv[0] ).string();
	const std::string only = argc > 1 ? argv[1] : "all";
	const int spp = argc > 2 ? std::atoi( argv[2] ) : 16;
	const int n = argc > 3 ? std::atoi( argv[3] ) : 8;
	std::cout << "TimeIndexedMotionBlurTest (DL-465)\n";
	if( only == "all" || only == "S" ) SectionS( spp );
	if( only == "all" || only == "A" ) SectionA( spp );
	if( only == "all" || only == "B" ) SectionB( spp, n );
	if( only == "all" || only == "C" ) SectionC( spp );
	if( only == "D" ) SectionD( spp, n );
	std::cout << "\n" << passCount << " passed, " << failCount << " failed\n";
	return failCount == 0 ? 0 : 1;
}
