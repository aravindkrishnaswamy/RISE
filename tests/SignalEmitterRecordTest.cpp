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
//    `Po` (row D) is the object-space hit point, and on a sphere of
//    radius 0.5 about its own object origin `|Po|^2 = 0.25` EXACTLY at
//    every surface point, so `clamp(8*|Po|^2, 0, 1)` saturates to 1
//    with 2x headroom.  The control is again `1.0`.
//
//  THE FIVE FAMILIES, and what each one is the only witness for
//
//    A  SDF sphere, `curv`.  The broadest row set: PT / BDPT / VCM in
//       RGB, all three again SPECTRAL, and BDPT-spectral once more with
//       HWSS on.  The spectral rows are the ONLY coverage of three of
//       the seven converted sites -- `EvaluateDirectLightingNM`, the NM
//       hero `Le` rebuild in `GenerateLightSubpathImpl`, and the HWSS
//       companion-wavelength rebuild beside it -- since nothing in an
//       RGB rasterizer reaches any of them.
//    B  analytic `sphere_geometry`, `curv`.  A completely different
//       curvature path (the Weingarten map via dndu/dndv, not an SDF
//       field Hessian) has to arrive at the record too.
//    C  flat quad + box neighbour, `proximity(1.0)`.  The cross-object
//       triple.  Run under BDPT and VCM as well as PT, because those
//       two are the only rows that carry the triple through
//       `ProbeEmitterSurfaceAlongNormal` rather than through the NEE
//       `ProbeEmitterSurface`.
//    D  SDF sphere, `Po`, with NO SIGNAL PAINTER ANYWHERE IN THE SCENE.
//       `Po` is not signal state, so it must not ride the probe's
//       process-wide gate; this family renders with that gate CLOSED
//       and still has to pass, and its extra gate-invariance block
//       renders the same emitter with an unrelated `curv` painter added
//       to the RECEIVER (which opens the gate without changing a pixel
//       of the receiver) and requires the emitter's mean not to move.
//    E  SDF sphere at `epsilon 0.002`, `curv`.  The luminary's own
//       `SelfHitRootFloor` is then four times the flat standoff the
//       normal-aligned probe used to use, so the probe was marched past
//       the face it was aimed at and refused -- PT live, BDPT and VCM
//       neutral.
//
//  SENSITIVITY -- the check cannot pass by insensitivity
//
//    Each family also renders a NEUTRAL document, the same expression
//    with the signal replaced by the value a neutral read would
//    produce (0 for `curv`, for `proximity` and for `Po`).  The suite
//    asserts that `mean(PT, EXPR) / mean(PT, NEUTRAL) - 1` is FAR
//    outside the band.  By construction the neutral emitter is 0.2/1.0
//    (curv, Po) or 0.2/0.84 (proximity) as bright, so EXPR/NEUTRAL is
//    ~5x / ~4.2x -- MEASURED at 399.5-400.3 % (curv and Po families)
//    and 316-326 % (proximity), i.e. 63-80x the band.  A regression to
//    neutral cannot hide inside a few percent of noise.
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
//    Derived from the observed run-to-run spread at 40x40 (renders seed
//    from the wall clock, and BDPT/VCM splat accumulation is
//    thread-order dependent, so every render is an independent draw).
//    FIVE full runs of the row set on an Apple-silicon Mac, worst
//    |EXPR/CONTROL - 1| per row, in percent:
//
//      A / PT                 0.043  0.063  0.039  0.018  0.007
//      A / BDPT               0.511  0.178  0.376  0.208  0.043
//      A / VCM                0.115  0.319  0.019  0.041  0.041
//      A / PT-spectral        1.442  0.771  0.801  0.491  0.976
//      A / BDPT-spectral      0.605  1.389  1.267  1.159  0.827
//      A / VCM-spectral       0.711  1.221  0.411  0.854  1.063
//      A / BDPT-spec/HWSS     0.242  0.190  0.278  0.253  0.331
//      B / PT                 0.082  0.024  0.058  0.008  0.036
//      C / PT                 0.854  0.499  0.149  0.548  0.338
//      C / BDPT               0.065  0.023  0.010  0.014  0.035
//      C / VCM                0.011  0.010  0.006  0.002  0.038
//      D / PT                 0.084  0.022  0.041  0.032  0.057
//      D / BDPT               0.181  0.428  0.416  0.245  0.355
//      D / VCM                0.214  0.264  0.271  0.018  0.144
//      D gate-inv CONTROL     0.038  0.052  0.030  0.030  0.038
//      D gate-inv EXPR        0.012  0.003  0.026  0.035  0.006
//      E / PT                 0.044  0.030  0.011  0.007  0.065
//      E / BDPT               0.388  0.377  0.232  0.229  0.280
//      E / VCM                0.064  0.047  0.280  0.046  0.052
//
//    Worst observed 1.44 %, on A / PT-spectral.  The 5 % band is 3.5x
//    that and 63x below the smallest sensitivity swing (316 %), so it
//    can absorb the noise and cannot absorb a neutral read.
//
//    SAMPLE COUNTS ARE NOT UNIFORM, and both departures from the base
//    48 are deliberate:
//
//      * ROW C RUNS AT 384.  It is the dimmest family by an order of
//        magnitude (mean ~0.048 against ~0.39), so its relative MC
//        noise is the largest in the suite.  Nine runs at the original
//        48 samples spread to 2.89 %, and five runs at 192 still
//        reached 1.67 %; 384 brings the worst of five to 0.85 %.
//      * THE NON-HWSS SPECTRAL ROWS RUN AT 1024.  `spectral_samples 1`
//        draws ONE wavelength per pixel sample out of 380-720 nm, so
//        the per-channel mean carries a CHROMATIC error the RGB rows do
//        not have -- 4.0 % (BDPT-spectral) and 5.2 % (VCM-spectral) at
//        48 samples, against a 5 % band, i.e. noise indistinguishable
//        from a failure.  384 brought it to 2.3 %, 1024 to 1.44 %.  The
//        HWSS row needs none of that (0.40 % at 48): a hero wavelength
//        with seven companions averages the bundle WITHIN each sample,
//        which is exactly the variance at issue.  It is given 96 for
//        headroom at negligible cost.
//
//    Whole-suite runtime at these counts is ~35 s.
//
//  RED-PROOF, performed in the slice's own isolated worktree (never
//  the shared checkout), on the state this file was committed with.
//  Each mutation was reverted with `git checkout --` and
//  `git status --short` checked clean before the next.
//
//    THE CHOKE POINT IS `EmitterProbeWanted()`, not
//    `ProbeEmitterSurface`.  An earlier draft of this header said to
//    force `ProbeEmitterSurface` to return false, which only disables
//    the two NEE sites -- the BDPT light-subpath root, its two NM twins
//    and VCM's light vertex all reach the probe through
//    `ProbeEmitterSurfaceAlongNormal`, a separate public entry point
//    with its own gate check.  Forcing `EmitterProbeWanted()` to return
//    false is the one edit that restores the pre-S3 fallback at ALL
//    SEVEN sites.
//
//    (1) `EmitterProbeWanted()` -> false.  Every MONEY row in every
//        family but D goes red; D (which keys on `Po`, deliberately
//        ungated) stays green, which is the whole point of that family:
//        RED_1_ROWS
//
//    (2) Drop `scene.GetObjects()` at the `SampleLight` probe site
//        (pass 0 instead), so the cross-object triple is never stamped
//        on the NORMAL-ALIGNED probe's payload while the NEE probe
//        keeps its own:
//        RED_2_ROWS
//
//    (3) Restore the flat `kEmitterProbeStandoffFraction * diag`
//        standoff (i.e. delete the SelfHitRootFloor-derived term):
//        RED_3_ROWS
//
//    (4) Skip `ApplyEmitterSurface` at the NM NEE site
//        (`LightSampler::EvaluateDirectLightingNM`):
//        RED_4_ROWS
//
//    (5) Skip `ApplyEmitterSurface` at BDPT's NM hero `Le` rebuild:
//        RED_5_ROWS
//
//    (6) Skip `ApplyEmitterSurface` at BDPT's HWSS companion rebuild:
//        RED_6_ROWS
//
//    (7) Carry `ptObjIntersec` on the GATED payload again (the state
//        this slice's first draft shipped), and compare family D's
//        gate-CLOSED render against its gate-OPEN one:
//        RED_7_ROWS
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

