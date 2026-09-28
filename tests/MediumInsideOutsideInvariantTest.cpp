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
//  DL-283: HETEROGENEOUS ROWS.  The same box with a Perlin-density
//  `painter_heterogeneous_medium` (64^3 majorant grid), so BDPT/VCM's
//  distance sampling is delta tracking with an OPEN-ENDED draw count.
//  Rows: BDPT pel, VCM pel, BDPT spectral hwss TRUE, and BDPT pel over a
//  black floor.  Measured in/out, three runs of n = 8 on the fixed build:
//    BDPT pel het               0.9989 / 0.9900 / 1.0025
//    VCM pel het                1.0027 / 0.9963 / 1.0034
//    BDPT hwss TRUE het         1.0078 / 1.0056 / 1.0076
//    BDPT pel het black floor   1.0006 / 0.9977 / 0.9956
//  Per-render sd is ~1% (ratio-tracking transmittance noise), so an
//  8-render ratio's run-to-run sd is ~0.5%; the +/-3% band is >= 4 sd
//  from the worst row mean.  These rows gate the heterogeneous
//  inside/outside invariant for BDPT/VCM, which nothing else did.  They
//  CANNOT see DL-283's defect, and neither can any unsalted row: the
//  pre-fix sampler-stream overrun biased camera-inside and camera-outside
//  renders by similar amounts (so the ratio is blind), and repeated
//  renders of a Sobol' build reuse the IDENTICAL Sobol' points -- the
//  pixel seed is deterministic -- so an unsalted run-to-run sd omits the
//  QMC error and cannot resolve a sub-percent mean shift either way.
//
//  DL-283: THE SAMPLER-BIAS ROW.  The pre-fix BDPT/VCM generators drew
//  each medium distance sample off the per-vertex Sobol' stream; delta
//  tracking in a thin, finely gridded heterogeneous medium runs far past
//  the stream's 32 slots into the stream the next vertex re-opens, so one
//  Sobol' dimension drove two decisions on one path -- a real BIAS.  This
//  row renders one BDPT scene (thin 256^3-grid Perlin medium, a quarter
//  of the coefficients above; black floor; camera inside; 128x128 x 4
//  spp) n = 48 times with a distinct per-render VALUE salt
//  (SobolSamplerTestHooks::ValueSalt, so each render is an independent
//  randomized-QMC replicate) and n = 48 times with every SobolSampler
//  draw replaced by an i.i.d. stream (SobolSamplerTestHooks::Independent,
//  a reference no sampler-correlation defect can reach), and requires
//  the two means to agree:
//                          Sobol/independent - 1     se      z
//    pre-DL-283 (6b91fd19)      -0.599 %           0.119 %  -5.04   RED
//    this slice, run 1          -0.048 %           0.118 %  -0.41
//    this slice, run 2          +0.195 %           0.131 %  +1.49
//  Band +/- 0.35% = 3 se (the pre-fix reading sits 2 se outside it; the
//  fixed build's mean offset over four salted measurements, incl. the
//  harness rows below, is about +0.06%).  Independent confirmation (salted, separate
//  harness, P = pre-fix, F = fixed, I = independent, same fixture):
//  pel inside n = 24 P-I -0.806% (z -3.7), F-I +0.005%, F-P paired
//  +0.818% (z +4.4); hwss TRUE inside n = 40 P-I -0.815% (z -5.1),
//  F-I +0.103%, F-P +0.926% (z +5.2); pel camera OUTSIDE n = 24 P-I
//  -0.250%, F-P +0.451% (z +2.4).  ~5.5 min of this test's runtime;
//  RISE_MIOIT_SALTED_REPEATS overrides n, RISE_MIOIT_ONLY_SAMPLER_ROW=1
//  runs this row alone.
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
#include "../src/Library/Utilities/SobolSampler.h"

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

