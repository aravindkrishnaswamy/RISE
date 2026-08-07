//////////////////////////////////////////////////////////////////////
//
//  BDPTPhantomStrategyWeightTest.cpp - Pins the 2026-08-01 fix for
//  BDPT's PHANTOM NEE STRATEGY in the (s=0) emitter-hit MIS weight.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-08-01
//  Tabs: 4
//  Comments:
//
//    THE DEFECT (pre-fix).  For an emitter that is NOT in the NEE light
//    set -- `IGeometry::CanBeAreaLight() == false` (e.g. an SDF whose
//    sampling mesh has PROVEN missed-feature cells) or a null-geometry
//    CSG emitter (LuminaryManager never admitted it) -- BDPT's (s=0)
//    strategy set eyeEnd.pdfRev = pdfSelect * pdfPosition = 0
//    (BDPTIntegrator.cpp, the s=0 block).  That zero is a TRUE zero:
//    "the light-sampling family of strategies has zero density here".
//    MISWeight's eye-side walk then hit it with the remap0 convention
//    (`pdfR = (vj.pdfRev != 0) ? vj.pdfRev : 1`), which exists to keep
//    the ratio chain alive across DELTA vertices -- and cannot tell a
//    delta zero from a strategy-does-not-exist zero.  The zero was
//    promoted to 1, so the ratio r_1 = pdfRev/pdfFwd became 1/pdfFwd
//    instead of 0, and a PHANTOM NEE strategy (plus every s>=2
//    light-tracing strategy downstream of it, since the walk multiplies
//    ratios cumulatively) landed in the power-heuristic denominator.
//    Result: weight = 1/(1 + r_1^2 + ...) < 1 for a strategy that is in
//    fact the ONLY one that can generate the path.  PT
//    (PathTracingIntegrator.cpp -- skips its emission-MIS block whenever
//    the emitter is not NEE-sampleable, leaving emissionMiWeight = 1)
//    and VCM (VCMIntegrator.cpp -- `wCamera = pdfSelect > 0 ?
//    wCameraJoint / pdfSelect : 0`) both give the correct FULL weight.
//    Net effect: a systematic ENERGY DEFICIT in BDPT (and in MLT, which
//    constructs a BDPTIntegrator) on exactly these emitters.
//
//    THE FIX.  BDPTVertex gains `lightSamplingStrategyAbsent`, set by
//    the s=0 block precisely when the light-sampling area pdf it just
//    computed is zero, and honoured by MISWeight's eye-side walk (the
//    ratio chain is zero from that vertex on, so every strategy it
//    indexes contributes 0 to sumWeights).  Delta-vertex remap0
//    semantics are untouched -- the flag is never set on a delta vertex,
//    and no delta path's weight changes.
//
//    WHAT THIS TEST PINS.  Three scenes x three integrators.  The two
//    defect scenes use emitters that no integrator can NEE-sample, so
//    all three integrators reduce to the same brute-force
//    BSDF-hit-emission estimator and MUST agree; the control scene uses
//    an ordinary analytic (sphere) area light that IS in the light set,
//    and proves the fix did not disturb the normal path.
//
//    RED-PROOF (measured on this tree at the scenes / sample counts
//    below; mean luma, and BDPT's ratio to PT in parentheses):
//
//      scene           PT mean   BDPT mean PRE-fix   BDPT mean POST-fix
//      emissive CSG    1.17154   0.000503 (0.0004)   1.17324 (1.0010)
//      emissive SDF    0.62337   0.000293 (0.0005)   0.62347 (1.0000)
//      control sphere  0.42970   0.434115 (1.0103)   0.43378 (1.0090)
//
//    i.e. pre-fix BDPT delivered ~1/2400 of the correct energy on both
//    non-NEE-sampleable emitters, while VCM tracked PT to within 0.2 %
//    throughout.  Post-fix BDPT lands on PT.  The control moved by
//    0.08 %, which is inside this harness's own run-to-run spread
//    (repeat runs of the FIXED binary give control BDPT means of
//    0.433779 / 0.433145 / 0.434159 -- the PRE-fix 0.434115 sits inside
//    that envelope, and untouched PT / VCM wander by the same 0.02-0.05 %
//    between runs).  Reverting either half of the fix -- the flag's
//    set-site in BDPTIntegrator.cpp's s=0 block, or the eye-walk gate in
//    MISWeight -- puts BDPT back at ~0.0004 and fails Tests 1 and 2.
//
//    BAND RATIONALE: see kBandDefect / kBandControl below.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#if defined( _WIN32 )
	#include <process.h>
	#include <io.h>
	#define RISE_TEST_GETPID _getpid
	#define RISE_TEST_DUP    _dup
	#define RISE_TEST_DUP2   _dup2
	#define RISE_TEST_CLOSE  _close
	#define RISE_TEST_FILENO _fileno
