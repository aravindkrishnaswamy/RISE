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
//  DL-471: per-type caps are per PATH in BDPT / VCM / MLT too (they were
//    per SUBPATH, so a joined path could carry ~2N + 2 vertices of the
//    type, and the MIS weights ignored them).  BDPTUtilities::
//    JoinedTypeCapStatus counts the lobe types of the joined path's
//    x_1 .. x_{K-1} (x_K, the vertex next to the light, free unless its
//    scatter is delta or it has no BSDF -- PT's rule), only caps below
//    max_eye_depth + max_light_depth can bind, and BDPT / MLT drop a
//    strategy whose counted endpoint's material has several possible lobe
//    types one of which is capped (IMaterial::ConnectionScatterTypes).
//
//      Row F  open Lambertian corner (floor + back wall, small area light,
//             black surround): BDPT (8,8) and VCM (8,8) at
//             max_diffuse_bounce 0 / 1 == PT at the same cap.  Pre-fix the
//             review measured BDPT +2.97 % at 0.
//      Row G  the same corner in `ggx_material` (a diffuse AND a glossy
//             lobe, so no declared type), all walls and (G2) the back wall
//             only: BDPT (which drops the strategies with such a counted
//             endpoint) and VCM (which estimates such a path with S0 + NEE
//             alone) == PT under max_glossy_bounce / max_diffuse_bounce.
//      Row H  a point-light caustic onto a GGX / Lambertian floor (only
//             light tracing, connections and merges reach it): a cap that
//             cannot bind or cannot apply to the floor's lobes equals the
//             no-cap render (the first cut read 0.71); a binding glossy
//             cap still loses it (DL-481, pinned).
//      Row P  a brute-force partition check on synthetic paths: with caps,
//             MISWeight over every strategy that evaluates a nonzero
//             contribution sums to exactly 1 for a path within the caps
//             and to 0 for one over them, for single-type, undeclared-type
//             and delta vertices, and at the free x_K.
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
#include "../src/Library/Shaders/BDPTIntegrator.h"
#include "../src/Library/Shaders/BDPTVertex.h"
#include "../src/Library/Utilities/BDPTUtilities.h"
#include "../src/Library/Utilities/StabilityConfig.h"

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