//! THE SAME HEAD with the receiver's albedo supplied by a `curv`-keyed
//! `expression_painter` instead of a `uniformcolor_painter`.
//!
//! It exists to OPEN THE PROBE GATE without changing a single pixel of
//! the receiver.  `curv` is exactly 0 on a planar primitive (the
//! descriptor says so, and `clippedplane_geometry` reports no curvature
//! at all), so `0.6 * (1 - clamp(curv,0,1))` is 0.6 everywhere -- the
//! same constant `pnt_albedo` carries -- while the compiled program's
//! `curv` reference bumps `SurfaceCurvatureDemand` and therefore flips
//! `EmitterProbeWanted()` from false to true process-wide.
//!
//! The expression is written as a MULTIPLY BY A CLAMP rather than
//! `0.6 + 0.0*curv` deliberately: a constant-folding pass may delete a
//! term multiplied by a literal zero, and with it the demand this head
//! exists to register.
static const char* kCommonHeadSignalReceiver =
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
	"expression_painter\n"
	"{\n"
	"\tname pnt_albedo\n"
	"\texpr vec3( 0.6*(1.0-clamp(curv,0,1)), 0.6*(1.0-clamp(curv,0,1)), 0.6*(1.0-clamp(curv,0,1)) )\n"
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

//! FAMILY D: the SAME SDF sphere as family A, but the exitance keys on
//! `Po` -- the object-space hit point -- rather than on a signal.
//!
//! WHY THIS ROW EXISTS (transport review of slice S3, P1-1).  `Po` rode
//! on the probe's GATED payload in this slice's first draft, which made
//! an emissive material keyed on it render 2.94x differently depending
//! on whether any signal painter anywhere in the process was alive.  It
//! is now filled by `LightSampler::EmitterObjectPoint` -- ungated and
//! ray-free -- at every emitter-record site, which ALSO closes a
//! PRE-EXISTING inconsistency older than this slice: a camera ray that
//! HIT the emitter read a live `Po` while every NEE record built from
//! the same sampled point read `(0,0,0)`.
//!
//! CLOSED FORM.  Every point of a sphere of radius 0.5 centred on its
//! object origin satisfies `|Po|^2 = 0.25` EXACTLY, so
//! `clamp(8*|Po|^2, 0, 1)` saturates to exactly 1 -- the 8 (rather than
//! 4) gives 2x headroom so the row tests the RECORD, not the last digit
//! of the SDF's Newton projection.  Control `1.0`; neutral `0.0`, since
//! `Po = (0,0,0)` is what a hand-built record used to carry.
//!
//! NO SIGNAL PAINTER APPEARS IN THIS SCENE, which is the point: the
//! probe gate is CLOSED for every row of this family, and the row still
//! has to pass.
static const char* kEmitterSdfSpherePo = kEmitterSdfSphere;

