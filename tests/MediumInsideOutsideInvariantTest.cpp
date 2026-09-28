//////////////////////////////////////////////////////////////////////
//
//  MediumInsideOutsideInvariantTest.cpp - Reference-free regression
//    guard for DL-247: a medium-walk continuation that reaches a surface
//    must sample (or attenuate) the medium along that segment before
//    handing off to the surface integrator.
//
//  THE INVARIANT.  A closed, index-matched (`ior 1.0`, delta pass-through)
//  box carries an ABSORBING and scattering `interior_medium`, with a
//  Lambertian floor inside it, under a uniform environment.  The camera
//  sits 1e-4 just INSIDE the box's front face in one render and 1e-4 just
//  OUTSIDE it in the other.  An index-matched delta boundary changes
//  nothing about transport, so the two images must agree to within the
//  2e-4 segment -- which no estimator can see.  No reference render and
//  no closed form are needed: the invariant is a property of the physics
//  alone, so it cannot be fooled by two estimators sharing one bug.
//
//  WHY IT DISCRIMINATES.  The two camera placements reach the medium
//  through DIFFERENT code:
//    - INSIDE, the camera ray starts in the medium, so the first scatter
//      is handled by the camera-ray volumetric walks
//      (`PathTracingIntegrator::IntegrateRayTemplated` for pel/NM,
//      `IntegrateRayHWSS` for `hwss TRUE`), and every later medium segment
//      reached after the floor bounce by the main loops
//      (`IntegrateFromHitTemplated` / `IntegrateFromHitHWSS`).
//    - OUTSIDE, the camera ray hits the delta shell first.  A dielectric
//      has no BSDF, so HWSS falls back to per-wavelength NM there, and
//      every medium segment is handled by the ordinary main loop.
//  A walk that hands a continuation to the surface integrator "as if
//  through vacuum" -- no transmittance, no chance to scatter along the
//  segment -- over-counts INSIDE and not OUTSIDE.
//
//  WHY NO EXISTING SUITE SAW IT.  `VolumeEnvFurnaceTest`'s rows are all
//  zero-absorption furnaces.  At L == L_env everywhere, "skip the
//  segment's attenuation AND its in-scatter" is EXACT -- the two errors
//  cancel.  A furnace with zero absorption cannot see a skipped segment;
//  this scene's sigma_a = 0.3 is what makes the defect visible.
//
//  MEASURED (32x32, 64 spp; mean of n renders per cell):
//                              pre-DL-247   master     this slice
//                              (9e2239b1)   (acf8eb5d) (debt-dl247b, n=8,
//                              (review n=4) (n=4)      three runs)
//    PT pel             in/out  1.840        0.9965     0.9973 / 0.9980 / 0.9955
//    PT spectral NM     in/out  1.843        0.9956     0.9913 / 0.9901 / 0.9885
//    PT spectral HWSS   in/out  1.911        1.9220     0.9945 / 0.9953 / 0.9948
//    BDPT pel           in/out  0.999        0.9986     0.9985 / 0.9987 / 0.9985
//    VCM pel            in/out  1.000        1.0003     1.0001 / 1.0000 / 1.0001
//    cap 2: BDPT/PT             -            1.4878     0.9875 / 0.9869 / 0.9860
//    cap 2: VCM/PT              -            1.6910     1.0003 / 0.9996 / 0.9988
//  The HWSS row needed TWO fixes: the hand-off itself (1.922 -> 1.062
//  from IntegrateRayHWSS's walk, -> 1.028 with IntegrateFromHitHWSS's in-
//  loop walk too), then per-scatter sampler streams for the walks
//  (PTVolumeWalkStream; 1.028 -> 0.995), without which the four lanes'
//  draws overran into the streams their own surface hand-off re-opens.
//
//  BAND.  +/-3%.  Run-to-run sd of an 8-render in/out ratio is <= 0.0014
//  on every row (three batches above) and the largest systematic offset
//  is the NM row's ~1%, so the band is >= 13 sd from any row's mean.  The
//  sub-1% PT offsets are below this suite's resolution and not claimed:
//  at n = 16, PT pel camera-outside reads 0.31623 +/- 0.00029 against
//  camera-inside 0.31525 +/- 0.00024 and VCM 0.31526 +/- 0.00001.
//
//  THE CAP ROWS.  `max_volume_bounce` N means, for every integrator, the
//  Neumann series truncated at N medium-scatter vertices per full path,
//  every medium segment carrying its true transmittance.  At N = 2 in the
//  same box that truncation dominates, so PT, BDPT and VCM must agree
//  with EACH OTHER there -- which they can only do if all three truncate
//  the same way (see DL-247's ruling in docs/DEBT_LEDGER.md).
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: September 27, 2026
//  Tabs: 4
//  Comments:
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