#else
	#include <unistd.h>
	#define RISE_TEST_GETPID getpid
	#define RISE_TEST_DUP    dup
	#define RISE_TEST_DUP2   dup2
	#define RISE_TEST_CLOSE  close
	#define RISE_TEST_FILENO fileno
#endif

#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Interfaces/ILog.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/Color/Color_Template.h"

using namespace RISE;
using namespace RISE::Implementation;

static int g_failures = 0;
static std::vector<std::string> g_tmpScenes;

static void Check( bool cond, const std::string& what )
{
	if( cond ) {
		std::cout << "  [ok] " << what << "\n";
	} else {
		std::cout << "  [FAIL] " << what << "\n";
		++g_failures;
	}
}

//-----------------------------------------------------------------------------
// Mean / max luma capture.  luma = (r+g+b)/3 in the linear working space --
// no tonemap, no quantization (the render never touches a file).
//-----------------------------------------------------------------------------
class LumaCapture
	: public virtual IRasterizerOutput
	, public virtual Reference
{
public:
	double meanLum;
	double maxLum;

	LumaCapture() : meanLum( 0.0 ), maxLum( 0.0 ) {}

protected:
	virtual ~LumaCapture() {}

public:
	virtual void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}

	virtual void OutputImage( const IRasterImage& pImage, const Rect*, const unsigned int ) override
	{
		const unsigned int w = pImage.GetWidth();
		const unsigned int h = pImage.GetHeight();
		double sum = 0.0;
		maxLum = 0.0;
		for( unsigned int y = 0; y < h; ++y ) {
			for( unsigned int x = 0; x < w; ++x ) {
				const RISEColor c = pImage.GetPEL( x, y );
				const double lum = ( c.base.r + c.base.g + c.base.b ) / 3.0;
				sum += lum;
				if( lum > maxLum ) maxLum = lum;
			}
		}
		meanLum = ( w && h ) ? ( sum / double( w * h ) ) : 0.0;
	}
};

static std::string WriteSceneToTempFile( const std::string& sceneText, const char* tag )
{
	const char* tempDir = std::getenv( "TMPDIR" );
#if defined( _WIN32 )
	if( !tempDir || !*tempDir ) tempDir = std::getenv( "TEMP" );
#endif
	if( !tempDir || !*tempDir ) tempDir = ".";

	std::string path( tempDir );
	if( path.back() != '/' && path.back() != '\\' ) path += '/';
	char filename[256];
	std::snprintf( filename, sizeof(filename), "bdpt_phantom_%s_%d.RISEscene",
		tag, static_cast<int>( RISE_TEST_GETPID() ) );
	path += filename;

	std::ofstream ofs( path.c_str() );
	if( !ofs.is_open() ) return std::string();
	ofs << sceneText;
	ofs.close();
	g_tmpScenes.push_back( path );
	return path;
}

struct RenderResult
{
	bool		rendered;
	double		meanLum;
	double		maxLum;
	std::string	log;		///< stdout captured during the render (GlobalLog's sink)
};

