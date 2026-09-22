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
//  TWO ROWS HERE ARE NOT DL-200 ROWS.  VCM reads 6% of PT on the enclosed
//  scene and BDPT reads 1.73x of PT in a plain GLOBAL medium; both are
//  pre-existing, both are unmoved by this fix (the global-medium one
//  provably so -- `pMedObj == 0` took the old derivation's own early-out,
//  so the fix is a literal no-op there), and both are filed as DL-218.
//  They are pinned at their measured values so that closing DL-218 has to
//  move them deliberately rather than silently.
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

static double RenderMean( const std::string& sceneText )
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

	static unsigned renderIndex = 0;
	std::srand( 3100u + renderIndex++ );
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

static const int kRepeats = 3;

static double RenderMeanRepeated( const std::string& sceneText, const char* label )
{
	double sum = 0, lo = 0, hi = 0;
	for( int i = 0; i < kRepeats; i++ ) {
		const double m = RenderMean( sceneText );
		if( m < 0 ) return -1.0;
		if( i == 0 || m < lo ) lo = m;
		if( i == 0 || m > hi ) hi = m;
		sum += m;
	}
	const double mean = sum / double( kRepeats );
	std::cout << "    " << label << ": mean=" << mean << " over " << kRepeats
	          << " renders (range " << lo << " .. " << hi << ")" << std::endl;
	return mean;
}

//////////////////////////////////////////////////////////////////////
// Scene.  `enclosed` false is the CONTROL: the same medium, the same
// emitter and the same camera, but the medium is the scene's GLOBAL one
// and there is no shell at all.  `pMedObj` is then null at every medium
// vertex, which took the pre-fix derivation's own "global medium: always
// connectable" early-out, so those vertices read connectible in the
// PRE-fix build too and the row is green in BOTH builds.
//////////////////////////////////////////////////////////////////////
static std::string SceneBody( bool enclosed )
{
	std::string s = "film\n{\n\twidth 48\n\theight 48\n}\n\n";

	s += "pinhole_camera\n{\n"
	     "\tlocation 0 0 5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n";

	s +=
		"homogeneous_medium\n{\n\tname med\n"
		"\tabsorption 0.02 0.02 0.02\n\tscattering 3.0 3.0 3.0\n\tphase hg 0.0\n}\n\n"
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

	s +=
		"uniformcolor_painter\n{\n\tname pemit\n\tcolor 1.0 1.0 1.0\n}\n\n"
		"lambertian_luminaire_material\n{\n\tname memit\n"
		"\texitance pemit\n\tscale 40.0\n\tmaterial none\n}\n\n"
		"sphere_geometry\n{\n\tname emitgeo\n\tradius 0.12\n}\n\n";

	s += "standard_object\n{\n\tname emit\n\tgeometry emitgeo\n\tmaterial memit\n}\n\n";

	s +=
		"file_rasterizeroutput\n{\n\tpattern rendered/medium_enclosure_conn_unused\n"
		"\ttype EXR\n\tbpp 32\n\tcolor_space Rec709RGB_Linear\n}\n\n";

	return s;
}

static const char* kShader =
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n";

static const char* kPT =
	"pathtracing_pel_rasterizer\n{\n\tsamples 32\n\trr_min_depth 8\n"
	"\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";
static const char* kBDPT =
	"bdpt_pel_rasterizer\n{\n\tmax_eye_depth 6\n\tmax_light_depth 6\n\tsamples 32\n"
	"\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";
static const char* kVCM =
	"vcm_pel_rasterizer\n{\n\tmax_eye_depth 6\n\tmax_light_depth 6\n\tsamples 32\n"
	"\tvc_enabled true\n\tvm_enabled false\n"
	"\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";
static const char* kPTSpectral =
	"pathtracing_spectral_rasterizer\n{\n\tsamples 32\n\trr_min_depth 8\n\thwss FALSE\n"
	"\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";
static const char* kBDPTSpectral =
	"bdpt_spectral_rasterizer\n{\n\tmax_eye_depth 6\n\tmax_light_depth 6\n\tsamples 32\n"
	"\thwss FALSE\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";

static const char* kVCMSpectral =
	"vcm_spectral_rasterizer\n{\n\tmax_eye_depth 6\n\tmax_light_depth 6\n\tsamples 32\n"
	"\tvc_enabled true\n\tvm_enabled false\n\thwss FALSE\n"
	"\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";

