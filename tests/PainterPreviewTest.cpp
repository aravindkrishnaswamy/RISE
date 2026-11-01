//////////////////////////////////////////////////////////////////////
//
//  PainterPreviewTest.cpp - doc 88 S10 (Tier-2 panel affordances):
//    proves the headless painter-preview engine (PainterPreview.h)
//    end to end -- direct engine calls against a plain Job for the
//    deterministic/refusal coverage, then the SceneEditController
//    wrapper (GetPainterPreview/GetRampStripPreview) against a LIVE
//    controller with a real in-flight render for the concurrency
//    refusal, and one C-ABI smoke call proving the bridge shim
//    (RISE_API_SceneEditController_PainterPreview) round-trips a real
//    buffer.
//
//  Coverage map (one test function each):
//    1  uniformcolor_painter preview: every pixel equals the exact
//       gamma-2.2-encoded expected RGBA (flat colour, no variation).
//    2  perlin3d_painter preview: NOT flat (genuine spatial variation)
//       and DETERMINISTIC (two independent renders byte-identical).
//    3  expression_painter: the final (vec3) preview differs from an
//       intermediate scalar def-stage preview; the scalar stage is
//       auto-range normalized with the reported range pinned, and its
//       grayscale is monotonically increasing left-to-right (the
//       known linear-in-u field the fixture's `def raw u` sets up).
//    4  ramp_painter gradient strip: the left/right edges land close
//       to the authored stop colours (raster golden, pixel-center
//       sampling means "close to", not exact), AND a direct
//       RampPainter::EvalAt call at the exact knot positions is
//       bit-for-bit the authored stop colour (true golden).
//    5  Refusals: unknown painter name; def-stage preview on a
//       non-expression painter; def-stage preview with an out-of-
//       range index; ramp-strip preview on a non-ramp painter; zero
//       and over-cap dimensions.
//    6  Concurrency: PainterPreview/RampStripPreview REFUSE (return
//       false, quickly -- no wedge) while a production render owns
//       the scene, and SUCCEED again once it completes.
//    7  C-ABI smoke: RISE_API_SceneEditController_PainterPreview
//       fills a caller-owned buffer for a real painter.
//    8  A high-frequency expression body (fbm at domain scale 820,
//       the shape every real procedural material uses) previews with
//       genuine structure through BOTH entry points -- the regression
//       guard for the synthesized-footprint flat-preview bug; see the
//       test's own comment.
//
//  Self-contained: no RISE_MEDIA_PATH; inline native-v7 scenes.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <string>
#include <fstream>
#include <cstdio>
#include <cmath>
#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
#include <vector>

#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/IPainter.h"
#include "../src/Library/Interfaces/IPainterManager.h"
#include "../src/Library/SceneEditor/SceneEditController.h"
#include "../src/Library/SceneEditor/PainterPreview.h"
#include "../src/Library/SceneEditor/MaterialPreview.h"
#include "../src/Library/Painters/Painter.h"
#include "../src/Library/Painters/RampPainter.h"
#include "../src/Library/Agent/AgentSession.h"

using namespace RISE;
using namespace RISE::Implementation;
using namespace RISE::Agent;

static int passCount = 0, failCount = 0;
static void Check( bool c, const char* n )
{
	if( c ) { ++passCount; }
	else    { ++failCount; std::cout << "  FAIL: " << n << std::endl; }
}

//////////////////////////////////////////////////////////////////////
// Fixtures
//////////////////////////////////////////////////////////////////////

// A headless (no camera/film/rasterizer) fixture -- direct-engine tests
// never render, so this mirrors PainterIntrospectionRoundTripTest's
// minimal tail (a luminaire material + sphere + object) purely so the
// scene parses; nothing here is ever rasterized.
static const char* kHeadlessScene =
	"RISE ASCII SCENE 7\n"
	"uniformcolor_painter\n{\nname solid\ncolor 0.2 0.4 0.6\n}\n"
	"uniformcolor_painter\n{\nname white\ncolor 1 1 1\n}\n"
	"uniformcolor_painter\n{\nname black\ncolor 0 0 0\n}\n"
	"perlin3d_painter\n{\nname noise\ncolora white\ncolorb black\noctaves 4\npersistence 0.5\nscale 2 2 2\n}\n"
	"expression_painter\n{\nname expr1\n"
		"def raw u\n"
		"def scaled raw*10-5\n"
		"expr vec3(scaled, scaled, scaled)\n"
	"}\n"
	// P2-3(a): a def stage whose body is a bare numeric literal -- constant
	// across the whole patch, so its auto-range is degenerate (min == max)
	// and RenderDefStagePreview's uniform-mid-gray branch is what's on test.
	"expression_painter\n{\nname exprFlat\n"
		"def flatval 5\n"
		"expr vec3(flatval, flatval, flatval)\n"
	"}\n"
	// P2-3(b): a VEC3-typed intermediate def stage (not the final expr) --
	// proves RenderDefStagePreview shows the true (R,G,B) triple for a
	// vec3 def, not a v[0]-broadcast grayscale.  `expr rgbdef` just passes
	// the def through so the chunk has a valid final expression; the test
	// only ever previews def[0].
	"expression_painter\n{\nname exprVec3Def\n"
		"def rgbdef vec3(u, v, 0.25)\n"
		"expr rgbdef\n"
	"}\n"
	// P2-4: a scalar_painter{expression} whose final expr is vec3-typed --
	// ExpressionScalarPainter::HasPerChannelVariation() is true for exactly
	// this shape (see ExpressionPainter.h), so RenderPainterPreview's
	// scalar pipe must render it as a genuine per-channel RGB triple,
	// jointly auto-ranged, rather than v[0]-only grayscale.
	"scalar_painter\n{\nname scalarVec3\n"
		"def rgbdef vec3(u*3, v, 0.1)\n"
		"expression rgbdef\n"
	"}\n"
	// Test 8: a HIGH-FREQUENCY noise body -- the shape every real
	// procedural material uses (plank_closeup's def stages run
	// 130 .. 820).  Its preview must show structure; see
	// PainterPreview.cpp's kPreviewFootprintWidth for why a synthesized
	// footprint made exactly this body preview as a uniform square.
	"expression_painter\n{\nname exprHiFreq\n"
		"def hf fbm(P*820.0, 3, 0.5, 2.0)\n"
		"expr vec3(0.5+0.5*hf, 0.5+0.5*hf, 0.5+0.5*hf)\n"
	"}\n"
	"ramp_painter\n{\nname strip1\ninput solid\nchannel R\ninterpolation linear\n"
		"stop 0.0 1 0 0\nstop 1.0 0 0 1\n}\n"
	"scalar_painter\n{\nname pnt_alpha\nvalue 0.08\n}\n"
	"scalar_painter\n{\nname pnt_ior\nvalue 1.5\n}\n"
	"scalar_painter\n{\nname pnt_ext\nvalue 2.2\n}\n"
	"scalar_painter\n{\nname pnt_film_ior\nvalue 1.38\n}\n"
	"scalar_painter\n{\nname pnt_thick_380\nvalue 380\n}\n"
	"scalar_painter\n{\nname pnt_thick_520\nvalue 520\n}\n"
	"scalar_painter\n{\nname pnt_tau\nvalue 0.9999\n}\n"
	"scalar_painter\n{\nname pnt_sellmeier\nsellmeier 1.03961212 0.231792344 1.01046945 0.00600069867 0.0200179144 103.560653\n}\n"
	"lambertian_material\n{\nname mat_lambert\nreflectance solid\n}\n"
	"ggx_material\n{\nname mat_thinfilm_380\nrd black\nrs white\nalphax pnt_alpha\nalphay pnt_alpha\nfresnel_mode thinfilm\nior pnt_ior\nextinction pnt_ext\nfilm_ior pnt_film_ior\nfilm_thickness pnt_thick_380\n}\n"
	"ggx_material\n{\nname mat_thinfilm_520\nrd black\nrs white\nalphax pnt_alpha\nalphay pnt_alpha\nfresnel_mode thinfilm\nior pnt_ior\nextinction pnt_ext\nfilm_ior pnt_film_ior\nfilm_thickness pnt_thick_520\n}\n"
	"dielectric_material\n{\nname mat_glass\ntau pnt_tau\nior pnt_sellmeier\nscattering pnt_alpha\n}\n"
	"lambertian_luminaire_material\n{\nname lum\nexitance white\nscale 5.0\nmaterial none\n}\n"
	"sphere_geometry\n{\nname s\nradius 1\n}\n"
	"standard_object\n{\nname obj\ngeometry s\nmaterial lum\n}\n";