//////////////////////////////////////////////////////////////////////
// Row E (DL-470): the MERGE's light-side window.  A glass sphere under
// a small one-sided area light casts a caustic (L S S D E) on a
// Lambertian floor; the camera sees the caustic and nothing of the
// sphere.  Strategies for the caustic: the t = 1 splat (light walk 3
// surfaces), the merge at the floor, and the eye hitting the light
// through the sphere (s = 0, eye walk 4 surfaces) -- so at
// max_eye_depth 1 / 2 / 3 the s = 0 strategy cannot be generated, and
// every merge there is at i <= max_eye_depth.  Before DL-470 the merge's
// wLight still reserved the s = 0 level (only its wCamera was windowed),
// so the caustic lost that share.  BDPT at the SAME caps estimates the
// same path set (VCM's merges reach no path its connections cannot here:
// the camera sees no specular surface), so it is the reference.
//   E2: three index-matched delta slabs in front of the lens make the
//   floor the eye's 7th surface: at the DEFAULT caps (8, 8) the merge at
//   i = 7 is the only caustic strategy (s = 0 needs 10, the splat is
//   blocked by the slabs), at (12, 8) s = 0 joins it -- same paths, so
//   VCM (8,8) == VCM (12,8) == PT (uncapped, s = 0 only).
//////////////////////////////////////////////////////////////////////
static std::string CausticScene( const std::string& rasterizerChunk, const bool slabs )
{
	std::string s =
		"RISE ASCII SCENE 7\n"
		"film\n"
		"{\n"
		"\twidth 48\n"
		"\theight 48\n"
		"}\n"
		"\n"
		"pinhole_camera\n"
		"{\n"
		"\tlocation 1.3 2.0 1.2\n"
		"\tlookat 1.0 0 0\n"
		"\tup 0 1 0\n"
		"\tfov 30.0\n"
		"}\n"
		"\n"
		"standard_shader\n"
		"{\n"
		"\tname global\n"
		"\tshaderop DefaultPathTracing\n"
		"}\n"
		"\n"
		"uniformcolor_painter\n"
		"{\n"
		"\tname pnt_floor\n"
		"\tcolor 0.8 0.8 0.8\n"
		"}\n"
		"\n"
		"lambertian_material\n"
		"{\n"
		"\tname mat_floor\n"
		"\treflectance pnt_floor\n"
		"}\n"
		"\n"
		"uniformcolor_painter\n"
		"{\n"
		"\tname pnt_lum\n"
		"\tcolor 30.0 30.0 30.0\n"
		"}\n"
		"\n"
		"lambertian_luminaire_material\n"
		"{\n"
		"\tname mat_lum\n"
		"\texitance pnt_lum\n"
		"\tscale 1.0\n"
		"\tmaterial none\n"
		"}\n"
		"\n"
		"dielectric_material\n"
		"{\n"
		"\tname glass\n"
		"\ttau 1.0 1.0 1.0\n"
		"\tior 1.5\n"
		"\tscattering 1000000.0\n"
		"}\n"
		"\n"
		"clippedplane_geometry\n"
		"{\n"
		"\tname floor_g\n"
		"\tpta -3 0 -3\n"
		"\tptb -3 0 3\n"
		"\tptc 3 0 3\n"
		"\tptd 3 0 -3\n"
		"}\n"
		"\n"
		"standard_object\n"
		"{\n"
		"\tname floor\n"
		"\tgeometry floor_g\n"
		"\tmaterial mat_floor\n"
		"}\n"
		"\n"
		"sphere_geometry\n"
		"{\n"
		"\tname sph_g\n"
		"\tradius 0.6\n"
		"}\n"
		"\n"
		"standard_object\n"
		"{\n"
		"\tname sph\n"
		"\tgeometry sph_g\n"
		"\tmaterial glass\n"
		"\tposition 0 1.0 0\n"
		"}\n"
		"\n"
		"clippedplane_geometry\n"
		"{\n"
		"\tname lum_g\n"
		"\tpta -1.5 3 -0.5\n"
		"\tptb -0.5 3 -0.5\n"
		"\tptc -0.5 3 0.5\n"
		"\tptd -1.5 3 0.5\n"
		"\tdoublesided FALSE\n"
		"}\n"
		"\n"
		"standard_object\n"
		"{\n"
		"\tname lum\n"
		"\tgeometry lum_g\n"
		"\tmaterial mat_lum\n"
		"}\n"
		"\n"
		"\n";
	if( slabs ) {
		s += "dielectric_material\n{\n\tname slabmat\n\ttau 1.0 1.0 1.0\n\tior 1.0\n\tscattering 1000000.0\n}\n\n";
		const double ys[3] = { 1.95, 1.90, 1.85 };
		for( int k = 0; k < 3; k++ ) {
			char buf[512];
			std::snprintf( buf, sizeof(buf),
				"box_geometry\n{\n\tname slab%d_g\n\twidth 0.8\n\theight 0.02\n\tdepth 0.8\n}\n\n"
				"standard_object\n{\n\tname slab%d\n\tgeometry slab%d_g\n\tmaterial slabmat\n\tposition 1.27 %.2f 1.02\n}\n\n",
				k, k, k, ys[k] );
			s += buf;
		}
	}
	s += rasterizerChunk;
	return s;
}

static void RowE()
{
	std::cout << "--- Row E: caustic box, VCM merge window (DL-470) ---" << std::endl;
	const int n = Repeats();
	const unsigned int spp = 64;
	unsigned int seed = 4700u;
	const unsigned int eyeCaps[] = { 1, 2, 3, 8 };
	for( unsigned int a : eyeCaps ) {
		char lb[64], lv[64];
		std::snprintf( lb, sizeof(lb), "BDPT (%u,8)", a );
		std::snprintf( lv, sizeof(lv), "VCM (%u,8)", a );
		const Stats b = RenderStatsSalted( CausticScene( BDPTChunk( a, 8, spp ), false ), n, seed );
		seed += 100u;
		const Stats v = RenderStatsSalted( CausticScene( VCMChunk( a, 8, spp ), false ), n, seed );
		seed += 100u;
		Print( lb, b );
		Print( lv, v );
		std::printf( "    VCM/BDPT %.4f\n", b.mean > 0 ? v.mean / b.mean : 0.0 );
		Check( b.ok && v.ok, std::string( lv ) + ": rendered" );
		Check( b.ok && v.ok && Agree( v, b, 4.0, 0.02 ), std::string( lv ) + " agrees with " + lb );
	}

	// E2: default caps with the floor at eye depth 7.
	const Stats v8 = RenderStatsSalted( CausticScene( VCMChunk( 8, 8, spp ), true ), n, seed );
	seed += 100u;
	const Stats v12 = RenderStatsSalted( CausticScene( VCMChunk( 12, 8, spp ), true ), n, seed );
	seed += 100u;
	const Stats pt = RenderStatsSalted( CausticScene(
		"pathtracing_pel_rasterizer\n{\n\tsamples 256\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n", true ), n, seed );
	Print( "slabs VCM (8,8)", v8 );
	Print( "slabs VCM (12,8)", v12 );
	Print( "slabs PT", pt );
	std::printf( "    VCM(8,8)/PT %.4f  VCM(12,8)/PT %.4f\n", pt.mean > 0 ? v8.mean / pt.mean : 0.0, pt.mean > 0 ? v12.mean / pt.mean : 0.0 );
	Check( v8.ok && v12.ok && pt.ok, "slabs: rendered" );
	Check( v8.ok && v12.ok && Agree( v8, v12, 4.0, 0.02 ), "slabs: VCM (8,8) agrees with VCM (12,8)" );
	Check( v8.ok && pt.ok && Agree( v8, pt, 4.0, 0.02 ), "slabs: VCM (8,8) agrees with PT" );
}