// Renders in-process and captures BOTH the pixel statistics and the render's
// stdout (so a test can assert a specific engine diagnostic fired -- used to
// prove the SDF scene really does trip CanBeAreaLight() == false rather than
// silently rendering an ordinary area light).
static RenderResult RenderScene( const std::string& sceneText, const char* tag )
{
	RenderResult result{ false, 0.0, 0.0, std::string() };

	const std::string scenePath = WriteSceneToTempFile( sceneText, tag );
	if( scenePath.empty() ) return result;
	const std::string capturePath = scenePath + ".stdout.txt";

	std::fflush( stdout );
	const int savedFd = RISE_TEST_DUP( RISE_TEST_FILENO( stdout ) );
	FILE* capFile = std::fopen( capturePath.c_str(), "w" );
	if( capFile ) {
		RISE_TEST_DUP2( RISE_TEST_FILENO( capFile ), RISE_TEST_FILENO( stdout ) );
	}

	{
		IJobPriv* pJob = 0;
		if( RISE_CreateJobPriv( &pJob ) && pJob ) {
			if( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ) ) {
				pJob->RemoveRasterizerOutputs();
				LumaCapture* pCap = new LumaCapture();
				GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "bdpt phantom-strategy luma capture" );
				pJob->GetRasterizer()->AddRasterizerOutput( pCap );
				result.rendered = pJob->Rasterize();
				result.meanLum  = pCap->meanLum;
				result.maxLum   = pCap->maxLum;
				safe_release( pCap );
			}
			safe_release( pJob );
		}
	}

	std::fflush( stdout );
	if( savedFd >= 0 ) {
		RISE_TEST_DUP2( savedFd, RISE_TEST_FILENO( stdout ) );
		RISE_TEST_CLOSE( savedFd );
	}
	if( capFile ) std::fclose( capFile );

	std::ifstream ifs( capturePath );
	if( ifs.is_open() ) {
		std::ostringstream oss;
		oss << ifs.rdbuf();
		result.log = oss.str();
	}
	std::remove( capturePath.c_str() );

	return result;
}

//=============================================================================
// Scene construction.
//
// Shared framing for all three scenes: a diffuse infinite floor lit by a large
// emitter overhead, with the camera tilted DOWN so the emitter itself is
// outside the frustum and every rendered pixel is floor.  That framing is
// chosen deliberately: the defect's magnitude is `1 / (1 + r^2)` with
// `r = 1 / pdfFwd(eyeEnd)`, and pdfFwd on a camera->floor->emitter path is the
// cosine-hemisphere density converted to area measure (order 0.01), so
// r >> 1 (~80 here) and the phantom term utterly dominates the denominator:
// weight ~1.4e-4.  A DIRECT camera view of the emitter sees a much larger
// pdfFwd (the camera's area-measure importance density), so r is order 1 and
// the deficit is a factor of a few rather than a factor of thousands -- still
// plainly wrong, but ~500x milder.  That is why the crash test's direct-view
// maxLum (9.26 vs the correct 42.00, i.e. weight ~0.22, r ~1.9) UNDERSTATED
// this defect rather than missing it: the framing here is chosen to measure
// it where it actually bites.
//
// Every rasterizer is capped at `max_diffuse_bounce 2` so the multi-bounce
// term -- the one place PT / BDPT / VCM legitimately truncate differently --
// cannot be confused with the MIS defect under test.
//=============================================================================
static const char* kScenePrefix =
	"RISE ASCII SCENE 7\n"
	"\n"
	"film\n{\n\twidth 24\n\theight 24\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 1.2 -2.5\n\tlookat 0 0 0.2\n\tup 0 1 0\n\tfov 40.0\n}\n\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n";

static const char* kSceneCommonMaterials =
	"\n"
	"uniformcolor_painter\n{\n\tname albedo\n\tcolor 0.5 0.5 0.5\n}\n\n"
	"lambertian_material\n{\n\tname matte\n\treflectance albedo\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_glow\n\tcolor 3.0 2.5 1.5\n}\n\n"
	"lambertian_luminaire_material\n{\n\tname mat_glow\n\texitance pnt_glow\n\tmaterial matte\n\tscale 4.0\n}\n\n"
	"infiniteplane_geometry\n{\n\tname floor_plane\n\txtile 1.0\n\tytile 1.0\n}\n\n"
	"standard_object\n{\n\tname obj_floor\n\tgeometry floor_plane\n\tmaterial matte\n\tposition 0 0 0\n\torientation -90 0 0\n}\n\n";

