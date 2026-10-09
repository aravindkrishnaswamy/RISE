//////////////////////////////////////////////////////////////////////
//
//  BDPTDepthCapMISTest.cpp - Red-proof and regression gate for DL-351:
//    BDPT's MIS weights must not reserve density for strategies that the
//    subpath DEPTH CAPS (`max_eye_depth`, `max_light_depth`, and the walk
//    iteration budget they share with `max_volume_bounce`) make
//    ungeneratable.
//
//  THE DEFECT (pre-fix).  `BDPTIntegrator::MISWeight` enumerated every
//    split (s', t') of a path, ignoring whether the eye walk could
//    produce a t'-vertex eye part or the light walk an s'-vertex light
//    part.  A strategy past a cap has density ZERO, but its ratio still
//    entered the power-heuristic denominator, so every strategy that DOES
//    exist was down-weighted: energy lost, and lost differently at each
//    cap setting.
//
//  THE INVARIANT (reference-free).  With cap-aware weights, BDPT at
//    (max_eye_depth a, max_light_depth b) estimates EXACTLY the paths that
//    have at least one CONNECTIBLE split within the caps.  In an
//    all-diffuse scene every vertex is a valid split point, so a path of K
//    surfaces is covered iff K <= a + b (the light part takes L <= b
//    surfaces, the eye part K - L <= a; the s = 0 strategy needs K + 1 <=
//    a because the emitter hit is an eye-walk surface hit, but NEE covers
//    the same path at K <= a) and the image depends on a + b ONLY.  (With a
//    delta vertex it does not: camera -> mirror -> floor -> light cannot
//    split at the mirror, so (2,0) reaches it and (1,1) / (0,2) do not.)
//    Both rows are delta-free, apart from the fog box's pass-through shell,
//    which the eye walk alone crosses at max_eye_depth 20:
//
//      Row A  closed Lambertian box (area light): (2,0) == (1,1) == (0,2),
//             and (3,0) == (2,1) == (1,2).
//      Row B  env-lit scattering box behind an index-matched delta shell
//             (MediumInsideOutsideInvariantTest's box, camera outside):
//             the shell crossings are surface hits, so at
//             max_eye_depth 20 every max_light_depth from 0 to 20 covers
//             the same paths; pre-fix the review measured 0.674 / 0.832 /
//             0.864 / 0.872 at 0 / 1 / 2 / 20.
//
//  Both rows are also gated against PT at large caps (the cap-free
//    estimate), within a combined-standard-error band.
//
//  Every row is a mean over salted randomized-QMC replicates of ONE loaded
//  scene (SobolSamplerTestHooks), so the sd includes the QMC error.
//
//  DL-467: VCM's recurrence MIS must honour the same caps.  Its running
//    quantities reserved density for every strategy whatever the caps, so
//    at (1,1) on Row A's box VCM read 0.0643 against BDPT's cap-invariant
//    0.0839.  Now VCM gets the same rows: Row A at the same (a, b) splits,
//    gated against each other AND against BDPT's group mean (VCM is
//    balance-heuristic, BDPT power-2, so only the estimated integral is
//    shared), and Row B's fog box (whose delta shell turns merging on, so
//    the merge weights' windows are exercised too).
//
//  DL-467 (3): PT's per-type caps.  At a vertex whose BSDF continuation
//    would exceed `max_diffuse_bounce`, PT used to DROP the continuation
//    while NEE there kept its MIS weight against it, losing the
//    BSDF-sampled share of that vertex's direct light.  The capped
//    continuation is now traced for its MIS-weighted emission only.  In
//    Row A's all-Lambertian box, PT at max_diffuse_bounce N then estimates
//    exactly the paths of at most N + 1 surfaces -- BDPT's (N + 1, 0):
//
//      Row C  PT max_diffuse_bounce 0 / 1 / 2  ==  BDPT (1,0) / (2,0) / (3,0).
//
//    Row C barely discriminates: its 1 x 1 emitter is small, so the
//    power heuristic gives the BSDF-sampled hit almost no weight and the
//    lost share is ~0.03 %.  The discriminating row is one where BSDF
//    sampling dominates:
//
//      Row D  a lone Lambertian quad (albedo 0.8) under a uniform unit
//             environment, filling the frame: nothing can interreflect, so
//             the image is 0.8 at EVERY max_diffuse_bounce, including 0.
//             Gated on RGB PT (closed form) and on spectral PT with
//             `hwss TRUE` (max_diffuse_bounce 0 == unlimited).
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