//////////////////////////////////////////////////////////////////////
// Row F / G (DL-471): an open corner -- floor and back wall, a small
// one-sided area light above, nothing else (escapes are black).
//////////////////////////////////////////////////////////////////////
//! `walls`: 0 all Lambertian, 1 all GGX, 2 GGX back wall over a Lambertian floor.
static std::string CornerScene( const std::string& rasterizerChunk, const int walls )
{
	std::string s = "RISE ASCII SCENE 7\n"
		"film\n{\n\twidth 32\n\theight 32\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 0.6 2.5\n\tlookat 0 -0.4 -1.5\n\tup 0 1 0\n\tfov 55.0\n}\n\n"
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_diffuse\n\tcolor 0.7 0.7 0.7\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_rs\n\tcolor 0.6 0.6 0.6\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_lum\n\tcolor 20.0 20.0 20.0\n}\n\n"
		"lambertian_luminaire_material\n{\n\tname mat_lum\n\texitance pnt_lum\n\tscale 1.0\n\tmaterial none\n}\n\n";
	s += "ggx_material\n{\n\tname mat_ggx\n\trd pnt_diffuse\n\trs pnt_rs\n\talphax 0.3\n\talphay 0.3\n"
	     "\tfresnel_mode schlick_f0\n}\n\n";
	s += "lambertian_material\n{\n\tname mat_lam\n\treflectance pnt_diffuse\n}\n\n";
	s += Wall( "floor", "-2 -1 -3", "-2 -1 1", "2 -1 1", "2 -1 -3", walls == 1 ? "mat_ggx" : "mat_lam" );
	s += Wall( "back",  "-2 -1 -3", "2 -1 -3", "2 3 -3", "-2 3 -3", walls == 0 ? "mat_lam" : "mat_ggx" );
	s += "clippedplane_geometry\n{\n\tname lum_g\n\tpta -0.5 2 -1.5\n\tptb 0.5 2 -1.5\n"
	     "\tptc 0.5 2 -0.5\n\tptd -0.5 2 -0.5\n\tdoublesided FALSE\n}\n"
	     "standard_object\n{\n\tname lum\n\tgeometry lum_g\n\tmaterial mat_lum\n}\n\n";
	s += rasterizerChunk;
	return s;
}

//! `kind` 0 PT, 1 BDPT (8,8), 2 VCM (8,8); `capLine` e.g. "\tmax_diffuse_bounce 0\n".
static std::string CappedChunk( const int kind, const std::string& capLine, const unsigned int spp )
{
	const char* names[] = { "pathtracing_pel_rasterizer", "bdpt_pel_rasterizer", "vcm_pel_rasterizer" };
	char buf[512];
	std::snprintf( buf, sizeof(buf), "%s\n{\n%s\tsamples %u\n%s\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n",
		names[kind], kind == 0 ? "" : "\tmax_eye_depth 8\n\tmax_light_depth 8\n", spp, capLine.c_str() );
	return buf;
}