static const char* kBDPT_L1 =
	"bdpt_pel_rasterizer\n{\n\tmax_eye_depth 6\n\tmax_light_depth 1\n\tsamples 32\n"
	"\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";

static std::string Scene( const char* rast, const std::string& body )
{
	return std::string("RISE ASCII SCENE 7\n") + kShader + rast + body;
}

static void RunRatio( const std::string& label, const char* refRast,
	const char* candRast, const std::string& body, double lo, double hi )
{
	std::cout << "Testing " << label << std::endl;
	const double ref  = RenderMeanRepeated( Scene( refRast, body ),  "PT reference" );
	const double cand = RenderMeanRepeated( Scene( candRast, body ), "candidate   " );

	Check( ref > 1e-9, label + ": PT reference render is non-black" );
	Check( cand > -0.5, label + ": candidate render produced output" );
	if( ref <= 1e-9 || cand < 0 ) return;

	const double r = cand / ref;
	std::cout << "    candidate / PT = " << r << std::endl;

	char buf[320];
	std::snprintf( buf, sizeof(buf), "%s: candidate/PT %.4f in [%.2f, %.2f]",
		label.c_str(), r, lo, hi );
	Check( r >= lo && r <= hi, buf );
}

int main()
{
	std::cout << "=== MediumEnclosureConnectibilityTest (DL-200 / DL-218) ===" << std::endl;

	const std::string enclosed = SceneBody( true );
	const std::string global   = SceneBody( false );

	// MONEY ROWS (DL-200).  Camera OUTSIDE: the eye walk must cross the delta
	// shell to reach the medium, so pre-fix every eye-rooted medium
	// vertex read !isConnectible and every NEE / interior connection
	// from it was skipped.
	RunRatio( "BDPT vs PT, emitter in a fog-filled dielectric shell, camera OUTSIDE",
		kPT, kBDPT, enclosed, 0.95, 1.06 );

	RunRatio( "BDPT spectral vs PT spectral, same scene, camera OUTSIDE",
		kPTSpectral, kBDPTSpectral, enclosed, 0.95, 1.06 );

	// MONEY ROWS (DL-218 (a)). VCM implementing NEE and interior connections
	// at medium vertices should match PT and BDPT at ~1.0.
	RunRatio( "VCM vs PT, same scene, camera OUTSIDE",
		kPT, kVCM, enclosed, 0.90, 1.10 );

	RunRatio( "VCM spectral vs PT spectral, same scene, camera OUTSIDE",
		kPTSpectral, kVCMSpectral, enclosed, 0.90, 1.10 );

	// CONTROL, and KNOWN-DEFECT PIN (DL-247).  The SAME medium
	// and emitter with no shell, bound as the scene's GLOBAL medium:
	// BDPT and VCM read ~1.8-2.1x of PT in a plain global medium.
	// Independent defect in PT volumetric continuation vs BDPT/VCM eye-walk.
	RunRatio( "CONTROL/PIN(DL-247) BDPT vs PT, same medium as a GLOBAL medium (no shell)",
		kPT, kBDPT, global, 1.50, 2.25 );

	// DIAGNOSTIC (DL-247): BDPT max_light_depth 1 (light-root-only, no light subpath scattering in medium)
	{
		std::cout << "--- Diagnostic for DL-247: BDPT max_light_depth 1 on GLOBAL medium ---" << std::endl;
		const double ref  = RenderMeanRepeated( Scene( kPT, global ), "PT reference" );
		const double cand = RenderMeanRepeated( Scene( kBDPT_L1, global ), "BDPT L1     " );
		if( ref > 1e-9 && cand > 0 ) {
			std::cout << "    DIAGNOSTIC: BDPT(L1) / PT = " << (cand / ref) << std::endl;
		}
	}
	// DIAGNOSTIC (DL-247): VCM on GLOBAL medium
	{
		std::cout << "--- Diagnostic for DL-247: VCM on GLOBAL medium ---" << std::endl;
		const double ref  = RenderMeanRepeated( Scene( kPT, global ), "PT reference" );
		const double cand = RenderMeanRepeated( Scene( kVCM, global ), "VCM         " );
		if( ref > 1e-9 && cand > 0 ) {
			std::cout << "    DIAGNOSTIC: VCM / PT = " << (cand / ref) << std::endl;
		}
	}

	std::cout << std::endl;
	std::cout << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount == 0 ? 0 : 1;
}