protected:
	virtual ~CapturingRasterizerOutput() {}

public:
	virtual void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}

	virtual void OutputImage( const IRasterImage& img, const Rect*, const unsigned int ) override
	{
		pixels.resize( img.GetWidth() * img.GetHeight() );
		for( unsigned int y = 0; y < img.GetHeight(); y++ ) {
			for( unsigned int x = 0; x < img.GetWidth(); x++ ) {
				pixels[y * img.GetWidth() + x] = img.GetPEL( x, y );
			}
		}
	}
};

struct Stats
{
	double mean;
	double sd;		///< sd of ONE replicate
	int n;
	bool ok;
	double se() const { return n > 0 ? sd / std::sqrt( double( n ) ) : 0; }
};

static Stats RenderStatsSalted( const std::string& sceneText, int n, unsigned int seedBase )
{
	Stats st = { 0, 0, n, false };
	char path[512];
	std::snprintf( path, sizeof(path), "%s/bdpt_depthcap_mis_%d.RISEscene",
		std::getenv( "TMPDIR" ) ? std::getenv( "TMPDIR" ) : "/tmp", static_cast<int>(::getpid()) );
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
			SobolSequence::HashCombine( seedBase + unsigned(i), 0x351u ) );
		std::srand( seedBase + unsigned(i) );
		if( !pJob->Rasterize() || pCap->pixels.empty() ) { ok = false; break; }
		double sum = 0;
		for( const RISEColor& c : pCap->pixels ) sum += ( c.base.r + c.base.g + c.base.b ) / 3.0;
		const double m = sum / double( pCap->pixels.size() );
		if( !std::isfinite( m ) ) { ok = false; break; }
		v.push_back( m );
	}
	SobolSamplerTestHooks::ValueSalt().store( 0u );
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

static int Repeats()
{
	const char* e = std::getenv( "RISE_DL351_REPEATS" );
	const int n = e ? std::atoi( e ) : 6;
	return n >= 2 ? n : 2;
}

//////////////////////////////////////////////////////////////////////
// Row A scene: closed 4x4x4 Lambertian box, camera inside, small area
// luminaire under the ceiling (BDPTEyeDepthConsistencyTest's box).
//////////////////////////////////////////////////////////////////////
static std::string Wall( const char* name, const char* a, const char* b, const char* c, const char* d,
	const char* mat )
{
	std::string s = "clippedplane_geometry\n{\n\tname ";
	s += name; s += "_g\n\tpta "; s += a; s += "\n\tptb "; s += b;
	s += "\n\tptc "; s += c; s += "\n\tptd "; s += d; s += "\n}\n";
	s += "standard_object\n{\n\tname "; s += name; s += "\n\tgeometry "; s += name;
	s += "_g\n\tmaterial "; s += mat; s += "\n}\n\n";
	return s;
}