static void CornerRow( const char* rowName, const int walls, const char* capName, const unsigned int capValue,
	unsigned int& seed )
{
	const int n = Repeats();
	char cap[96];
	std::snprintf( cap, sizeof(cap), "\t%s %u\n", capName, capValue );
	const Stats pt = RenderStatsSalted( CornerScene( CappedChunk( 0, cap, 64 ), walls ), n, seed );
	const Stats bd = RenderStatsSalted( CornerScene( CappedChunk( 1, cap, 32 ), walls ), n, seed + 30u );
	const Stats vc = RenderStatsSalted( CornerScene( CappedChunk( 2, cap, 32 ), walls ), n, seed + 60u );
	seed += 100u;
	char lp[96], lb[96], lv[96];
	std::snprintf( lp, sizeof(lp), "%s PT %s %u", rowName, capName, capValue );
	std::snprintf( lb, sizeof(lb), "%s BDPT %s %u", rowName, capName, capValue );
	std::snprintf( lv, sizeof(lv), "%s VCM %s %u", rowName, capName, capValue );
	Check( pt.ok && bd.ok && vc.ok, std::string( lp ) + ": rendered" );
	Print( lp, pt );
	Print( lb, bd );
	Print( lv, vc );
	std::printf( "    BDPT/PT %.4f  VCM/PT %.4f\n", pt.mean > 0 ? bd.mean / pt.mean : 0.0, pt.mean > 0 ? vc.mean / pt.mean : 0.0 );
	Check( pt.ok && bd.ok && Agree( bd, pt, 4.0, 0.005 ), std::string( lb ) + " agrees with PT" );
	Check( pt.ok && vc.ok && Agree( vc, pt, 4.0, 0.005 ), std::string( lv ) + " agrees with PT" );
}

static void RowF()
{
	std::cout << "--- Row F: open Lambertian corner, per-type cap per PATH (DL-471) ---" << std::endl;
	unsigned int seed = 47100u;
	CornerRow( "F", 0, "max_diffuse_bounce", 0, seed );
	CornerRow( "F", 0, "max_diffuse_bounce", 1, seed );
}

static void RowG()
{
	std::cout << "--- Row G: GGX corner (no declared lobe type: BDPT drops strategies, VCM falls back to S0 + NEE) ---" << std::endl;
	unsigned int seed = 47200u;
	CornerRow( "G", 1, "max_glossy_bounce", 0, seed );
	CornerRow( "G", 1, "max_glossy_bounce", 1, seed );
	CornerRow( "G", 1, "max_diffuse_bounce", 0, seed );
	CornerRow( "G", 1, "max_diffuse_bounce", 2, seed );
	CornerRow( "G2", 2, "max_glossy_bounce", 0, seed );
	CornerRow( "G2", 2, "max_diffuse_bounce", 1, seed );
}

//////////////////////////////////////////////////////////////////////
// Row H (DL-471 review P1): a point-light caustic -- omni light, glass
// sphere, floor, the camera above.  Light tracing, connections and merges
// are its only strategies (the eye cannot hit a point light, NEE cannot
// see it through the glass), so a strategy exclusion at the floor loses
// it.  A cap that cannot bind (max_transmission_bounce 100 at depths
// 10 + 10) or cannot apply to the floor's lobes (max_translucent_bounce 0
// on GGX: diffuse + glossy only) must render exactly the no-cap image; the
// first-cut DL-471 read GGX VCM 0.711 of it (caustic centre 0.025).
// A cap that binds on a floor lobe (max_glossy_bounce 0 on GGX) still
// loses the caustic: DL-481, pinned here so a fix shows up.
//////////////////////////////////////////////////////////////////////
static std::string PointCausticScene( const std::string& rasterizerChunk, const bool ggxFloor )
{
	std::string s = "RISE ASCII SCENE 7\n"
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		"film\n{\n\twidth 48\n\theight 48\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 3 4\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 30.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_d\n\tcolor 0.7 0.7 0.7\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_rs\n\tcolor 0.04 0.04 0.04\n}\n\n"
		"ggx_material\n{\n\tname mat_ggx\n\trd pnt_d\n\trs pnt_rs\n\talphax 0.3\n\talphay 0.3\n\tfresnel_mode schlick_f0\n}\n\n"
		"lambertian_material\n{\n\tname mat_lam\n\treflectance pnt_d\n}\n\n"
		"dielectric_material\n{\n\tname mat_glass\n\ttau 1.0 1.0 1.0\n\tior 1.5\n\tscattering 1000000.0\n}\n\n"
		"clippedplane_geometry\n{\n\tname floor_g\n\tpta -2 0 -2\n\tptb -2 0 2\n\tptc 2 0 2\n\tptd 2 0 -2\n}\n\n";
	s += std::string( "standard_object\n{\n\tname floor\n\tgeometry floor_g\n\tmaterial " ) +
		( ggxFloor ? "mat_ggx" : "mat_lam" ) + "\n}\n\n";
	s += "sphere_geometry\n{\n\tname sph\n\tradius 0.5\n}\n\n"
		"standard_object\n{\n\tname ball\n\tgeometry sph\n\tposition 0 1.0 0\n\tmaterial mat_glass\n}\n\n"
		"omni_light\n{\n\tname key\n\tpower 50.0\n\tcolor 1 1 1\n\tposition 0 2.5 0\n}\n\n";
	s += rasterizerChunk;
	return s;
}