//! FAMILY E: the SAME unit-scaled SDF sphere as family A, at a SURFACE
//! EPSILON of 0.002 of the diagonal instead of the 5e-5 default.
//!
//! WHY THIS ROW EXISTS (transport review of slice S3, P1-3).  The
//! normal-aligned probe -- the one the BDPT light-subpath root and VCM's
//! light vertex go through -- stands off the surface before firing back
//! at it, and this slice's first draft used a flat `0.001 * diagonal` on
//! the claim that it sat "~9 orders above every geometry's
//! SelfHitRootFloor".  `SelfHitRootFloor` is VIRTUAL with eleven in-tree
//! overrides and that claim held only for the INTERFACE DEFAULT.
//! `SDFGeometry`'s is `min( 2*m_eps/shrink/cosI, 0.5*diagonal )` with
//! `m_eps = max( diagonal*m_epsFrac, 1e-6 )`, so at `epsilon 0.002` it
//! is `2 * 0.002 = 0.004` of the diagonal -- FOUR TIMES the old flat
//! standoff.  Below its own floor the SDF treats the probe origin as
//! spawned ON the surface and marches it forward in `4*m_eps` steps
//! until it clears the band, i.e. straight past the face it was aimed
//! at: the probe misses and REFUSES.
//!
//! THE RESULTING DISAGREEMENT IS THE ONE THIS SLICE EXISTS TO REMOVE.
//! The NEE probe stands off not at all, so PT keeps reading the
//! emitter's `curv` LIVE while BDPT and VCM read it NEUTRAL -- exactly
//! what the narrowed containment warning now claims no longer happens.
//! The row therefore runs under all three, and the header's red-proof
//! records what restoring the flat standoff does to each.
//!
//! WHY EPSILON AND NOT A NON-UNIFORMLY SCALED PART.  A part authored
//! `scale 0.1 1 1` raises the same floor by the same mechanism (the
//! field's Lipschitz `shrink` divides it), and was tried first -- but it
//! also squashes the sphere into a 10:1 oblate spheroid, and `curv` is
//! then NOT the sphere's radius-independent `2*sqrt(3)`.  The measured
//! `clamp(curv,0,1)` did not saturate there (PT read 27.5 % off its
//! baked-1.0 control, with the probe working), so the family loses its
//! exact closed-form control and the row would be testing the curvature
//! estimator rather than the record.  Epsilon moves the floor without
//! moving the shape.
//!
//! 0.002 rather than something larger is bounded from ABOVE by the
//! probe's acceptance tolerance: the march stops at `|Map| <= m_eps`, so
//! the hit can sit `m_eps / shrink = 0.002 * diagonal` short of the true
//! surface, against an acceptance window of `0.01 * diagonal`.  Five
//! times the error, four times the old standoff -- the widest separation
//! the two constraints leave.
static const char* kEmitterSdfCoarseEps =
	"sdf_geometry\n"
	"{\n"
	"\tname g_lum\n"
	"\tepsilon 0.002\n"
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

