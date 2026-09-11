//////////////////////////////////////////////////////////////////////
//
//  SignalIntegratorConsistencyTest.cpp -- docs/SIGNALS_UNDER_BIDIRECTIONAL_
//  TRANSPORT.md S2 (section 6): the money test that PT, BDPT, VCM (and,
//  as a smoke check, MLT) price a geometry-derived shading signal
//  (`curv`, `occlusion(r)`, `convexity(r)`, `thickness(r)`, `proximity(r)`,
//  `interior(r)`) THE SAME WAY.
//
//  THE BUG THIS PINS.  Before S1 (docs/SIGNALS_UNDER_BIDIRECTIONAL_
//  TRANSPORT.md section 3), `PathVertexEval::PopulateRIGFromVertex` -- the
//  helper every BDPT/VCM/MLT BSDF/pdf evaluation at a REBUILT vertex funnels
//  through -- did not copy `ri.geometric.derivatives` or `.signals` onto the
//  rebuilt record, so every painter evaluated downstream of a stored vertex
//  read the documented NEUTRAL value for all six signals, no matter what the
//  live geometry actually carried.  PT never rebuilds a vertex -- every
//  painter it evaluates sees the object manager's own stamped hit -- so it
//  is the reference-free-of-the-bug integrator this file is written against.
//  Because BDPT/VCM/MLT eye rays carry no ray differentials at all
//  (docs/SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md section 2, "BDPT/VCM/MLT
//  eye rays carry no ray differentials"), `fw`/`fwo` read 0 there both
//  before and after S1 -- a PT-vs-bidirectional difference that has nothing
//  to do with signals, so every scene below is built so NO expression reads
//  `fw`/`fwo` (no `fbm`/noise call anywhere): the six signal builtins and
//  plain arithmetic only.
//
//  THIS FILE RUNS AT COMMIT ed3e6063 (S2, in a worktree BEFORE S1 has
//  landed): the widening has not happened, so this test is EXPECTED TO
//  FAIL on every BDPT/VCM/MLT row and PASS on every PT row.  That is the
//  RED half of the red-proof the design doc's section 6.1 asks for; the
//  observed numbers are recorded in section "OBSERVED PRE-FIX NUMBERS"
//  below.  Do NOT loosen any band to make a BDPT/VCM row pass here -- the
//  bands are derived from PT's own run-to-run noise (section "BAND
//  DERIVATION"), not tuned to hide the bug.
//
//  TWO LAYERS (docs/SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md section 6):
//
//  LAYER 1 -- CONSTANT-SIGNAL UNIT SCENES (section 6.1, "the sharp gate").
//  Four small inline scenes, each built so ONE signal is a KNOWN CONSTANT
//  over the entire framed region -- known either by an exact closed form
//  (curv on a sphere: `2*sqrt(3)`, independent of radius, from
//  SurfaceCurvatureTest; proximity/interior on box-family receivers: the
//  design doc's closed forms `1-h/r` and `depth/r`) or by a DIRECT,
//  non-rendering evaluation of the SAME production code the painter calls
//  (`ExpressionScalarPainter::GetValuesAt` against a hand-built
//  `SDFGeometry` hit, matching the render geometry exactly field-for-field
//  -- see "REFERENCE HARNESS" below) for occlusion/convexity/thickness,
//  which have no simple closed form on a general SDF.  The CONTROL scene
//  is the SAME expression body with the signal call replaced by that known
//  constant (not by its neutral).  For each scene and integrator I in
//  {PT, BDPT, VCM} (plus one MLT smoke row on the curv scene):
//
//      | mean(I, expr) / mean(I, control) - 1 | < band
//
//  and, on the PT row only, a SENSITIVITY check that the same expression
//  moved the mean by a wide margin between the signal's LIVE value and its
//  documented NEUTRAL:
//
//      | mean(PT, expr) / mean(PT, neutral-baked) - 1 | > band
//
//  so the consistency check can never pass by insensitivity (a broken
//  program that always reads 0 would trivially match a control of 0).
//
//  LAYER 2 -- SHOWCASE RATIO-OF-RATIOS (section 6.2, "the money numbers").
//  For each of plank_closeup, tidal_stones, shelf_bunny (Textures/) and
//  pavilion_colonnade (Combined/) -- loaded through the canonical CST path
//  at reduced resolution, oidn_denoise FALSE, pixel_filter box, no adaptive
//  sampling -- two text variants: E (the file as shipped, signals live) and
//  B (every signal call rewritten to its NEUTRAL constant by whole-word /
//  balanced-paren text substitution: `occlusion(...)`->`1`, `thickness(...)`
//  ->`1`, `convexity(...)`->`0`, `proximity(...)`->`0`, `interior(...)`->`0`,
//  whole-word `curv`->`0`, whole-word `curvR`->`0`).  For I in {BDPT, VCM}:
//
//      R_E = mean(I,E) / mean(PT,E);  R_B = mean(I,B) / mean(PT,B)
//      | R_E / R_B - 1 | < band
//
//  with a same-shaped sensitivity check that `mean(PT,E)/mean(PT,B) - 1` is
//  itself outside the band (a showcase that fails this is not a witness for
//  the gap and is DROPPED from the layer-2 assertion, with a printed note,
//  never silently passed).
//
//  BAND DERIVATION.  Both layers assert on RENDERED MEANS, which carry
//  Monte Carlo noise even under PT.  The bands below were chosen by
//  rendering each layer-1 PT row 3 times at its final (scene, spp) and
//  reading the run-to-run spread of `mean(PT,expr)/mean(PT,control)`
//  (design doc target: <=1% noise on the mean at the chosen spp, 3% band).
//  Layer 2's band (5%) is the design doc's own number, derived the same
//  way but at the coarser (160x120, low-spp) showcase resolution, where
//  Monte Carlo noise on a full-scene mean is larger.  See "BAND DERIVATION
//  -- MEASURED SPREAD" for the actual observed numbers from this run.
//
//  REFERENCE HARNESS.  Occlusion/convexity/thickness have no closed form on
//  a general SDF, so their "known constant" is obtained by intersecting a
//  hand-built `SDFGeometry` -- constructed from the IDENTICAL `part` lines
//  the scene text below authors, so there is no risk of the two geometries
//  drifting apart -- with the exact ray the render camera's chief ray casts
//  at the framed station, and evaluating `occlusion(r)` / `convexity(r)` /
//  `thickness(r)` through `ExpressionScalarPainter::GetValuesAt` (the SAME
//  call a `scalar_painter { expression ... }` chunk makes at render time,
//  just invoked directly instead of through a full path trace).  This is
//  the same methodology `tests/SurfaceSignalsTest.cpp`'s `EvalAtHit` uses
//  and the same one the tidal_stones / plank_closeup showcase tests use to
//  validate a rendered ratio against a "predicted" number
//  (docs/CROSS_OBJECT_PROXIMITY_DESIGN.md section 8's showcase notes).  A
//  strong oracle: it is not a theoretical approximation, it is the actual
//  estimator the render will call, evaluated once outside the Monte Carlo
//  loop.
//
//  RED-PROOF PROTOCOL (post-S1, to be exercised in the S1 worktree, not
//  here): dropping the `signals` copy from `PopulateRIGFromVertex` must
//  turn every BDPT/VCM/MLT row of the occlusion/convexity/thickness/
//  proximity/interior scenes AND both layer-2 showcase invariants red,
//  while every PT row and the curv scene's BDPT/VCM/MLT rows stay green
//  (curv depends on `derivatives`, not `signals`).  Dropping the
//  `derivatives` copy must turn ONLY the curv scene's BDPT/VCM/MLT rows
//  red.  Both directions are asserted mechanically by design (there is no
//  single flag this file can flip to simulate the pre-S1/post-S1 states
//  without literally being run against each tree), so the protocol is
//  exercised by the S1 worker against this same file, not encoded here.
//
//  OBSERVED PRE-FIX NUMBERS (worktree signals-bidir-test at ed3e6063, two
//  independent runs -- full log in
//  /private/tmp/claude-501/-Users-aravind-Working-GitHub-RISE/
//  f398a734-fdcc-484a-9edd-d32d2ce33bf5/scratchpad/s2_prefix_run.txt).
//  Layer 1, `mean(I,expr)/mean(I,control) - 1` per scene/integrator:
//
//      scene       PT             BDPT       VCM        MLT (curv only)
//      curv        -0.00008       -0.755     -0.755     -0.755
//      convexity   +0.014         -0.541     -0.550     --
//      occlusion   +0.0000017     -0.822     -0.824     --
//      thickness   -0.0000003     +1.183     +1.183     --
//      proximity   +0.00004       -0.694     -0.692     --
//      interior    +0.00002       -0.756     -0.758     --
//
//  Every PT row passes the 3% band; every BDPT/VCM row (and the MLT row)
//  fails it by 54-118 percentage points -- the RED half of the red-proof,
//  exactly as section 3 predicts pre-S1.  Sensitivity margins
//  (`mean(PT,expr)/mean(PT,neutral)-1`, all comfortably outside the 3%
//  band): curv 3.09, convexity 1.25, occlusion 3.93, thickness -0.54,
//  proximity 2.13, interior 3.16.  13/13 expected red rows fired, 0
//  unexpected reds, 117 passed / 13 failed overall.
//
//  BAND DERIVATION -- MEASURED SPREAD.  Two independent PT runs at the
//  final (scene, 48 spp) settings gave `mean(PT,expr)/mean(PT,control)-1`
//  agreeing to within 1e-4 on every scene except convexity (9.3e-3 vs
//  1.4e-2 across two harness formulations, both well inside 3%) -- i.e.
//  <0.05% Monte Carlo noise on five of six scenes and a single-digit-
//  percent APPROXIMATION gap (not noise) on convexity, whose estimator's
//  per-hit sampling "spin" (the expression_painter builtin doc's own
//  language) is measurably position-dependent on the sphere (a 9-point
//  harness quadrature spanning the camera's frustum found convexity(0.3)
//  ranging 0.304-0.440, average 0.369, against a single off-axis
//  station's 0.316 -- a 13.6-percentage-point spread that made the
//  single-point reference miss the image mean by 9.3%; the 9x9 quadrature
//  average brings PT to 1.4%).  3% comfortably covers both the <0.05%
//  noise floor and convexity's ~1.4% quadrature-approximation residual,
//  while remaining a fraction of every observed BDPT/VCM bias (54-118
//  points).  Layer 2's 5% band is the design doc's own number, at the
//  coarser (160x120, 16 spp) showcase resolution.
//
//  LAYER 2 FINDING: at 160x120 / 16 spp, all FOUR showcases' `mean(PT,E)`
//  vs `mean(PT,B)` differ by well under the 5% sensitivity band (plank
//  0.3-0.4%, tidal_stones ~0.004%, shelf_bunny ~1.8-3.3%, pavilion_
//  colonnade ~0.9-1.1%) -- each of their signal-driven regions (a nail's
//  contact seam, a buried stone's waterline, dust under a bunny's foot, a
//  column's crevice wear) is too small a fraction of the WHOLE-IMAGE mean
//  the design doc's section 6.2 formula integrates over to move it past
//  noise at this resolution.  Per section 6.2's own rule ("if a showcase
//  is insensitive... dropped... with a printed note"), all four are
//  reported and dropped rather than asserted on; this run therefore has
//  NO layer-2 red/green witness (0 of the 4 R_E/R_B checks fire either
//  way).  This is disclosed, not hidden: Layer 1 alone already gives the
//  clean, strong red/green separation the design's money test is for.
//  Also observed (does not affect any assertion, since these rows are
//  dropped): VCM's mean on 3 of the 4 showcases (tidal_stones, shelf_
//  bunny, pavilion_colonnade) is 2-3 ORDERS OF MAGNITUDE above PT/BDPT's
//  (e.g. shelf_bunny VCM mean 3604 vs PT's 0.80) at these reduced-
//  resolution/16-spp settings -- almost certainly the auto merge-radius
//  estimator destabilizing at very low sample counts on scenes it was
//  never tuned for at this resolution, not a signals-consistency finding;
//  flagged here for whoever next touches VCM's auto-radius pre-pass.
//
//  KNOBS.  `SIGNAL_CONSISTENCY_FILTER` (env, substring match against `unit`,
//  `showcase`, and the four showcase names `plank`, `tidal`, `bunny`,
//  `pavilion`) restricts which layer/scene runs, like FabricRenderTest's
//  own filter.  Unset (the default, and the CI invocation) runs BOTH
//  layers, all four unit scenes and all four showcases.  Default runtime
//  on this machine: ~38 seconds wall (layer 1 ~20s, layer 2 ~18s).
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <cmath>
#include <string>
#include <algorithm>
#include <regex>
#include <filesystem>
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
#include "../src/Library/Geometry/SDFGeometry.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Painters/ExpressionEval.h"
#include "../src/Library/Painters/ExpressionPainter.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"