static std::string BoxScene( const std::string& rasterizerChunk )
{
	std::string s = "RISE ASCII SCENE 7\n"
		"film\n{\n\twidth 32\n\theight 32\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 0 1.5\n\tlookat 0 0 -1\n\tup 0 1 0\n\tfov 60.0\n}\n\n"
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_diffuse\n\tcolor 0.7 0.7 0.7\n}\n\n"
		"lambertian_material\n{\n\tname mat_diffuse\n\treflectance pnt_diffuse\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_lum\n\tcolor 20.0 20.0 20.0\n}\n\n"
		"lambertian_luminaire_material\n{\n\tname mat_lum\n\texitance pnt_lum\n\tscale 1.0\n\tmaterial none\n}\n\n";
	s += Wall( "floor",   "-2 -2 -2", "2 -2 -2", "2 -2 2",  "-2 -2 2", "mat_diffuse" );
	s += Wall( "ceiling", "-2 2 -2",  "-2 2 2",  "2 2 2",   "2 2 -2",  "mat_diffuse" );
	s += Wall( "back",    "-2 -2 -2", "-2 2 -2", "2 2 -2",  "2 -2 -2", "mat_diffuse" );
	s += Wall( "front",   "-2 -2 2",  "2 -2 2",  "2 2 2",   "-2 2 2",  "mat_diffuse" );
	s += Wall( "left",    "-2 -2 -2", "-2 -2 2", "-2 2 2",  "-2 2 -2", "mat_diffuse" );
	s += Wall( "right",   "2 -2 -2",  "2 2 -2",  "2 2 2",   "2 -2 2",  "mat_diffuse" );
	// One-sided panel facing down (doublesided FALSE: DL-320).
	s += "clippedplane_geometry\n{\n\tname lum_g\n\tpta -0.5 1.99 -0.5\n\tptb 0.5 1.99 -0.5\n"
	     "\tptc 0.5 1.99 0.5\n\tptd -0.5 1.99 0.5\n\tdoublesided FALSE\n}\n"
	     "standard_object\n{\n\tname lum\n\tgeometry lum_g\n\tmaterial mat_lum\n}\n\n";
	s += rasterizerChunk;
	return s;
}

static std::string BDPTChunk( unsigned int eyeDepth, unsigned int lightDepth, unsigned int spp )
{
	char buf[512];
	std::snprintf( buf, sizeof(buf),
		"bdpt_pel_rasterizer\n{\n\tmax_eye_depth %u\n\tmax_light_depth %u\n\tsamples %u\n"
		"\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n", eyeDepth, lightDepth, spp );
	return buf;
}

static std::string VCMChunk( unsigned int eyeDepth, unsigned int lightDepth, unsigned int spp )
{
	char buf[512];
	std::snprintf( buf, sizeof(buf),
		"vcm_pel_rasterizer\n{\n\tmax_eye_depth %u\n\tmax_light_depth %u\n\tsamples %u\n"
		"\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n", eyeDepth, lightDepth, spp );
	return buf;
}

static void Print( const char* label, const Stats& s )
{
	std::printf( "    %-28s mean %.6f  sd %.6f  se %.6f  (n=%d)\n", label, s.mean, s.sd, s.se(), s.n );
}

// |a - b| within k combined standard errors, plus a small relative floor
// for the QMC replicate sd's own estimation error at small n.
static bool Agree( const Stats& a, const Stats& b, double k, double relFloor )
{
	const double se = std::sqrt( a.se() * a.se() + b.se() * b.se() );
	return std::fabs( a.mean - b.mean ) <= k * se + relFloor * 0.5 * ( a.mean + b.mean );
}