// A fully renderable fixture (camera/film/pathtracing rasterizer),
// mirroring AgentRenderAsyncTest.cpp's kScene, plus one extra painter
// (`probe`) so the concurrency test has something real to preview.
static const char* kLiveScene =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 8\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 24\n\theight 24\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname probe\n\tcolor 0.3 0.6 0.9\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_albedo\n\tcolor 0.5 0.5 0.5\n}\n\n"
	"lambertian_material\n{\n\tname mat_diffuse\n\treflectance pnt_albedo\n}\n\n"
	"sphere_geometry\n{\n\tname sph\n\tradius 0.8\n}\n\n"
	"standard_object\n{\n\tname obj_sph\n\tgeometry sph\n\tmaterial mat_diffuse\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1.0 1.0 1.0\n}\n\n"
	"lambertian_luminaire_material\n{\n\tname mat_emit\n\texitance pnt_emit\n\tscale 30.0\n\tmaterial none\n}\n\n"
	"clippedplane_geometry\n{\n\tname quad_emit\n\tpta -0.6 0.6 3.5\n\tptb 0.6 0.6 3.5\n\tptc 0.6 -0.6 3.5\n\tptd -0.6 -0.6 3.5\n}\n\n"
	"standard_object\n{\n\tname obj_emit\n\tgeometry quad_emit\n\tmaterial mat_emit\n}\n";

static Job* LoadScene( const char* text, const char* tmpPath )
{
	{ std::ofstream o( tmpPath, std::ios::binary ); o << text; }
	Job* pJob = new Job();
	if( !pJob->LoadAsciiSceneViaCst( tmpPath ) ) {
		pJob->release();
		std::remove( tmpPath );
		return nullptr;
	}
	return pJob;
}

// Same fixed [0,1]-clamp + gamma-2.2 encode PainterPreview.cpp uses --
// duplicated here deliberately (not #include-d from the library) so the
// test is an independent check of the documented contract, not a tautology
// against the implementation it is meant to catch regressions in.
static unsigned char ExpectedChannel( double linear )
{
	double c = linear;
	if( !( c > 0.0 ) ) c = 0.0;
	if( c > 1.0 ) c = 1.0;
	const double enc = std::pow( c, 1.0 / 2.2 );
	int v = (int)( enc * 255.0 + 0.5 );
	if( v < 0 ) v = 0;
	if( v > 255 ) v = 255;
	return (unsigned char)v;
}