using namespace RISE;
using namespace RISE::Implementation;
namespace fs = std::filesystem;

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

//======================================================================
// Env filter (FabricRenderTest / PrimitiveSelfHitTest pattern)
//======================================================================

static std::string g_filter;

static bool FilterAllows( const char* keyword )
{
	if( g_filter.empty() ) return true;
	return g_filter.find( keyword ) != std::string::npos;
}

//! Layer 1 (the four constant-signal unit scenes) runs when the filter is
//! unset or names "unit".
static bool WantsLayer1()
{
	return g_filter.empty() || FilterAllows( "unit" );
}

//! One showcase runs when the filter is unset, names "showcase" (all
//! four), or names that specific showcase's own keyword.
static bool WantsShowcase( const char* keyword )
{
	return g_filter.empty() || FilterAllows( "showcase" ) || FilterAllows( keyword );
}

//////////////////////////////////////////////////////////////////////
// CapturingRasterizerOutput -- same pattern as BDPTStrategyBalanceTest /
// VCMStrategyBalanceTest / ShelfBunnyShowcaseTest: capture the final
// image into memory, no file I/O.
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

//////////////////////////////////////////////////////////////////////
// Image mean -- composited-over-black (base*alpha), the convention
// BDPTStrategyBalanceTest's ComputeStats uses (see
// docs/INTEGRATOR_BUGFIX_FINDINGS.md Bug 2: PT and BDPT disagree on the
// unpremultiplied-vs-premultiplied alpha convention at partial-coverage
// silhouette pixels; base*alpha is convention-independent).  None of this
// file's scenes have partial-coverage edges in their framed region (every
// station is a full-coverage interior hit), so this mostly just protects
// against a stray background pixel creeping into the frame.
//////////////////////////////////////////////////////////////////////
struct ImageStats
{
	double meanRGB[3];
	double mean;		// average of the three channels
	bool   valid;
};

static ImageStats ComputeStats( const CapturingRasterizerOutput& cap )
{
	ImageStats s{};
	if( cap.pixels.empty() ) return s;

	double sum[3] = { 0, 0, 0 };
	for( const RISEColor& c : cap.pixels ) {
		const double cov = c.a;
		sum[0] += c.base.r * cov;
		sum[1] += c.base.g * cov;
		sum[2] += c.base.b * cov;
	}
	for( int c = 0; c < 3; c++ ) s.meanRGB[c] = sum[c] / double(cap.pixels.size());
	s.mean = ( s.meanRGB[0] + s.meanRGB[1] + s.meanRGB[2] ) / 3.0;
	s.valid = true;
	return s;
}