// DL-283: n renders of ONE loaded scene, each an INDEPENDENT randomized-
// QMC replicate -- a distinct per-render value salt through
// SobolSamplerTestHooks -- and, with `independent`, every SobolSampler
// draw replaced by an i.i.d. stream (the unbiased reference).  Without
// the salt, repeated renders reuse the identical Sobol' points and their
// sd omits the QMC error (the unsalted rows above cannot see a sampler-
// correlation bias at all).  The scene is loaded once: its 256^3
// majorant grid dominates the per-render cost otherwise.
static Stats RenderStatsSalted( const std::string& sceneText, int n, unsigned int seedBase,
	bool independent )
{
	Stats st = { 0, 0, n, false };
	char path[512];
	std::snprintf( path, sizeof(path), "/tmp/medium_inside_outside_salted_%d.RISEscene",
		static_cast<int>(::getpid()) );
	{
		std::ofstream ofs( path );
		if( !ofs.is_open() ) return st;
		ofs << sceneText;
	}
	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) { std::remove( path ); return st; }
	if( !pJob->LoadAsciiSceneViaCst( path ) ) { safe_release( pJob ); std::remove( path ); return st; }
	std::remove( path );
	pJob->RemoveRasterizerOutputs();
	CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "test capture output" );
	pJob->GetRasterizer()->AddRasterizerOutput( pCap );

	std::vector<double> v;
	bool ok = true;
	for( int i = 0; i < n && ok; i++ ) {
		SobolSamplerTestHooks::ValueSalt().store(
			SobolSequence::HashCombine( seedBase + unsigned(i), independent ? 0x1u : 0x5u ) );
		SobolSamplerTestHooks::Independent().store( independent );
		std::srand( seedBase + unsigned(i) );
		if( !pJob->Rasterize() || pCap->pixels.empty() ) { ok = false; break; }
		double sum = 0;
		for( const RISEColor& c : pCap->pixels ) sum += ( c.base.r + c.base.g + c.base.b ) / 3.0;
		const double m = sum / double( pCap->pixels.size() );
		if( !std::isfinite( m ) ) { ok = false; break; }
		v.push_back( m );
	}
	SobolSamplerTestHooks::ValueSalt().store( 0u );
	SobolSamplerTestHooks::Independent().store( false );
	safe_release( pCap );
	safe_release( pJob );
	if( !ok || n < 2 ) return st;
	for( double x : v ) st.mean += x;
	st.mean /= double( n );
	double var = 0;
	for( double x : v ) var += ( x - st.mean ) * ( x - st.mean );
	st.sd = std::sqrt( var / double( n - 1 ) );
	st.ok = true;
	return st;
}

//////////////////////////////////////////////////////////////////////
// Scene
//////////////////////////////////////////////////////////////////////

static const char* kEnv =
	"\tradiance_map pnt_env\n\tradiance_scale 1.0\n\tradiance_background TRUE\n";

enum MediumKind { kHomogeneous, kHeterogeneous, kHeterogeneousThinFine };