static void Check( bool condition, const std::string& testName )
{
	if( condition ) {
		passCount++;
	} else {
		failCount++;
		std::cout << "  FAIL: " << testName << std::endl;
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

// Renders are seeded from libc rand() (no srand anywhere in the library),
// so each render re-seeds explicitly.  Thread scheduling still makes a
// render non-deterministic, which is why every cell is a mean over n.
static double RenderMean( const std::string& sceneText, unsigned int seed )
{
	char path[512];
	std::snprintf( path, sizeof(path), "/tmp/medium_inside_outside_%d.RISEscene",
		static_cast<int>(::getpid()) );
	{
		std::ofstream ofs( path );
		if( !ofs.is_open() ) return -1.0;
		ofs << sceneText;
	}

	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) return -1.0;
	if( !pJob->LoadAsciiSceneViaCst( path ) ) { safe_release( pJob ); return -1.0; }
	pJob->RemoveRasterizerOutputs();

	CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "test capture output" );
	pJob->GetRasterizer()->AddRasterizerOutput( pCap );

	std::srand( seed );
	if( !pJob->Rasterize() ) { safe_release( pCap ); safe_release( pJob ); return -1.0; }

	double sum = 0;
	bool finite = true;
	for( const RISEColor& c : pCap->pixels ) {
		const double a = ( c.base.r + c.base.g + c.base.b ) / 3.0;
		if( !std::isfinite( a ) ) { finite = false; break; }
		sum += a;
	}
	const double mean = ( finite && !pCap->pixels.empty() )
		? sum / double( pCap->pixels.size() ) : -1.0;

	safe_release( pCap );
	safe_release( pJob );
	std::remove( path );
	return mean;
}

struct Stats
{
	double mean;
	double sd;
	int n;
	bool ok;
};

static Stats RenderStats( const std::string& sceneText, int n, unsigned int seedBase )
{
	Stats s = { 0, 0, n, true };
	std::vector<double> v;
	for( int i = 0; i < n; i++ ) {
		const double m = RenderMean( sceneText, seedBase + unsigned(i) );
		if( m < 0 ) { s.ok = false; return s; }
		v.push_back( m );
	}
	for( double x : v ) s.mean += x;
	s.mean /= double( n );
	double var = 0;
	for( double x : v ) var += ( x - s.mean ) * ( x - s.mean );
	s.sd = n > 1 ? std::sqrt( var / double( n - 1 ) ) : 0;
	return s;
}

//////////////////////////////////////////////////////////////////////
// Scene
//////////////////////////////////////////////////////////////////////

static const char* kEnv =
	"\tradiance_map pnt_env\n\tradiance_scale 1.0\n\tradiance_background TRUE\n";