static void RowA()
{
	std::cout << "--- Row A: closed Lambertian box, image depends on max_eye_depth + max_light_depth only ---" << std::endl;
	const int n = Repeats();
	const unsigned int spp = 32;
	struct Cfg { unsigned int a, b; const char* label; };
	const Cfg sum2[] = { { 2, 0, "BDPT (2,0)" }, { 1, 1, "BDPT (1,1)" }, { 0, 2, "BDPT (0,2)" } };
	const Cfg sum3[] = { { 3, 0, "BDPT (3,0)" }, { 2, 1, "BDPT (2,1)" }, { 1, 2, "BDPT (1,2)" } };
	unsigned int seed = 3510u;
	double bdptGroupMean[2] = { 0, 0 };
	Stats bdptRef[2];
	int g = 0;
	for( const auto* group : { sum2, sum3 } ) {
		Stats st[3];
		for( int k = 0; k < 3; k++ ) {
			st[k] = RenderStatsSalted( BoxScene( BDPTChunk( group[k].a, group[k].b, spp ) ), n, seed );
			seed += 100u;
			Check( st[k].ok, std::string( group[k].label ) + ": rendered" );
			Print( group[k].label, st[k] );
		}
		for( int k = 1; k < 3; k++ ) {
			Check( Agree( st[0], st[k], 4.0, 0.01 ),
				std::string( group[k].label ) + " agrees with " + group[0].label + " (same a + b)" );
		}
		bdptGroupMean[g] = ( st[0].mean + st[1].mean + st[2].mean ) / 3.0;
		bdptRef[g] = st[0];
		g++;
	}

	// DL-467: VCM over the same splits.
	const Cfg vsum2[] = { { 2, 0, "VCM (2,0)" }, { 1, 1, "VCM (1,1)" }, { 0, 2, "VCM (0,2)" } };
	const Cfg vsum3[] = { { 3, 0, "VCM (3,0)" }, { 2, 1, "VCM (2,1)" }, { 1, 2, "VCM (1,2)" } };
	g = 0;
	for( const auto* group : { vsum2, vsum3 } ) {
		Stats st[3];
		for( int k = 0; k < 3; k++ ) {
			st[k] = RenderStatsSalted( BoxScene( VCMChunk( group[k].a, group[k].b, spp ) ), n, seed );
			seed += 100u;
			Check( st[k].ok, std::string( group[k].label ) + ": rendered" );
			Print( group[k].label, st[k] );
		}
		for( int k = 1; k < 3; k++ ) {
			Check( Agree( st[0], st[k], 4.0, 0.01 ),
				std::string( group[k].label ) + " agrees with " + group[0].label + " (same a + b)" );
		}
		for( int k = 0; k < 3; k++ ) {
			Stats ref = bdptRef[g];
			ref.mean = bdptGroupMean[g];
			Check( Agree( st[k], ref, 4.0, 0.015 ),
				std::string( group[k].label ) + " agrees with BDPT at the same a + b" );
		}
		g++;
	}

	// The DEFAULT caps (8, 8) against PT (fixed 128) are informational:
	// paths past 16 surfaces carry ~0.3 % here.  At (32, 32) BDPT must
	// agree with PT -- the cap-free estimate.
	const Stats def = RenderStatsSalted( BoxScene( BDPTChunk( 8, 8, spp ) ), n, seed + 100u );
	Print( "BDPT (8,8) [info]", def );
	const Stats big = RenderStatsSalted( BoxScene( BDPTChunk( 32, 32, spp ) ), n, seed + 300u );
	Print( "BDPT (32,32)", big );
	const Stats pt = RenderStatsSalted( BoxScene(
		"pathtracing_pel_rasterizer\n{\n\tsamples 32\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n" ), n, seed + 200u );
	Print( "PT", pt );
	Check( big.ok && pt.ok && Agree( big, pt, 4.0, 0.0 ), "BDPT (32,32) agrees with PT within 4 combined se" );
}