static std::string MediumChunk( MediumKind kind )
{
	if( kind == kHomogeneous ) {
		return "homogeneous_medium\n{\n\tname med\n"
			"\tabsorption 0.3 0.3 0.3\n\tscattering 0.7 0.7 0.7\n\tphase isotropic\n}\n\n";
	}
	// DL-283: a HETEROGENEOUS medium, so BDPT/VCM's distance sampling is
	// delta tracking with an open-ended draw count (one per majorant-grid
	// cell crossed plus two per tentative collision).  Perlin density in
	// [0, 1] over the box, twice the homogeneous box's coefficients at
	// full density -- or, for the sampler-bias row (kHeterogeneousThinFine,
	// SobolDimensionBudgetTest Test H's medium), a quarter of them over a
	// 256^3 majorant grid, so a free flight crosses many cells and one
	// distance sample draws far past a 32-slot stream.
	const bool thin = ( kind == kHeterogeneousThinFine );
	return std::string(
		"uniformcolor_painter\n{\n\tname pnt_dense\n\tcolor 1.0 1.0 1.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_sparse\n\tcolor 0.0 0.0 0.0\n}\n\n"
		"perlin3d_painter\n{\n\tname pnt_density\n\tpersistence 0.65\n\toctaves 4\n"
		"\tcolora pnt_dense\n\tcolorb pnt_sparse\n\tscale 1.5 1.5 1.5\n\tshift 0 0 0\n}\n\n" )
		+ std::string( thin
			? "painter_heterogeneous_medium\n{\n\tname med\n\tabsorption 0.15 0.15 0.15\n"
			  "\tscattering 0.35 0.35 0.35\n\tphase isotropic\n\tdensity_painter pnt_density\n"
			  "\tresolution 256\n\tcolor_to_scalar luminance\n\tbbox_min -2 -2 -2\n\tbbox_max 2 2 2\n}\n\n"
			: "painter_heterogeneous_medium\n{\n\tname med\n\tabsorption 0.6 0.6 0.6\n"
			  "\tscattering 1.4 1.4 1.4\n\tphase isotropic\n\tdensity_painter pnt_density\n"
			  "\tresolution 64\n\tcolor_to_scalar luminance\n\tbbox_min -2 -2 -2\n\tbbox_max 2 2 2\n}\n\n" );
}