// --- Scene A: emissive csg_object (null geometry -> never admitted to the
// luminaries list).  Same construction as the crash test's three-integrator
// scene, lifted out of frame.
static const char* kEmitterCSG =
	"box_geometry\n{\n\tname ceil_a\n\twidth 14.0\n\theight 0.6\n\tdepth 14.0\n}\n\n"
	"box_geometry\n{\n\tname ceil_b\n\twidth 14.0\n\theight 0.6\n\tdepth 14.0\n}\n\n"
	"standard_object\n{\n\tname csg_opA\n\tgeometry ceil_a\n\tmaterial matte\n\tposition 0 0 0\n}\n\n"
	"standard_object\n{\n\tname csg_opB\n\tgeometry ceil_b\n\tmaterial matte\n\tposition 0.05 0 0\n}\n\n"
	"csg_object\n{\n\tname csg_glow\n\tobja csg_opA\n\tobjb csg_opB\n\toperation union\n\tposition 0 3.0 0\n\tmaterial mat_glow\n}\n";

// --- Scene B: emissive sdf_geometry whose sampling mesh PROVABLY misses a
// feature -> SDFGeometry::CanBeAreaLight() == false (SDFGeometry.h).
//
// Construction (object space, and it is arithmetic, not luck -- the detector
// in SDFGeometry::EnsureSamplingStructure flags a cell whose 8 CORNERS share a
// sign while its CENTRE has the other):
//   parts   : sphere r = 0.6 at the origin, plus a plate of half-extents
//             (0.9, 0.05, 0.9) centred at y = +0.1.
//   bbox    : x,z in [-0.9, 0.9] (extent 1.8), y in [-0.6, 0.6] (extent 1.2).
//   grid    : longest = 1.8, sampling_detail = 8 -> hTarget = 0.225;
//             nx = nz = 8 (h = 0.225), ny = ceil(1.2/0.225) = 6 (h = 0.2),
//             so the y corner planes are -0.6, -0.4, ..., 0.6 and the y cell
//             centres are -0.5, -0.3, -0.1, +0.1, +0.3, +0.5.
//   the miss: the plate spans y in [0.05, 0.15] -- it CONTAINS the +0.1 cell
//             centre and CLEARS both bounding corner planes (0.0 and 0.2).  In
//             the corner columns (|x|, |z| >= 0.675) the sphere is far away, so
//             all 8 corners are outside everything while the centre is inside
//             the plate: a provable miss.
// The sphere keeps GetArea() > 0, so CanBeAreaLight() is false specifically
// because of SuspectedMissedFeatureCells() > 0 -- the class under test.  The
// test asserts the engine's own warning fired, so a future change to the
// detector or the grid cannot leave this scene silently testing nothing.
static const char* kEmitterSDF =
	"sdf_geometry\n{\n\tname glow_sdf\n"
	"\tpart sphere union 0  0 0 0  0 0 0  1 1 1  0.60 0 0  0\n"
	"\tpart box union 0  0 0.10 0  0 0 0  1 1 1  0.90 0.05 0.90  0\n"
	"\tsampling_detail 8\n}\n\n"
	"standard_object\n{\n\tname sdf_glow\n\tgeometry glow_sdf\n\tmaterial mat_glow\n\tposition 0 6.0 0\n\tscale 6 6 6\n}\n";

// --- Scene C (CONTROL): an ordinary analytic sphere area light of comparable
// size and placement.  IS admitted to the luminaries list, IS NEE-sampleable,
// so pdfRev > 0 at the s=0 vertex and the new flag is never set -- BDPT must
// be unchanged by the fix.
static const char* kEmitterSphere =
	"sphere_geometry\n{\n\tname glow_sphere\n\tradius 3.6\n}\n\n"
	"standard_object\n{\n\tname sph_glow\n\tgeometry glow_sphere\n\tmaterial mat_glow\n\tposition 0 6.0 0\n}\n";