//////////////////////////////////////////////////////////////////////
// Scene-text rendering: write to a unique temp file, load via the
// canonical LoadAsciiSceneViaCst path, capture in memory, rasterize.
//////////////////////////////////////////////////////////////////////
static std::string WriteSceneToTempFile( const std::string& sceneText, const char* tag, int counter )
{
	char path[512];
	std::snprintf( path, sizeof(path),
		"/tmp/signal_consistency_%s_%d_%d.RISEscene",
		tag, static_cast<int>(::getpid()), counter );
	std::ofstream ofs( path );
	if( !ofs.is_open() ) return std::string();
	ofs << sceneText;
	ofs.close();
	return std::string( path );
}

static ImageStats RenderSceneText( const std::string& sceneText, const char* tag )
{
	static int counter = 0;
	ImageStats result{};

	const std::string path = WriteSceneToTempFile( sceneText, tag, counter++ );
	if( path.empty() ) return result;

	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) {
		std::remove( path.c_str() );
		return result;
	}

	if( !pJob->LoadAsciiSceneViaCst( path.c_str() ) ) {
		safe_release( pJob );
		std::remove( path.c_str() );
		return result;
	}

	pJob->RemoveRasterizerOutputs();

	CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "signal consistency capture output" );
	pJob->GetRasterizer()->AddRasterizerOutput( pCap );

	const bool bRendered = pJob->Rasterize();
	if( bRendered ) {
		result = ComputeStats( *pCap );
	}

	safe_release( pCap );
	safe_release( pJob );
	std::remove( path.c_str() );
	return result;
}

//////////////////////////////////////////////////////////////////////
// Rasterizer chunk builders.  Every one forces `oidn_denoise FALSE` +
// `pixel_filter box` + no adaptive sampling (adaptive_max_samples
// defaults to 0 = disabled, so simply never setting it), per
// docs/skills/bdpt-vcm-mis-balance.md Step 0's non-MIS-cause checklist.
//////////////////////////////////////////////////////////////////////
static std::string RasterizerChunkPT( unsigned int samples )
{
	std::ostringstream ss;
	ss << "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n";
	ss << "pathtracing_pel_rasterizer\n{\n\tsamples " << samples
	   << "\n\trr_min_depth 8\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";
	ss << "file_rasterizeroutput\n{\n\tpattern /tmp/signal_consistency_pt_unused\n\ttype PNG\n\tbpp 8\n\tcolor_space sRGB\n}\n\n";
	return ss.str();
}

static std::string RasterizerChunkBDPT( unsigned int samples )
{
	std::ostringstream ss;
	ss << "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n";
	ss << "bdpt_pel_rasterizer\n{\n\tmax_eye_depth 3\n\tmax_light_depth 3\n\tsamples " << samples
	   << "\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";
	ss << "file_rasterizeroutput\n{\n\tpattern /tmp/signal_consistency_bdpt_unused\n\ttype PNG\n\tbpp 8\n\tcolor_space sRGB\n}\n\n";
	return ss.str();
}

static std::string RasterizerChunkVCM( unsigned int samples )
{
	std::ostringstream ss;
	ss << "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n";
	ss << "vcm_pel_rasterizer\n{\n\tmax_eye_depth 3\n\tmax_light_depth 3\n\tsamples " << samples
	   << "\n\tmerge_radius 0.0\n\tvc_enabled true\n\tvm_enabled true\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";
	ss << "file_rasterizeroutput\n{\n\tpattern /tmp/signal_consistency_vcm_unused\n\ttype PNG\n\tbpp 8\n\tcolor_space sRGB\n}\n\n";
	return ss.str();
}

static std::string RasterizerChunkMLT()
{
	std::ostringstream ss;
	ss << "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n";
	ss << "mlt_rasterizer\n{\n\tmax_eye_depth 3\n\tmax_light_depth 3\n\tbootstrap_samples 4096\n"
	      "\tchains 64\n\tmutations_per_pixel 16\n\tlarge_step_prob 0.3\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";
	ss << "file_rasterizeroutput\n{\n\tpattern /tmp/signal_consistency_mlt_unused\n\ttype PNG\n\tbpp 8\n\tcolor_space sRGB\n}\n\n";
	return ss.str();
}

enum class Integrator { PT, BDPT, VCM, MLT };

static const char* IntegratorName( Integrator i )
{
	switch( i ) {
		case Integrator::PT:   return "PT";
		case Integrator::BDPT: return "BDPT";
		case Integrator::VCM:  return "VCM";
		case Integrator::MLT:  return "MLT";
	}
	return "?";
}

static std::string RasterizerChunk( Integrator i, unsigned int samples )
{
	switch( i ) {
		case Integrator::PT:   return RasterizerChunkPT( samples );
		case Integrator::BDPT: return RasterizerChunkBDPT( samples );
		case Integrator::VCM:  return RasterizerChunkVCM( samples );
		case Integrator::MLT:  return RasterizerChunkMLT();
	}
	return std::string();
}

//======================================================================
// LAYER 1 -- REFERENCE HARNESS: evaluate a signal expression directly
// against a hand-built SDFGeometry hit, matching the render scene's own
// `part` lines field-for-field.  Adapted from tests/SurfaceSignalsTest.cpp's
// EvalAtHit / BuildSdf* helpers.
//======================================================================

static RayIntersection MkRI( const Point3& origin, const Vector3& dir )
{
	return RayIntersection( Ray( origin, dir ), nullRasterizerState );
}

static bool HitObject( const Object* obj, RayIntersection& ri )
{
	obj->IntersectRay( ri, RISE_INFINITY, true, true, false );
	return ri.geometric.bHit;
}

static bool CompileWithContext( const std::string& body, ExpressionProgram& out )
{
	ExpressionProgram::Builder b;
	b.EnableContextVars( true );
	return b.Finalize( body, out );
}

//! Evaluate `body` (an expression referencing occlusion/convexity/thickness)
//! at the first hit of (origin,dir) against an SDFGeometry built from
//! `parts`.  Returns false on miss or compile failure.
static bool EvalAtSdfHit(
	const std::vector<SDFGeometry::Part>& parts,
	const Point3& origin, const Vector3& dir,
	const std::string& body, Scalar& outValue )
{
	SDFGeometry* g = new SDFGeometry( parts, 512, Scalar( 1e-5 ) );
	Object* obj = new Object( g );
	safe_release( g );
	obj->FinalizeTransformations();

	ExpressionProgram prog = ExpressionProgram::Invalid();
	bool ok = false;
	if( CompileWithContext( body, prog ) ) {
		RayIntersection ri = MkRI( origin, dir );
		if( HitObject( obj, ri ) ) {
			std::vector<ParamSpec> specs;
			ExpressionScalarPainter* painter = new ExpressionScalarPainter( prog, specs );
			outValue = painter->GetValuesAt( ri.geometric ).v[0];
			painter->release();
			ok = true;
		}
	}
	obj->release();
	return ok;
}

//======================================================================
// LAYER 1 -- the shared material/lighting recipe.  ONE material reads
// the signal-driven scalar `t` in BOTH its colour (expression_painter,
// a ramp from a dark/rough "unworn" look to a light/smooth "worn" look)
// AND its GGX roughness (scalar_painter on alphax/alphay) -- exactly the
// "forward-live, priced-neutral" pattern the design doc's section 1
// describes: one material, one vertex, sampled and priced through the
// SAME `t`.  `tBody` is substituted three ways per scene: the real
// signal call (EXPR), a literal matching the harness/closed-form value
// (CONTROL), and the signal's documented neutral (NEUTRAL, PT-only,
// for the sensitivity check).
//======================================================================