static std::string BoxScene( const std::string& rasterizerChunk, double camZ,
	MediumKind medium = kHomogeneous, double floorAlbedo = 0.8, int filmRes = 32 )
{
	std::string s = "RISE ASCII SCENE 7\n"
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n";
	char film[128];
	std::snprintf( film, sizeof(film), "film\n{\n\twidth %d\n\theight %d\n}\n\n", filmRes, filmRes );
	s += film;

	char cam[256];
	std::snprintf( cam, sizeof(cam),
		"pinhole_camera\n{\n\tlocation 0 0 %.6f\n\tlookat 0 -1.0 0\n\tup 0 1 0\n\tfov 50.0\n}\n\n",
		camZ );
	s += cam;

	char floorPainter[160];
	std::snprintf( floorPainter, sizeof(floorPainter),
		"uniformcolor_painter\n{\n\tname pnt_floor\n\tcolor %g %g %g\n}\n\n",
		floorAlbedo, floorAlbedo, floorAlbedo );

	s += "uniformcolor_painter\n{\n\tname pnt_env\n\tcolor 1.0 1.0 1.0\n}\n\n";
	s += floorPainter;
	s += "lambertian_material\n{\n\tname mat_floor\n\treflectance pnt_floor\n}\n\n";
	s += MediumChunk( medium );
	s +=
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
	} else if( kind == "bdpthwss" ) {
		body = "bdpt_spectral_rasterizer\n{\n\tmax_eye_depth 20\n\tmax_light_depth 20\n\tsamples 64\n"
		       "\thwss TRUE\n\tnmbegin 380\n\tnmend 720\n\tnum_wavelengths 8\n\tspectral_samples 1\n";
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
	double band, unsigned int seedBase,
	MediumKind medium = kHomogeneous, double floorAlbedo = 0.8 )
{
	const int n = Repeats();
	const std::string rast = Rasterizer( kind, "" );
	const Stats in  = RenderStats( BoxScene( rast, kCameraInside,  medium, floorAlbedo ), n, seedBase );
	const Stats out = RenderStats( BoxScene( rast, kCameraOutside, medium, floorAlbedo ), n, seedBase + 1000u );

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
// DL-283 sampler-bias row: the same BDPT render, salted Sobol' vs the
// independent-sampler reference, black floor, camera inside, thin
// 256^3-grid medium.  See the header's DL-283 paragraph.
//////////////////////////////////////////////////////////////////////
static int SaltedRepeats()
{
	const char* e = std::getenv( "RISE_MIOIT_SALTED_REPEATS" );
	const int n = e ? std::atoi( e ) : 48;
	return n >= 4 ? n : 4;
}

static void SamplerBiasRow( const std::string& label, double band, unsigned int seedBase )
{
	const int n = SaltedRepeats();
	const std::string rast =
		"bdpt_pel_rasterizer\n{\n\tmax_eye_depth 20\n\tmax_light_depth 20\n\tsamples 4\n"
		"\tpixel_filter box\n\toidn_denoise FALSE\n" + std::string( kEnv ) + "}\n\n";
	const std::string scene = BoxScene( rast, kCameraInside, kHeterogeneousThinFine, 0.0, 128 );
	const Stats sob = RenderStatsSalted( scene, n, seedBase, false );
	const Stats ind = RenderStatsSalted( scene, n, seedBase + 5000u, true );
	Check( sob.ok && ind.ok, label + ": salted renders produced output" );
	if( !sob.ok || !ind.ok || ind.mean <= 1e-6 ) return;
	const double rel = sob.mean / ind.mean - 1.0;
	const double se = std::sqrt( sob.sd * sob.sd / n + ind.sd * ind.sd / n ) / ind.mean;
	std::printf( "  %-26s Sobol %.6f +/- %.6f  independent %.6f +/- %.6f  (sd, n=%d each)  "
		"Sobol/indep - 1 = %+.3f%% (se %.3f%%, z %+.2f)\n",
		label.c_str(), sob.mean, sob.sd, ind.mean, ind.sd, n, 100.0 * rel, 100.0 * se, rel / se );
	char buf[320];
	std::snprintf( buf, sizeof(buf), "%s: Sobol/independent - 1 = %+.3f%% within +/- %.2f%%",
		label.c_str(), 100.0 * rel, 100.0 * band );
	Check( std::fabs( rel ) <= band, buf );
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
	// Heterogeneous rows: +/- 3% too -- see the header's DL-283 paragraph
	// for the measured sd.
	const double kBandHet = 0.03;
	// DL-283 sampler-bias row: +/- 0.35% = 3 se of the measured 0.118%
	// (n = 48 each side); the pre-fix build read -0.599% (z -5.0).  See
	// the header's DL-283 paragraph.
	const double kBandSampler = 0.0035;

	// RISE_MIOIT_ONLY_SAMPLER_ROW=1 runs just the DL-283 sampler-bias row
	// (for measuring it; the gate runs everything).
	if( std::getenv( "RISE_MIOIT_ONLY_SAMPLER_ROW" ) ) {
		SamplerBiasRow( "BDPT pel black floor thin", kBandSampler, 9100u );
		std::cout << std::endl << passCount << " passed, " << failCount << " failed" << std::endl;
		return failCount == 0 ? 0 : 1;
	}

	std::cout << "Inside vs outside an index-matched absorbing medium box:" << std::endl;
	InsideOutsideRow( "PT pel",                   "pt",     kBand, 7100u );
	InsideOutsideRow( "PT spectral hwss FALSE",   "ptnm",   kBand, 7200u );
	InsideOutsideRow( "PT spectral hwss TRUE",    "pthwss", kBand, 7300u );
	InsideOutsideRow( "BDPT pel",                 "bdpt",   kBand, 7400u );
	InsideOutsideRow( "VCM pel",                  "vcm",    kBand, 7500u );

	// DL-283: heterogeneous rows -- see the header's DL-283 paragraph.
	std::cout << "Same box, HETEROGENEOUS medium (delta tracking; DL-283):" << std::endl;
	InsideOutsideRow( "BDPT pel het",             "bdpt",     kBandHet, 8100u, kHeterogeneous );
	InsideOutsideRow( "VCM pel het",              "vcm",      kBandHet, 8200u, kHeterogeneous );
	InsideOutsideRow( "BDPT hwss TRUE het",       "bdpthwss", kBandHet, 8300u, kHeterogeneous );
	InsideOutsideRow( "BDPT pel het black floor", "bdpt",     kBandHet, 8400u, kHeterogeneous, 0.0 );

	std::cout << "Salted Sobol' vs independent sampler, same BDPT render (DL-283):" << std::endl;
	SamplerBiasRow( "BDPT pel black floor thin", kBandSampler, 9100u );

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
