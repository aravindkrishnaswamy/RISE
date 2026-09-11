//////////////////////////////////////////////////////////////////////
//
//  SignalEmitterRecordTest.cpp - Does an EMISSIVE material keyed on a
//    geometry shading signal read that signal LIVE when its light is
//    reached by NEE or as a light-subpath root?
//
//    Slice S3 of docs/SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md §5.
//
//  THE PROPERTY UNDER TEST
//
//    A light sample is a SAMPLED POINT, not a hit:
//    `IObject::UniformRandomPoint` returns a position, a normal and a
//    UV and nothing else, so every emitter record built from one --
//    LightSampler's two NEE records, `SampleLight`'s emission record,
//    BDPT's NM hero + HWSS companion rebuilds, VCM's light-vertex NEE
//    record, and the `type == LIGHT` subpath root -- used to carry
//    DEFAULT `derivatives` and `signals`.  An emissive material whose
//    exitance keys on `curv` or `proximity(r)` therefore read the LIVE
//    value when a camera ray hit the emitter and the documented
//    NEUTRAL value when NEE reached the same point on the same surface
//    in the same frame.  That is a within-scene inconsistency and it
//    reaches PLAIN PT, not just the bidirectional families.
//
//    Slice S3 recovers the payload from a REAL intersection
//    (`LightSampler::ProbeEmitterSurface`).  This suite is the
//    end-to-end witness.
//
//  THE MEASUREMENT, and why it is reference-free
//
//    Every row renders the SAME scene twice and compares the two means
//    to each other -- never to a hand-computed radiance, and never
//    across integrators:
//
//      EXPR    : exitance = 0.2 + 0.8 * <signal>
//      CONTROL : exitance = 0.2 + 0.8 * <the signal's KNOWN CONSTANT>
//
//      assert  | mean(I, EXPR) / mean(I, CONTROL) - 1 | < band
//
//    The two documents differ in ONE token, so geometry, sampling,
//    filtering, MIS and the integrator's own bias all cancel exactly.
//    PT has been the broken reference twice this year (debts 25/26 and
//    the env double-count), which is why no row compares one integrator
//    against another.
//
//  WHY A CONSTANT SIGNAL MAKES THE CONTROL EXACT
//
//    `curv` is `H * scaleHint`, and on ANY sphere H = 1/r while
//    scaleHint is the bbox diagonal 2r*sqrt(3) -- so curv = 2*sqrt(3)
//    ~ 3.464 at every point, independent of the radius
//    (SurfaceCurvatureTest pins exactly this).  `clamp(curv,0,1)`
//    therefore SATURATES to exactly 1 everywhere on the emitter, and
//    the control is the literal `1.0`.  Saturation is what makes the
//    row robust: an SDF sphere's field-derived H only has to land
//    anywhere above 1/(2*sqrt(3)) ~ 0.289 of the true value for the
//    clamp to give the same answer, so the row tests the RECORD, not
//    the curvature estimator's last digit.
//
//    `proximity(r)` is `clamp(1 - d/r, 0, 1)` with `r` a world length.
//    Row C parks a 2x2x2 box so that its near face is a uniform
//    h = 0.2 in front of the whole emitter quad (the box is wider than
//    the quad in both lateral directions, so every quad point's nearest
//    box point is straight ahead) -- the closed form from
//    docs/CROSS_OBJECT_PROXIMITY_DESIGN.md.  With r = 1.0 the signal is
//    exactly 0.8 over the entire emitter, and the control is `0.8`.
//    This row also proves the CROSS-OBJECT triple (`pScene` / `pSelf` /
//    `ptWorld`) is stamped on the probed record: without it
//    `proximity()` returns its neutral 0 and the row goes red.
//
//  SENSITIVITY -- the check cannot pass by insensitivity
//
//    Each family also renders a NEUTRAL document, the same expression
//    with the signal replaced by the value a neutral read would
//    produce (0 for `curv` and for `proximity`).  The suite asserts
//    that `mean(PT, EXPR) / mean(PT, NEUTRAL) - 1` is FAR outside the
//    band.  By construction the neutral emitter is 0.2/1.0 (curv) or
//    0.2/0.84 (proximity) as bright, so EXPR/NEUTRAL is ~5x / ~4.2x --
//    MEASURED at 400 % (curv families) and 318-328 % (proximity),
//    i.e. 64-80x the band.  A regression to neutral cannot hide inside
//    a few percent of noise.
//
//  THE CAMERA SEES ONLY THE RECEIVER
//
//    The emitter sits at (0, 1.8, 1.2); the camera at (0, 0, 3.5) with
//    a 30-degree fov sees +-0.94 of the receiver plane at z = 0.  The
//    emitter is 38 degrees off axis -- well outside the frustum -- so
//    no camera ray hits it and its radiance reaches the film ONLY
//    through NEE (PT, BDPT, VCM), a light-subpath root (BDPT, VCM), or
//    a BSDF-sampled continuation off the receiver.  That last route
//    already used a live record before S3, which only makes the test
//    CONSERVATIVE: it dilutes the signal a correct probe must restore.
//
//  BAND: 5 % relative on the per-channel mean.
//
//    Derived from the observed run-to-run spread at 40x40 / 48 spp
//    (renders seed from the wall clock, and BDPT/VCM splat
//    accumulation is thread-order dependent, so every render is an
//    independent draw).  Measured over FOUR full runs of the row set on
//    an Apple-silicon Mac, worst |EXPR/CONTROL - 1| per row:
//
//      A / PT    0.007  0.030  0.042  0.072   %
//      A / BDPT  0.323  0.360  0.185  0.617   %
//      A / VCM   0.172  0.114  0.071  0.058   %
//      B / PT    0.022  0.016  0.004  0.017   %
//      C / PT    1.558  0.020  0.476  0.865   %
//
//    Worst observed 1.56 %, on row C -- the dimmest family (mean
//    ~0.047 vs ~0.39), so its relative MC noise is the largest.  The
//    5 % band is 3.2x that worst case and 64x below the smallest
//    sensitivity swing (318 %), so it can absorb the noise and cannot
//    absorb a neutral read.  Tightening it below ~2.5 % would make row
//    C flaky; widening it past ~10 % would start to admit a partial
//    neutral read (row C goes only 44-46 % red -- see below).
//
//  RED-PROOF, performed in the slice's own isolated worktree (never
//  the shared checkout), both directions, on the state this file was
//  committed with:
//
//    (1) Force `LightSampler::ProbeEmitterSurface` to return false --
//        i.e. restore the pre-S3 fallback at EVERY site.  Every MONEY
//        row goes red: A/PT 75.3 %, A/BDPT 74.7 %, A/VCM 68.7 %,
//        B/PT 75.3 %, C/PT 45.6 % (7 FAILs, 14 passes).  That PT goes
//        red at all is the point: this slice is not a
//        bidirectional-only fix.  The A and B SENSITIVITY rows also go
//        red under this mutation (23.5 %, i.e. inside the 25 %
//        requirement) -- correctly so, because with the probe off EXPR
//        has collapsed most of the way onto NEUTRAL; the ~23.5 %
//        residual is the BSDF-sampled continuation that hits the
//        emitter through a REAL record and was never neutral.
//
//    (2) Suppress ONLY the cross-object triple
//        (`signals.pScene` / `pSelf` / `ptWorld`) inside the probe,
//        keeping everything else.  Row C alone goes red (44.1 %);
//        A and B stay green at 0.05 / 0.18 / 0.06 / 0.02 %.  This is
//        what pins that row C is testing the triple and not just the
//        own-surface half.
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <vector>
#include <cmath>
#include <string>
#include <algorithm>
#ifdef _WIN32
	#include <process.h>		// _getpid()
	#define getpid _getpid