// Matched depth caps across integrators (see the block comment above).
static const char* kRastPT =
	"pathtracing_pel_rasterizer\n{\n\tsamples 256\n\tshow_luminaires TRUE\n"
	"\tmax_diffuse_bounce 2\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";
static const char* kRastBDPT =
	"bdpt_pel_rasterizer\n{\n\tsamples 64\n\tshow_luminaires TRUE\n"
	"\tmax_eye_depth 4\n\tmax_light_depth 4\n"
	"\tmax_diffuse_bounce 2\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";
static const char* kRastVCM =
	"vcm_pel_rasterizer\n{\n\tsamples 64\n\tshow_luminaires TRUE\n"
	"\tmax_eye_depth 4\n\tmax_light_depth 4\n"
	"\tmax_diffuse_bounce 2\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";

static std::string BuildScene( const char* rasterizer, const char* emitter )
{
	return std::string( kScenePrefix ) + rasterizer + kSceneCommonMaterials + emitter;
}

//=============================================================================
// Tolerance bands.
//
// kBandDefect -- the acceptance band for "BDPT agrees with PT" on the two
// scenes whose emitter is outside the NEE light set.  There, NO integrator can
// light-sample the emitter, so PT / BDPT / VCM all reduce to the same
// brute-force BSDF-hit-emission estimator; the only residual differences are
// Monte-Carlo noise at the sample counts above plus the small multi-bounce
// term that the matched `max_diffuse_bounce 2` bounds.  MEASURED |ratio - 1|
// over repeated post-fix runs: <= 0.0011 (BDPT) and <= 0.0013 (VCM).  The
// band below is ~45x that spread -- it cannot flake on thread scheduling or a
// different core count -- while the defect it guards is a WEIGHT collapse to
// ~0.0004 of the correct value, which misses this band by three orders of
// magnitude.  There is no band between "flaky" and "blind" here; the two
// regimes are nowhere near each other.
//
// kBandControl -- the control scene's PT-vs-BDPT band.  Deliberately looser:
// the control emitter IS NEE-sampleable, so PT and BDPT genuinely use
// different strategy mixes (PT: NEE + BSDF under power-2; BDPT: the full (s,t)
// family), and their agreement is a finite-sample convergence property, not an
// identity -- measured at ratio 1.008-1.010, stable across runs.  The band
// gives that ~1 % offset ~8x headroom.  The control's real no-regression proof
// is the before/after A/B table in this file's header (pre-fix BDPT mean sits
// INSIDE the post-fix run-to-run envelope); the band here is the automated
// don't-drift guard.
//=============================================================================
static const double kBandDefect  = 0.05;	// +/- 5 % of PT's mean
static const double kBandControl = 0.08;	// +/- 8 % of PT's mean

struct Triple { RenderResult pt, bdpt, vcm; };

static Triple RenderTriple( const char* emitter, const char* tag )
{
	Triple r;
	r.pt   = RenderScene( BuildScene( kRastPT,   emitter ), ( std::string( tag ) + "_pt"   ).c_str() );
	r.bdpt = RenderScene( BuildScene( kRastBDPT, emitter ), ( std::string( tag ) + "_bdpt" ).c_str() );
	r.vcm  = RenderScene( BuildScene( kRastVCM,  emitter ), ( std::string( tag ) + "_vcm"  ).c_str() );
	return r;
}

static void ReportTriple( const char* label, const Triple& r )
{
	std::printf( "  A/B-ROW  %-14s  PT   mean=%.6f  max=%.6f\n", label, r.pt.meanLum,   r.pt.maxLum );
	std::printf( "  A/B-ROW  %-14s  BDPT mean=%.6f  max=%.6f   (BDPT/PT = %.4f)\n",
		label, r.bdpt.meanLum, r.bdpt.maxLum,
		( r.pt.meanLum > 0 ) ? ( r.bdpt.meanLum / r.pt.meanLum ) : 0.0 );
	std::printf( "  A/B-ROW  %-14s  VCM  mean=%.6f  max=%.6f   (VCM/PT  = %.4f)\n",
		label, r.vcm.meanLum,  r.vcm.maxLum,
		( r.pt.meanLum > 0 ) ? ( r.vcm.meanLum / r.pt.meanLum ) : 0.0 );
}