static std::string PointCausticChunk( const char* kind, const std::string& capLine )
{
	char buf[512];
	std::snprintf( buf, sizeof(buf), "%s_pel_rasterizer\n{\n\tmax_eye_depth 10\n\tmax_light_depth 10\n"
		"\tsamples 32\n%s\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n", kind, capLine.c_str() );
	return buf;
}

static void RowH()
{
	std::cout << "--- Row H: point-light caustic onto GGX / Lambertian floors, caps that cannot bind (DL-471 review) ---" << std::endl;
	const int n = Repeats();
	unsigned int seed = 47300u;
	const char* kinds[] = { "bdpt", "vcm" };
	for( const char* kind : kinds ) {
		for( int ggx = 1; ggx >= 0; ggx-- ) {
			const Stats none = RenderStatsSalted( PointCausticScene( PointCausticChunk( kind, "" ), ggx != 0 ), n, seed );
			seed += 100u;
			char l0[96];
			std::snprintf( l0, sizeof(l0), "%s %s no cap", kind, ggx ? "GGX" : "Lambertian" );
			Print( l0, none );
			// max_glossy_bounce 100 hits a GGX floor lobe, so only the depth
			// bound (rule (a): 100 >= 10 + 10) keeps it inert.
			const char* caps[] = { "\tmax_transmission_bounce 100\n", "\tmax_translucent_bounce 0\n", "\tmax_glossy_bounce 100\n" };
			const char* capNames[] = { "max_transmission_bounce 100", "max_translucent_bounce 0", "max_glossy_bounce 100" };
			for( int c = 0; c < 3; c++ ) {
				const Stats st = RenderStatsSalted( PointCausticScene( PointCausticChunk( kind, caps[c] ), ggx != 0 ), n, seed );
				seed += 100u;
				char l[128];
				std::snprintf( l, sizeof(l), "%s %s %s", kind, ggx ? "GGX" : "Lambertian", capNames[c] );
				Print( l, st );
				std::printf( "    ratio to no cap %.4f\n", none.mean > 0 ? st.mean / none.mean : 0.0 );
				Check( none.ok && st.ok && Agree( st, none, 4.0, 0.01 ), std::string( l ) + " equals the no-cap render" );
			}
			if( ggx ) {
				// DL-481 residual, pinned: a binding cap on a floor lobe.
				const Stats st = RenderStatsSalted( PointCausticScene( PointCausticChunk( kind, "\tmax_glossy_bounce 0\n" ), true ), n, seed );
				seed += 100u;
				char l[128];
				std::snprintf( l, sizeof(l), "%s GGX max_glossy_bounce 0 (DL-481)", kind );
				Print( l, st );
				std::printf( "    ratio to no cap %.4f (the caustic is lost; the exact value is the no-cap image less the floor's glossy share of it)\n",
					none.mean > 0 ? st.mean / none.mean : 0.0 );
				Check( none.ok && st.ok && st.mean < 0.9 * none.mean, std::string( l ) + ": DL-481 loss still present (pin)" );
			}
		}
	}
}

//////////////////////////////////////////////////////////////////////
// Row P (DL-471): brute-force MIS partition under per-type caps.
//////////////////////////////////////////////////////////////////////
namespace
{
	class TypedStubMaterial : public IMaterial, public Reference
	{
		unsigned int mask;
	protected:
		virtual ~TypedStubMaterial() {}
	public:
		explicit TypedStubMaterial( unsigned int m ) : mask( m ) {}
		IBSDF* GetBSDF() const override { return nullptr; }
		ISPF* GetSPF() const override { return nullptr; }
		IEmitter* GetEmitter() const override { return nullptr; }
		unsigned int ConnectionScatterTypes() const override { return mask; }
	};

	struct PVert
	{
		int kind;					///< 0 root (area light), 1 surface, 2 camera, 3 root (point light)
		const IMaterial* mat;
		bool delta;					///< sampled lobe was a delta (a mirror)
		unsigned int label;			///< ScatRayType of the scatter here
		Scalar l, e;				///< light- / eye-side area densities
	};