#else
	#include <unistd.h>			// getpid()
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

static void Check( bool condition, const char* testName )
{
	if( condition ) {
		passCount++;
	} else {
		failCount++;
		std::cout << "  FAIL: " << testName << std::endl;
	}
}

//////////////////////////////////////////////////////////////////////
// CapturingRasterizerOutput -- the in-process capture the balance
// suites use.  Stores the LINEAR radiance buffer, so nothing is
// tone-mapped or quantised before the means are taken.
//
// `oidn_denoise FALSE` on every rasterizer string below is not
// optional: the default `OutputDenoisedImage` forwards POST-denoise
// pixels to `OutputImage`, so with the denoiser on this suite would be
// measuring OIDN, not the integrator (the trap swept across seven
// suites in 2026-08).
//////////////////////////////////////////////////////////////////////
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

struct ImageStats
{
	double mean[3];
	bool   valid;
};

//! Composited-over-black radiance (base * coverage-alpha) -- the
//! convention-independent quantity, because PT and BDPT resolve
//! partial-coverage pixels with different alpha conventions.  Every
//! pixel here is full-coverage receiver, so this is a no-op; it is kept
//! so the statistic means the same thing as in the balance suites.
static ImageStats ComputeStats( const CapturingRasterizerOutput& cap )
{
	ImageStats s{};
	if( cap.pixels.empty() ) {
		return s;
	}
	double sum[3] = { 0, 0, 0 };
	for( const RISEColor& c : cap.pixels ) {
		const double cov = c.a;
		sum[0] += c.base.r * cov;
		sum[1] += c.base.g * cov;
		sum[2] += c.base.b * cov;
	}
	const double n = double( cap.pixels.size() );
	for( int c = 0; c < 3; c++ ) {
		s.mean[c] = sum[c] / n;
	}
	s.valid = true;
	return s;
}