//! THE INTEGRATOR CHUNKS, built rather than hard-coded, because three
//! things vary across the rows: the integrator, the sample count (row C
//! is the dimmest family and needs more; see the band derivation in the
//! header) and RGB-versus-spectral.
//!
//! Everything else is held fixed -- the same `standard_shader`, the same
//! `pixel_filter box`, the same `oidn_denoise FALSE` -- so a row's EXPR
//! and CONTROL can never differ by a rasterizer setting.  (No row
//! compares one integrator to another; see the header.)
//!
//! THE SPECTRAL KINDS EXIST FOR THE NM SITES.  `EvaluateDirectLightingNM`,
//! `GenerateLightSubpathImpl`'s NM hero `Le` rebuild and its HWSS
//! companion-wavelength rebuild are three of the seven emitter-record
//! sites this slice converts, and NOTHING in an RGB rasterizer reaches
//! any of them.  The chunk parameters are copied from
//! tests/EnvLightBalanceTest.cpp's spectral rows (380-720 nm, 8
//! wavelengths, 1 spectral sample) minus its env-only `radiance_map`.
//! `hwss true` additionally turns on the companion-wavelength bundle, the
//! only route to the third of those sites.
enum RastKind
{
	eRK_PT = 0,
	eRK_BDPT,
	eRK_VCM,
	eRK_PT_SPECTRAL,
	eRK_BDPT_SPECTRAL,
	eRK_VCM_SPECTRAL
};

static const char* RastName( RastKind k, bool hwss )
{
	switch( k ) {
	case eRK_PT:            return "PT";
	case eRK_BDPT:          return "BDPT";
	case eRK_VCM:           return "VCM";
	case eRK_PT_SPECTRAL:   return hwss ? "PT-spectral/HWSS"   : "PT-spectral";
	case eRK_BDPT_SPECTRAL: return hwss ? "BDPT-spectral/HWSS" : "BDPT-spectral";
	case eRK_VCM_SPECTRAL:  return hwss ? "VCM-spectral/HWSS"  : "VCM-spectral";
	}
	return "?";
}