static std::string BoxScene( const std::string& rasterizerChunk, double camZ )
{
	std::string s = "RISE ASCII SCENE 7\n"
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		"film\n{\n\twidth 32\n\theight 32\n}\n\n";

	char cam[256];
	std::snprintf( cam, sizeof(cam),
		"pinhole_camera\n{\n\tlocation 0 0 %.6f\n\tlookat 0 -1.0 0\n\tup 0 1 0\n\tfov 50.0\n}\n\n",
		camZ );
	s += cam;

	s +=
		"uniformcolor_painter\n{\n\tname pnt_env\n\tcolor 1.0 1.0 1.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_floor\n\tcolor 0.8 0.8 0.8\n}\n\n"
		"lambertian_material\n{\n\tname mat_floor\n\treflectance pnt_floor\n}\n\n"
		"homogeneous_medium\n{\n\tname med\n"
		"\tabsorption 0.3 0.3 0.3\n\tscattering 0.7 0.7 0.7\n\tphase isotropic\n}\n\n"
		// `scattering 1000000` is the delta pass-through spelling and
		// `ior 1.0` makes the boundary index-matched: a transport no-op.
		"dielectric_material\n{\n\tname mat_shell\n\ttau 1.0 1.0 1.0\n"
		"\tior 1.0\n\tscattering 1000000.0\n}\n\n"
		"box_geometry\n{\n\tname shell_box\n\twidth 4.0\n\theight 4.0\n\tdepth 4.0\n}\n\n"
		"standard_object\n{\n\tname obj_shell\n\tgeometry shell_box\n"
		"\tmaterial mat_shell\n\tinterior_medium med\n}\n\n"
		"clippedplane_geometry\n{\n\tname floor_quad\n"
		"\tpta -1.9 -1.5 -1.9\n\tptb -1.9 -1.5 1.9\n\tptc 1.9 -1.5 1.9\n\tptd 1.9 -1.5 -1.9\n}\n\n"
		"standard_object\n{\n\tname obj_floor\n\tgeometry floor_quad\n\tmaterial mat_floor\n}\n\n";

	s += rasterizerChunk;

	s += "file_rasterizeroutput\n{\n\tpattern rendered/medium_inside_outside_unused\n"
	     "\ttype EXR\n\tbpp 32\n\tcolor_space Rec709RGB_Linear\n}\n";
	return s;
}

static std::string Rasterizer( const std::string& kind, const std::string& extra )
{
	std::string body;
	if( kind == "pt" ) {
		body = "pathtracing_pel_rasterizer\n{\n\tsamples 64\n";
	} else if( kind == "ptnm" ) {
		body = "pathtracing_spectral_rasterizer\n{\n\tsamples 64\n\thwss FALSE\n"
		       "\tnmbegin 380\n\tnmend 720\n\tnum_wavelengths 8\n\tspectral_samples 1\n";
	} else if( kind == "pthwss" ) {
		body = "pathtracing_spectral_rasterizer\n{\n\tsamples 64\n\thwss TRUE\n"
		       "\tnmbegin 380\n\tnmend 720\n\tnum_wavelengths 8\n\tspectral_samples 1\n";
	} else if( kind == "bdpt" ) {
		body = "bdpt_pel_rasterizer\n{\n\tmax_eye_depth 20\n\tmax_light_depth 20\n\tsamples 64\n";
	} else if( kind == "vcm" ) {
		body = "vcm_pel_rasterizer\n{\n\tmax_eye_depth 20\n\tmax_light_depth 20\n\tsamples 64\n"
		       "\tvc_enabled true\n\tvm_enabled false\n";
	}
	body += "\tpixel_filter box\n\toidn_denoise FALSE\n";
	body += extra;
	body += kEnv;
	body += "}\n\n";
	return body;
}

static int Repeats()
{
	const char* e = std::getenv( "RISE_MIOIT_REPEATS" );
	const int n = e ? std::atoi( e ) : 8;
	return n >= 2 ? n : 2;
}

static const double kCameraInside  = 1.9999;
static const double kCameraOutside = 2.0001;

//////////////////////////////////////////////////////////////////////
// Inside/outside row: the same rasterizer, camera just inside vs just
// outside the index-matched shell.  The two means must agree.
//////////////////////////////////////////////////////////////////////
static void InsideOutsideRow( const std::string& label, const std::string& kind,
	double band, unsigned int seedBase )
{
	const int n = Repeats();
	const std::string rast = Rasterizer( kind, "" );
	const Stats in  = RenderStats( BoxScene( rast, kCameraInside ),  n, seedBase );
	const Stats out = RenderStats( BoxScene( rast, kCameraOutside ), n, seedBase + 1000u );

	Check( in.ok && out.ok, label + ": both renders produced output" );
	if( !in.ok || !out.ok ) return;
	Check( out.mean > 1e-6, label + ": outside render is non-black" );
	if( out.mean <= 1e-6 ) return;

	const double r = in.mean / out.mean;
	std::printf( "  %-26s inside %.6f +/- %.6f  outside %.6f +/- %.6f  (n=%d)  in/out %.4f\n",
		label.c_str(), in.mean, in.sd, out.mean, out.sd, n, r );

	char buf[320];
	std::snprintf( buf, sizeof(buf), "%s: inside/outside %.4f within 1 +/- %.2f",
		label.c_str(), r, band );
	Check( std::fabs( r - 1.0 ) <= band, buf );
}