static std::string WriteSceneToTempFile( const std::string& sceneText, const char* tag )
{
	char path[512];
	std::snprintf( path, sizeof(path),
		"/tmp/signal_emitter_record_%s_%d.RISEscene",
		tag, static_cast<int>(::getpid()) );

	std::ofstream ofs( path );
	if( !ofs.is_open() ) {
		return std::string();
	}
	ofs << sceneText;
	ofs.close();
	return std::string( path );
}

static ImageStats RenderAndComputeStats( const char* scenePath )
{
	ImageStats result{};

	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) {
		return result;
	}

	if( !pJob->LoadAsciiSceneViaCst( scenePath ) ) {
		safe_release( pJob );
		return result;
	}

	pJob->RemoveRasterizerOutputs();

	CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "test capture output" );
	pJob->GetRasterizer()->AddRasterizerOutput( pCap );

	const bool bRendered = pJob->Rasterize();
	if( !bRendered ) {
		safe_release( pCap );
		safe_release( pJob );
		return result;
	}

	result = ComputeStats( *pCap );

	safe_release( pCap );
	safe_release( pJob );
	return result;
}

//////////////////////////////////////////////////////////////////////
// SCENE ASSEMBLY
//
// Every document is built from the same four pieces, so two documents
// in one row differ in EXACTLY the exitance expression token.
//////////////////////////////////////////////////////////////////////

//! Receiver + camera.  The receiver spans [-4,4]^2 so the 30-degree
//! camera (which sees +-0.94 at z = 0) is filled edge to edge by it --
//! every pixel in the mean is receiver, none is background.
static const char* kCommonHead =
	"RISE ASCII SCENE 7\n"
	"film\n"
	"{\n"
	"\twidth 40\n"
	"\theight 40\n"
	"}\n"
	"\n"
	"pinhole_camera\n"
	"{\n"
	"\tlocation 0 0 3.5\n"
	"\tlookat 0 0 0\n"
	"\tup 0 1 0\n"
	"\tfov 30.0\n"
	"}\n"
	"\n"
	"uniformcolor_painter\n"
	"{\n"
	"\tname pnt_albedo\n"
	"\tcolor 0.6 0.6 0.6\n"
	"}\n"
	"\n"
	"lambertian_material\n"
	"{\n"
	"\tname mat_diffuse\n"
	"\treflectance pnt_albedo\n"
	"}\n"
	"\n"
	"clippedplane_geometry\n"
	"{\n"
	"\tname g_receiver\n"
	"\tpta -4 -4 0\n"
	"\tptb 4 -4 0\n"
	"\tptc 4 4 0\n"
	"\tptd -4 4 0\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_receiver\n"
	"\tgeometry g_receiver\n"
	"\tmaterial mat_diffuse\n"
	"}\n"
	"\n";

//! The emitter material.  `scale` is chosen per family so the receiver
//! mean lands around 0.3-0.6 in linear radiance -- bright enough that
//! the mean is dominated by signal rather than by the black tail, dim
//! enough that nothing clips.
static std::string MakeEmitterMaterial( const char* exitanceExpr, double scale )
{
	char buf[1024];
	std::snprintf( buf, sizeof(buf),
		"expression_painter\n"
		"{\n"
		"\tname p_exitance\n"
		"\texpr vec3( %s, %s, %s )\n"
		"}\n"
		"\n"
		"lambertian_luminaire_material\n"
		"{\n"
		"\tname mat_lum\n"
		"\texitance p_exitance\n"
		"\tscale %g\n"
		"\tmaterial none\n"
		"}\n"
		"\n",
		exitanceExpr, exitanceExpr, exitanceExpr, scale );
	return std::string( buf );
}

