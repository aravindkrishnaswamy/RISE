//////////////////////////////////////////////////////////////////////
//
//  MediumEnclosureConnectibilityTest.cpp - Red-proof and regression
//    guard for DL-200: `BDPTIntegrator`'s medium-vertex `isConnectible`
//    derivation demoted a MEDIUM vertex to non-connectible whenever it
//    sat inside an enclosure whose boundary vertex was itself
//    non-connectible, inheriting the flag from `vertices.back()`.
//
//  THE DEFECT.  `isConnectible` is supposed to describe THE VERTEX'S OWN
//  SCATTERING FUNCTION -- "can a connection through it carry nonzero
//  density" -- and a phase function is never a delta, so a MEDIUM vertex
//  is always connectible.  The demotion was a VISIBILITY heuristic
//  ("connection rays are blocked by the specular surface") wearing that
//  flag's hat, and it was wrong three ways:
//
//    (1) Visibility is a property of the PAIR of endpoints, not of one
//        vertex.  Two points inside the SAME enclosure are perfectly
//        connectible; only a connection LEAVING it is blocked.  A light
//        inside a glass shell filled with fog is exactly that case.
//    (2) It read `vertices.back()`, so it was ASYMMETRIC between the two
//        walks: a LIGHT-rooted predecessor (type LIGHT) never fails the
//        SURFACE/MEDIUM type check, so a light emitting straight into an
//        enclosed medium left that vertex connectible, while an EYE walk
//        crossing the same boundary to a coincident point read false.
//    (3) SURFACE vertices were never treated this way -- a diffuse
//        surface inside the same shell reads `GetBSDF() != 0` = true
//        regardless of enclosure -- and PT's own volume NEE connects out
//        of interior medium vertices through the boundary using the same
//        transparent-shadow transmittance.  Only MEDIUM vertices
//        disagreed with the PT reference.
//
//  It became actively harmful when DL-126 made `MISWeight` SKIP a
//  strategy whose endpoint is `!isConnectible`: a flag that depends on
//  WHICH WALK created the vertex then feeds the MIS denominator.
//
//  THE SCENE.  A dielectric sphere shell (r = 1.2, `scattering 1000000`,
//  i.e. a true delta boundary) carrying a strongly scattering
//  `interior_medium`, with a small emissive sphere INSIDE it, viewed from
//  outside.  That is the configuration two previous investigations could
//  not construct (the DL-200 row records 0 connection attempts out of 0
//  opportunities on both of them).  Scratch instrumentation on the
//  generators and on every `isConnectible` connection gate measured it
//  reached on the FIRST variant:
//
//    medium verts:  LIGHT-rooted enclosed  conn=4683400  nonconn=2132904
//                   EYE-rooted   enclosed  conn=0        nonconn=2197533
//    conn attempts at a MEDIUM endpoint:
//                   light-side  ok=27474287  skipped=12942973
//                   eye-side    ok=0         skipped=32085651
//
//  MEASURED (48x48, 32 spp, PT as the reference; isolated A/B with
//  BDPTIntegrator.cpp reverted to this slice's base commit, library and
//  test rebuilt, run, then restored):
//
//                                pre-fix   post-fix
//    BDPT / PT  (RGB)      n=4    0.90338   1.00356
//                          n=3    0.91476   1.00827
//    BDPT / PT  (spectral) n=4    0.79556   0.98217
//                          n=3    0.78357   0.98905
//    VCM  / PT  (RGB)      n=3    0.06073   0.06041   (DL-218, unmoved)
//    BDPT / PT global med  n=3    1.79333   1.73429   (DL-218, unmoved)
//
//  10 passed / 2 failed pre-fix against the bands below (12 checks: 4 rows x 3; only the two
//  money bands can go red -- the two DL-218 pins sit inside their bands in both builds); 12 / 0 post-fix.
//  (Counters corrected at merge, 2026-09-18: the slice's own report said 8/4 and 15/0, which this file cannot produce.)
//  BDPT's own run-to-run sd also falls 2.94% -> 0.37% (RGB): the dropped
//  eye-side NEE was a variance cost as well as a bias.  The RGB row is
//  the marginal one (0.905-0.915 pre-fix against a 0.95 floor); the
//  SPECTRAL row is the decisive red at 0.78.
//
//  TWO ROW FAMILIES HERE WERE NOT DL-200 ROWS.  VCM read 6% of PT on the
//  enclosed scene (DL-218, closed 2026-09-22: VCM now takes NEE and
//  interior connections at medium vertices) and BDPT/VCM read ~1.8-2.1x
//  of PT in a plain GLOBAL medium (DL-247).  DL-247's first closure
//  (`db71fdfd`) was reopened on review: its global-medium scene was so
//  dense (albedo 0.9934, ~150 expected scatters) that the `max_volume_bounce`
//  64 truncation it had just made symmetric dominated the answer -- every
//  estimator read ~15-18% of the untruncated one, so the "parity" compared
//  two truncations -- and its PT reference was firefly-dominated with a
//  +/-10% band at ~1 sd (24/0, 23/1, 23/1 over three runs).
//
//  THE GLOBAL ROWS NOW (debt-dl247b).  The same camera in a THIN global
//  medium (sigma_a 0.1, sigma_s 0.4: albedo 0.8, mean scatter count
//  a/(1-a) = 4, P(> 64 scatters) = 0.8^64 ~ 6e-7), so PT at
//  `max_volume_bounce` 64 and at 1000 agree (checked below) and the rows
//  measure TRANSPORT, not truncation.  The emitter is a larger, dimmer
//  sphere (r 0.5, scale 2.3: the same power as the enclosed rows' r 0.12,
//  scale 40): the small hot one made every estimator heavy-tailed -- VCM
//  spectral's per-render sd was 6.7% at 64 spp and a 16-render mean read
//  0.95 of PT while 16 renders at 256 spp read 1.003, i.e. the "bias" was
//  the skew of a heavy-tailed mean.  Measured per-render sd at 64 spp with
//  the new emitter: PT 1.2%, PT spectral 1.3%, BDPT 0.24%, VCM 0.48%,
//  BDPT spectral 0.94%, VCM spectral 1.6%.  Every render is seeded
//  (`std::srand`); references are means of 32 renders (sd of the mean
//  ~0.2%), candidates of 4 (<= 0.8%).  The +/-4% band is >= 6x the
//  references' sd and >= 4 sd of the noisiest ratio (VCM spectral, ~0.85%).
//  The BDPT `max_light_depth 1` row restores the diagnostic the first
//  closure deleted: it and full BDPT are both unbiased, so they agree with
//  each other and with PT.  The enclosed rows' PT references are likewise
//  rendered once, 16 renders at 256 spp (sd of the mean ~0.3%).
//
//  WHAT THE PREDICTED MECHANISM TURNED OUT TO BE WORTH: ZERO.  DL-200
//  predicted a partition-of-unity violation from the ASYMMETRY.  The
//  alternative fix that removes the asymmetry WITHOUT restoring the
//  dropped connections -- deriving the flag from the enclosure boundary's
//  own material, symmetrically false on both sides for a delta boundary
//  -- was implemented and measured: it leaves the rendered image
//  BIT-IDENTICAL to the pre-fix baseline (0.033181734 RGB / 0.030185761
//  spectral on the same four seeds, to eight significant figures).  So
//  the light-side connections it removes contribute exactly nothing and
//  the asymmetry cost nothing on its own; the whole 10% / 20% gap is the
//  EYE side dropping NEE and interior connections at medium vertices
//  inside an enclosure that contains the light.  Quote that, not a
//  partition-violation story.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: September 18, 2026
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