//////////////////////////////////////////////////////////////////////
// Cap row: camera inside, `max_volume_bounce 2`.  Truncation dominates,
// so the candidate must match the PT reference only if both truncate the
// Neumann series at the same order with the same segment transmittance.
//////////////////////////////////////////////////////////////////////
static Stats CapRender( const std::string& kind, unsigned int cap, unsigned int seedBase )
{
	char extra[64];
	std::snprintf( extra, sizeof(extra), "\tmax_volume_bounce %u\n", cap );
	return RenderStats( BoxScene( Rasterizer( kind, extra ), kCameraInside ), Repeats(), seedBase );
}

int main()
{
	std::cout << "=== MediumInsideOutsideInvariantTest (DL-247) ===" << std::endl;

	// Band: +/- 3% -- see the header's BAND paragraph for the measurement.
	const double kBand = 0.03;

	std::cout << "Inside vs outside an index-matched absorbing medium box:" << std::endl;
	InsideOutsideRow( "PT pel",                   "pt",     kBand, 7100u );
	InsideOutsideRow( "PT spectral hwss FALSE",   "ptnm",   kBand, 7200u );
	InsideOutsideRow( "PT spectral hwss TRUE",    "pthwss", kBand, 7300u );
	InsideOutsideRow( "BDPT pel",                 "bdpt",   kBand, 7400u );
	InsideOutsideRow( "VCM pel",                  "vcm",    kBand, 7500u );

	std::cout << "Truncation parity at max_volume_bounce 2 (camera inside):" << std::endl;
	const Stats ptCap   = CapRender( "pt",   2, 7600u );
	const Stats bdptCap = CapRender( "bdpt", 2, 7700u );
	const Stats vcmCap  = CapRender( "vcm",  2, 7800u );
	const Stats ptFull  = CapRender( "pt",  64, 7900u );
	Check( ptCap.ok && bdptCap.ok && vcmCap.ok && ptFull.ok, "cap rows produced output" );
	if( ptCap.ok && bdptCap.ok && vcmCap.ok && ptFull.ok && ptCap.mean > 1e-6 )
	{
		std::printf( "  PT   mvb 2   %.6f +/- %.6f\n", ptCap.mean, ptCap.sd );
		std::printf( "  BDPT mvb 2   %.6f +/- %.6f   /PT %.4f\n", bdptCap.mean, bdptCap.sd, bdptCap.mean / ptCap.mean );
		std::printf( "  VCM  mvb 2   %.6f +/- %.6f   /PT %.4f\n", vcmCap.mean, vcmCap.sd, vcmCap.mean / ptCap.mean );
		std::printf( "  PT   mvb 64  %.6f +/- %.6f   (mvb2/mvb64 %.4f)\n", ptFull.mean, ptFull.sd, ptCap.mean / ptFull.mean );

		char buf[320];
		std::snprintf( buf, sizeof(buf), "VCM/PT at max_volume_bounce 2: %.4f within 1 +/- %.2f",
			vcmCap.mean / ptCap.mean, kBand );
		Check( std::fabs( vcmCap.mean / ptCap.mean - 1.0 ) <= kBand, buf );
		std::snprintf( buf, sizeof(buf), "BDPT/PT at max_volume_bounce 2: %.4f within 1 +/- %.2f",
			bdptCap.mean / ptCap.mean, kBand );
		Check( std::fabs( bdptCap.mean / ptCap.mean - 1.0 ) <= kBand, buf );
	}

	std::cout << std::endl;
	std::cout << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount == 0 ? 0 : 1;
}