static std::string BuildSignalScene(
	Integrator integrator, unsigned int samples,
	const std::string& geometryCameraLight,
	const std::string& tBody )
{
	std::ostringstream ss;
	ss << "RISE ASCII SCENE 7\n";
	ss << RasterizerChunk( integrator, samples );

	ss << "uniformcolor_painter\n{\n\tname sig_f0\n\tcolor 0.04 0.04 0.04\n}\n\n";
	ss << "uniformcolor_painter\n{\n\tname sig_gray\n\tcolor 0.5 0.5 0.5\n}\n\n";
	ss << "lambertian_material\n{\n\tname sig_neutral_mat\n\treflectance sig_gray\n}\n\n";

	ss << "scalar_painter\n{\n\tname sig_rough\n\tdef t " << tBody << "\n\texpression t\n}\n\n";
	ss << "expression_painter\n{\n\tname sig_col\n\tdef t " << tBody
	   << "\n\texpr mix(vec3(0.2,0.2,0.2), vec3(0.8,0.8,0.8), t)\n}\n\n";
	ss << "ggx_material\n{\n\tname sig_mat\n\trd sig_col\n\trs sig_f0\n\talphax sig_rough\n\talphay sig_rough\n\tfresnel_mode schlick_f0\n}\n\n";

	ss << geometryCameraLight;
	return ss.str();
}

//! The four layer-1 unit-scene descriptors.
struct SignalUnitScene
{
	const char* name;
	std::string geometryCameraLight;
	std::string exprT;		// the live builtin call
	std::string controlT;	// literal matching the harness/closed-form value
	std::string neutralT;	// literal matching the documented neutral
	double controlValue;	// the numeric value behind controlT (for printing)
};

//======================================================================
// LAYER 1 -- scene geometry.  Film is 32x32 for all four (BDPTStrategy-
// BalanceTest's own precedent: sub-percent MC noise on a smooth scene's
// mean at moderate spp over ~1000 pixels).
//======================================================================

static const char* kFilm32 = "film\n{\n\twidth 32\n\theight 32\n}\n\n";

// --- (1) curv: one analytic sphere_geometry, radius-independent
// closed form curv = 2*sqrt(3) (SurfaceCurvatureTest section (h)), which
// SATURATES clamp(curv,0,1) to EXACTLY 1.0 on ANY sphere (curv = 3.4641
// > 1).  Neutral curv = 0 => clamp(0,0,1) = 0.0.  So control = "1.0" is
// not a measurement, it is exact arithmetic.
static SignalUnitScene BuildCurvScene()
{
	SignalUnitScene s;
	s.name = "curv";
	std::ostringstream g;
	g << "sphere_geometry\n{\n\tname curv_sphere\n\tradius 2.0\n}\n\n";
	g << "standard_object\n{\n\tname obj_curv\n\tgeometry curv_sphere\n\tmaterial sig_mat\n}\n\n";
	g << "pinhole_camera\n{\n\tlocation 0 0 7\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 36.0\n}\n\n";
	g << "omni_light\n{\n\tname l_curv\n\tpower 8.0\n\tcolor 1 1 1\n\tposition 3 3 7\n}\n\n";
	g << kFilm32;
	s.geometryCameraLight = g.str();
	s.exprT = "clamp(curv,0,1)";
	s.controlT = "1.0";
	s.controlValue = 1.0;
	s.neutralT = "0.0";
	return s;
}

// --- (2) convexity: one SDF sphere (rho = 1.5), fraction r = 0.3.
//
// convexity(r)'s estimator is a 32-DIRECTION DISCRETE sample of the query
// ball, "spun about the normal per hit so the answer is an expectation
// rather than a multiple of 1/12" (the expression_painter builtin doc).
// That per-hit spin is measured (below) to depend on the hit's LOCATION
// on the sphere, not just its rotational-symmetry class: a single
// hand-picked station's ComputeConvexity answer is NOT representative of
// the image-wide mean the render actually integrates (measured spread
// ~9% between a single off-axis station and the true camera-weighted
// average -- see docs/SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md S2's "BAND
// DERIVATION -- MEASURED SPREAD").  The reference used as CONTROL is
// therefore the AVERAGE of the SAME direct evaluation over a grid of
// rays matching the render camera's own pinhole projection (fov, aspect,
// image plane) -- i.e. a coarse quadrature of the exact quantity PT's
// Monte Carlo mean estimates, using the SAME production ComputeConvexity
// call, just off the Monte Carlo path.  This is still independent of the
// render (no pixel from the actual render feeds back into it) and still
// a strong oracle (direct evaluation, not a theoretical approximation).
static SignalUnitScene BuildConvexityScene( double* outHarnessValue )
{
	SignalUnitScene s;
	s.name = "convexity";
	const double rho = 1.5;
	const double rFrac = 0.3;
	const Point3 camLoc( 0, 0, 6 );
	const double fovDeg = 32.0;
	std::ostringstream g;
	g << "sdf_geometry\n{\n\tname convex_sphere\n\tpart sphere union 0  0 0 0  0 0 0  1 1 1  " << rho << " 0 0  0\n"
	  << "\tmaxsteps 512\n\tepsilon 0.00001\n\tsampling_detail 64\n}\n\n";
	g << "standard_object\n{\n\tname obj_convex\n\tgeometry convex_sphere\n\tmaterial sig_mat\n}\n\n";
	g << "pinhole_camera\n{\n\tlocation 0 0 6\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov " << fovDeg << "\n}\n\n";
	g << "omni_light\n{\n\tname l_convex\n\tpower 6.0\n\tcolor 1 1 1\n\tposition 3 3 6\n}\n\n";
	g << kFilm32;
	s.geometryCameraLight = g.str();

	std::ostringstream tb;
	tb << "convexity(" << rFrac << ")";
	s.exprT = tb.str();

	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart(
		SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3( 0, 0, 0 ), 0, 0, 0, Vector3( 1, 1, 1 ), Scalar(rho), 0, 0, 0 ) );

	// Pinhole quadrature: an NxN grid over normalized device coordinates
	// [-0.9,0.9]^2 (staying off the silhouette, where a grazing ray can
	// miss the sphere entirely), matching a square-aspect pinhole camera
	// with vertical fov = fovDeg.
	const int N = 9;
	const double halfFov = fovDeg * 0.5 * (M_PI / 180.0);
	const double tanHalf = std::tan( halfFov );
	double sum = 0.0;
	int count = 0;
	double minV = 1e30, maxV = -1e30;
	for( int iy = 0; iy < N; ++iy ) {
		for( int ix = 0; ix < N; ++ix ) {
			const double ndcX = -0.9 + 1.8 * ( double(ix) / double(N-1) );
			const double ndcY = -0.9 + 1.8 * ( double(iy) / double(N-1) );
			Vector3 dir = Vector3Ops::Normalize( Vector3(
				ndcX * tanHalf, ndcY * tanHalf, -1.0 ) );
			Scalar v = 0;
			if( EvalAtSdfHit( parts, camLoc, dir, tb.str(), v ) ) {
				sum += double(v);
				count++;
				minV = std::min( minV, double(v) );
				maxV = std::max( maxV, double(v) );
			}
		}
	}
	Check( count > ( N * N ) / 2, "(harness) convexity quadrature hits the sphere over most of the grid" );
	const double avg = count > 0 ? sum / double(count) : 0.0;
	std::cout << "  (harness) convexity(" << rFrac << ") quadrature: N=" << count
	          << " avg=" << avg << " min=" << minV << " max=" << maxV
	          << " spread=" << ( maxV - minV ) << std::endl;

	*outHarnessValue = avg;
	std::ostringstream cv;
	cv.precision( 10 );
	cv << avg;
	s.controlT = cv.str();
	s.controlValue = avg;
	s.neutralT = "0.0";
	return s;
}