	BDPTVertex MakeP( const std::vector<PVert>& p, unsigned int i, bool lightSide )
	{
		BDPTVertex v;
		v.position = Point3( 0.1 * i, 0.37 * ( i % 2 ), 0.2 * i );
		const PVert& d = p[i];
		if( d.kind == 0 || d.kind == 3 ) {
			v.type = BDPTVertex::LIGHT;
			v.isDelta = ( d.kind == 3 );
		} else if( d.kind == 2 ) {
			v.type = BDPTVertex::CAMERA;
		} else {
			v.type = BDPTVertex::SURFACE;
			v.pMaterial = d.mat;
			v.isDelta = d.delta;
			v.isConnectible = true;
		}
		v.isLightSubpathVertex = lightSide;
		v.pdfFwd = lightSide ? d.l : d.e;
		v.pdfRev = lightSide ? d.e : d.l;
		return v;
	}

	//! Sum of MISWeight over every strategy whose connection evaluates
	//! (neither endpoint a delta vertex), with each walk's `capType`
	//! stamped as the walks stamp it.
	Scalar PartitionSum( const std::vector<PVert>& p, const StabilityConfig& stab )
	{
		BDPTIntegrator* integ = new BDPTIntegrator( 16, 16, stab );
		const unsigned int n = static_cast<unsigned int>( p.size() );
		Scalar sum = 0;
		for( unsigned int s = 0; s + 1 <= n; s++ ) {
			const unsigned int t = n - s;
			if( t < 1 ) continue;
			std::vector<BDPTVertex> lv, ev;
			for( unsigned int i = 0; i < s; i++ ) {
				BDPTVertex v = MakeP( p, i, true );
				// The light walk does not count its free first vertex.
				if( i >= 2 || ( i == 1 && ( v.isDelta || !v.isConnectible ) ) ) {
					v.capType = static_cast<ScatteredRay::ScatRayType>( p[i].label );
				}
				lv.push_back( v );
			}
			for( unsigned int j = 0; j < t; j++ ) {
				BDPTVertex v = MakeP( p, n - 1 - j, false );
				if( j >= 1 && p[n - 1 - j].kind == 1 ) {
					v.capType = static_cast<ScatteredRay::ScatRayType>( p[n - 1 - j].label );
				}
				ev.push_back( v );
			}
			// The eye walk cannot hit a point light.
			if( s == 0 && p[0].kind == 3 ) continue;
			// A connection through a delta endpoint evaluates nothing.
			if( s >= 1 && lv[s - 1].type == BDPTVertex::SURFACE && lv[s - 1].isDelta ) continue;
			if( t >= 1 && ev[t - 1].type == BDPTVertex::SURFACE && ev[t - 1].isDelta ) continue;
			sum += integ->MISWeight( lv, ev, s, t );
		}
		safe_release( integ );
		return sum;
	}
}