//////////////////////////////////////////////////////////////////////
// Row B scene: MediumInsideOutsideInvariantTest's env-lit box -- an
// index-matched delta-pass-through shell with an absorbing+scattering
// interior medium and a Lambertian floor inside, camera outside.
//////////////////////////////////////////////////////////////////////
static std::string FogBoxScene( const std::string& rasterizerBody )
{
	std::string s = "RISE ASCII SCENE 7\n"
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		"film\n{\n\twidth 32\n\theight 32\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 0 2.0001\n\tlookat 0 -1.0 0\n\tup 0 1 0\n\tfov 50.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_env\n\tcolor 1.0 1.0 1.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_floor\n\tcolor 0.8 0.8 0.8\n}\n\n"
		"lambertian_material\n{\n\tname mat_floor\n\treflectance pnt_floor\n}\n\n"
		"homogeneous_medium\n{\n\tname med\n"
		"\tabsorption 0.3 0.3 0.3\n\tscattering 0.7 0.7 0.7\n\tphase isotropic\n}\n\n"
		"dielectric_material\n{\n\tname mat_shell\n\ttau 1.0 1.0 1.0\n"
		"\tior 1.0\n\tscattering 1000000.0\n}\n\n"
		"box_geometry\n{\n\tname shell_box\n\twidth 4.0\n\theight 4.0\n\tdepth 4.0\n}\n\n"
		"standard_object\n{\n\tname obj_shell\n\tgeometry shell_box\n"
		"\tmaterial mat_shell\n\tinterior_medium med\n}\n\n"
		"clippedplane_geometry\n{\n\tname floor_quad\n"
		"\tpta -1.9 -1.5 -1.9\n\tptb -1.9 -1.5 1.9\n\tptc 1.9 -1.5 1.9\n\tptd 1.9 -1.5 -1.9\n}\n\n"
		"standard_object\n{\n\tname obj_floor\n\tgeometry floor_quad\n\tmaterial mat_floor\n}\n\n";
	s += rasterizerBody;
	return s;
}

static std::string FogRasterizer( const char* kind, unsigned int eyeDepth, unsigned int lightDepth )
{
	char buf[512];
	if( std::string( kind ) == "pt" ) {
		std::snprintf( buf, sizeof(buf), "pathtracing_pel_rasterizer\n{\n\tsamples 32\n" );
	} else if( std::string( kind ) == "vcm" ) {
		std::snprintf( buf, sizeof(buf),
			"vcm_pel_rasterizer\n{\n\tmax_eye_depth %u\n\tmax_light_depth %u\n\tsamples 32\n",
			eyeDepth, lightDepth );
	} else {
		std::snprintf( buf, sizeof(buf),
			"bdpt_pel_rasterizer\n{\n\tmax_eye_depth %u\n\tmax_light_depth %u\n\tsamples 32\n",
			eyeDepth, lightDepth );
	}
	std::string s = buf;
	s += "\tpixel_filter box\n\toidn_denoise FALSE\n"
	     "\tradiance_map pnt_env\n\tradiance_scale 1.0\n\tradiance_background TRUE\n}\n\n";
	return s;
}

static void RowB()
{
	std::cout << "--- Row B: env-lit fog box, max_eye_depth 20, max_light_depth swept ---" << std::endl;
	const int n = Repeats();
	const unsigned int lightDepths[] = { 0, 1, 2, 20 };
	Stats st[4];
	unsigned int seed = 35100u;
	for( int k = 0; k < 4; k++ ) {
		st[k] = RenderStatsSalted( FogBoxScene( FogRasterizer( "bdpt", 20, lightDepths[k] ) ), n, seed );
		seed += 100u;
		char label[64];
		std::snprintf( label, sizeof(label), "BDPT (20,%u)", lightDepths[k] );
		Check( st[k].ok, std::string( label ) + ": rendered" );
		Print( label, st[k] );
	}
	for( int k = 0; k < 3; k++ ) {
		char label[96];
		std::snprintf( label, sizeof(label), "BDPT (20,%u) agrees with (20,20)", lightDepths[k] );
		Check( Agree( st[k], st[3], 4.0, 0.015 ), label );
	}
	const Stats pt = RenderStatsSalted( FogBoxScene( FogRasterizer( "pt", 0, 0 ) ), n, seed );
	Print( "PT", pt );
	Check( pt.ok && Agree( st[3], pt, 4.0, 0.0 ), "BDPT (20,20) agrees with PT within 4 combined se" );

	// DL-467: VCM over the same sweep.  The shell is a delta surface, so
	// merging is live here.  VCM's sd is larger (merging is consistent,
	// not unbiased), hence the wider relative floor.
	seed += 100u;
	Stats vst[4];
	for( int k = 0; k < 4; k++ ) {
		vst[k] = RenderStatsSalted( FogBoxScene( FogRasterizer( "vcm", 20, lightDepths[k] ) ), n, seed );
		seed += 100u;
		char label[64];
		std::snprintf( label, sizeof(label), "VCM (20,%u)", lightDepths[k] );
		Check( vst[k].ok, std::string( label ) + ": rendered" );
		Print( label, vst[k] );
	}
	for( int k = 0; k < 3; k++ ) {
		char label[96];
		std::snprintf( label, sizeof(label), "VCM (20,%u) agrees with (20,20)", lightDepths[k] );
		Check( Agree( vst[k], vst[3], 4.0, 0.02 ), label );
	}
	Check( pt.ok && Agree( vst[3], pt, 4.0, 0.02 ), "VCM (20,20) agrees with PT" );
}