// --- (3) occlusion: a spherical pocket carved from a big SDF sphere by
// subtraction (tests/SurfaceSignalsTest.cpp section (k)'s "spherical
// pocket" construction, folded into ONE SDFGeometry's own multi-part
// field instead of a CSGObject of two -- SDFGeometry's own boolean
// subtract composes a single continuous signed field, so no CSG-level
// normal/field-sense re-pairing is needed for THIS geometry family).  The
// camera sits INSIDE the carved-out cavity (an established RISE pattern:
// vcm_sdf_luminaire_jellyfish's submerged camera), looking at the pocket
// floor -- the only way to frame a genuinely occluded station without an
// optical trick, since a solid opaque neighbour and "visible from
// outside" are mutually exclusive for a fully enclosed cavity.
static SignalUnitScene BuildOcclusionScene( double* outHarnessValue )
{
	SignalUnitScene s;
	s.name = "occlusion";
	const double bigR = 4.0;
	const double rho = 1.5;
	const double rFrac = 0.15;
	std::ostringstream g;
	g << "sdf_geometry\n{\n\tname pocket_sdf\n"
	  << "\tpart sphere union 0     0 0 0    0 0 0   1 1 1   " << bigR << " 0 0   0\n"
	  << "\tpart sphere subtract 0  0 0 2.0  0 0 0   1 1 1   " << rho << " 0 0   0\n"
	  << "\tmaxsteps 512\n\tepsilon 0.00001\n\tsampling_detail 64\n}\n\n";
	g << "standard_object\n{\n\tname obj_pocket\n\tgeometry pocket_sdf\n\tmaterial sig_mat\n}\n\n";
	g << "pinhole_camera\n{\n\tlocation 0 0 2.9\n\tlookat 0 0 0.5\n\tup 0 1 0\n\tfov 8.0\n}\n\n";
	g << "omni_light\n{\n\tname l_pocket\n\tpower 3.0\n\tcolor 1 1 1\n\tposition 0.3 0.3 2.7\n}\n\n";
	g << kFilm32;
	s.geometryCameraLight = g.str();

	std::ostringstream tb;
	tb << "clamp(1-occlusion(" << rFrac << "),0,1)";
	s.exprT = tb.str();

	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart(
		SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3( 0, 0, 0 ), 0, 0, 0, Vector3( 1, 1, 1 ), Scalar(bigR), 0, 0, 0 ) );
	parts.push_back( SDFGeometry::MakePart(
		SDFGeometry::ePrimSphere, SDFGeometry::eOpSubtract, 0,
		Point3( 0, 0, 2.0 ), 0, 0, 0, Vector3( 1, 1, 1 ), Scalar(rho), 0, 0, 0 ) );
	Scalar measured = 0;
	Check( EvalAtSdfHit( parts, Point3( 0, 0, 2.9 ), Vector3( 0, 0, -1 ), tb.str(), measured ),
		"(harness) occlusion reference evaluates on the pocket floor" );

	*outHarnessValue = double( measured );
	std::ostringstream cv;
	cv.precision( 10 );
	cv << double( measured );
	s.controlT = cv.str();
	s.controlValue = double( measured );
	s.neutralT = "0.0";		// occlusion neutral 1 => clamp(1-1,0,1) = 0
	return s;
}

// --- (4) thickness: a wide, thin SDF box slab (tests/SurfaceSignalsTest
// .cpp's BuildSdfSlab pattern) viewed on its broad face, framed to a
// small central patch far from every edge -- the slab's own field is
// EXACTLY translation-invariant there (a flat box interior has no
// curvature), so the constant is exact within the estimator's own
// numerical precision, not merely "close to within 1%".
static SignalUnitScene BuildThicknessScene( double* outHarnessValue )
{
	SignalUnitScene s;
	s.name = "thickness";
	const double halfW = 0.15;	// slab half-thickness (full width 0.3)
	const double rFrac = 0.1;
	std::ostringstream g;
	g << "sdf_geometry\n{\n\tname slab_sdf\n\tpart box union 0  0 0 0  0 0 0  1 1 1  5.0 5.0 " << halfW << "  0\n"
	  << "\tmaxsteps 512\n\tepsilon 0.00001\n\tsampling_detail 64\n}\n\n";
	g << "standard_object\n{\n\tname obj_slab\n\tgeometry slab_sdf\n\tmaterial sig_mat\n}\n\n";
	g << "pinhole_camera\n{\n\tlocation 0 0 5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 4.0\n}\n\n";
	g << "omni_light\n{\n\tname l_slab\n\tpower 6.0\n\tcolor 1 1 1\n\tposition 2 2 5\n}\n\n";
	g << kFilm32;
	s.geometryCameraLight = g.str();

	std::ostringstream tb;
	tb << "thickness(" << rFrac << ")";
	s.exprT = tb.str();

	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart(
		SDFGeometry::ePrimBox, SDFGeometry::eOpUnion, 0,
		Point3( 0, 0, 0 ), 0, 0, 0, Vector3( 1, 1, 1 ), 5.0, 5.0, Scalar(halfW), 0 ) );
	Scalar measured = 0;
	Check( EvalAtSdfHit( parts, Point3( 0, 0, 5.0 ), Vector3( 0, 0, -1 ), tb.str(), measured ),
		"(harness) thickness reference evaluates on the slab's broad face" );

	*outHarnessValue = double( measured );
	std::ostringstream cv;
	cv.precision( 10 );
	cv << double( measured );
	s.controlT = cv.str();
	s.controlValue = double( measured );
	s.neutralT = "1.0";		// thickness neutral is 1 (thick), not 0
	return s;
}

// --- (5) proximity: a receiver clipped-plane under a wide flat box, gap
// h = 0.5, query radius r = 1.0 => proximity = clamp(1-h/r,0,1) = 0.5
// EXACTLY (docs/CROSS_OBJECT_PROXIMITY_DESIGN.md's closed form; box is
// one of the EXACT-family receivers/neighbours).  Camera sits IN the gap,
// between the two, looking straight down -- the box is directly behind
// the camera's view direction and never enters frame.
static SignalUnitScene BuildProximityScene()
{
	SignalUnitScene s;
	s.name = "proximity";
	std::ostringstream g;
	g << "clippedplane_geometry\n{\n\tname recv_plane\n\tpta -2 0 -2\n\tptb 2 0 -2\n\tptc 2 0 2\n\tptd -2 0 2\n}\n\n";
	g << "box_geometry\n{\n\tname proxbox_geom\n\twidth 6\n\theight 0.6\n\tdepth 6\n}\n\n";
	g << "standard_object\n{\n\tname obj_recv\n\tgeometry recv_plane\n\tmaterial sig_mat\n}\n\n";
	g << "standard_object\n{\n\tname obj_proxbox\n\tgeometry proxbox_geom\n\tmaterial sig_neutral_mat\n\tposition 0 0.8 0\n}\n\n";
	g << "pinhole_camera\n{\n\tlocation 0 0.2 0\n\tlookat 0 0 0\n\tup 0 0 1\n\tfov 20.0\n}\n\n";
	g << "omni_light\n{\n\tname l_prox\n\tpower 4.0\n\tcolor 1 1 1\n\tposition 1.0 0.3 0\n}\n\n";
	g << kFilm32;
	s.geometryCameraLight = g.str();
	s.exprT = "proximity(1.0)";
	s.controlT = "0.5";
	s.controlValue = 0.5;
	s.neutralT = "0.0";
	return s;
}

// --- (6) interior: a receiver clipped-plane embedded 5 world units deep
// inside a 10x10x10 box (half-extent 5 in every axis), query radius
// r = 0.5 => interior = clamp(depth/r,0,1) = clamp(5/0.5,0,1) = 1.0
// EXACTLY (saturates, so the exact depth doesn't even need to be 5 to
// 12 significant figures -- any depth >= r gives the same 1.0).  Camera
// sits INSIDE the box too (between the box's own far wall and the
// receiver, so the receiver -- much closer -- is hit first and the box's
// own surface is never reached; the same "camera and target share a
// solid's interior" pattern the occlusion scene uses, but here the box
// truly has open space inside it for ray-intersection purposes, since
// RISE's box_geometry is a boundary representation with no participating
// volume).
static SignalUnitScene BuildInteriorScene()
{
	SignalUnitScene s;
	s.name = "interior";
	std::ostringstream g;
	g << "clippedplane_geometry\n{\n\tname recv_plane2\n\tpta -2 0 -2\n\tptb 2 0 -2\n\tptc 2 0 2\n\tptd -2 0 2\n}\n\n";
	g << "box_geometry\n{\n\tname intbox_geom\n\twidth 10\n\theight 10\n\tdepth 10\n}\n\n";
	g << "standard_object\n{\n\tname obj_recv2\n\tgeometry recv_plane2\n\tmaterial sig_mat\n}\n\n";
	g << "standard_object\n{\n\tname obj_intbox\n\tgeometry intbox_geom\n\tmaterial sig_neutral_mat\n}\n\n";
	g << "pinhole_camera\n{\n\tlocation 0 2 0\n\tlookat 0 0 0\n\tup 0 0 1\n\tfov 20.0\n}\n\n";
	g << "omni_light\n{\n\tname l_int\n\tpower 4.0\n\tcolor 1 1 1\n\tposition 1.0 3 0\n}\n\n";
	g << kFilm32;
	s.geometryCameraLight = g.str();
	s.exprT = "interior(0.5)";
	s.controlT = "1.0";
	s.controlValue = 1.0;
	s.neutralT = "0.0";
	return s;
}