static double RenderMean( const std::string& sceneText, unsigned int seed )
{
	char path[512];
	std::snprintf( path, sizeof(path), "/tmp/medium_enclosure_conn_%d.RISEscene",
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

	// Renders seed from libc rand() (nothing in the library calls srand),
	// so every render re-seeds explicitly.  Thread scheduling still makes a
	// render non-deterministic, which is why every row is a mean over n.
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

static unsigned int g_seed = 3100u;

// Mean of n seeded renders; prints the mean and the sd OF THE MEAN.
static double RenderMeanRepeated( const std::string& sceneText, const char* label, int n )
{
	std::vector<double> v;
	for( int i = 0; i < n; i++ ) {
		const double m = RenderMean( sceneText, g_seed++ );
		if( m < 0 ) return -1.0;
		v.push_back( m );
	}
	double mean = 0;
	for( double x : v ) mean += x;
	mean /= double( n );
	double var = 0;
	for( double x : v ) var += ( x - mean ) * ( x - mean );
	const double sdMean = n > 1 ? std::sqrt( var / double( n - 1 ) / double( n ) ) : 0;
	std::printf( "    %s: mean=%.6g over %d renders, sd of mean %.3g (%.2f%%)\n",
		label, mean, n, sdMean, mean > 0 ? 100.0 * sdMean / mean : 0.0 );
	return mean;
}

//////////////////////////////////////////////////////////////////////
// Scene.  `enclosed` puts the medium inside a delta dielectric shell
// around the emitter (DL-200 / DL-218); otherwise the medium is the
// scene's GLOBAL one and there is no shell (DL-247).
//////////////////////////////////////////////////////////////////////
static std::string SceneBody( bool enclosed, double sigmaA, double sigmaS,
	double emitterRadius, double emitterScale )
{
	std::string s = "film\n{\n\twidth 48\n\theight 48\n}\n\n";

	s += "pinhole_camera\n{\n"
	     "\tlocation 0 0 5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n";

	char med[256];
	std::snprintf( med, sizeof(med),
		"homogeneous_medium\n{\n\tname med\n"
		"\tabsorption %g %g %g\n\tscattering %g %g %g\n\tphase hg 0.0\n}\n\n",
		sigmaA, sigmaA, sigmaA, sigmaS, sigmaS, sigmaS );
	s += med;

	s +=
		"scalar_painter\n{\n\tname tau1\n\tvalue 1.0\n}\n\n"
		"scalar_painter\n{\n\tname ior15\n\tvalue 1.5\n}\n\n"
		// `scattering 1000000` is the DELTA pass-through spelling; a
		// finite value would make the boundary a non-delta transmitter
		// with a real BSDF, which is not the configuration this row is
		// about (the boundary vertex must read isConnectible == false).
		"dielectric_material\n{\n\tname glass\n"
		"\tior ior15\n\ttau tau1\n\tscattering 1000000\n}\n\n"
		"sphere_geometry\n{\n\tname shellgeo\n\tradius 1.2\n}\n\n";

	if( enclosed ) {
		s += "standard_object\n{\n\tname shell\n\tgeometry shellgeo\n"
		     "\tmaterial glass\n\tinterior_medium med\n}\n\n";
	} else {
		s += "global_medium\n{\n\tmedium med\n}\n\n";
	}

	char emit[512];
	std::snprintf( emit, sizeof(emit),
		"uniformcolor_painter\n{\n\tname pemit\n\tcolor 1.0 1.0 1.0\n}\n\n"
		"lambertian_luminaire_material\n{\n\tname memit\n"
		"\texitance pemit\n\tscale %g\n\tmaterial none\n}\n\n"
		"sphere_geometry\n{\n\tname emitgeo\n\tradius %g\n}\n\n",
		emitterScale, emitterRadius );
	s += emit;

	s += "standard_object\n{\n\tname emit\n\tgeometry emitgeo\n\tmaterial memit\n}\n\n";

	s +=
		"file_rasterizeroutput\n{\n\tpattern rendered/medium_enclosure_conn_unused\n"
		"\ttype EXR\n\tbpp 32\n\tcolor_space Rec709RGB_Linear\n}\n\n";

	return s;
}

static const char* kShader =
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n";

static const char* kBDPT =
	"bdpt_pel_rasterizer\n{\n\tmax_eye_depth 6\n\tmax_light_depth 6\n\tsamples 32\n"
	"\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";
static const char* kVCM =
	"vcm_pel_rasterizer\n{\n\tmax_eye_depth 6\n\tmax_light_depth 6\n\tsamples 32\n"
	"\tvc_enabled true\n\tvm_enabled false\n"
	"\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";
static const char* kBDPTSpectral =
	"bdpt_spectral_rasterizer\n{\n\tmax_eye_depth 6\n\tmax_light_depth 6\n\tsamples 32\n"
	"\thwss FALSE\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";
static const char* kVCMSpectral =
	"vcm_spectral_rasterizer\n{\n\tmax_eye_depth 6\n\tmax_light_depth 6\n\tsamples 32\n"
	"\tvc_enabled true\n\tvm_enabled false\n\thwss FALSE\n"
	"\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";

// Enclosed-scene references: 256 spp (the 32-spp PT reference the rows
// used to re-render per row was heavy-tailed -- a 4-render mean moved by
// 2-3%, enough to take the spectral row outside its band on its own).
static const char* kPT256Enc =
	"pathtracing_pel_rasterizer\n{\n\tsamples 256\n\trr_min_depth 8\n"
	"\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";
static const char* kPTSpectral256Enc =
	"pathtracing_spectral_rasterizer\n{\n\tsamples 256\n\trr_min_depth 8\n\thwss FALSE\n"
	"\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";

// Global-medium (DL-247) rasterizers, all 64 spp: references are means of
// 32 renders, candidates of 4.  `max_volume_bounce 1000` is the
// truncation control.
static const char* kPT64 =
	"pathtracing_pel_rasterizer\n{\n\tsamples 64\n\trr_min_depth 8\n"
	"\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";
static const char* kPTSpectral64 =
	"pathtracing_spectral_rasterizer\n{\n\tsamples 64\n\trr_min_depth 8\n\thwss FALSE\n"
	"\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";
static const char* kPT64Mvb1000 =
	"pathtracing_pel_rasterizer\n{\n\tsamples 64\n\trr_min_depth 8\n"
	"\tmax_volume_bounce 1000\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";
static const char* kBDPT64 =
	"bdpt_pel_rasterizer\n{\n\tmax_eye_depth 6\n\tmax_light_depth 6\n\tsamples 64\n"
	"\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";
static const char* kBDPTL1_64 =
	"bdpt_pel_rasterizer\n{\n\tmax_eye_depth 6\n\tmax_light_depth 1\n\tsamples 64\n"
	"\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";
static const char* kVCM64 =
	"vcm_pel_rasterizer\n{\n\tmax_eye_depth 6\n\tmax_light_depth 6\n\tsamples 64\n"
	"\tvc_enabled true\n\tvm_enabled false\n"
	"\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";
static const char* kBDPTSpectral64 =
	"bdpt_spectral_rasterizer\n{\n\tmax_eye_depth 6\n\tmax_light_depth 6\n\tsamples 64\n"
	"\thwss FALSE\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";
static const char* kVCMSpectral64 =
	"vcm_spectral_rasterizer\n{\n\tmax_eye_depth 6\n\tmax_light_depth 6\n\tsamples 64\n"
	"\tvc_enabled true\n\tvm_enabled false\n\thwss FALSE\n"
	"\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";

static std::string Scene( const char* rast, const std::string& body )
{
	return std::string("RISE ASCII SCENE 7\n") + kShader + rast + body;
}

static void CheckRatio( const std::string& label, double ref, double cand, double lo, double hi )
{
	Check( ref > 1e-9, label + ": reference render is non-black" );
	Check( cand > -0.5, label + ": candidate render produced output" );
	if( ref <= 1e-9 || cand < 0 ) return;

	const double r = cand / ref;
	std::cout << "    candidate / reference = " << r << std::endl;

	char buf[320];
	std::snprintf( buf, sizeof(buf), "%s: candidate/reference %.4f in [%.2f, %.2f]",
		label.c_str(), r, lo, hi );
	Check( r >= lo && r <= hi, buf );
}

int main()
{
	std::cout << "=== MediumEnclosureConnectibilityTest (DL-200 / DL-218 / DL-247) ===" << std::endl;

	// The enclosed rows keep the original dense medium: the DL-200 /
	// DL-218 configuration is a light INSIDE a strongly scattering shell.
	// Each reference is rendered ONCE (16 renders at 256 spp) and shared.
	const std::string enclosed = SceneBody( true, 0.02, 3.0, 0.12, 40.0 );

	std::cout << "Enclosed shell, references:" << std::endl;
	const double encRef   = RenderMeanRepeated( Scene( kPT256Enc, enclosed ), "PT pel reference     ", 16 );
	const double encSpRef = RenderMeanRepeated( Scene( kPTSpectral256Enc, enclosed ), "PT spectral reference", 16 );

	// MONEY ROWS (DL-200).  Camera OUTSIDE: the eye walk must cross the delta
	// shell to reach the medium, so pre-fix every eye-rooted medium
	// vertex read !isConnectible and every NEE / interior connection
	// from it was skipped.
	std::cout << "Testing BDPT vs PT, emitter in a fog-filled dielectric shell, camera OUTSIDE" << std::endl;
	CheckRatio( "BDPT vs PT, emitter in a fog-filled dielectric shell, camera OUTSIDE", encRef,
		RenderMeanRepeated( Scene( kBDPT, enclosed ), "BDPT                 ", 8 ), 0.95, 1.06 );

	std::cout << "Testing BDPT spectral vs PT spectral, same scene, camera OUTSIDE" << std::endl;
	CheckRatio( "BDPT spectral vs PT spectral, same scene, camera OUTSIDE", encSpRef,
		RenderMeanRepeated( Scene( kBDPTSpectral, enclosed ), "BDPT spectral        ", 8 ), 0.95, 1.06 );

	// MONEY ROWS (DL-218 (a)). VCM implementing NEE and interior connections
	// at medium vertices should match PT and BDPT at ~1.0.
	std::cout << "Testing VCM vs PT, same scene, camera OUTSIDE" << std::endl;
	CheckRatio( "VCM vs PT, same scene, camera OUTSIDE", encRef,
		RenderMeanRepeated( Scene( kVCM, enclosed ), "VCM                  ", 8 ), 0.90, 1.10 );

	std::cout << "Testing VCM spectral vs PT spectral, same scene, camera OUTSIDE" << std::endl;
	CheckRatio( "VCM spectral vs PT spectral, same scene, camera OUTSIDE", encSpRef,
		RenderMeanRepeated( Scene( kVCMSpectral, enclosed ), "VCM spectral         ", 8 ), 0.90, 1.10 );

	// MONEY ROWS (DL-247).  A THIN global medium (albedo 0.8) with no
	// shell -- see the header for why the medium is thin and how the band
	// was measured.  Each reference is rendered ONCE and shared by its rows.
	const std::string global = SceneBody( false, 0.1, 0.4, 0.5, 2.3 );
	const double kLo = 0.96, kHi = 1.04;

	std::cout << "DL-247 global medium, references:" << std::endl;
	const double ptRef   = RenderMeanRepeated( Scene( kPT64, global ), "PT pel reference     ", 32 );
	const double ptSpRef = RenderMeanRepeated( Scene( kPTSpectral64, global ), "PT spectral reference", 32 );

	// Truncation control: the thin medium's PT answer must not depend on
	// the cap, or the rows below would again compare truncations.
	std::cout << "Testing PT max_volume_bounce 1000 vs 64 (truncation control)" << std::endl;
	CheckRatio( "PT mvb 1000 vs mvb 64, thin global medium", ptRef,
		RenderMeanRepeated( Scene( kPT64Mvb1000, global ), "PT mvb 1000          ", 32 ), kLo, kHi );

	std::cout << "Testing BDPT vs PT, GLOBAL medium" << std::endl;
	CheckRatio( "BDPT vs PT, GLOBAL medium", ptRef,
		RenderMeanRepeated( Scene( kBDPT64, global ), "BDPT                 ", 4 ), kLo, kHi );

	// DIAGNOSTIC, restored (review P2-4): light subpaths reduced to the
	// emitter vertex.  Still an unbiased estimator of the same integral.
	std::cout << "Testing BDPT max_light_depth 1 vs PT, GLOBAL medium" << std::endl;
	CheckRatio( "BDPT max_light_depth 1 vs PT, GLOBAL medium", ptRef,
		RenderMeanRepeated( Scene( kBDPTL1_64, global ), "BDPT L1              ", 4 ), kLo, kHi );

	std::cout << "Testing VCM vs PT, GLOBAL medium" << std::endl;
	CheckRatio( "VCM vs PT, GLOBAL medium", ptRef,
		RenderMeanRepeated( Scene( kVCM64, global ), "VCM                  ", 4 ), kLo, kHi );

	std::cout << "Testing BDPT spectral vs PT spectral, GLOBAL medium" << std::endl;
	CheckRatio( "BDPT spectral vs PT spectral, GLOBAL medium", ptSpRef,
		RenderMeanRepeated( Scene( kBDPTSpectral64, global ), "BDPT spectral        ", 4 ), kLo, kHi );

	std::cout << "Testing VCM spectral vs PT spectral, GLOBAL medium" << std::endl;
	CheckRatio( "VCM spectral vs PT spectral, GLOBAL medium", ptSpRef,
		RenderMeanRepeated( Scene( kVCMSpectral64, global ), "VCM spectral         ", 4 ), kLo, kHi );

	std::cout << std::endl;
	std::cout << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount == 0 ? 0 : 1;
}