//! FAMILY A: the emitter is an SDF sphere (`sdf_geometry`, one sphere
//! part).  The SDF family answers `curv` from its own field, and is the
//! family docs/SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md §5 names.
static const char* kEmitterSdfSphere =
	"sdf_geometry\n"
	"{\n"
	"\tname g_lum\n"
	"\tpart sphere union 0  0 0 0  0 0 0  1 1 1  0.5 0 0  0\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_lum\n"
	"\tgeometry g_lum\n"
	"\tmaterial mat_lum\n"
	"\tposition 0 1.8 1.2\n"
	"}\n"
	"\n";

//! FAMILY B: the same row on an ANALYTIC primitive, so the probe is
//! shown to work for a non-SDF luminary -- `sphere_geometry` reports
//! `curv` through the Weingarten map (dndu/dndv), a completely
//! different code path from the SDF field's Hessian, and the two must
//! both arrive at the record.
static const char* kEmitterAnalyticSphere =
	"sphere_geometry\n"
	"{\n"
	"\tname g_lum\n"
	"\tradius 0.5\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_lum\n"
	"\tgeometry g_lum\n"
	"\tmaterial mat_lum\n"
	"\tposition 0 1.8 1.2\n"
	"}\n"
	"\n";

//! FAMILY C: a flat emitter quad with a wide box parked a uniform
//! h = 0.2 in FRONT of it (further from the receiver, so it never
//! occludes the light path; it is identical between the two documents
//! in any case and cancels).
//!
//! The box spans x in [-1,1], y in [0.8,2.8], z in [1.4,3.4]; the
//! emitter quad is x in [-0.4,0.4], y in [1.4,2.2], z = 1.2 -- inside
//! the box's lateral extent everywhere, so the nearest box point from
//! ANY point of the quad is straight along +z at exactly 0.2.
//! `proximity(1.0)` is therefore exactly 1 - 0.2/1.0 = 0.8 over the
//! whole emitter.
//!
//! The receiver is the only other candidate and sits 1.2 away -- beyond
//! the 1.0 radius -- so it cannot perturb the closed form.
static const char* kEmitterQuadWithNeighbour =
	"clippedplane_geometry\n"
	"{\n"
	"\tname g_lum\n"
	"\tpta -0.4 -0.4 0\n"
	"\tptb 0.4 -0.4 0\n"
	"\tptc 0.4 0.4 0\n"
	"\tptd -0.4 0.4 0\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_lum\n"
	"\tgeometry g_lum\n"
	"\tmaterial mat_lum\n"
	"\tposition 0 1.8 1.2\n"
	"}\n"
	"\n"
	"box_geometry\n"
	"{\n"
	"\tname g_neighbour\n"
	"\twidth 2.0\n"
	"\theight 2.0\n"
	"\tdepth 2.0\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_neighbour\n"
	"\tgeometry g_neighbour\n"
	"\tmaterial mat_diffuse\n"
	"\tposition 0 1.8 2.4\n"
	"}\n"
	"\n";

//! The three integrator chunks.  They differ ONLY in the rasterizer;
//! every one carries the same shader, the same `pixel_filter box` and
//! the same `oidn_denoise FALSE`, so nothing but the integrator varies
//! between them.  (No row compares one to another -- see the header --
//! but keeping them matched means a row's EXPR and CONTROL can never
//! differ by a rasterizer setting either.)
static const char* kRasterizerPT =
	"standard_shader\n"
	"{\n"
	"\tname global\n"
	"\tshaderop DefaultPathTracing\n"
	"}\n"
	"\n"
	"pathtracing_pel_rasterizer\n"
	"{\n"
	"\tsamples 48\n"
	"\trr_min_depth 8\n"
	"\tpixel_filter box\n"
	"\toidn_denoise FALSE\n"
	"}\n"
	"\n"
	"file_rasterizeroutput\n"
	"{\n"
	"\tpattern /tmp/signal_emitter_record_unused\n"
	"\ttype PNG\n"
	"\tbpp 8\n"
	"\tcolor_space sRGB\n"
	"}\n";