//======================================================================
// LAYER 1 driver
//======================================================================

static const unsigned int kLayer1Samples = 48;

struct Layer1Row
{
	std::string integrator;
	double meanExpr, meanControl, ratio;
	bool pass;
};

static std::vector<Layer1Row> g_layer1Rows;

static void RunLayer1Scene( const SignalUnitScene& scene, bool includeMlt )
{
	std::cout << "\n=== LAYER 1: " << scene.name << " (control = " << scene.controlT << ") ===" << std::endl;

	std::vector<Integrator> integrators = { Integrator::PT, Integrator::BDPT, Integrator::VCM };
	if( includeMlt ) integrators.push_back( Integrator::MLT );

	const double band = 0.03;			// 3% -- see header BAND DERIVATION
	const double sensitivityBand = 0.03;	// must be OUTSIDE this

	double ptExprMean = 0.0, ptNeutralMean = 0.0;
	bool ptOk = false;

	for( Integrator integ : integrators ) {
		const unsigned int samples = ( integ == Integrator::MLT ) ? 0 : kLayer1Samples;
		const std::string exprScene    = BuildSignalScene( integ, samples, scene.geometryCameraLight, scene.exprT );
		const std::string controlScene = BuildSignalScene( integ, samples, scene.geometryCameraLight, scene.controlT );

		const ImageStats exprStats    = RenderSceneText( exprScene, ( scene.name + std::string("_expr") ).c_str() );
		const ImageStats controlStats = RenderSceneText( controlScene, ( scene.name + std::string("_ctrl") ).c_str() );

		const bool rendersOk = exprStats.valid && controlStats.valid;
		Check( rendersOk, std::string( scene.name ) + " " + IntegratorName(integ) + ": expr and control both render" );

		Layer1Row row;
		row.integrator = IntegratorName( integ );
		row.meanExpr = exprStats.mean;
		row.meanControl = controlStats.mean;
		row.ratio = rendersOk && controlStats.mean != 0.0 ? ( exprStats.mean / controlStats.mean - 1.0 ) : 1e9;
		row.pass = rendersOk && std::fabs( row.ratio ) < band;
		g_layer1Rows.push_back( row );

		std::cout << "  " << IntegratorName( integ )
		          << ": mean(expr)=" << exprStats.mean
		          << " mean(control)=" << controlStats.mean
		          << " ratio-1=" << row.ratio
		          << ( row.pass ? "  [pass]" : "  [FAIL]" ) << std::endl;

		Check( row.pass, std::string( scene.name ) + " " + IntegratorName(integ)
			+ ": | mean(expr)/mean(control) - 1 | < " + std::to_string(band) );

		if( integ == Integrator::PT ) {
			ptExprMean = exprStats.mean;
			ptOk = rendersOk;
		}
	}

	// Sensitivity: PT expr vs PT neutral-baked must be OUTSIDE the band.
	if( ptOk ) {
		const std::string neutralScene = BuildSignalScene( Integrator::PT, kLayer1Samples, scene.geometryCameraLight, scene.neutralT );
		const ImageStats neutralStats = RenderSceneText( neutralScene, ( scene.name + std::string("_neutral") ).c_str() );
		Check( neutralStats.valid, std::string( scene.name ) + " PT: neutral-baked render succeeds" );
		if( neutralStats.valid ) {
			ptNeutralMean = neutralStats.mean;
			const double sensRatio = ptNeutralMean != 0.0
				? ( ptExprMean / ptNeutralMean - 1.0 )
				: ( ptExprMean != 0.0 ? 1e9 : 0.0 );
			std::cout << "  SENSITIVITY: mean(PT,expr)=" << ptExprMean
			          << " mean(PT,neutral=" << scene.neutralT << ")=" << ptNeutralMean
			          << " ratio-1=" << sensRatio << std::endl;
			Check( std::fabs( sensRatio ) > sensitivityBand,
				std::string( scene.name ) + " PT: expr vs neutral-baked is OUTSIDE the band (real sensitivity)" );
		}
	}
}

//======================================================================
// LAYER 2 -- showcase ratio-of-ratios
//======================================================================

static fs::path FindRepoRoot()
{
	const char* candidates[] = { ".", "..", "../..", "../../.." };
	for( const char* c : candidates ) {
		const fs::path p( c );
		if( fs::exists( p / "scenes" / "FeatureBased" / "Textures" / "plank_closeup.RISEscene" ) ) {
			return p;
		}
	}
	return fs::path();
}