static std::string MakeRasterizer( RastKind k, int samples, bool hwss )
{
	static const char* kShader =
		"standard_shader\n"
		"{\n"
		"\tname global\n"
		"\tshaderop DefaultPathTracing\n"
		"}\n"
		"\n";
	static const char* kOutput =
		"\n"
		"file_rasterizeroutput\n"
		"{\n"
		"\tpattern /tmp/signal_emitter_record_unused\n"
		"\ttype PNG\n"
		"\tbpp 8\n"
		"\tcolor_space sRGB\n"
		"}\n";

	const bool bSpectral =
		( k == eRK_PT_SPECTRAL || k == eRK_BDPT_SPECTRAL || k == eRK_VCM_SPECTRAL );
	const bool bBDPT = ( k == eRK_BDPT || k == eRK_BDPT_SPECTRAL );
	const bool bVCM  = ( k == eRK_VCM  || k == eRK_VCM_SPECTRAL  );

	const char* keyword =
		( k == eRK_PT )            ? "pathtracing_pel_rasterizer" :
		( k == eRK_BDPT )          ? "bdpt_pel_rasterizer" :
		( k == eRK_VCM )           ? "vcm_pel_rasterizer" :
		( k == eRK_PT_SPECTRAL )   ? "pathtracing_spectral_rasterizer" :
		( k == eRK_BDPT_SPECTRAL ) ? "bdpt_spectral_rasterizer" :
		                             "vcm_spectral_rasterizer";

	std::string s( kShader );
	s += keyword;
	s += "\n{\n";

	char buf[256];
	std::snprintf( buf, sizeof(buf), "\tsamples %d\n", samples );
	s += buf;

	if( bBDPT || bVCM ) {
		s += "\tmax_eye_depth 3\n\tmax_light_depth 3\n";
	}
	if( k == eRK_PT ) {
		s += "\trr_min_depth 8\n";
	}
	if( k == eRK_PT_SPECTRAL ) {
		s += "\tmax_diffuse_bounce 3\n";
	}
	if( bVCM ) {
		s += "\tmerge_radius 0.0\n\tvc_enabled true\n\tvm_enabled true\n";
	}
	if( bSpectral ) {
		s += "\tnmbegin 380\n"
		     "\tnmend 720\n"
		     "\tnum_wavelengths 8\n"
		     "\tspectral_samples 1\n";
		s += hwss ? "\thwss true\n" : "\thwss false\n";
	}
	s += "\tpixel_filter box\n\toidn_denoise FALSE\n}\n";
	s += kOutput;
	return s;
}

static std::string MakeScene(
	const char* head,
	const char* emitterChunks,
	const char* exitanceExpr,
	double scale,
	const std::string& rasterizerChunk )
{
	std::string s( head );
	s += MakeEmitterMaterial( exitanceExpr, scale );
	s += emitterChunks;
	s += rasterizerChunk;
	return s;
}