static const char* kRasterizerBDPT =
	"standard_shader\n"
	"{\n"
	"\tname global\n"
	"\tshaderop DefaultPathTracing\n"
	"}\n"
	"\n"
	"bdpt_pel_rasterizer\n"
	"{\n"
	"\tmax_eye_depth 3\n"
	"\tmax_light_depth 3\n"
	"\tsamples 48\n"
	"\tpixel_filter box\n"
	"\toidn_denoise FALSE\n"
	"}\n"
	"\n"
	"file_rasterizeroutput\n"
	"{\n"
	"\tpattern /tmp/signal_emitter_record_unused\n"
	"\ttype PNG\n"
	"\tbpp 8\n"
	"\tcolor_space sRGB\n"
	"}\n";

static const char* kRasterizerVCM =
	"standard_shader\n"
	"{\n"
	"\tname global\n"
	"\tshaderop DefaultPathTracing\n"
	"}\n"
	"\n"
	"vcm_pel_rasterizer\n"
	"{\n"
	"\tmax_eye_depth 3\n"
	"\tmax_light_depth 3\n"
	"\tsamples 48\n"
	"\tmerge_radius 0.0\n"
	"\tvc_enabled true\n"
	"\tvm_enabled true\n"
	"\tpixel_filter box\n"
	"\toidn_denoise FALSE\n"
	"}\n"
	"\n"
	"file_rasterizeroutput\n"
	"{\n"
	"\tpattern /tmp/signal_emitter_record_unused\n"
	"\ttype PNG\n"
	"\tbpp 8\n"
	"\tcolor_space sRGB\n"
	"}\n";

static std::string MakeScene(
	const char* emitterChunks,
	const char* exitanceExpr,
	double scale,
	const char* rasterizerChunk )
{
	std::string s( kCommonHead );
	s += MakeEmitterMaterial( exitanceExpr, scale );
	s += emitterChunks;
	s += rasterizerChunk;
	return s;
}

static ImageStats Render(
	const char* emitterChunks,
	const char* exitanceExpr,
	double scale,
	const char* rasterizerChunk,
	const char* tag )
{
	const std::string text = MakeScene( emitterChunks, exitanceExpr, scale, rasterizerChunk );
	const std::string path = WriteSceneToTempFile( text, tag );
	if( path.empty() ) {
		return ImageStats{};
	}
	const ImageStats s = RenderAndComputeStats( path.c_str() );
	std::remove( path.c_str() );
	return s;
}

//! Worst per-channel |a/b - 1|, with an absolute floor so a black
//! render cannot divide by zero (a black render fails the `valid` /
//! brightness checks separately).
static double WorstRelDiff( const ImageStats& a, const ImageStats& b )
{
	double worst = 0;
	for( int c = 0; c < 3; c++ ) {
		const double denom = std::fmax( std::fabs( b.mean[c] ), 1e-6 );
		worst = std::fmax( worst, std::fabs( a.mean[c] - b.mean[c] ) / denom );
	}
	return worst;
}

static void PrintMeans( const char* label, const ImageStats& s )
{
	if( !s.valid ) {
		std::cout << "    " << label << ": INVALID (render failed)" << std::endl;
		return;
	}
	std::cout << "    " << label << ": mean=("
	          << s.mean[0] << ", " << s.mean[1] << ", " << s.mean[2] << ")" << std::endl;
}

//! THE BAND.  See the file header for the derivation.
static const double kBand = 0.05;

//! The sensitivity rows must sit at least this far outside the band, so
//! "the signal is live" cannot be confused with "the signal does not
//! matter here".  The constructed swing is 76-80 %; requiring 5x the
//! band (25 %) leaves an enormous margin and is the figure design §6.1
//! names.
static const double kMinSensitivity = 0.25;

//////////////////////////////////////////////////////////////////////
// One family = one emitter + one signal + its known constant.
//////////////////////////////////////////////////////////////////////
struct Family
{
	const char*	name;
	const char*	emitterChunks;
	const char*	exprSignal;		//!< the live expression
	const char*	exprControl;	//!< the same, signal replaced by its KNOWN CONSTANT
	const char*	exprNeutral;	//!< the same, signal replaced by its NEUTRAL value
	double		scale;
	bool		bidirectional;	//!< also run BDPT and VCM
};