//////////////////////////////////////////////////////////////////////
// Row C (DL-467): PT max_diffuse_bounce N == BDPT (N+1, 0) on Row A's box.
//////////////////////////////////////////////////////////////////////
static void RowC()
{
	std::cout << "--- Row C: PT max_diffuse_bounce N vs BDPT (N+1, 0), closed Lambertian box ---" << std::endl;
	const int n = Repeats();
	unsigned int seed = 46700u;
	for( unsigned int N = 0; N <= 2; N++ ) {
		char buf[512];
		std::snprintf( buf, sizeof(buf),
			"pathtracing_pel_rasterizer\n{\n\tsamples 32\n\tmax_diffuse_bounce %u\n"
			"\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n", N );
		const Stats pt = RenderStatsSalted( BoxScene( buf ), n, seed );
		const Stats bd = RenderStatsSalted( BoxScene( BDPTChunk( N + 1, 0, 32 ) ), n, seed + 50u );
		seed += 100u;
		char lp[64], lb[64];
		std::snprintf( lp, sizeof(lp), "PT max_diffuse_bounce %u", N );
		std::snprintf( lb, sizeof(lb), "BDPT (%u,0)", N + 1 );
		Check( pt.ok && bd.ok, std::string( lp ) + ": rendered" );
		Print( lp, pt );
		Print( lb, bd );
		Check( Agree( pt, bd, 4.0, 0.01 ), std::string( lp ) + " agrees with " + lb );
	}

	// The HWSS loop's emission-only segment ends on this box's BSDF-less
	// luminaire, where the bundle would otherwise hand each lane to the
	// NM body: `hwss TRUE` must agree with the per-wavelength NM walk.
	for( unsigned int N = 0; N <= 1; N++ ) {
		Stats sp[2];
		for( int h = 0; h < 2; h++ ) {
			char buf[512];
			std::snprintf( buf, sizeof(buf),
				"pathtracing_spectral_rasterizer\n{\n\tsamples 32\n\tnmbegin 380\n\tnmend 720\n"
				"\tnum_wavelengths 8\n\tspectral_samples 1\n\thwss %s\n\tmax_diffuse_bounce %u\n"
				"\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n", h ? "TRUE" : "FALSE", N );
			sp[h] = RenderStatsSalted( BoxScene( buf ), n, seed + 10u * unsigned( h ) );
			char l[64];
			std::snprintf( l, sizeof(l), "PT spectral hwss %s mdb %u", h ? "TRUE" : "FALSE", N );
			Print( l, sp[h] );
		}
		seed += 100u;
		char l[96];
		std::snprintf( l, sizeof(l), "PT spectral mdb %u: hwss TRUE agrees with hwss FALSE", N );
		Check( sp[0].ok && sp[1].ok && Agree( sp[0], sp[1], 4.0, 0.01 ), l );
	}
}