static ImageStats Render(
	const char* head,
	const char* emitterChunks,
	const char* exitanceExpr,
	double scale,
	const std::string& rasterizerChunk,
	const char* tag )
{
	const std::string text = MakeScene( head, emitterChunks, exitanceExpr, scale, rasterizerChunk );
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
struct RowSpec
{
	RastKind	kind;
	bool		hwss;
	int			samples;	//!< 0 = use the family's own count
};

struct Family
{
	const char*		name;
	const char*		head;			//!< receiver + camera; kCommonHead, or the curv-keyed variant
	const char*		emitterChunks;
	const char*		exprSignal;		//!< the live expression
	const char*		exprControl;	//!< the same, signal replaced by its KNOWN CONSTANT
	const char*		exprNeutral;	//!< the same, signal replaced by its NEUTRAL value
	double			scale;
	int				samples;
	const RowSpec*	rows;
	int				nRows;
};

static void RunFamily( const Family& f )
{
	std::cout << "  [" << f.name << "]" << std::endl;

	for( int i = 0; i < f.nRows; i++ ) {
		const int spp = f.rows[i].samples > 0 ? f.rows[i].samples : f.samples;
		const std::string rast = MakeRasterizer( f.rows[i].kind, spp, f.rows[i].hwss );
		const ImageStats expr = Render(
			f.head, f.emitterChunks, f.exprSignal, f.scale, rast, "expr" );
		const ImageStats ctrl = Render(
			f.head, f.emitterChunks, f.exprControl, f.scale, rast, "ctrl" );

		std::string lbl = std::string( f.name ) + " / " + RastName( f.rows[i].kind, f.rows[i].hwss );

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
		const std::string rast = MakeRasterizer( eRK_PT, f.samples, false );
		const ImageStats expr = Render(
			f.head, f.emitterChunks, f.exprSignal, f.scale, rast, "expr" );
		const ImageStats neut = Render(
			f.head, f.emitterChunks, f.exprNeutral, f.scale, rast, "neut" );
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

//////////////////////////////////////////////////////////////////////
// FAMILY D's EXTRA CHECK -- `Po` must NOT depend on the probe gate.
//
// `Po` is not signal state (`expression_painter` exposes it,
// `voronoi3d_painter` and `mapping_painter` key on it), and the probe's
// gate is PROCESS-WIDE, so an emissive material keyed on `Po` must
// render the same whether or not some unrelated painter elsewhere keeps
// a signal demand alive.  This renders the SAME emitter twice, changing
// only the RECEIVER's albedo painter between a plain
// `uniformcolor_painter` (no demand: the gate is CLOSED) and a
// `curv`-keyed `expression_painter` that evaluates to the SAME 0.6 on a
// planar receiver (a demand: the gate is OPEN).
//
// Two checks, in this order, because the second is only attributable
// once the first holds: the CONTROL pair proves the two receiver
// painters really are albedo-identical, and only then does the EXPR pair
// isolate `Po`.
//////////////////////////////////////////////////////////////////////
static void RunGateInvariance( const Family& f )
{
	const std::string rast = MakeRasterizer( eRK_PT, f.samples, false );

	const ImageStats ctrlClosed = Render(
		kCommonHead, f.emitterChunks, f.exprControl, f.scale, rast, "gcc" );
	const ImageStats ctrlOpen = Render(
		kCommonHeadSignalReceiver, f.emitterChunks, f.exprControl, f.scale, rast, "gco" );
	const ImageStats exprClosed = Render(
		kCommonHead, f.emitterChunks, f.exprSignal, f.scale, rast, "gec" );
	const ImageStats exprOpen = Render(
		kCommonHeadSignalReceiver, f.emitterChunks, f.exprSignal, f.scale, rast, "geo" );

	std::cout << "  [D gate-invariance: does an UNRELATED curv painter on the RECEIVER "
	          << "move the Po-keyed emitter?]" << std::endl;
	Check( ctrlClosed.valid && ctrlOpen.valid && exprClosed.valid && exprOpen.valid,
	       "D gate-invariance: all four renders produced an image" );
	if( !ctrlClosed.valid || !ctrlOpen.valid || !exprClosed.valid || !exprOpen.valid ) {
		return;
	}
	PrintMeans( "D CONTROL / gate CLOSED", ctrlClosed );
	PrintMeans( "D CONTROL / gate OPEN  ", ctrlOpen );
	PrintMeans( "D EXPR    / gate CLOSED", exprClosed );
	PrintMeans( "D EXPR    / gate OPEN  ", exprOpen );

	const double dCtrl = WorstRelDiff( ctrlOpen, ctrlClosed );
	std::cout << "      CONTROL: |OPEN/CLOSED - 1| = " << ( dCtrl * 100.0 ) << " %" << std::endl;
	Check( dCtrl < kBand,
	       "D gate-invariance: the curv-keyed receiver painter is albedo-identical to the "
	       "plain one (so the EXPR comparison below is attributable to Po alone)" );

	const double dExpr = WorstRelDiff( exprOpen, exprClosed );
	std::cout << "      EXPR   : |OPEN/CLOSED - 1| = " << ( dExpr * 100.0 ) << " %" << std::endl;
	Check( dExpr < kBand,
	       "D gate-invariance: MONEY -- adding an unrelated curv painter somewhere else in "
	       "the scene does NOT change the Po-keyed emitter's mean" );
}

//////////////////////////////////////////////////////////////////////
// THE ROW SETS.
//
// Family A carries the SPECTRAL rows because it is the only family
// whose emitter answers a signal through every geometry path AND whose
// control is exact: the NM NEE site
// (`LightSampler::EvaluateDirectLightingNM`), BDPT's NM hero `Le`
// rebuild and its HWSS companion-wavelength rebuild are three of the
// seven emitter-record sites and NOTHING in an RGB rasterizer reaches
// any of them.
//////////////////////////////////////////////////////////////////////
static const RowSpec kRowsRGB3[3] = {
	{ eRK_PT, false, 0 }, { eRK_BDPT, false, 0 }, { eRK_VCM, false, 0 }
};
static const RowSpec kRowsPTOnly[1] = { { eRK_PT, false, 0 } };

//! THE SPECTRAL ROWS CARRY THEIR OWN SAMPLE COUNT, and it is eight
//! times the RGB rows'.  Not because the emitter record is any noisier
//! there, but because `spectral_samples 1` draws ONE wavelength per
//! pixel sample out of the 380-720 nm band: the per-channel mean then
//! carries a CHROMATIC MC error the RGB rows do not have, and at 48
//! samples it measured 4.0 % (BDPT-spectral) and 5.2 % (VCM-spectral)
//! against a 5 % band -- noise, not a signal read, but enough to make
//! the row meaningless either way.  384 samples brings it under 1.5 %.
//! The HWSS row needs none of that (it measured 0.40 % at 48): a hero
//! wavelength with seven companions averages the bundle within each
//! sample, which is exactly the chromatic variance at issue.  It is
//! given 96 anyway, for headroom at no meaningful cost.
static const RowSpec kRowsFull[7] = {
	{ eRK_PT, false, 0 }, { eRK_BDPT, false, 0 }, { eRK_VCM, false, 0 },
	{ eRK_PT_SPECTRAL, false, 1024 },
	{ eRK_BDPT_SPECTRAL, false, 1024 },
	{ eRK_VCM_SPECTRAL, false, 1024 },
	// HWSS on the BDPT-spectral row: the companion-wavelength `rigW`
	// rebuild in `GenerateLightSubpathImpl` lives there and nowhere else.
	{ eRK_BDPT_SPECTRAL, true, 96 }
};

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
		kCommonHead,
		kEmitterSdfSphere,
		"0.2 + 0.8*clamp(curv,0,1)",
		"0.2 + 0.8*1.0",
		"0.2 + 0.8*0.0",
		60.0,
		48,
		kRowsFull, 7
	};

	static const Family kAnalyticCurv = {
		"B: analytic sphere_geometry emitter, exitance keyed on curv",
		kCommonHead,
		kEmitterAnalyticSphere,
		"0.2 + 0.8*clamp(curv,0,1)",
		"0.2 + 0.8*1.0",
		"0.2 + 0.8*0.0",
		60.0,
		48,
		kRowsPTOnly, 1
	};

	// proximity(1.0) = 1 - 0.2/1.0 = 0.8 exactly, over the whole emitter
	// quad -- see kEmitterQuadWithNeighbour for the geometry that makes
	// the closed form uniform.
	//
	// 192 SAMPLES, four times every other family's, for two reasons.
	// (1) It is by far the dimmest row (mean ~0.047 against ~0.39), so
	// its relative MC noise is the largest -- nine runs of the original
	// 48-sample row spread to 2.89 %, against a 5 % band.  (2) It now
	// runs BDPT and VCM as well as PT, and those two are the only rows in
	// the suite that exercise the cross-object triple through
	// `ProbeEmitterSurfaceAlongNormal` rather than through the NEE
	// `ProbeEmitterSurface`.
	static const Family kProximity = {
		"C: quad emitter + box neighbour at h=0.2, exitance keyed on proximity(1.0)",
		kCommonHead,
		kEmitterQuadWithNeighbour,
		"0.2 + 0.8*proximity(1.0)",
		"0.2 + 0.8*0.8",
		"0.2 + 0.8*0.0",
		60.0,
		384,
		kRowsRGB3, 3
	};

	// |Po|^2 = 0.25 exactly on a radius-0.5 sphere about its object
	// origin, so clamp(8*|Po|^2,0,1) saturates to exactly 1.  NO SIGNAL
	// PAINTER IS IN THIS SCENE: the probe gate is closed for every row
	// here, and `Po` must land anyway.
	static const Family kObjectPoint = {
		"D: SDF sphere emitter, exitance keyed on Po (NO signal painter in the scene)",
		kCommonHead,
		kEmitterSdfSpherePo,
		"0.2 + 0.8*clamp(8*(Po.x*Po.x + Po.y*Po.y + Po.z*Po.z),0,1)",
		"0.2 + 0.8*1.0",
		"0.2 + 0.8*0.0",
		60.0,
		48,
		kRowsRGB3, 3
	};

	// The self-hit-floor row: the same SDF sphere as family A at
	// `epsilon 0.002`, whose SelfHitRootFloor is 4e-3 of the diagonal --
	// four times the flat standoff this slice's first draft used.
	static const Family kShrunkSdfCurv = {
		"E: SDF sphere at epsilon 0.002 (self-hit floor 4x the old standoff), keyed on curv",
		kCommonHead,
		kEmitterSdfCoarseEps,
		"0.2 + 0.8*clamp(curv,0,1)",
		"0.2 + 0.8*1.0",
		"0.2 + 0.8*0.0",
		60.0,
		48,
		kRowsRGB3, 3
	};

	RunFamily( kSdfCurv );
	RunFamily( kAnalyticCurv );
	RunFamily( kProximity );
	RunFamily( kObjectPoint );
	RunGateInvariance( kObjectPoint );
	RunFamily( kShrunkSdfCurv );

	std::cout << std::endl
	          << "Passed: " << passCount << "  Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