static void RowP()
{
	std::cout << "--- Row P: brute-force MIS partition under per-type caps (DL-471) ---" << std::endl;
	TypedStubMaterial* lam = new TypedStubMaterial( 1u << ScatteredRay::eRayDiffuse );
	TypedStubMaterial* multi = new TypedStubMaterial( IMaterial::kAllConnectionScatterTypes );
	TypedStubMaterial* mirror = new TypedStubMaterial( IMaterial::kAllConnectionScatterTypes );
	// A GGX-like receiver: diffuse and glossy lobes, never transmission or translucency.
	TypedStubMaterial* ggx = new TypedStubMaterial( ( 1u << ScatteredRay::eRayDiffuse ) | ( 1u << ScatteredRay::eRayReflection ) );
	const unsigned int T = ScatteredRay::eRayRefraction;
	const unsigned int D = ScatteredRay::eRayDiffuse, G = ScatteredRay::eRayReflection;
	auto root = []() { PVert v = { 0, nullptr, false, 0, 0.8, 0.6 }; return v; };
	auto cam  = []() { PVert v = { 2, nullptr, false, 0, 0.5, 1.0 }; return v; };
	auto point = []() { PVert v = { 3, nullptr, true, 0, 0.8, 0.0 }; return v; };
	auto surf = []( const IMaterial* m, bool delta, unsigned int label, Scalar l, Scalar e ) {
		PVert v = { 1, m, delta, label, l, e }; return v; };

	struct Case { const char* name; std::vector<PVert> path; unsigned int mdb, mgb; Scalar expect; unsigned int mtb = UINT_MAX, mlb = UINT_MAX; };
	std::vector<Case> cases;
	// root, x_K .. x_1, camera (path order from the light)
	cases.push_back( { "3 Lambertian, diffuse cap 1 (2 counted)", { root(), surf( lam, false, D, 0.3, 0.9 ), surf( lam, false, D, 1.7, 0.4 ), surf( lam, false, D, 0.6, 2.2 ), cam() }, 1, UINT_MAX, 0 } );
	cases.push_back( { "3 Lambertian, diffuse cap 2", { root(), surf( lam, false, D, 0.3, 0.9 ), surf( lam, false, D, 1.7, 0.4 ), surf( lam, false, D, 0.6, 2.2 ), cam() }, 2, UINT_MAX, 1 } );
	cases.push_back( { "4 Lambertian, diffuse cap 3", { root(), surf( lam, false, D, 0.3, 0.9 ), surf( lam, false, D, 1.7, 0.4 ), surf( lam, false, D, 0.2, 1.1 ), surf( lam, false, D, 0.6, 2.2 ), cam() }, 3, UINT_MAX, 1 } );
	cases.push_back( { "4 Lambertian, diffuse cap 2", { root(), surf( lam, false, D, 0.3, 0.9 ), surf( lam, false, D, 1.7, 0.4 ), surf( lam, false, D, 0.2, 1.1 ), surf( lam, false, D, 0.6, 2.2 ), cam() }, 2, UINT_MAX, 0 } );
	cases.push_back( { "undeclared x2 (diffuse label), diffuse cap 2", { root(), surf( lam, false, D, 0.3, 0.9 ), surf( multi, false, D, 1.7, 0.4 ), surf( lam, false, D, 0.6, 2.2 ), cam() }, 2, UINT_MAX, 1 } );
	cases.push_back( { "undeclared x2 (diffuse label), diffuse cap 1", { root(), surf( lam, false, D, 0.3, 0.9 ), surf( multi, false, D, 1.7, 0.4 ), surf( lam, false, D, 0.6, 2.2 ), cam() }, 1, UINT_MAX, 0 } );
	cases.push_back( { "undeclared x2 (glossy label), glossy cap 1, diffuse cap 1", { root(), surf( lam, false, D, 0.3, 0.9 ), surf( multi, false, G, 1.7, 0.4 ), surf( lam, false, D, 0.6, 2.2 ), cam() }, 1, 1, 1 } );
	cases.push_back( { "undeclared x2 (glossy label), glossy cap 0", { root(), surf( lam, false, D, 0.3, 0.9 ), surf( multi, false, G, 1.7, 0.4 ), surf( lam, false, D, 0.6, 2.2 ), cam() }, UINT_MAX, 0, 0 } );
	cases.push_back( { "undeclared free x_K (glossy label), glossy cap 0", { root(), surf( multi, false, G, 0.3, 0.9 ), surf( lam, false, D, 1.7, 0.4 ), surf( lam, false, D, 0.6, 2.2 ), cam() }, 2, 0, 1 } );
	cases.push_back( { "two undeclared, diffuse cap 3", { root(), surf( multi, false, D, 0.3, 0.9 ), surf( multi, false, D, 1.7, 0.4 ), surf( multi, false, D, 0.2, 1.1 ), surf( lam, false, D, 0.6, 2.2 ), cam() }, 3, UINT_MAX, 1 } );
	cases.push_back( { "delta mirror at x_K counted, glossy cap 1", { root(), surf( mirror, true, G, 1.3, 0.7 ), surf( lam, false, D, 1.7, 0.4 ), surf( lam, false, D, 0.6, 2.2 ), cam() }, UINT_MAX, 1, 1 } );
	cases.push_back( { "delta mirror at x_K counted, glossy cap 0", { root(), surf( mirror, true, G, 1.3, 0.7 ), surf( lam, false, D, 1.7, 0.4 ), surf( lam, false, D, 0.6, 2.2 ), cam() }, UINT_MAX, 0, 0 } );
	cases.push_back( { "delta mirror at x2, glossy cap 0", { root(), surf( lam, false, D, 0.3, 0.9 ), surf( mirror, true, G, 1.3, 0.7 ), surf( lam, false, D, 0.6, 2.2 ), cam() }, UINT_MAX, 0, 0 } );
	cases.push_back( { "delta mirror at x2, glossy cap 1, diffuse cap 1", { root(), surf( lam, false, D, 0.3, 0.9 ), surf( mirror, true, G, 1.3, 0.7 ), surf( lam, false, D, 0.6, 2.2 ), cam() }, 1, 1, 1 } );
	// A point-light caustic: point light -> glass (delta, x_K) -> receiver -> camera.
	// Only the light-tracing splat reaches it.
	cases.push_back( { "point caustic, GGX-like receiver, translucent cap 0 (cannot apply to it)", { point(), surf( mirror, true, T, 1.3, 0.7 ), surf( ggx, false, D, 0.6, 2.2 ), cam() }, UINT_MAX, UINT_MAX, 1, UINT_MAX, 0 } );
	cases.push_back( { "point caustic, GGX-like receiver, transmission cap 100 (cannot bind)", { point(), surf( mirror, true, T, 1.3, 0.7 ), surf( ggx, false, D, 0.6, 2.2 ), cam() }, UINT_MAX, UINT_MAX, 1, 100, UINT_MAX } );
	// Rule (a) alone (MakeBounceTypeCaps' depth bound): an all-types
	// receiver, so its type mask cannot excuse it; the cap 100 is above
	// max_eye_depth + max_light_depth = 32 and must be ignored.
	cases.push_back( { "point caustic, all-types receiver, transmission cap 100 (depth bound only)", { point(), surf( mirror, true, T, 1.3, 0.7 ), surf( multi, false, D, 0.6, 2.2 ), cam() }, UINT_MAX, UINT_MAX, 1, 100, UINT_MAX } );
	cases.push_back( { "DL-481: point caustic, undeclared receiver, transmission cap 1 -> lost", { point(), surf( mirror, true, T, 1.3, 0.7 ), surf( multi, false, D, 0.6, 2.2 ), cam() }, UINT_MAX, UINT_MAX, 0, 1, UINT_MAX } );
	// DL-481 residual, pinned: a binding cap on one of the receiver's lobe
	// types leaves the splat (the only strategy) excluded -- the path is lost.
	cases.push_back( { "DL-481: point caustic, GGX-like receiver, glossy cap 0 -> lost", { point(), surf( mirror, true, T, 1.3, 0.7 ), surf( ggx, false, D, 0.6, 2.2 ), cam() }, UINT_MAX, 0, 0 } );
	cases.push_back( { "point caustic, Lambertian receiver, diffuse cap 1", { point(), surf( mirror, true, T, 1.3, 0.7 ), surf( lam, false, D, 0.6, 2.2 ), cam() }, 1, UINT_MAX, 1 } );
	cases.push_back( { "no caps (control)", { root(), surf( multi, false, D, 0.3, 0.9 ), surf( mirror, true, G, 1.3, 0.7 ), surf( lam, false, D, 0.6, 2.2 ), cam() }, UINT_MAX, UINT_MAX, 1 } );

	for( Case& c : cases ) {
		// A delta scatter generates its neighbours' densities as Diracs:
		// zero (remap0 in MISWeight).
		for( std::size_t i = 1; i + 1 < c.path.size(); i++ ) {
			if( c.path[i].delta ) {
				c.path[i + 1].l = 0;
				c.path[i - 1].e = 0;
			}
		}
		StabilityConfig stab;
		stab.maxDiffuseBounce = c.mdb;
		stab.maxGlossyBounce = c.mgb;
		stab.maxTransmissionBounce = c.mtb;
		stab.maxTranslucentBounce = c.mlb;
		const Scalar sum = PartitionSum( c.path, stab );
		std::printf( "    %-60s sum %.12f (expect %.0f)\n", c.name, sum, c.expect );
		Check( std::fabs( sum - c.expect ) < 1e-9, std::string( "partition: " ) + c.name );
	}
	safe_release( lam );
	safe_release( multi );
	safe_release( mirror );
	safe_release( ggx );
}

int main()
{
	std::cout << "BDPTDepthCapMISTest (DL-351, DL-467, DL-470, DL-471)" << std::endl;
	// RISE_DL351_ROWS (e.g. "CD") runs a subset; default all.
	const char* rows = std::getenv( "RISE_DL351_ROWS" );
	const std::string sel = rows ? rows : "ABCDEFGHP";
	if( sel.find( 'A' ) != std::string::npos ) RowA();
	if( sel.find( 'B' ) != std::string::npos ) RowB();
	if( sel.find( 'C' ) != std::string::npos ) RowC();
	if( sel.find( 'D' ) != std::string::npos ) RowD();
	if( sel.find( 'E' ) != std::string::npos ) RowE();
	if( sel.find( 'F' ) != std::string::npos ) RowF();
	if( sel.find( 'G' ) != std::string::npos ) RowG();
	if( sel.find( 'H' ) != std::string::npos ) RowH();
	if( sel.find( 'P' ) != std::string::npos ) RowP();
	std::cout << std::endl << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount == 0 ? 0 : 1;
}