static void RunFamily( const Family& f )
{
	std::cout << "  [" << f.name << "]" << std::endl;

	struct Row { const char* label; const char* rasterizer; };
	const Row rows[3] = {
		{ "PT",   kRasterizerPT },
		{ "BDPT", kRasterizerBDPT },
		{ "VCM",  kRasterizerVCM },
	};
	const int nRows = f.bidirectional ? 3 : 1;

	for( int i = 0; i < nRows; i++ ) {
		const ImageStats expr = Render(
			f.emitterChunks, f.exprSignal, f.scale, rows[i].rasterizer, "expr" );
		const ImageStats ctrl = Render(
			f.emitterChunks, f.exprControl, f.scale, rows[i].rasterizer, "ctrl" );

		std::string lbl = std::string( f.name ) + " / " + rows[i].label;

		Check( expr.valid && ctrl.valid, ( lbl + ": both renders produced an image" ).c_str() );
		if( !expr.valid || !ctrl.valid ) {
			continue;
		}
		PrintMeans( ( lbl + " EXPR   " ).c_str(), expr );
		PrintMeans( ( lbl + " CONTROL" ).c_str(), ctrl );

		// A dark render would make the ratio meaningless -- and would
		// itself be the bug (an emitter reaching nothing).
		Check( ctrl.mean[0] > 0.02,
		       ( lbl + ": the control render is actually lit (mean > 0.02)" ).c_str() );

		const double d = WorstRelDiff( expr, ctrl );
		std::cout << "      worst |EXPR/CONTROL - 1| = " << ( d * 100.0 ) << " %" << std::endl;
		Check( d < kBand,
		       ( lbl + ": MONEY -- the signal-keyed emitter matches its baked-constant "
		               "twin, i.e. the emitter record read the signal LIVE" ).c_str() );
	}

	// SENSITIVITY, on the PT row: the same expression with the signal
	// replaced by what a NEUTRAL read would give must move the mean far
	// outside the band.  Without this the whole family could pass by
	// being insensitive to the signal in the first place.
	{
		const ImageStats expr = Render(
			f.emitterChunks, f.exprSignal, f.scale, kRasterizerPT, "expr" );
		const ImageStats neut = Render(
			f.emitterChunks, f.exprNeutral, f.scale, kRasterizerPT, "neut" );
		std::string lbl = std::string( f.name ) + " / PT sensitivity";
		Check( expr.valid && neut.valid, ( lbl + ": both renders produced an image" ).c_str() );
		if( expr.valid && neut.valid ) {
			PrintMeans( ( lbl + " NEUTRAL" ).c_str(), neut );
			const double d = WorstRelDiff( expr, neut );
			std::cout << "      |EXPR/NEUTRAL - 1| = " << ( d * 100.0 ) << " %" << std::endl;
			Check( d > kMinSensitivity,
			       ( lbl + ": the signal moves the mean far outside the band, so a "
			               "neutral read cannot hide inside it" ).c_str() );
		}
	}
}

int main()
{
	std::cout << "SignalEmitterRecordTest -- emissive materials keyed on a geometry signal,"
	          << std::endl
	          << "reached ONLY through NEE / the light-subpath root"
	          << std::endl
	          << "(docs/SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md §5, slice S3)"
	          << std::endl << std::endl;

	// curv on a sphere = 2*sqrt(3) ~ 3.464, radius-independent, so
	// clamp(curv,0,1) is exactly 1 and the control is the literal 1.0.
	static const Family kSdfCurv = {
		"A: SDF sphere emitter, exitance keyed on curv",
		kEmitterSdfSphere,
		"0.2 + 0.8*clamp(curv,0,1)",
		"0.2 + 0.8*1.0",
		"0.2 + 0.8*0.0",
		60.0,
		true
	};

	static const Family kAnalyticCurv = {
		"B: analytic sphere_geometry emitter, exitance keyed on curv",
		kEmitterAnalyticSphere,
		"0.2 + 0.8*clamp(curv,0,1)",
		"0.2 + 0.8*1.0",
		"0.2 + 0.8*0.0",
		60.0,
		false
	};

	// proximity(1.0) = 1 - 0.2/1.0 = 0.8 exactly, over the whole emitter
	// quad -- see kEmitterQuadWithNeighbour for the geometry that makes
	// the closed form uniform.
	static const Family kProximity = {
		"C: quad emitter + box neighbour at h=0.2, exitance keyed on proximity(1.0)",
		kEmitterQuadWithNeighbour,
		"0.2 + 0.8*proximity(1.0)",
		"0.2 + 0.8*0.8",
		"0.2 + 0.8*0.0",
		60.0,
		false
	};

	RunFamily( kSdfCurv );
	RunFamily( kAnalyticCurv );
	RunFamily( kProximity );

	std::cout << std::endl
	          << "Passed: " << passCount << "  Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