static std::string ReadFile( const fs::path& p )
{
	std::ifstream in( p );
	std::stringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

//! Replace every whole-word call `fnName(...)` (balanced parens, so a
//! nested call inside the argument list is handled correctly) with
//! `constant`.  Whole-word: the character before `fnName` (if any) must
//! not be alnum/`_`, so this never fires inside a longer identifier.
static std::string ReplaceCallsWithConstant( const std::string& text, const std::string& fnName, const std::string& constant )
{
	std::string out;
	out.reserve( text.size() );
	std::size_t i = 0;
	while( i < text.size() ) {
		std::size_t pos = text.find( fnName + "(", i );
		if( pos == std::string::npos ) { out += text.substr( i ); break; }
		const bool boundaryOk = ( pos == 0 ) ||
			!( std::isalnum( static_cast<unsigned char>(text[pos-1]) ) || text[pos-1] == '_' );
		if( !boundaryOk ) {
			out += text.substr( i, pos - i + fnName.size() );
			i = pos + fnName.size();
			continue;
		}
		out += text.substr( i, pos - i );
		std::size_t parenStart = pos + fnName.size();
		int depth = 0;
		std::size_t j = parenStart;
		for( ; j < text.size(); ++j ) {
			if( text[j] == '(' ) depth++;
			else if( text[j] == ')' ) { depth--; if( depth == 0 ) { j++; break; } }
		}
		out += constant;
		i = j;
	}
	return out;
}

static std::string ReplaceWholeWord( const std::string& text, const std::string& word, const std::string& constant )
{
	std::regex re( "\\b" + word + "\\b" );
	return std::regex_replace( text, re, constant );
}

//! Build the neutral (variant B) text: every geometry signal call
//! rewritten to its documented neutral, per design doc section 6.2.
//! Order matters only for curv/curvR (curvR first is harmless either way
//! since \b already excludes it from the `curv` pattern, but doing it
//! first keeps the intent obvious to a reader).
static std::string BuildNeutralVariant( const std::string& original )
{
	std::string t = original;
	t = ReplaceCallsWithConstant( t, "occlusion", "1" );
	t = ReplaceCallsWithConstant( t, "thickness", "1" );
	t = ReplaceCallsWithConstant( t, "convexity", "0" );
	t = ReplaceCallsWithConstant( t, "proximity", "0" );
	t = ReplaceCallsWithConstant( t, "interior", "0" );
	t = ReplaceWholeWord( t, "curvR", "0" );
	t = ReplaceWholeWord( t, "curv", "0" );
	return t;
}

struct RasterizerBlockInfo
{
	bool found = false;
	std::size_t blockStart = 0, blockEnd = 0;
	bool hasRadianceMap = false;
	std::string radianceMap;
	bool hasRadianceBackground = false;
	std::string radianceBackground;
};

static const char* kKnownRasterizerKeywords[] = {
	"pathtracing_pel_rasterizer", "pathtracing_spectral_rasterizer",
	"bdpt_pel_rasterizer", "bdpt_spectral_rasterizer",
	"vcm_pel_rasterizer", "vcm_spectral_rasterizer",
	"mlt_rasterizer", "mlt_spectral_rasterizer",
	"pixelpel_rasterizer", "pixelspectral_rasterizer",
	"auto_rasterizer", "auto_spectral_rasterizer",
};

static RasterizerBlockInfo FindRasterizerBlock( const std::string& text )
{
	RasterizerBlockInfo info;
	for( const char* kw : kKnownRasterizerKeywords ) {
		const std::string kws( kw );
		std::size_t pos = text.find( kws );
		while( pos != std::string::npos ) {
			const bool preOk = ( pos == 0 ) ||
				!( std::isalnum( static_cast<unsigned char>(text[pos-1]) ) || text[pos-1] == '_' );
			const std::size_t after = pos + kws.size();
			const bool postOk = after < text.size() &&
				( text[after] == '\n' || text[after] == ' ' || text[after] == '\t' || text[after] == '\r' );
			if( preOk && postOk ) {
				std::size_t brace = text.find( '{', after );
				if( brace == std::string::npos ) return info;
				int depth = 0;
				std::size_t k = brace;
				for( ; k < text.size(); ++k ) {
					if( text[k] == '{' ) depth++;
					else if( text[k] == '}' ) { depth--; if( depth == 0 ) { k++; break; } }
				}
				info.found = true;
				info.blockStart = pos;
				info.blockEnd = k;
				const std::string block = text.substr( pos, k - pos );
				std::smatch m;
				if( std::regex_search( block, m, std::regex( "radiance_map\\s+(\\S+)" ) ) ) {
					info.hasRadianceMap = true;
					info.radianceMap = m[1];
				}
				if( std::regex_search( block, m, std::regex( "radiance_background\\s+(\\S+)" ) ) ) {
					info.hasRadianceBackground = true;
					info.radianceBackground = m[1];
				}
				return info;
			}
			pos = text.find( kws, pos + 1 );
		}
	}
	return info;
}

//! Replace the film chunk's width/height with an aspect-preserving
//! ~160-wide target (design doc section 6.2: "reduced resolution, ~
//! 160x120").
static std::string ResizeFilmChunk( const std::string& text, unsigned int targetWidth )
{
	std::smatch filmMatch;
	static const std::regex filmRe( "film\\s*\\{" );
	if( !std::regex_search( text, filmMatch, filmRe ) ) return text;
	const std::size_t filmStart = filmMatch.position(0);
	std::size_t brace = text.find( '{', filmStart );
	int depth = 0;
	std::size_t k = brace;
	for( ; k < text.size(); ++k ) {
		if( text[k] == '{' ) depth++;
		else if( text[k] == '}' ) { depth--; if( depth == 0 ) { k++; break; } }
	}
	std::string block = text.substr( filmStart, k - filmStart );
	std::smatch wm, hm;
	unsigned int origW = 800, origH = 600;
	if( std::regex_search( block, wm, std::regex( "width\\s+(\\d+)" ) ) ) origW = std::stoul( wm[1] );
	if( std::regex_search( block, hm, std::regex( "height\\s+(\\d+)" ) ) ) origH = std::stoul( hm[1] );
	unsigned int newW = targetWidth;
	unsigned int newH = std::max<unsigned int>( 1u, (unsigned int)std::lround( double(targetWidth) * double(origH) / double(origW) ) );

	std::string newBlock = std::regex_replace( block, std::regex( "width\\s+\\d+" ), "width " + std::to_string(newW) );
	newBlock = std::regex_replace( newBlock, std::regex( "height\\s+\\d+" ), "height " + std::to_string(newH) );

	return text.substr( 0, filmStart ) + newBlock + text.substr( k );
}

//! Swap the showcase's own rasterizer chunk for the requested integrator,
//! carrying over radiance_map/radiance_background if the original chunk
//! set them, forcing oidn_denoise FALSE + pixel_filter box + a reduced
//! sample count.  No adaptive_* params are set (default is disabled).
static std::string SwapRasterizer( const std::string& text, Integrator integrator, unsigned int samples )
{
	const RasterizerBlockInfo info = FindRasterizerBlock( text );
	Check( info.found, "Layer 2: the showcase's rasterizer chunk is found" );
	if( !info.found ) return text;

	// NOTE: no standard_shader chunk is emitted here.  Every one of the
	// four showcases already declares `standard_shader { name global ...
	// }` (plank_closeup / tidal_stones / shelf_bunny with `shaderop
	// DefaultPathTracing`, pavilion_colonnade with `shaderop
	// DefaultDirectLighting`).  Emitting a second chunk of the same name
	// is a duplicate-name derive failure; the modern PT/BDPT/VCM
	// rasterizers below don't consult the shader chain's op at all --
	// they only need `defaultshader` to resolve to a real shader chunk
	// (BDPTStrategyBalanceTest's own header explains why the legacy
	// pixelpel_rasterizer, unlike these, DOES execute the chain
	// literally) -- so pavilion_colonnade's DefaultDirectLighting "global"
	// is a perfectly valid reference for these three swapped-in chunks.
	std::ostringstream ss;
	switch( integrator ) {
		case Integrator::PT:
			ss << "pathtracing_pel_rasterizer\n{\n\tdefaultshader global\n\tsamples " << samples << "\n\trr_min_depth 8\n";
			break;
		case Integrator::BDPT:
			ss << "bdpt_pel_rasterizer\n{\n\tdefaultshader global\n\tmax_eye_depth 4\n\tmax_light_depth 4\n\tsamples " << samples << "\n";
			break;
		case Integrator::VCM:
			ss << "vcm_pel_rasterizer\n{\n\tdefaultshader global\n\tmax_eye_depth 4\n\tmax_light_depth 4\n\tsamples " << samples
			   << "\n\tmerge_radius 0.0\n\tvc_enabled true\n\tvm_enabled true\n";
			break;
		case Integrator::MLT:
			ss << "mlt_rasterizer\n{\n\tdefaultshader global\n\tmax_eye_depth 4\n\tmax_light_depth 4\n\tbootstrap_samples 4096\n\tchains 64\n\tmutations_per_pixel 16\n\tlarge_step_prob 0.3\n";
			break;
	}
	ss << "\tpixel_filter box\n\toidn_denoise FALSE\n";
	if( info.hasRadianceMap )        ss << "\tradiance_map " << info.radianceMap << "\n";
	if( info.hasRadianceBackground ) ss << "\tradiance_background " << info.radianceBackground << "\n";
	ss << "}\n\n";
	ss << "file_rasterizeroutput\n{\n\tpattern /tmp/signal_consistency_showcase_unused\n\ttype PNG\n\tbpp 8\n\tcolor_space sRGB\n}\n\n";

	return text.substr( 0, info.blockStart ) + ss.str() + text.substr( info.blockEnd );
}

struct ShowcaseSpec
{
	const char* keyword;
	const char* relPath;
};

static const ShowcaseSpec kShowcases[] = {
	{ "plank",    "scenes/FeatureBased/Textures/plank_closeup.RISEscene" },
	{ "tidal",    "scenes/FeatureBased/Textures/tidal_stones.RISEscene" },
	{ "bunny",    "scenes/FeatureBased/Textures/shelf_bunny.RISEscene" },
	{ "pavilion", "scenes/FeatureBased/Combined/pavilion_colonnade.RISEscene" },
};

static const unsigned int kLayer2TargetWidth = 160;
static const unsigned int kLayer2Samples = 16;
static const double kLayer2Band = 0.05;			// design doc section 6.2's own number
static const double kLayer2SensitivityBand = 0.05;

static void RunLayer2Showcase( const fs::path& root, const ShowcaseSpec& spec )
{
	const fs::path scenePath = root / spec.relPath;
	const std::string original = ReadFile( scenePath );
	Check( !original.empty(), std::string( "Layer 2: " ) + spec.keyword + " scene file reads" );
	if( original.empty() ) return;

	std::string base = ResizeFilmChunk( original, kLayer2TargetWidth );

	const std::string variantE = base;
	const std::string variantB = BuildNeutralVariant( base );

	std::cout << "\n=== LAYER 2: " << spec.keyword << " ===" << std::endl;

	double meanPT_E = 0, meanPT_B = 0;
	bool ptOk = false;
	struct Row { Integrator integ; double meanE, meanB; };
	std::vector<Row> rows;

	for( Integrator integ : { Integrator::PT, Integrator::BDPT, Integrator::VCM } ) {
		const std::string sceneE = SwapRasterizer( variantE, integ, kLayer2Samples );
		const std::string sceneB = SwapRasterizer( variantB, integ, kLayer2Samples );

		Cst::Document docE = Cst::ParseToCst( sceneE );
		Cst::Document docB = Cst::ParseToCst( sceneB );

		Job* jobE = new Job();
		Job* jobB = new Job();
		std::vector<std::string> diagsE, diagsB;
		Cst::DeriveToJob( docE, *jobE, &diagsE );
		Cst::DeriveToJob( docB, *jobB, &diagsB );
		for( const auto& d : diagsE ) std::cout << "  " << spec.keyword << " " << IntegratorName(integ) << " E diagnostic: " << d << std::endl;
		for( const auto& d : diagsB ) std::cout << "  " << spec.keyword << " " << IntegratorName(integ) << " B diagnostic: " << d << std::endl;
		Check( diagsE.empty(), std::string( spec.keyword ) + " " + IntegratorName(integ) + " variant E derives with no diagnostics" );
		Check( diagsB.empty(), std::string( spec.keyword ) + " " + IntegratorName(integ) + " variant B derives with no diagnostics" );
		if( !diagsE.empty() || !diagsB.empty() ) { safe_release( jobE ); safe_release( jobB ); continue; }

		jobE->RemoveRasterizerOutputs();
		jobB->RemoveRasterizerOutputs();

		CapturingRasterizerOutput* capE = new CapturingRasterizerOutput(); capE->addref();
		CapturingRasterizerOutput* capB = new CapturingRasterizerOutput(); capB->addref();
		jobE->GetRasterizer()->AddRasterizerOutput( capE );
		jobB->GetRasterizer()->AddRasterizerOutput( capB );

		const bool renderedE = jobE->Rasterize();
		const bool renderedB = jobB->Rasterize();
		Check( renderedE, std::string( spec.keyword ) + " " + IntegratorName(integ) + " variant E renders" );
		Check( renderedB, std::string( spec.keyword ) + " " + IntegratorName(integ) + " variant B renders" );

		ImageStats statsE = renderedE ? ComputeStats( *capE ) : ImageStats{};
		ImageStats statsB = renderedB ? ComputeStats( *capB ) : ImageStats{};
		capE->release();
		capB->release();
		safe_release( jobE );
		safe_release( jobB );

		std::cout << "  " << IntegratorName(integ) << ": mean(E)=" << statsE.mean << " mean(B)=" << statsB.mean << std::endl;

		if( integ == Integrator::PT ) {
			meanPT_E = statsE.mean;
			meanPT_B = statsB.mean;
			ptOk = statsE.valid && statsB.valid;
		}
		rows.push_back( { integ, statsE.mean, statsB.mean } );
	}

	if( !ptOk || meanPT_B == 0.0 ) {
		std::cout << "  NOTE: " << spec.keyword << " PT reference unavailable -- skipping ratio assertions for this showcase." << std::endl;
		return;
	}

	const double sensitivity = meanPT_E / meanPT_B - 1.0;
	std::cout << "  SENSITIVITY: mean(PT,E)=" << meanPT_E << " mean(PT,B)=" << meanPT_B
	          << " ratio-1=" << sensitivity << std::endl;

	if( std::fabs( sensitivity ) <= kLayer2SensitivityBand ) {
		std::cout << "  NOTE: " << spec.keyword << " is NOT sensitive to its own signal calls at this resolution/spp "
		          << "(|ratio-1| = " << std::fabs(sensitivity) << " <= " << kLayer2SensitivityBand
		          << ") -- dropped from the R_E/R_B assertion (cannot witness the gap either way)." << std::endl;
		return;
	}

	for( const Row& r : rows ) {
		if( r.integ == Integrator::PT ) continue;
		if( r.meanE == 0.0 || meanPT_E == 0.0 || r.meanB == 0.0 ) {
			Check( false, std::string( spec.keyword ) + " " + IntegratorName(r.integ) + ": non-zero means for R_E/R_B" );
			continue;
		}
		const double R_E = r.meanE / meanPT_E;
		const double R_B = r.meanB / meanPT_B;
		const double ratio = R_E / R_B - 1.0;
		std::cout << "  " << IntegratorName(r.integ) << ": R_E=" << R_E << " R_B=" << R_B
		          << " R_E/R_B-1=" << ratio
		          << ( std::fabs(ratio) < kLayer2Band ? "  [pass]" : "  [FAIL]" ) << std::endl;
		Check( std::fabs( ratio ) < kLayer2Band,
			std::string( spec.keyword ) + " " + IntegratorName(r.integ) + ": | R_E/R_B - 1 | < " + std::to_string(kLayer2Band) );
	}
}

//======================================================================
// main
//======================================================================

int main( int argc, char** argv )
{
	(void)argc; (void)argv;

	if( const char* env = std::getenv( "SIGNAL_CONSISTENCY_FILTER" ) ) {
		g_filter = env;
	}

	std::cout << "SignalIntegratorConsistencyTest -- docs/SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md S2" << std::endl;
	if( !g_filter.empty() ) std::cout << "Filter: " << g_filter << std::endl;

	if( WantsLayer1() ) {
		double convexityHarness = 0, occlusionHarness = 0, thicknessHarness = 0;

		SignalUnitScene curvScene = BuildCurvScene();
		SignalUnitScene convexityScene = BuildConvexityScene( &convexityHarness );
		SignalUnitScene occlusionScene = BuildOcclusionScene( &occlusionHarness );
		SignalUnitScene thicknessScene = BuildThicknessScene( &thicknessHarness );
		SignalUnitScene proximityScene = BuildProximityScene();
		SignalUnitScene interiorScene = BuildInteriorScene();

		std::cout << "\nReference harness values: convexity(0.3)=" << convexityHarness
		          << "  occlusion-derived t=" << occlusionHarness
		          << "  thickness(0.1)=" << thicknessHarness << std::endl;

		RunLayer1Scene( curvScene, /*includeMlt=*/true );
		RunLayer1Scene( convexityScene, false );
		RunLayer1Scene( occlusionScene, false );
		RunLayer1Scene( thicknessScene, false );
		RunLayer1Scene( proximityScene, false );
		RunLayer1Scene( interiorScene, false );
	}

	bool anyShowcase = false;
	for( const ShowcaseSpec& spec : kShowcases ) if( WantsShowcase( spec.keyword ) ) anyShowcase = true;
	if( anyShowcase ) {
		const fs::path root = FindRepoRoot();
		Check( !root.empty(), "Layer 2: repo root found" );
		if( !root.empty() ) {
			for( const ShowcaseSpec& spec : kShowcases ) {
				if( !WantsShowcase( spec.keyword ) ) continue;
				RunLayer2Showcase( root, spec );
			}
		}
	}

	std::cout << "\n========================================" << std::endl;
	std::cout << "Passed: " << passCount << "  Failed: " << failCount << std::endl;
	std::cout << "========================================" << std::endl;

	return failCount > 0 ? 1 : 0;
}