//////////////////////////////////////////////////////////////////////
// Test 1: flat uniformcolor preview
//////////////////////////////////////////////////////////////////////
static void TestUniformColorFlat()
{
	std::cout << "Test 1: uniformcolor_painter preview is flat and matches the documented encode..." << std::endl;
	const char* tmp = "painterpreview_flat.RISEscene";
	Job* j = LoadScene( kHeadlessScene, tmp );
	Check( j != nullptr, "headless fixture scene loads" );
	if( !j ) return;

	const PainterPreview::Result r = PainterPreview::RenderPainterPreview( *j, String( "solid" ), 8, 8 );
	Check( r.status == PainterPreview::Status::Ok, "preview of `solid` succeeds" );
	if( r.status == PainterPreview::Status::Ok )
	{
		Check( r.width == 8 && r.height == 8, "reports the requested dimensions" );
		Check( r.rgba.size() == 8u * 8u * 4u, "buffer is exactly w*h*4 bytes" );
		Check( !r.wasScalar, "a colour-pipe preview is not marked scalar" );

		const unsigned char eR = ExpectedChannel( 0.2 ), eG = ExpectedChannel( 0.4 ), eB = ExpectedChannel( 0.6 );
		bool allMatch = true;
		for( std::size_t p = 0; p < r.rgba.size(); p += 4 ) {
			if( r.rgba[p+0] != eR || r.rgba[p+1] != eG || r.rgba[p+2] != eB || r.rgba[p+3] != 255 )
				allMatch = false;
		}
		Check( allMatch, "every pixel equals the gamma-2.2-encoded (0.2,0.4,0.6) -- flat colour, no positional variation" );
	}
	j->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 2: perlin3d varies + is deterministic
//////////////////////////////////////////////////////////////////////
static void TestPerlinVariesDeterministic()
{
	std::cout << "Test 2: perlin3d_painter preview varies spatially and is deterministic..." << std::endl;
	const char* tmp = "painterpreview_perlin.RISEscene";
	Job* j = LoadScene( kHeadlessScene, tmp );
	Check( j != nullptr, "headless fixture scene loads" );
	if( !j ) return;

	const PainterPreview::Result r1 = PainterPreview::RenderPainterPreview( *j, String( "noise" ), 32, 32 );
	const PainterPreview::Result r2 = PainterPreview::RenderPainterPreview( *j, String( "noise" ), 32, 32 );
	Check( r1.status == PainterPreview::Status::Ok && r2.status == PainterPreview::Status::Ok,
	       "both perlin3d previews succeed" );
	if( r1.status == PainterPreview::Status::Ok && r2.status == PainterPreview::Status::Ok )
	{
		Check( r1.rgba == r2.rgba, "two independent renders of the same painter are byte-identical (deterministic)" );

		bool varies = false;
		for( std::size_t p = 4; p < r1.rgba.size(); p += 4 ) {
			if( r1.rgba[p] != r1.rgba[0] || r1.rgba[p+1] != r1.rgba[1] || r1.rgba[p+2] != r1.rgba[2] ) { varies = true; break; }
		}
		Check( varies, "the patch is NOT flat -- genuine spatial structure from the 3D-context P sweep" );
	}
	j->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 3: expression def-stage vs final, scalar auto-range
//////////////////////////////////////////////////////////////////////
static void TestExpressionDefStages()
{
	std::cout << "Test 3: expression_painter def-stage preview differs from final; scalar auto-range is pinned..." << std::endl;
	const char* tmp = "painterpreview_expr.RISEscene";
	Job* j = LoadScene( kHeadlessScene, tmp );
	Check( j != nullptr, "headless fixture scene loads" );
	if( !j ) return;

	const unsigned int W = 16, H = 4;
	const PainterPreview::Result stage0 = PainterPreview::RenderDefStagePreview( *j, String( "expr1" ), 0, W, H );  // def raw = u
	const PainterPreview::Result stage1 = PainterPreview::RenderDefStagePreview( *j, String( "expr1" ), 1, W, H );  // def scaled = raw*10-5
	const PainterPreview::Result final_ = PainterPreview::RenderPainterPreview( *j, String( "expr1" ), W, H );      // vec3(scaled,scaled,scaled)

	Check( stage0.status == PainterPreview::Status::Ok, "def[0] (`raw`) preview succeeds" );
	Check( stage1.status == PainterPreview::Status::Ok, "def[1] (`scaled`) preview succeeds" );
	Check( final_.status == PainterPreview::Status::Ok, "final-expression preview succeeds" );
	if( stage0.status != PainterPreview::Status::Ok || stage1.status != PainterPreview::Status::Ok ||
	    final_.status != PainterPreview::Status::Ok ) { j->release(); std::remove( tmp ); return; }

	Check( stage0.wasScalar && stage1.wasScalar, "both def stages are scalar-typed (`u`, `raw*10-5`)" );
	Check( !final_.wasScalar, "the final vec3(...) expression is NOT marked scalar" );

	// def[0] = u in [~0,~1] (pixel-centre sampled, so not exactly the endpoints).
	Check( stage0.scalarRangeMin >= -1e-6 && stage0.scalarRangeMin < 0.2, "def[0] auto-range min is near 0" );
	Check( stage0.scalarRangeMax > 0.8 && stage0.scalarRangeMax <= 1.0 + 1e-6, "def[0] auto-range max is near 1" );

	// def[1] = raw*10-5, so its range is def[0]'s range scaled/shifted -- pin
	// the documented auto-range CONTRACT (the actual data range is reported,
	// not a fixed [0,1]), not just "some plausible number".
	Check( stage1.scalarRangeMin > -5.5 && stage1.scalarRangeMin < -3.5, "def[1] auto-range min is near -5 (10*range_min-5)" );
	Check( stage1.scalarRangeMax > 3.5  && stage1.scalarRangeMax < 5.5,  "def[1] auto-range max is near +5 (10*range_max-5)" );

	Check( stage1.rgba != final_.rgba, "an intermediate def-stage preview differs from the final-expression preview" );

	// Monotonic increasing brightness left-to-right on row 0 of stage1 (u
	// increases monotonically with x, and `scaled` is monotonic in `raw`) --
	// pins that the grayscale encode tracks the field's actual shape, not
	// just "some range was reported."
	bool monotonic = true;
	for( unsigned int x = 1; x < W; ++x ) {
		const unsigned char prev = stage1.rgba[ (x - 1) * 4 ];
		const unsigned char cur  = stage1.rgba[ x * 4 ];
		if( cur < prev ) { monotonic = false; break; }
	}
	Check( monotonic, "def[1]'s grayscale preview increases monotonically left-to-right (matches the linear-in-u field)" );

	j->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 4: ramp gradient strip
//////////////////////////////////////////////////////////////////////
static void TestRampStrip()
{
	std::cout << "Test 4: ramp_painter gradient strip matches its stop colours at the knots..." << std::endl;
	const char* tmp = "painterpreview_ramp.RISEscene";
	Job* j = LoadScene( kHeadlessScene, tmp );
	Check( j != nullptr, "headless fixture scene loads" );
	if( !j ) return;

	const unsigned int W = 256, H = 1;
	const PainterPreview::Result r = PainterPreview::RenderRampStripPreview( *j, String( "strip1" ), W, H );
	Check( r.status == PainterPreview::Status::Ok, "ramp strip preview succeeds" );
	if( r.status == PainterPreview::Status::Ok )
	{
		// Raster golden: with W=256 pixel-centre sampling, column 0's t is
		// ~0.002 from the t=0 knot and column W-1's t is ~0.002 from t=1.
		// The dominant channel (linear ~0.998) lands within a couple of
		// 8-bit steps of full-scale after gamma-2.2 encoding, but the
		// GROWING channel (linear ~0.002) is NOT correspondingly close to
		// zero -- gamma-2.2 encode (x^(1/2.2)) steeply BOOSTS small linear
		// values (0.002 encodes to ~15/255, not ~0), so this checks
		// "small", not "near-zero"; the true bit-exact knot check is the
		// direct RampPainter::EvalAt call below.
		const unsigned char leftR = r.rgba[0], leftG = r.rgba[1], leftB = r.rgba[2];
		const std::size_t rightIdx = (std::size_t)( W - 1 ) * 4;
		const unsigned char rightR = r.rgba[rightIdx+0], rightG = r.rgba[rightIdx+1], rightB = r.rgba[rightIdx+2];
		Check( leftR >= 250 && leftG == 0 && leftB <= 20, "left edge ~= authored stop 0 colour (1,0,0)" );
		Check( rightB >= 250 && rightG == 0 && rightR <= 20, "right edge ~= authored stop 1 colour (0,0,1)" );

		// Strip rows are a replicated single row (h forced by replication,
		// not new data) -- assert row 0 == every other row when H>1.
	}

	// True golden: RampPainter::EvalAt is bit-exact AT the knot positions --
	// reach the live painter directly (same dynamic_cast pattern
	// PainterIntrospection.cpp uses) rather than through the raster, which
	// never lands exactly on t=0/t=1 under pixel-centre sampling.
	IPainterManager* pm = j->GetPainters();
	IPainter* p = pm ? pm->GetItem( "strip1" ) : nullptr;
	const RampPainter* ramp = p ? dynamic_cast<const RampPainter*>( p ) : nullptr;
	Check( ramp != nullptr, "`strip1` resolves to a live RampPainter" );
	if( ramp )
	{
		const RISEPel c0 = ramp->EvalAt( ramp->StopPos( 0 ) );
		const RISEPel c1 = ramp->EvalAt( ramp->StopPos( ramp->StopCount() - 1 ) );
		Check( std::abs( (double)c0.r - 1.0 ) < 1e-9 && c0.g == 0 && c0.b == 0,
		       "EvalAt(firstStopPos) is bit-exact stop 0 (1,0,0)" );
		Check( c1.r == 0 && c1.g == 0 && std::abs( (double)c1.b - 1.0 ) < 1e-9,
		       "EvalAt(lastStopPos) is bit-exact stop 1 (0,0,1)" );
	}

	j->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 5: refusals
//////////////////////////////////////////////////////////////////////
static void TestRefusals()
{
	std::cout << "Test 5: clean refusals for unknown names, bad def indices, wrong painter kinds, bad dims..." << std::endl;
	const char* tmp = "painterpreview_refusals.RISEscene";
	Job* j = LoadScene( kHeadlessScene, tmp );
	Check( j != nullptr, "headless fixture scene loads" );
	if( !j ) return;

	{
		const PainterPreview::Result r = PainterPreview::RenderPainterPreview( *j, String( "does_not_exist" ), 8, 8 );
		Check( r.status == PainterPreview::Status::Refused && !r.refusalReason.empty(),
		       "unknown painter name is refused with a reason" );
	}
	{
		// `solid` is a uniformcolor_painter, not an expression painter.
		const PainterPreview::Result r = PainterPreview::RenderDefStagePreview( *j, String( "solid" ), 0, 8, 8 );
		Check( r.status == PainterPreview::Status::Refused, "def-stage preview on a non-expression painter is refused" );
	}
	{
		// expr1 has DefCount() == 2 (raw, scaled); index 2 is out of range.
		const PainterPreview::Result r = PainterPreview::RenderDefStagePreview( *j, String( "expr1" ), 2, 8, 8 );
		Check( r.status == PainterPreview::Status::Refused, "out-of-range defIndex is refused" );
	}
	{
		const PainterPreview::Result r = PainterPreview::RenderDefStagePreview( *j, String( "expr1" ), -1, 8, 8 );
		Check( r.status == PainterPreview::Status::Refused, "negative defIndex is refused" );
	}
	{
		// `noise` (perlin3d_painter) is not a ramp_painter.
		const PainterPreview::Result r = PainterPreview::RenderRampStripPreview( *j, String( "noise" ), 8, 1 );
		Check( r.status == PainterPreview::Status::Refused, "ramp-strip preview on a non-ramp painter is refused" );
	}
	{
		const PainterPreview::Result r = PainterPreview::RenderPainterPreview( *j, String( "solid" ), 0, 8 );
		Check( r.status == PainterPreview::Status::Refused, "zero width is refused" );
	}
	{
		const PainterPreview::Result r = PainterPreview::RenderPainterPreview( *j, String( "solid" ), 8, 0 );
		Check( r.status == PainterPreview::Status::Refused, "zero height is refused" );
	}
	{
		const PainterPreview::Result r = PainterPreview::RenderPainterPreview( *j, String( "solid" ),
			PainterPreview::kMaxDim + 1, 8 );
		Check( r.status == PainterPreview::Status::Refused, "over-cap width is refused" );
	}
	{
		// P2-3(c): the symmetric over-cap check on HEIGHT -- the width
		// case above exercised ValidDims' `w > kMaxDim` branch only; this
		// pins the `h > kMaxDim` branch independently.
		const PainterPreview::Result r = PainterPreview::RenderPainterPreview( *j, String( "solid" ),
			8, PainterPreview::kMaxDim + 1 );
		Check( r.status == PainterPreview::Status::Refused, "over-cap height is refused" );
	}

	j->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 5b (P2-3a): flat/degenerate scalar def stage -> uniform mid-gray
//////////////////////////////////////////////////////////////////////
static void TestFlatScalarDefStageIsUniformGray()
{
	std::cout << "Test 5b: a constant (flat) scalar def stage auto-ranges to uniform mid-gray, not divide-by-zero..." << std::endl;
	const char* tmp = "painterpreview_flatdef.RISEscene";
	Job* j = LoadScene( kHeadlessScene, tmp );
	Check( j != nullptr, "headless fixture scene loads" );
	if( !j ) return;

	const PainterPreview::Result r = PainterPreview::RenderDefStagePreview( *j, String( "exprFlat" ), 0, 8, 8 );
	Check( r.status == PainterPreview::Status::Ok, "exprFlat def[0] (`flatval` = 5) preview succeeds" );
	if( r.status == PainterPreview::Status::Ok )
	{
		Check( r.wasScalar, "a constant def stage is still scalar-typed" );
		Check( r.scalarRangeMin == 5.0 && r.scalarRangeMax == 5.0,
		       "auto-range collapses to [5,5] -- a literal has no spatial variation" );

		const unsigned char eGray = ExpectedChannel( 0.5 );
		bool allGray = true;
		for( std::size_t p = 0; p < r.rgba.size(); p += 4 ) {
			if( r.rgba[p+0] != eGray || r.rgba[p+1] != eGray || r.rgba[p+2] != eGray || r.rgba[p+3] != 255 )
				allGray = false;
		}
		Check( allGray, "every pixel is the degenerate-range uniform mid-gray (0.5), not a divide-by-zero artifact" );
	}

	j->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 5c (P2-3b): a vec3-typed def stage shows the true triple, not a
// v[0] broadcast.
//////////////////////////////////////////////////////////////////////
static void TestVec3DefStageShowsTrueTriple()
{
	std::cout << "Test 5c: a vec3-typed def stage's preview shows the true (R,G,B), not a scalar broadcast..." << std::endl;
	const char* tmp = "painterpreview_vec3def.RISEscene";
	Job* j = LoadScene( kHeadlessScene, tmp );
	Check( j != nullptr, "headless fixture scene loads" );
	if( !j ) return;

	const unsigned int W = 8, H = 4;
	const PainterPreview::Result r = PainterPreview::RenderDefStagePreview( *j, String( "exprVec3Def" ), 0, W, H );
	Check( r.status == PainterPreview::Status::Ok, "exprVec3Def def[0] (`rgbdef` = vec3(u,v,0.25)) preview succeeds" );
	if( r.status == PainterPreview::Status::Ok )
	{
		Check( !r.wasScalar, "a vec3-typed def stage is NOT marked scalar (no auto-range applied)" );

		// Pixel (x=2,y=1): u = 2.5/8 = 0.3125, v = 1.5/4 = 0.375, so R != G
		// (a broadcast bug would make every channel equal) and B is the
		// fixed 0.25 constant embedded in the def body.
		const std::size_t idx = ( (std::size_t)1 * W + 2 ) * 4;
		const unsigned char R = r.rgba[idx+0], G = r.rgba[idx+1], B = r.rgba[idx+2];
		Check( R != G, "R (from u) and G (from v) differ at a generic pixel -- the true triple, not a broadcast" );
		Check( R == ExpectedChannel( 0.3125 ), "R matches the exact gamma-2.2 encode of u at (x=2,y=1)" );
		Check( G == ExpectedChannel( 0.375 ),  "G matches the exact gamma-2.2 encode of v at (x=2,y=1)" );
		Check( B == ExpectedChannel( 0.25 ),   "B matches the def body's fixed 0.25 constant" );

		// Every pixel's B channel is the same fixed constant.
		bool bConstant = true;
		for( std::size_t p = 2; p < r.rgba.size(); p += 4 ) {
			if( r.rgba[p] != r.rgba[2] ) { bConstant = false; break; }
		}
		Check( bConstant, "B is constant across the whole patch (the def body's literal 0.25, unaffected by u/v)" );
	}

	j->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 5d (P2-4): a HasPerChannelVariation() scalar_painter renders its
// triple as RGB, jointly auto-ranged, not a v[0]-only grayscale.
//////////////////////////////////////////////////////////////////////
static void TestPerChannelScalarPreviewIsJointlyRangedRGB()
{
	std::cout << "Test 5d: a vec3-typed scalar_painter{expression} previews as a jointly-auto-ranged RGB triple..." << std::endl;
	const char* tmp = "painterpreview_perchannel.RISEscene";
	Job* j = LoadScene( kHeadlessScene, tmp );
	Check( j != nullptr, "headless fixture scene loads" );
	if( !j ) return;

	const unsigned int W = 8, H = 4;
	const PainterPreview::Result r = PainterPreview::RenderPainterPreview( *j, String( "scalarVec3" ), W, H );
	Check( r.status == PainterPreview::Status::Ok, "scalarVec3 (vec3-typed scalar_painter{expression}) preview succeeds" );
	if( r.status != PainterPreview::Status::Ok ) { j->release(); std::remove( tmp ); return; }

	Check( r.wasScalar, "the scalar pipe of origin is still reported (wasScalar == true) even though it renders as RGB" );

	// rgbdef = vec3(u*3, v, 0.1).  With pixel-centre sampling:
	//   u*3 ranges over ((0.5/8)*3, (7.5/8)*3) = (0.1875, 2.8125)
	//   v   ranges over (0.5/4, 3.5/4)         = (0.125, 0.875)
	//   the B channel is the fixed constant 0.1 at every pixel.
	// The JOINT range across all three channels is therefore
	// [0.1, 2.8125] -- the fixed 0.1 constant is the global min (lower
	// than any u*3 or v sample), and the top-right u*3 sample is the
	// global max.  A per-channel-INDEPENDENT range (the bug this test
	// guards against) would instead report something like [0,1] per
	// channel and never surface this cross-channel relationship.
	Check( std::abs( r.scalarRangeMin - 0.1 ) < 1e-9,
	       "joint auto-range MIN is the B channel's fixed 0.1 constant (lower than any u*3/v sample)" );
	Check( std::abs( r.scalarRangeMax - 2.8125 ) < 1e-9,
	       "joint auto-range MAX is the top u*3 sample (2.8125), not a per-channel-independent [0,1]" );

	// Not a broadcast: R and G differ at a generic pixel.
	bool rgDiffer = false;
	for( std::size_t p = 0; p < r.rgba.size(); p += 4 ) {
		if( r.rgba[p+0] != r.rgba[p+1] ) { rgDiffer = true; break; }
	}
	Check( rgDiffer, "R (from u*3) and G (from v) are NOT all equal -- a genuine per-channel triple" );

	// B is the fixed 0.1 constant at every pixel -- after the JOINT range
	// maps it to norm = (0.1-0.1)/(2.8125-0.1) = 0, every pixel's B
	// channel is the same encoded value.
	bool bConstant = true;
	for( std::size_t p = 2; p < r.rgba.size(); p += 4 ) {
		if( r.rgba[p] != r.rgba[2] ) { bConstant = false; break; }
	}
	Check( bConstant, "B (the fixed 0.1 constant) is uniform across the whole patch after joint normalization" );
	Check( r.rgba[2] == ExpectedChannel( 0.0 ), "B's normalized value is exactly 0 (it IS the joint range minimum)" );

	// The pixel with the maximum u*3 (x=W-1, any y) should have R at the
	// top of the encode (norm == 1.0, since that sample IS the joint max).
	const std::size_t topIdx = ( (std::size_t)0 * W + ( W - 1 ) ) * 4;
	Check( r.rgba[topIdx+0] == ExpectedChannel( 1.0 ),
	       "the pixel holding the joint-range maximum encodes R at full scale" );

	j->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 5e (P2-1): the evaluation-sample budget decimates a 2048x2048
// request instead of evaluating every one of its ~4.2M pixels, and
// still fills the full requested buffer.
//////////////////////////////////////////////////////////////////////

// A trivial IPainter that counts every GetColor() call -- lets the test
// observe the ACTUAL number of distinct evaluations PainterPreview
// performs, independent of timing (which would make the test flaky
// under CI load).  Minimal Painter subclass, same shape as
//////////////////////////////////////////////////////////////////////
// Test 8: a high-frequency noise body previews with structure
//
// REGRESSION GUARD.  This module used to synthesize a filter width of
// 1 / max(gw, gh) for its `ri`.  Once fbm/turbulence/ridged began
// rescaling `fw` by their position argument's compile-time domain
// scale (2026-09-06, ExpressionEval.h's Builder::NoiseFwScales), that
// synthetic width was multiplied by the body's own scale k and crossed
// the octave fade's hi = 0.6 at k ~= 58 -- so EVERY high-frequency
// body (the shipped plank_closeup stages run 130 .. 820) previewed as
// one uniform square, auto-range 0, while the real render of the same
// body was full of detail.  Both preview entry points are covered: the
// painter pipe (`RenderPainterPreview`, colour) and the def-stage pipe
// (`RenderDefStagePreview`, scalar + auto-range).
//
// Test 2 does not cover this: perlin3d_painter is a native painter and
// never runs through the expression VM, so no `fw` of any value can
// flatten it.
//////////////////////////////////////////////////////////////////////
static void TestHighFrequencyNoiseBodyIsNotFlat()
{
	std::cout << "Test 8: a high-frequency fbm(P*820) body previews with real structure, not a uniform square..." << std::endl;
	const char* tmp = "painterpreview_hifreq.RISEscene";
	Job* j = LoadScene( kHeadlessScene, tmp );
	Check( j != nullptr, "headless fixture scene loads" );
	if( !j ) return;

	const unsigned int W = 64, H = 64;

	// (a) The def stage -- scalar, so the AUTO-RANGE it reports is a
	// direct, encode-independent measurement of the field's spread.
	// 0.2 is a deliberately loose floor: the flat-preview bug reported
	// EXACTLY 0 here, and the measured spread of this body point-
	// sampled on a 64x64 patch is several times the threshold.
	const PainterPreview::Result stage = PainterPreview::RenderDefStagePreview( *j, String( "exprHiFreq" ), 0, W, H );
	Check( stage.status == PainterPreview::Status::Ok, "def[0] (`hf`, an fbm at domain scale 820) preview succeeds" );
	if( stage.status == PainterPreview::Status::Ok ) {
		Check( stage.wasScalar, "the fbm def stage is scalar-typed" );
		const double range = stage.scalarRangeMax - stage.scalarRangeMin;
		Check( range > 0.2, "the def stage's auto-range spread is > 0.2 -- the field is NOT constant across the patch" );

		// The reported range could in principle come from a couple of
		// outlier pixels, so also require the RASTER to carry real
		// variety: many distinct grey levels, not two.
		bool seen[256] = { false };
		int distinct = 0;
		for( std::size_t p = 0; p < stage.rgba.size(); p += 4 ) {
			if( !seen[ stage.rgba[p] ] ) { seen[ stage.rgba[p] ] = true; ++distinct; }
		}
		Check( distinct > 32, "the def-stage raster shows > 32 distinct grey levels (structure, not a two-tone artifact)" );
	}

	// (b) The final colour expression through the painter pipe -- the
	// path a panel swatch actually takes.  No auto-range here (colour
	// previews use the fixed [0,1] encode), so the measurement is the
	// raster's own spread.
	const PainterPreview::Result full = PainterPreview::RenderPainterPreview( *j, String( "exprHiFreq" ), W, H );
	Check( full.status == PainterPreview::Status::Ok, "the full expression_painter preview succeeds" );
	if( full.status == PainterPreview::Status::Ok ) {
		unsigned char lo = 255, hi = 0;
		for( std::size_t p = 0; p < full.rgba.size(); p += 4 ) {
			if( full.rgba[p] < lo ) lo = full.rgba[p];
			if( full.rgba[p] > hi ) hi = full.rgba[p];
		}
		Check( hi > lo, "the colour preview is NOT flat" );
		Check( (int)hi - (int)lo > 24, "the colour preview's 8-bit spread is > 24 levels (visible structure, not dithering)" );
	}

	j->release();
	std::remove( tmp );
}

// UniformColorPainter but with no state beyond the counter.
class CountingProbePainter : public Implementation::Painter
{
public:
	mutable std::atomic<long> evalCount{ 0 };

	RISEPel GetColor( const RayIntersectionGeometric& ) const override
	{
		evalCount.fetch_add( 1, std::memory_order_relaxed );
		return RISEPel( 0.5, 0.5, 0.5 );
	}
	IKeyframeParameter* KeyframeFromParameters( const String&, const String& ) override { return nullptr; }
	void SetIntermediateValue( const IKeyframeParameter& ) override {}
	void RegenerateData() override {}
};

static void TestEvaluationBudgetDecimatesLargeRequests()
{
	std::cout << "Test 5e: a 2048x2048 preview request is decimated to the sample budget, not evaluated pixel-for-pixel..." << std::endl;
	const char* tmp = "painterpreview_budget.RISEscene";
	Job* j = LoadScene( kHeadlessScene, tmp );
	Check( j != nullptr, "headless fixture scene loads" );
	if( !j ) return;

	CountingProbePainter* probe = new CountingProbePainter();
	IPainterManager* pm = j->GetPainters();
	Check( pm != nullptr, "job exposes a painter manager" );
	bool added = false;
	if( pm ) {
		added = pm->AddItem( probe, "probe_budget" );
		probe->release();   // AddItem took its own reference; drop the local one.
	}
	Check( added, "the counting probe painter registers under `probe_budget`" );

	const auto t0 = std::chrono::steady_clock::now();
	const PainterPreview::Result r = PainterPreview::RenderPainterPreview(
		*j, String( "probe_budget" ), 2048, 2048 );
	const auto t1 = std::chrono::steady_clock::now();
	const long ms = std::chrono::duration_cast<std::chrono::milliseconds>( t1 - t0 ).count();

	Check( r.status == PainterPreview::Status::Ok, "the 2048x2048 preview request succeeds" );
	if( r.status == PainterPreview::Status::Ok )
	{
		Check( r.width == 2048 && r.height == 2048, "reports the FULL requested dimensions (ABI contract unchanged)" );
		Check( r.rgba.size() == 2048u * 2048u * 4u, "the FULL w*h*4 buffer is filled (nearest-upscale from the grid)" );

		const long count = probe->evalCount.load( std::memory_order_relaxed );
		std::cout << "  (probe evaluated " << count << " times in " << ms << "ms for a "
		           << ( 2048u * 2048u ) << "-pixel request)" << std::endl;
		Check( count > 0, "the probe was actually invoked at least once" );
		Check( (unsigned long)count <= PainterPreview::kMaxSampleBudget,
		       "MONEY: distinct evaluations are bounded by kMaxSampleBudget (9216), not the requested 4.2M pixels" );
		// Generous, non-flaky upper bound: proves the budget actually
		// short-circuits the compute (a full 2048x2048 evaluation of even
		// a trivial painter, held behind a coarse-grained lock in the real
		// controller path, is the P2-1 bug this decimation exists to fix)
		// without pinning a specific millisecond figure that CI load could
		// blow through on an unrelated slow day.
		Check( ms < 5000, "the decimated request completes in a reasonable time, not the multi-second stall the budget prevents" );
	}

	if( pm ) pm->RemoveItem( "probe_budget" );
	j->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 6: concurrency -- refused (not blocked) while a render owns the
// scene, succeeds again once it completes.  Mirrors
// AgentRenderAsyncTest.cpp's RunRenderOwnsSceneGuardTest recipe.
//////////////////////////////////////////////////////////////////////

class CanaryJob : public Job
{
public:
	CanaryJob( unsigned int sleepMs, std::shared_ptr<std::atomic<int>> counter )
	: Job(), mSleepMs( sleepMs ), mCounter( std::move( counter ) ) {}
	bool Rasterize() override
	{
		mCounter->fetch_add( 1, std::memory_order_acq_rel );
		std::this_thread::sleep_for( std::chrono::milliseconds( mSleepMs ) );
		const bool ok = Job::Rasterize();
		mCounter->fetch_sub( 1, std::memory_order_acq_rel );
		return ok;
	}
private:
	unsigned int mSleepMs;
	std::shared_ptr<std::atomic<int>> mCounter;
};

// Runs `fn` on a background (detached) thread and waits up to `timeoutMs`
// for it to finish; returns false (FAIL) if the watchdog trips -- turns a
// wedge into a deterministic test failure instead of a suite hang.
static bool RunWatchdogged( const std::string& what, unsigned int timeoutMs, const std::function<void()>& fn )
{
	auto done = std::make_shared<std::atomic<bool>>( false );
	std::thread worker( [fn, done]() {
		fn();
		done->store( true, std::memory_order_release );
	} );
	worker.detach();

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( timeoutMs );
	while( std::chrono::steady_clock::now() < deadline ) {
		if( done->load( std::memory_order_acquire ) ) return true;
		std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
	}
	std::printf( "  FAIL: WATCHDOG TRIPPED -- \"%s\" did not complete within %ums\n", what.c_str(), timeoutMs );
	return false;
}

static void TestConcurrencyRefusal()
{
	std::cout << "Test 6: PainterPreview REFUSES (does not wedge) while a render owns the scene..." << std::endl;
	const char* tmp = "painterpreview_concurrency.RISEscene";
	{ std::ofstream o( tmp, std::ios::binary ); o << kLiveScene; }

	auto insideRasterize = std::make_shared<std::atomic<int>>( 0 );
	CanaryJob* pJob = new CanaryJob( /*sleepMs*/ 500, insideRasterize );
	Check( pJob->LoadAsciiSceneViaCst( tmp ), "live fixture scene loads via the CST path" );

	SceneEditController controller( *pJob, /*interactiveRasterizer*/0 );
	controller.Start( /*suppressInitialRender=*/true );
	Check( !controller.ForTest_RenderOwnsScene(), "render-owns-scene flag is FALSE before any render" );

	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "AgentSession wraps the CanaryJob" );
	if( session )
	{
		session->AttachController( &controller );
		const AgentSession::AgentRenderAsyncResult ar = session->RenderAsync( AgentRenderParams() );
		Check( ar.accepted && ar.renderJobId != 0, "async render accepted" );

		bool inside = false;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( 2000 );
		while( std::chrono::steady_clock::now() < deadline ) {
			if( insideRasterize->load( std::memory_order_acquire ) > 0 ) { inside = true; break; }
			std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
		}
		Check( inside, "worker entered Rasterize() (render now owns mMutex)" );
		Check( controller.ForTest_RenderOwnsScene(), "render-owns-scene flag is TRUE while the render runs" );

		std::vector<unsigned char> rgba;
		const bool previewQuick = RunWatchdogged(
			"GetPainterPreview() while a render owns the scene", 200, [&]{
				(void)controller.GetPainterPreview( String( "probe" ), -1, 8, 8, rgba );
			} );
		Check( previewQuick, "MONEY: GetPainterPreview() did NOT wedge on mMutex during a render" );
		Check( rgba.empty(), "GetPainterPreview() REFUSED (empty buffer) while the render owns the scene" );

		std::vector<unsigned char> rgba2;
		const bool stripQuick = RunWatchdogged(
			"GetRampStripPreview() while a render owns the scene", 200, [&]{
				(void)controller.GetRampStripPreview( String( "probe" ), 8, 1, rgba2 );
			} );
		Check( stripQuick, "MONEY: GetRampStripPreview() did NOT wedge on mMutex during a render" );

		std::vector<unsigned char> rgbaMatBusy;
		const bool matQuick = RunWatchdogged(
			"GetMaterialPreview() while a render owns the scene", 200, [&]{
				(void)controller.GetMaterialPreview( String( "mat_diffuse" ), 8, 8, rgbaMatBusy );
			} );
		Check( matQuick, "MONEY: GetMaterialPreview() did NOT wedge on mMutex during a render" );
		Check( rgbaMatBusy.empty(), "GetMaterialPreview() REFUSED (empty buffer) while the render owns the scene" );

		const bool completed = session->RenderWait( ar.renderJobId, 5000 );
		Check( completed, "render completed within the timeout" );
		Check( !controller.ForTest_RenderOwnsScene(), "render-owns-scene flag CLEARED after the render finished" );

		std::vector<unsigned char> rgba3;
		const bool afterOk = controller.GetPainterPreview( String( "probe" ), -1, 8, 8, rgba3 );
		Check( afterOk && rgba3.size() == 8u * 8u * 4u,
		       "GetPainterPreview() SUCCEEDS again once the render has finished" );

		std::vector<unsigned char> rgbaMatAfter;
		const bool matAfterOk = controller.GetMaterialPreview( String( "mat_diffuse" ), 8, 8, rgbaMatAfter );
		Check( matAfterOk && rgbaMatAfter.size() == 8u * 8u * 4u,
		       "GetMaterialPreview() SUCCEEDS again once the render has finished" );

		session->AttachController( nullptr );
	}
	controller.Stop();
	pJob->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 7: C-ABI smoke
//////////////////////////////////////////////////////////////////////
static void TestCAbiSmoke()
{
	std::cout << "Test 7: RISE_API_SceneEditController_PainterPreview fills a caller buffer..." << std::endl;
	const char* tmp = "painterpreview_cabi.RISEscene";
	Job* j = LoadScene( kHeadlessScene, tmp );
	Check( j != nullptr, "headless fixture scene loads" );
	if( !j ) { return; }

	SceneEditController controller( *j, /*interactiveRasterizer*/0 );
	controller.Start( /*suppressInitialRender=*/true );

	const unsigned int W = 4, H = 4;
	std::vector<unsigned char> buf( (std::size_t)W * H * 4, 0xEE );  // poison, so a no-op call is visible
	bool wasScalar = true; double rmin = -1, rmax = -1;
	const bool ok = RISE_API_SceneEditController_PainterPreview(
		&controller, "solid", -1, W, H, buf.data(), &wasScalar, &rmin, &rmax );
	Check( ok, "C-ABI PainterPreview call succeeds for a real painter" );
	Check( !wasScalar, "C-ABI out-param: colour pipe is not scalar" );
	if( ok )
	{
		const unsigned char eR = ExpectedChannel( 0.2 ), eG = ExpectedChannel( 0.4 ), eB = ExpectedChannel( 0.6 );
		Check( buf[0] == eR && buf[1] == eG && buf[2] == eB && buf[3] == 255,
		       "C-ABI buffer's first pixel matches the direct-engine encode" );
	}

	const bool refused = RISE_API_SceneEditController_PainterPreview(
		&controller, "does_not_exist", -1, W, H, buf.data(), nullptr, nullptr, nullptr );
	Check( !refused, "C-ABI call refuses an unknown painter name" );

	std::vector<unsigned char> matBuf( (std::size_t)W * H * 4, 0xEE );
	const bool matOk = RISE_API_SceneEditController_MaterialPreview(
		&controller, "mat_lambert", W, H, matBuf.data() );
	Check( matOk, "C-ABI MaterialPreview call succeeds for a real material" );
	if( matOk ) {
		Check( matBuf[3] == 255, "C-ABI MaterialPreview writes opaque alpha" );
	}
	const bool matRefused = RISE_API_SceneEditController_MaterialPreview(
		&controller, "does_not_exist_mat", W, H, matBuf.data() );
	Check( !matRefused, "C-ABI MaterialPreview call refuses an unknown material name" );

	controller.Stop();
	j->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 9 (Slice 5): MaterialPreview 3D shaderball engine -- Lambertian
// directional shading, GGX thin-film interference iridescence,
// Sellmeier dielectric refraction, luminaire emission, refusals, and
// sample-budget decimation.
//////////////////////////////////////////////////////////////////////
static void TestMaterialPreviewShaderball()
{
	std::cout << "Test 9: MaterialPreview renders a deterministic 3D shaderball across Lambertian, thin-film GGX, Sellmeier glass, and luminaire materials..." << std::endl;
	const char* tmp = "materialpreview_shaderball.RISEscene";
	Job* j = LoadScene( kHeadlessScene, tmp );
	Check( j != nullptr, "headless fixture scene loads for MaterialPreview" );
	if( !j ) return;

	const unsigned int W = 32, H = 32;

	// (a) Lambertian 3D shading + determinism
	const MaterialPreview::Result r1 = MaterialPreview::RenderMaterialPreview( *j, String( "mat_lambert" ), W, H );
	const MaterialPreview::Result r2 = MaterialPreview::RenderMaterialPreview( *j, String( "mat_lambert" ), W, H );
	Check( r1.status == MaterialPreview::Status::Ok, "lambertian_material shaderball preview succeeds" );
	Check( r1.width == W && r1.height == H && r1.rgba.size() == (std::size_t)W * H * 4,
	       "lambertian_material buffer matches requested W*H*4 dimensions" );
	Check( r1.rgba == r2.rgba, "MaterialPreview is deterministic across repeated calls" );

	if( r1.status == MaterialPreview::Status::Ok ) {
		// Compare key-lit upper-left sphere pixel (x=11, y=11) vs lower-right sphere pixel (x=21, y=21).
		const std::size_t idxKey  = ( (std::size_t)11 * W + 11 ) * 4;
		const std::size_t idxFill = ( (std::size_t)21 * W + 21 ) * 4;
		const int lumaKey  = (int)r1.rgba[idxKey]  + (int)r1.rgba[idxKey+1]  + (int)r1.rgba[idxKey+2];
		const int lumaFill = (int)r1.rgba[idxFill] + (int)r1.rgba[idxFill+1] + (int)r1.rgba[idxFill+2];
		Check( lumaKey > lumaFill, "3D sphere shading: key-lit upper-left hemisphere is brighter than shadow hemisphere" );
		// Solid has color (0.2, 0.4, 0.6), so B > G > R on the lit sphere.
		Check( r1.rgba[idxKey+2] > r1.rgba[idxKey+1] && r1.rgba[idxKey+1] > r1.rgba[idxKey+0],
		       "lambertian_material preserves the blue-dominant (0.2, 0.4, 0.6) albedo tint on the sphere" );
	}

	// (b) GGX thin-film interference: spectral quadrature produces chromatic
	// iridescence even though rs=white and extinction/ior are uniform, and
	// changing film_thickness from 380nm to 520nm shifts the interference colour.
	const MaterialPreview::Result tf380 = MaterialPreview::RenderMaterialPreview( *j, String( "mat_thinfilm_380" ), W, H );
	const MaterialPreview::Result tf520 = MaterialPreview::RenderMaterialPreview( *j, String( "mat_thinfilm_520" ), W, H );
	Check( tf380.status == MaterialPreview::Status::Ok && tf520.status == MaterialPreview::Status::Ok,
	       "GGX thin-film materials (380nm and 520nm) both preview cleanly" );
	if( tf380.status == MaterialPreview::Status::Ok && tf520.status == MaterialPreview::Status::Ok ) {
		Check( tf380.rgba != tf520.rgba,
		       "MONEY: spectral quadrature differentiates 380nm vs 520nm thin-film interference thickness" );
	}

	// (c) Sellmeier dielectric glass refraction
	const MaterialPreview::Result glass = MaterialPreview::RenderMaterialPreview( *j, String( "mat_glass" ), W, H );
	Check( glass.status == MaterialPreview::Status::Ok, "dielectric_material with Sellmeier IOR previews cleanly" );
	if( glass.status == MaterialPreview::Status::Ok && r1.status == MaterialPreview::Status::Ok ) {
		Check( glass.rgba != r1.rgba, "refractive glass shaderball is distinct from opaque Lambertian shaderball" );
	}

	// (d) Luminaire emission
	const MaterialPreview::Result emit = MaterialPreview::RenderMaterialPreview( *j, String( "lum" ), W, H );
	Check( emit.status == MaterialPreview::Status::Ok, "lambertian_luminaire_material previews cleanly" );
	if( emit.status == MaterialPreview::Status::Ok ) {
		const std::size_t centerIdx = ( (std::size_t)( H / 2 ) * W + ( W / 2 ) ) * 4;
		Check( emit.rgba[centerIdx] > 200 && emit.rgba[centerIdx+1] > 200 && emit.rgba[centerIdx+2] > 200,
		       "emissive luminaire material renders bright at the sphere center" );
	}

	// (e) Refusals & sample budget
	const MaterialPreview::Result badName = MaterialPreview::RenderMaterialPreview( *j, String( "no_such_mat" ), W, H );
	Check( badName.status == MaterialPreview::Status::Refused && !badName.refusalReason.empty(),
	       "unknown material returns Status::Refused with a diagnostic reason" );
	const MaterialPreview::Result nullMat = MaterialPreview::RenderMaterialPreview( *j, String( "none" ), W, H );
	Check( nullMat.status == MaterialPreview::Status::Refused && !nullMat.refusalReason.empty(),
	       "the 'none' null material returns Status::Refused" );
	const MaterialPreview::Result zeroDim = MaterialPreview::RenderMaterialPreview( *j, String( "mat_lambert" ), 0, H );
	Check( zeroDim.status == MaterialPreview::Status::Refused && !zeroDim.refusalReason.empty(),
	       "zero width returns Status::Refused" );
	const MaterialPreview::Result overDim = MaterialPreview::RenderMaterialPreview( *j, String( "mat_lambert" ), MaterialPreview::kMaxDim + 1, H );
	Check( overDim.status == MaterialPreview::Status::Refused && !overDim.refusalReason.empty(),
	       "over-cap width returns Status::Refused" );

	j->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 10 (Slice 6): Spectral Differentiators (D1 Blackbody & Sellmeier,
// D2 Measured Metal (n,k) presets, D3 Thin-Film Iridescence Angle-Sweep
// Strip, and atomic ApplySpectralPreset + Undo/Redo).
//////////////////////////////////////////////////////////////////////
static void TestSpectralDifferentiatorsSlice6()
{
	std::cout << "Test 10: Spectral Differentiators (D1 Blackbody/Sellmeier, D2 Measured Metals, D3 Thin-Film Angle Strip & Preset Undo/Redo)..." << std::endl;
	const char* tmp = "spectral_diff_slice6.RISEscene";
	Job* j = LoadScene( kHeadlessScene, tmp );
	Check( j != nullptr, "headless fixture scene loads for Slice 6" );
	if( !j ) return;

	SceneEditController controller( *j, /*interactiveRasterizer*/0 );
	controller.Start( /*suppressInitialRender=*/true );

	// (a) D3 Thin-Film Angle-Sweep Strip & single-angle evaluation
	std::vector<unsigned char> tfStrip380, tfStrip520, tfRefused;
	double d380 = 0.0, n380 = 0.0;
	Check( RISE_API_SceneEditController_ThinFilmAngleStrip(
		&controller, "mat_thinfilm_380", 64, 8,
		( tfStrip380.resize( 64 * 8 * 4 ), tfStrip380.data() ), &d380, &n380 ),
		"D3 ThinFilmAngleStrip succeeds for mat_thinfilm_380" );
	Check( std::fabs( d380 - 380.0 ) < 1e-4 && std::fabs( n380 - 1.38 ) < 1e-4,
	       "D3 ThinFilmAngleStrip reports live thickness=380nm and film_ior=1.38" );

	double d520 = 0.0, n520 = 0.0;
	Check( RISE_API_SceneEditController_ThinFilmAngleStrip(
		&controller, "mat_thinfilm_520", 64, 8,
		( tfStrip520.resize( 64 * 8 * 4 ), tfStrip520.data() ), &d520, &n520 ),
		"D3 ThinFilmAngleStrip succeeds for mat_thinfilm_520" );
	Check( tfStrip380 != tfStrip520,
	       "D3 ThinFilmAngleStrip differentiates 380nm vs 520nm film thickness" );

	tfRefused.resize( 16 * 4 * 4 );
	Check( !RISE_API_SceneEditController_ThinFilmAngleStrip(
		&controller, "mat_lambert", 16, 4, tfRefused.data(), nullptr, nullptr ),
		"D3 ThinFilmAngleStrip refuses a non-GGX material" );

	unsigned char c0[4] = {0}, c75[4] = {0};
	Check( RISE_API_SceneEditController_EvaluateThinFilmAtAngle( &controller, "mat_thinfilm_380", 0.0, c0, nullptr, nullptr ) &&
	       RISE_API_SceneEditController_EvaluateThinFilmAtAngle( &controller, "mat_thinfilm_380", 75.0, c75, nullptr, nullptr ),
	       "D3 EvaluateThinFilmAtAngle succeeds at 0 deg and 75 deg" );
	Check( c0[0] != c75[0] || c0[1] != c75[1] || c0[2] != c75[2],
	       "D3 EvaluateThinFilmAtAngle shifts interference colour between normal (0 deg) and grazing (75 deg) incidence" );

	// (b) D1 Blackbody Kelvin strip & Wien peak wavelength
	std::vector<unsigned char> bbStrip( 64 * 4 * 4, 0 );
	Check( RISE_API_SceneEditController_BlackbodyKelvinStrip( 64, 4, 1000.0, 12000.0, bbStrip.data() ),
	       "D1 BlackbodyKelvinStrip succeeds for 1000..12000 K" );
	double peak1800 = 0.0, peak5500 = 0.0, peak10000 = 0.0;
	unsigned char bb1800[4] = {0}, bb10000[4] = {0};
	Check( RISE_API_SceneEditController_EvaluateBlackbodyKelvin( 1800.0, &peak1800, bb1800 ) &&
	       RISE_API_SceneEditController_EvaluateBlackbodyKelvin( 5500.0, &peak5500, nullptr ) &&
	       RISE_API_SceneEditController_EvaluateBlackbodyKelvin( 10000.0, &peak10000, bb10000 ),
	       "D1 EvaluateBlackbodyKelvin succeeds across 1800K, 5500K, 10000K" );
	Check( std::fabs( peak5500 - 526.8676 ) < 0.1,
	       "D1 Wien peak wavelength at 5500K is ~526.9 nm" );
	Check( bb1800[0] > bb1800[2] && bb10000[2] > bb10000[0],
	       "D1 Blackbody 1800K is warm (R > B) and 10000K is cool (B > R)" );

	// (c) D1 Sellmeier optical glass presets & n_d / Abbe V_d metrics
	std::string coeffsNBK7, coeffsSF11;
	Check( MaterialPreview::LookupSellmeierGlassPreset( String( "N-BK7" ), coeffsNBK7 ) &&
	       MaterialPreview::LookupSellmeierGlassPreset( String( "SF11" ), coeffsSF11 ),
	       "D1 LookupSellmeierGlassPreset resolves N-BK7 and SF11" );
	double ndBK7 = 0.0, vdBK7 = 0.0, ndSF11 = 0.0, vdSF11 = 0.0;
	Check( RISE_API_SceneEditController_EvaluateSellmeierMetrics( coeffsNBK7.c_str(), &ndBK7, &vdBK7 ) &&
	       RISE_API_SceneEditController_EvaluateSellmeierMetrics( coeffsSF11.c_str(), &ndSF11, &vdSF11 ),
	       "D1 EvaluateSellmeierMetrics succeeds for N-BK7 and SF11" );
	Check( std::fabs( ndBK7 - 1.5168 ) < 0.002 && std::fabs( vdBK7 - 64.17 ) < 0.5,
	       "D1 N-BK7 Fraunhofer metrics match reference (n_d ~ 1.5168, V_d ~ 64.17)" );
	Check( std::fabs( ndSF11 - 1.7847 ) < 0.002 && std::fabs( vdSF11 - 25.76 ) < 0.5,
	       "D1 SF11 Fraunhofer metrics match high-dispersion flint reference (n_d ~ 1.7847, V_d ~ 25.76)" );

	// (d) D2 Measured metal presets & swatches
	unsigned char swAu[4] = {0}, swAg[4] = {0}, swCu[4] = {0}, swAl[4] = {0}, swCr[4] = {0}, swTi[4] = {0};
	Check( RISE_API_SceneEditController_MeasuredMetalPresetSwatch( "Au", swAu ) &&
	       RISE_API_SceneEditController_MeasuredMetalPresetSwatch( "Ag", swAg ) &&
	       RISE_API_SceneEditController_MeasuredMetalPresetSwatch( "Cu", swCu ) &&
	       RISE_API_SceneEditController_MeasuredMetalPresetSwatch( "Al", swAl ) &&
	       RISE_API_SceneEditController_MeasuredMetalPresetSwatch( "Cr", swCr ) &&
	       RISE_API_SceneEditController_MeasuredMetalPresetSwatch( "Ti", swTi ),
	       "D2 MeasuredMetalPresetSwatch resolves all 6 curated metals (Au, Ag, Cu, Al, Cr, Ti)" );
	Check( swAu[0] > swAu[2] && swCu[0] > swCu[1] && swAg[2] > swAu[2],
	       "D2 physics-integrated conductor Fresnel swatches reflect Gold/Copper warmth and Silver neutrality" );

	// (e) ApplySpectralPreset + Undo/Redo atomic operation-level batching
	controller.SetSelection( SceneEditController::Category::Material, String( "mat_thinfilm_380" ) );
	Check( RISE_API_SceneEditController_ApplySpectralPreset(
		&controller, static_cast<int>( SceneEditController::Category::Material ), "Cu" ),
		"D2 ApplySpectralPreset('Cu') succeeds on ggx_material" );

	Check( j->GetScalarPainters()->GetItem( "mat_thinfilm_380_cu_n" ) != nullptr &&
	       j->GetScalarPainters()->GetItem( "mat_thinfilm_380_cu_k" ) != nullptr,
	       "ApplySpectralPreset('Cu') splices and derives <mat>_cu_n and <mat>_cu_k scalar_painters" );

	Check( controller.Undo(), "Undo of ApplySpectralPreset('Cu') succeeds" );
	Check( j->GetScalarPainters()->GetItem( "mat_thinfilm_380_cu_n" ) == nullptr &&
	       j->GetScalarPainters()->GetItem( "mat_thinfilm_380_cu_k" ) == nullptr,
	       "Undo removes both inserted scalar_painter chunks and restores original ggx_material slots" );

	Check( controller.Redo(), "Redo of ApplySpectralPreset('Cu') succeeds" );
	Check( j->GetScalarPainters()->GetItem( "mat_thinfilm_380_cu_n" ) != nullptr &&
	       j->GetScalarPainters()->GetItem( "mat_thinfilm_380_cu_k" ) != nullptr,
	       "Redo re-inserts both scalar_painter chunks before the material and re-binds ior/extinction" );

	// Also test Dielectric Sellmeier glass preset + Undo
	controller.SetSelection( SceneEditController::Category::Material, String( "mat_glass" ) );
	Check( RISE_API_SceneEditController_ApplySpectralPreset(
		&controller, static_cast<int>( SceneEditController::Category::Material ), "SF11" ),
		"D1 ApplySpectralPreset('SF11') succeeds on dielectric_material" );
	Check( j->GetScalarPainters()->GetItem( "mat_glass_sf11_ior" ) != nullptr,
	       "ApplySpectralPreset('SF11') splices and derives mat_glass_sf11_ior scalar_painter before mat_glass" );
	Check( controller.Undo(), "Undo of ApplySpectralPreset('SF11') succeeds" );
	Check( j->GetScalarPainters()->GetItem( "mat_glass_sf11_ior" ) == nullptr,
	       "Undo removes mat_glass_sf11_ior and restores mat_glass.ior" );

	controller.Stop();
	j->release();
	std::remove( tmp );
}

int main()
{
	std::cout << "=== PainterPreviewTest (doc 88 S10) ===" << std::endl;

	TestUniformColorFlat();
	TestPerlinVariesDeterministic();
	TestExpressionDefStages();
	TestRampStrip();
	TestRefusals();
	TestFlatScalarDefStageIsUniformGray();
	TestVec3DefStageShowsTrueTriple();
	TestPerChannelScalarPreviewIsJointlyRangedRGB();
	TestHighFrequencyNoiseBodyIsNotFlat();
	TestEvaluationBudgetDecimatesLargeRequests();
	TestConcurrencyRefusal();
	TestCAbiSmoke();
	TestMaterialPreviewShaderball();
	TestSpectralDifferentiatorsSlice6();

	std::cout << "\n" << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount == 0 ? 0 : 1;
}