static void CheckAgreement( const char* label, const Triple& r, double band )
{
	Check( r.pt.rendered && r.bdpt.rendered && r.vcm.rendered,
		std::string( label ) + ": all three integrators rendered" );
	Check( r.pt.meanLum > 0.0, std::string( label ) + ": PT produced non-zero radiance" );
	if( r.pt.meanLum <= 0.0 ) return;

	const double ratioB = r.bdpt.meanLum / r.pt.meanLum;
	const double ratioV = r.vcm.meanLum  / r.pt.meanLum;

	char msg[256];
	std::snprintf( msg, sizeof(msg),
		"%s: BDPT mean is within +/-%.0f%% of PT (ratio %.4f)", label, band * 100.0, ratioB );
	Check( std::fabs( ratioB - 1.0 ) <= band, msg );

	std::snprintf( msg, sizeof(msg),
		"%s: VCM mean is within +/-%.0f%% of PT (ratio %.4f)", label, band * 100.0, ratioV );
	Check( std::fabs( ratioV - 1.0 ) <= band, msg );
}

//-----------------------------------------------------------------------------
// Test 1: emissive csg_object (null geometry, never admitted as a luminary).
//-----------------------------------------------------------------------------
static void TestEmissiveCSGAgreesAcrossIntegrators()
{
	std::cout << "Test 1: emissive csg_object (null geometry) -- BDPT must match PT/VCM...\n";
	const Triple r = RenderTriple( kEmitterCSG, "csg" );
	ReportTriple( "emissive-CSG", r );
	CheckAgreement( "emissive-CSG", r, kBandDefect );
}

//-----------------------------------------------------------------------------
// Test 2: emissive sdf_geometry with PROVEN missed-feature cells, i.e. the
// CanBeAreaLight() == false class.  The log assertion is what makes this a
// test of that class rather than of an ordinary SDF area light.
//-----------------------------------------------------------------------------
static void TestEmissiveSDFAgreesAcrossIntegrators()
{
	std::cout << "Test 2: emissive sdf_geometry (CanBeAreaLight()==false) -- BDPT must match PT/VCM...\n";
	const Triple r = RenderTriple( kEmitterSDF, "sdf" );
	ReportTriple( "emissive-SDF", r );

	Check( r.pt.log.find( "PROVABLY missed surface" ) != std::string::npos,
		"emissive-SDF: the SDF's sampling mesh really does report missed-feature cells "
		"(CanBeAreaLight() == false), so this scene exercises the intended class" );

	CheckAgreement( "emissive-SDF", r, kBandDefect );
}

//-----------------------------------------------------------------------------
// Test 3 (CONTROL): an ordinary sphere area light -- IS NEE-sampleable, so the
// s=0 vertex has pdfRev > 0 and the fix's flag is never set.  No-regression
// proof that the change is inert on the normal path.
//-----------------------------------------------------------------------------
static void TestNormalSphereEmitterUnaffected()
{
	std::cout << "Test 3 (control): ordinary sphere area light -- BDPT must be unaffected...\n";
	const Triple r = RenderTriple( kEmitterSphere, "sphere" );
	ReportTriple( "control-sphere", r );

	Check( r.pt.log.find( "PROVABLY missed surface" ) == std::string::npos,
		"control-sphere: no missed-feature machinery involved (ordinary analytic emitter)" );

	CheckAgreement( "control-sphere", r, kBandControl );
}

int main()
{
	TestEmissiveCSGAgreesAcrossIntegrators();
	TestEmissiveSDFAgreesAcrossIntegrators();
	TestNormalSphereEmitterUnaffected();

	for( const std::string& f : g_tmpScenes ) { std::remove( f.c_str() ); }

	if( g_failures == 0 ) {
		std::cout << "All BDPTPhantomStrategyWeight tests passed.\n";
		return 0;
	}
	std::cout << g_failures << " BDPTPhantomStrategyWeight check(s) FAILED.\n";
	return 1;
}