//////////////////////////////////////////////////////////////////////
// Row D (DL-467): env-lit lone floor -- max_diffuse_bounce must not matter.
//////////////////////////////////////////////////////////////////////
static std::string EnvFloorScene( const std::string& rasterizerBody )
{
	std::string s = "RISE ASCII SCENE 7\n"
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		"film\n{\n\twidth 32\n\theight 32\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 3 0.001\n\tlookat 0 0 0\n\tup 0 0 -1\n\tfov 30.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_env\n\tcolor 1.0 1.0 1.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_floor\n\tcolor 0.8 0.8 0.8\n}\n\n"
		"lambertian_material\n{\n\tname mat_floor\n\treflectance pnt_floor\n}\n\n"
		"clippedplane_geometry\n{\n\tname floor_quad\n"
		"\tpta -4 0 -4\n\tptb -4 0 4\n\tptc 4 0 4\n\tptd 4 0 -4\n}\n\n"
		"standard_object\n{\n\tname obj_floor\n\tgeometry floor_quad\n\tmaterial mat_floor\n}\n\n";
	s += rasterizerBody;
	return s;
}

static std::string EnvFloorPT( bool spectral, unsigned int mdb )
{
	std::string s = spectral ?
		"pathtracing_spectral_rasterizer\n{\n\tsamples 32\n\tnmbegin 380\n\tnmend 720\n"
		"\tnum_wavelengths 8\n\tspectral_samples 1\n\thwss TRUE\n" :
		"pathtracing_pel_rasterizer\n{\n\tsamples 32\n";
	if( mdb != 0xFFFFFFFFu ) {
		char buf[64];
		std::snprintf( buf, sizeof(buf), "\tmax_diffuse_bounce %u\n", mdb );
		s += buf;
	}
	s += "\tpixel_filter box\n\toidn_denoise FALSE\n"
	     "\tradiance_map pnt_env\n\tradiance_scale 1.0\n\tradiance_background TRUE\n}\n\n";
	return s;
}

static void RowD()
{
	std::cout << "--- Row D: env-lit lone Lambertian floor, max_diffuse_bounce must not matter ---" << std::endl;
	const int n = Repeats();
	unsigned int seed = 46800u;
	const unsigned int mdbs[] = { 0, 1, 0xFFFFFFFFu };
	Stats pel[3], spec[3];
	for( int k = 0; k < 3; k++ ) {
		char lp[64], ls[64];
		if( mdbs[k] == 0xFFFFFFFFu ) {
			std::snprintf( lp, sizeof(lp), "PT unlimited" );
			std::snprintf( ls, sizeof(ls), "PT spectral hwss unlimited" );
		} else {
			std::snprintf( lp, sizeof(lp), "PT max_diffuse_bounce %u", mdbs[k] );
			std::snprintf( ls, sizeof(ls), "PT spectral hwss mdb %u", mdbs[k] );
		}
		pel[k] = RenderStatsSalted( EnvFloorScene( EnvFloorPT( false, mdbs[k] ) ), n, seed );
		spec[k] = RenderStatsSalted( EnvFloorScene( EnvFloorPT( true, mdbs[k] ) ), n, seed + 50u );
		seed += 100u;
		Check( pel[k].ok && spec[k].ok, std::string( lp ) + ": rendered" );
		Print( lp, pel[k] );
		Print( ls, spec[k] );
		Stats closed = { 0.8, 0, n, true };
		Check( Agree( pel[k], closed, 4.0, 0.005 ), std::string( lp ) + " equals the closed form 0.8" );
	}
	for( int k = 0; k < 2; k++ ) {
		Check( Agree( spec[k], spec[2], 4.0, 0.005 ),
			std::string( "PT spectral hwss mdb " ) + ( k == 0 ? "0" : "1" ) + " agrees with unlimited" );
	}
}

int main()
{
	std::cout << "BDPTDepthCapMISTest (DL-351, DL-467)" << std::endl;
	// RISE_DL351_ROWS (e.g. "CD") runs a subset; default all.
	const char* rows = std::getenv( "RISE_DL351_ROWS" );
	const std::string sel = rows ? rows : "ABCD";
	if( sel.find( 'A' ) != std::string::npos ) RowA();
	if( sel.find( 'B' ) != std::string::npos ) RowB();
	if( sel.find( 'C' ) != std::string::npos ) RowC();
	if( sel.find( 'D' ) != std::string::npos ) RowD();
	std::cout << std::endl << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount == 0 ? 0 : 1;
}
