//////////////////////////////////////////////////////////////////////
//
//  AgentAddWearTest.cpp - docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md
//    sec 11 / sec 13 Phase 4 (2026-08-30): add_wear, the VERB half of
//    design-note condition L and the C-VERB escalation the 2026-08-29
//    curv census earned.
//
//  WHAT THIS VERB HAS TO GET RIGHT, and therefore what is measured here.
//  Its contract is "band around the colour that is already there" --
//  vary_material's contract, on the colour pipe -- with one extra
//  obligation vary_material does not carry: the composition it writes
//  is only worth anything if the GEOMETRY SIGNAL actually drives it.  A
//  wear painter that parses, derives and renders a perfectly uniform
//  base colour is a silent no-op dressed as a success, and it is the
//  specific failure the census measured models talking themselves into
//  (a `P.z` threshold that "looks like" wear).  So the positive cases
//  assert four things a weaker test would miss --
//    (a) the minted colour painter responds to CURVATURE in the right
//        DIRECTION: evaluated through IPainter::GetColor at three
//        synthetic hits that differ ONLY in mean curvature (same world
//        point, so the fbm breakup contributes identically), a convex
//        hit must come back LIGHTER than a flat one and a concave hit
//        DARKER -- an execution-level proof of the sign convention, not
//        a render-pixel check that Monte-Carlo noise would satisfy on
//        its own;
//    (b) the minted roughness painter reads the SAME two masks, in the
//        same direction (polished on the rubbed edge, rougher in the
//        crevice), and every sample lands inside the band its `param`
//        metadata advertises;
//    (c) the numbers it emitted really are the ones that were already
//        there -- base_r/g/b equal the authored uniformcolor RGB,
//        rough_base equals the authored roughness, and the band is
//        vary_material's own VaryBandFor_ rule;
//    (d) the material chunk really points at both minted chunks, read
//        back out of the DOCUMENT, and the whole scene still derives
//        and renders.
//
//  Cases:
//    A  QUALIFY + SELECT + REWRITE.  Three flat-colour ggx materials on
//       sdf_geometry; the bare call takes the one bound to the MOST
//       objects (not the first in the document), mints an
//       expression_painter AND a scalar_painter, rebinds rd + alphax +
//       alphay, and the document still derives + renders.
//    A2 BAND-AROUND-EXISTING, numerically (the (c) claim above).
//    A3 THE SIGNAL DRIVES IT (the (a)/(b) claims above).
//    B  SELECTION: `material` overrides the auto-pick; the lexicographic
//       tie-break decides between two equally-used materials.
//    B2 A LAMBERTIAN (no microsurface at all) gets the colour half only
//       -- not a refusal, and no roughness chunk minted.
//    C  DETERMINISM: two runs from the SAME input document produce
//       BYTE-IDENTICAL output.  No clock, no PRNG state.
//    D  REFUSALS, each with the document BYTE-IDENTICAL afterwards:
//       nothing qualifies; a colour that already varies; a material
//       already reading curv; PLANAR-ONLY geometry; an unknown name; a
//       named chunk with no colour slot this can read.
//    E  UNDO through a live SceneEditController restores the pre-verb
//       document EXACTLY -- two painters and three slot rebinds are ONE
//       undo step.
//    F  AUTONOMY: refused under Read; refused with a Propose-specific
//       message under Propose; refused (document byte-identical) under
//       External authority.
//    G  THE NOTE fires exactly on the predicate (>= 3 candidates), NAMES
//       the verb and the material, is byte-identical across its two
//       carriers, must NOT fire at 2, must NOT fire on planar-only
//       geometry, and STOPS firing once the verb has taken the document
//       below the gate.
//    H  WIRE SURFACE: dispatches through AgentRpcDispatcher, is declared
//       in the shared chat-codec tool table, and -- the 1ed4e7c3
//       two-list-drift lesson -- is ADVERTISED and ROUTABLE on MCP.
//
//  Self-contained: no RISE_MEDIA_PATH, inline native-v7 scenes.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "../src/Library/Job.h"
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/Interfaces/IScene.h"
#include "../src/Library/Interfaces/IPainter.h"
#include "../src/Library/Interfaces/IPainterManager.h"
#include "../src/Library/Interfaces/IScalarPainter.h"
#include "../src/Library/Interfaces/IScalarPainterManager.h"
#include "../src/Library/Intersection/RayIntersection.h"
#include "../src/Library/Agent/AgentSession.h"
#include "../src/Library/Agent/AgentRpc.h"
#include "../src/Library/Agent/AgentMcpAdapter.h"
#include "../src/Library/Agent/AgentChatCodecs.h"
#include "../src/Library/Agent/AgentDiagnostic.h"
#include "../src/Library/Agent/Json.h"
#include "../src/Library/SceneEditor/SceneEditController.h"

using namespace RISE;
using namespace RISE::Implementation;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const std::string& w )
{
	if( c ) ++g_pass;
	else { ++g_fail; std::printf( "  FAIL: %s\n", w.c_str() ); }
}

static std::string TempPath( const char* name )
{
	const char* base = std::getenv( "TMPDIR" );
	std::string dir = base ? base : "/tmp";
	if( !dir.empty() && dir[dir.size()-1] != '/' ) dir += '/';
	return dir + name;
}

static Job* LoadScene( const std::string& text, const std::string& path )
{
	{ std::ofstream o( path.c_str(), std::ios::binary ); o << text; }
	Job* pJob = new Job();
	if( !pJob->LoadAsciiSceneViaCst( path.c_str() ) ) {
		pJob->release();
		std::remove( path.c_str() );
		return nullptr;
	}
	return pJob;
}

//----------------------------------------------------------------------
// A NO-OP-render SceneEditController, so case E can drive the REAL
// Undo() the GUI's Cmd-Z drives.  No Start() -- the test never needs a
// render thread, and not spawning one keeps the case deterministic.
//----------------------------------------------------------------------
class QuietController : public SceneEditController
{
public:
	explicit QuietController( IJobPriv& job )
	: SceneEditController( job, /*interactiveRasterizer*/0 ) {}
protected:
	void DoOneRenderPass() override {}
};

//----------------------------------------------------------------------
// THE OBSERVABLE, and the reason this file exists rather than a render
// diff: a hit record whose ONLY varying quantity is the mean curvature.
// Same world point, same UV, same everything -- so the fbm breakup term
// contributes the SAME amount to all three probes and any difference in
// the result is attributable to `curv` alone.
//
// `curvatureValid` + `curvature` is the SDF family's own path into
// ExpressionPainter::BuildContext (RayIntersectionGeometric.h's
// SurfaceDerivativesInfo doc: "BuildContext PREFERS this when
// curvatureValid"), and `scaleHint` 1.0 is the honest default, so `curv`
// reads back exactly the number set here.
//----------------------------------------------------------------------
static RayIntersectionGeometric ProbeHit( double meanCurvature )
{
	RayIntersectionGeometric ri( Ray(), nullRasterizerState );
	ri.bHit = true;
	ri.ptCoord = Point2( 0.31, 0.62 );
	ri.ptIntersection = Point3( 0.4, -0.2, 0.7 );
	ri.ptObjIntersec  = Point3( 0.4, -0.2, 0.7 );
	ri.vNormal = Vector3( 0, 1, 0 );
	ri.onb = OrthonormalBasis3D( Vector3( 1, 0, 0 ), Vector3( 0, 0, 1 ), Vector3( 0, 1, 0 ) );
	ri.derivatives.scaleHint      = 1.0;
	ri.derivatives.curvature      = meanCurvature;
	ri.derivatives.curvatureValid = true;
	return ri;
}

static double LumaOf( const RISEPel& p )
{
	return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
}

//! The value on the FIRST `param <which> <value> ...` line at or after
//! chunk `chunkName`'s `name` line.
static double ParamValueInChunk( const std::string& doc, const std::string& chunkName,
                                 const std::string& which, bool& ok )
{
	ok = false;
	const std::size_t namePos = doc.find( chunkName );
	if( namePos == std::string::npos ) return 0.0;
	std::size_t pos = namePos;
	while( true ) {
		pos = doc.find( "param", pos );
		if( pos == std::string::npos ) return 0.0;
		const std::size_t eol = doc.find( '\n', pos );
		const std::string line = doc.substr( pos, eol - pos );
		const std::size_t w = line.find( which );
		if( w != std::string::npos ) {
			const std::size_t vs = line.find_first_not_of( " \t", w + which.size() );
			if( vs != std::string::npos ) {
				ok = true;
				return std::strtod( line.c_str() + vs, nullptr );
			}
		}
		pos = ( eol == std::string::npos ) ? doc.size() : eol;
		if( pos >= doc.size() ) return 0.0;
	}
}

//----------------------------------------------------------------------
// Fixtures.  Each derives on its own -- shader, rasterizer, film,
// camera, painters, materials, geometry -- so a failure is never "the
// scene did not load".
//----------------------------------------------------------------------
static std::string Preamble()
{
	return
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
		"film\n{\n\twidth 24\n\theight 24\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 2 9\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 45.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_bronze\n\tcolor 0.42 0.28 0.14\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_spec\n\tcolor 0.6 0.6 0.6\n}\n\n"
		"directional_light\n{\n\tname key\n\tpower 3.0\n\tcolor 1 1 1\n\tdirection 0.4 0.5 0.9\n}\n\n";
}

//! An sdf_geometry blob -- the family `curv` is EXACT on, and the one
//! the design doc's own worked example uses.
static std::string SdfBlob( const std::string& name )
{
	return "sdf_geometry\n{\n\tname " + name + "\n"
	       "\tpart sphere union 0     0 0 0     0 0 0   1 1 1   0.75 0 0   0\n"
	       "\tpart sphere subtract 0.08   0.35 0.1 0.55   0 0 0   1 1 1   0.42 0 0   0\n"
	       "\tpart sphere union 0     -0.455 0.273 0.727   0 0 0   1 1 1   0.25 0 0   0\n}\n\n";
}

static std::string Ggx( const std::string& name, const std::string& rd, const std::string& alpha )
{
	return "ggx_material\n{\n\tname " + name + "\n\trd " + rd + "\n\trs pnt_spec\n"
	       "\talphax " + alpha + "\n\talphay " + alpha + "\n\tior 1.5\n\textinction 0.0\n}\n\n";
}

static std::string Obj( const std::string& name, const std::string& geo,
                        const std::string& mat, double x )
{
	char buf[64];
	std::snprintf( buf, sizeof( buf ), "%g", x );
	return "standard_object\n{\n\tname " + name + "\n\tgeometry " + geo + "\n\tmaterial " + mat +
	       "\n\tposition " + buf + " 0 0\n}\n\n";
}

//! Three qualifying flat-colour ggx materials on curv-bearing geometry.
//! `mat_b` is bound to THREE objects, `mat_a` to two and `mat_c` to one,
//! so the bare call has a unique most-prominent answer that is NOT the
//! first in document order -- a fixture whose document order and
//! prominence agreed could not tell the two selection rules apart.
static std::string SceneThreeMaterials()
{
	std::string s = Preamble();
	s += SdfBlob( "blob" );
	s += Ggx( "mat_a", "pnt_bronze", "0.3" );
	s += Ggx( "mat_b", "pnt_bronze", "0.25" );
	s += Ggx( "mat_c", "pnt_bronze", "0.4" );
	s += Obj( "o1", "blob", "mat_a", -3 );
	s += Obj( "o2", "blob", "mat_a", -2 );
	s += Obj( "o3", "blob", "mat_b", -1 );
	s += Obj( "o4", "blob", "mat_b", 0 );
	s += Obj( "o5", "blob", "mat_b", 1 );
	s += Obj( "o6", "blob", "mat_c", 2 );
	return s;
}

//----------------------------------------------------------------------

static void TestQualifySelectRewrite()
{
	std::printf( "A: qualify + select + rewrite -- the bare call takes the most-bound material\n" );
	const std::string tmp = TempPath( "addwear_a.RISEscene" );
	Job* pJob = LoadScene( SceneThreeMaterials(), tmp );
	Check( pJob != nullptr, "A: fixture derives" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const std::string docBefore = sess->ReadDocument();

	const Agent::AgentSession::AgentAddWearResult r = sess->AddWear();
	Check( r.ok && r.applied, std::string( "A: the no-argument call APPLIED -- " ) + r.message );
	Check( r.status == "applied", "A: status is \"applied\"" );
	Check( r.material == "mat_b",
	       "A MONEY: it took `mat_b` -- the material bound to the MOST objects (3), not `mat_a`, "
	       "which is FIRST in the document (2 objects)" );
	Check( r.materialKind == "ggx_material", "A: the result names the material's kind" );
	Check( r.qualifyingMaterials == 3, "A: all three flat-colour materials qualified" );
	Check( r.boundObjects == 3, "A: it reports how many objects bind the chosen material" );
	Check( r.geometryKind == "sdf_geometry",
	       "A MONEY: it names the geometry family that makes the signal READ -- the clause that "
	       "separates a real wear pass from one that renders the flat colour it started from" );
	Check( r.colorSlot == "rd",
	       "A: it rebound `rd`, the ALBEDO slot, not `rs` (the preference list decides, not "
	       "descriptor order alone)" );
	Check( r.roughnessSlots.size() == 2 &&
	       r.roughnessSlots[0] == "alphax" && r.roughnessSlots[1] == "alphay",
	       "A: BOTH ggx roughness slots were rebound (a ggx with only alphax varying would be "
	       "anisotropic by accident)" );
	Check( !r.colorPainter.empty() && !r.roughnessPainter.empty(),
	       "A: it names BOTH chunks it minted" );

	const std::string docAfter = sess->ReadDocument();
	Check( docBefore != docAfter, "A: the document really changed" );
	Check( docAfter.find( "expression_painter" ) != std::string::npos,
	       "A: an expression_painter chunk landed (the colour half)" );
	Check( docAfter.find( "scalar_painter" ) != std::string::npos,
	       "A: ...and a scalar_painter chunk (the microsurface half)" );
	Check( docAfter.find( "rd " + r.colorPainter ) != std::string::npos,
	       "A MONEY: the DOCUMENT's rd names the minted colour chunk (the rebind is real, read back "
	       "out of the text rather than assumed from the result struct)" );
	Check( docAfter.find( "alphax " + r.roughnessPainter ) != std::string::npos &&
	       docAfter.find( "alphay " + r.roughnessPainter ) != std::string::npos,
	       "A: ...and both microsurface slots name the minted roughness chunk" );
	Check( docAfter.find( "curv*edge_wear" ) != std::string::npos &&
	       docAfter.find( "-curv*crevice_grime" ) != std::string::npos,
	       "A MONEY: the emitted body reads `curv` in BOTH signs -- positive for the edge mask, "
	       "negative for the crevice mask, which IS the composition the census found models will "
	       "not author unprompted" );
	Check( docAfter.find( "occlusion(0.08)" ) != std::string::npos,
	       "A MONEY: the occlusion radius is a NUMERIC LITERAL -- a `param`-bound radius compiles to "
	       "the DynR twin, which reads the neutral fallback on every triangle mesh and would "
	       "silently delete the cavity term (ExpressionEval.h's literalRadiusArg)" );
	// THE HUMAN-EDITABILITY CONTRACT: this verb's own output must obey the
	// rule the skills teach, or the worked example a model copies is wrong.
	Check( docAfter.find( "min " ) != std::string::npos && docAfter.find( "max " ) != std::string::npos &&
	       docAfter.find( "step " ) != std::string::npos && docAfter.find( "label " ) != std::string::npos,
	       "A MONEY: every emitted `param` carries min/max/step/label metadata -- the verb's own "
	       "output follows the human-editability contract it exists to teach" );
	Check( docAfter.find( "\tseed" ) != std::string::npos, "A: ...and a `seed` line for per-instance jitter" );
	// The two untouched materials are untouched.
	Check( docAfter.find( "rd pnt_bronze" ) != std::string::npos,
	       "A: the other two materials keep their own flat colour -- ONE material was worn, not all "
	       "three" );

	Check( pJob->GetScene() != nullptr, "A: the rewritten document still derives" );

	// It renders, non-black, with no diagnostics.
	{
		Agent::AgentRenderParams rp;
		rp.width = 32; rp.height = 32; rp.samples = 4;
		const Agent::AgentRenderResult rr = sess->Render( rp );
		Check( rr.ok, "A: the rewritten scene renders" );
		Check( rr.meanR + rr.meanG + rr.meanB > 0.0, "A: ...and is non-black" );
	}
	{
		const std::vector<Agent::AgentDiagnostic> diags = Agent::AgentSession::ValidateText( docAfter );
		bool anyError = false;
		for( const Agent::AgentDiagnostic& d : diags )
			if( d.severity == Agent::AgentDiagnostic::Severity::Error ) anyError = true;
		Check( !anyError,
		       "A MONEY: the rewritten document validates with ZERO error diagnostics -- the "
		       "generated expression bodies really compile in the VM, not merely parse as text" );
	}

	// ---- A2: BAND AROUND WHAT IS ALREADY THERE, numerically -------------
	{
		bool okR = false, okG = false, okB = false;
		const double br = ParamValueInChunk( docAfter, r.colorPainter, "base_r", okR );
		const double bg = ParamValueInChunk( docAfter, r.colorPainter, "base_g", okG );
		const double bb = ParamValueInChunk( docAfter, r.colorPainter, "base_b", okB );
		Check( okR && okG && okB, "A2: the three base-colour params are readable out of the chunk" );
		Check( std::fabs( br - 0.42 ) < 1e-9 && std::fabs( bg - 0.28 ) < 1e-9 &&
		       std::fabs( bb - 0.14 ) < 1e-9,
		       "A2 MONEY: base_r/g/b are EXACTLY the authored `pnt_bronze` 0.42 0.28 0.14 -- the "
		       "composition is banded around the colour that was already there, never a colour this "
		       "verb invented" );
		Check( std::fabs( r.baseR - 0.42 ) < 1e-9 && std::fabs( r.baseG - 0.28 ) < 1e-9 &&
		       std::fabs( r.baseB - 0.14 ) < 1e-9,
		       "A2: ...and the result struct reports the same three numbers" );

		bool okBase = false, okLo = false, okHi = false;
		const double rBase = ParamValueInChunk( docAfter, r.roughnessPainter, "rough_base", okBase );
		const double rLo   = ParamValueInChunk( docAfter, r.roughnessPainter, "rough_polished", okLo );
		const double rHi   = ParamValueInChunk( docAfter, r.roughnessPainter, "rough_crusted", okHi );
		Check( okBase && okLo && okHi, "A2: the three roughness params are readable out of the chunk" );
		Check( std::fabs( rBase - 0.25 ) < 1e-9,
		       "A2 MONEY: rough_base is EXACTLY the authored 0.25" );
		Check( std::fabs( r.previousRoughness - 0.25 ) < 1e-12,
		       "A2: ...and the result struct reports the constant it banded around" );
		// vary_material's OWN band rule (VaryBandFor_): 0.7x .. 1.4x below 1.
		// Pinned numerically, not merely as an inequality, so a private
		// second band rule growing here is caught.
		Check( std::fabs( rLo - 0.25 * 0.7 ) < 1e-9 && std::fabs( rHi - 0.25 * 1.4 ) < 1e-9,
		       "A2 MONEY: the roughness band is vary_material's own VaryBandFor_ rule (0.7x .. 1.4x "
		       "of 0.25), SHARED rather than re-derived -- two verbs cannot grow two ideas of a safe "
		       "roughness band" );
	}

	// ---- A3: THE SIGNAL DRIVES IT ---------------------------------------
	{
		IPainterManager* pm = pJob->GetPainters();
		IPainter* colourField = pm ? pm->GetItem( r.colorPainter.c_str() ) : nullptr;
		Check( colourField != nullptr, "A3: the minted colour painter resolved in the live manager" );
		if( colourField ) {
			const double convex = LumaOf( colourField->GetColor( ProbeHit(  1.5 ) ) );
			const double flat   = LumaOf( colourField->GetColor( ProbeHit(  0.0 ) ) );
			const double concav = LumaOf( colourField->GetColor( ProbeHit( -1.5 ) ) );
			std::printf( "    A3: colour luma  convex %.4f  flat %.4f  concave %.4f\n",
			             convex, flat, concav );
			Check( convex > flat + 1e-4,
			       "A3 MONEY: a CONVEX hit comes back LIGHTER than a flat one at the SAME world "
			       "point -- the edge mask is genuinely driven by positive `curv`, with the fbm "
			       "breakup held constant across the three probes so nothing else can explain it" );
			Check( concav < flat - 1e-4,
			       "A3 MONEY: a CONCAVE hit comes back DARKER than flat -- the crevice mask is "
			       "driven by negative `curv`. Together these two pin the SIGN CONVENTION the "
			       "descriptor states, which is the one thing a mask built from `P.z` can never "
			       "reproduce" );
		}

		IScalarPainterManager* sm = pJob->GetScalarPainters();
		IScalarPainter* roughField = sm ? sm->GetItem( r.roughnessPainter.c_str() ) : nullptr;
		Check( roughField != nullptr, "A3: the minted roughness painter resolved in the live manager" );
		if( roughField ) {
			const double convex = roughField->GetValuesAt( ProbeHit(  1.5 ) ).v[0];
			const double flat   = roughField->GetValuesAt( ProbeHit(  0.0 ) ).v[0];
			const double concav = roughField->GetValuesAt( ProbeHit( -1.5 ) ).v[0];
			std::printf( "    A3: roughness    convex %.4f  flat %.4f  concave %.4f\n",
			             convex, flat, concav );
			Check( convex < flat - 1e-4,
			       "A3 MONEY: a CONVEX hit is SMOOTHER -- a rubbed edge is polished, which is what "
			       "the composition claims" );
			Check( concav > flat + 1e-4,
			       "A3 MONEY: a CONCAVE hit is ROUGHER -- crusted crevice. The colour and roughness "
			       "chunks read the SAME two masks, so they cannot disagree about where the wear is" );
			const double lo = 0.25 * 0.7, hi = 0.25 * 1.4;
			Check( convex >= lo - 1e-9 && concav <= hi + 1e-9 &&
			       flat >= lo - 1e-9 && flat <= hi + 1e-9,
			       "A3: every probe lands INSIDE the band the emitted params advertise -- the clamps "
			       "on the two masks are what make that metadata true rather than decorative" );
		}
	}

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

static void TestSelection()
{
	std::printf( "B: selection -- `material` overrides, and ties break lexicographically\n" );

	// (1) A NAMED material overrides the most-prominent pick.
	{
		const std::string tmp = TempPath( "addwear_b1.RISEscene" );
		Job* pJob = LoadScene( SceneThreeMaterials(), tmp );
		Check( pJob != nullptr, "B1: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const Agent::AgentSession::AgentAddWearResult r = sess->AddWear( "mat_c" );
			Check( r.ok && r.applied && r.material == "mat_c",
			       std::string( "B1 MONEY: `material` takes the named one, not the most prominent -- " ) +
			       r.message );
			sess.reset();
			pJob->release();
			std::remove( tmp.c_str() );
		}
	}

	// (2) TWO equally-used materials: the LEXICOGRAPHIC tie-break decides,
	//     not document position -- so reordering two equally-bound chunks
	//     cannot silently change which one a bare call takes.
	{
		std::string body = Preamble();
		body += SdfBlob( "blob" );
		body += Ggx( "zeta", "pnt_bronze", "0.3" );
		body += Ggx( "alpha", "pnt_bronze", "0.3" );
		body += Obj( "o1", "blob", "zeta", -1 );
		body += Obj( "o2", "blob", "alpha", 1 );
		const std::string tmp = TempPath( "addwear_b2.RISEscene" );
		Job* pJob = LoadScene( body, tmp );
		Check( pJob != nullptr, "B2: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const Agent::AgentSession::AgentAddWearResult r = sess->AddWear();
			Check( r.ok && r.applied && r.material == "alpha",
			       std::string( "B2 MONEY: the tie between two 1-object materials breaks "
			                    "LEXICOGRAPHICALLY (`alpha`), not by document order (`zeta` is "
			                    "first) -- " ) + r.message );
			sess.reset();
			pJob->release();
			std::remove( tmp.c_str() );
		}
	}
}

static void TestLambertianColourOnly()
{
	std::printf( "B3: a material with NO microsurface gets the colour half only -- not a refusal\n" );
	std::string body = Preamble();
	body += SdfBlob( "blob" );
	body += "lambertian_material\n{\n\tname mat_flat\n\treflectance pnt_bronze\n}\n\n";
	body += Obj( "o1", "blob", "mat_flat", 0 );
	const std::string tmp = TempPath( "addwear_b3.RISEscene" );
	Job* pJob = LoadScene( body, tmp );
	Check( pJob != nullptr, "B3: fixture derives" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const Agent::AgentSession::AgentAddWearResult r = sess->AddWear();
	Check( r.ok && r.applied, std::string( "B3: applied -- " ) + r.message );
	Check( r.material == "mat_flat" && r.colorSlot == "reflectance",
	       "B3: it rebound lambertian's `reflectance`" );
	Check( r.roughnessSlots.empty() && r.roughnessPainter.empty(),
	       "B3 MONEY: NO roughness chunk was minted -- the microsurface half is opportunistic, never "
	       "part of qualification, so a lambertian is a perfectly good colour-wear target rather "
	       "than a refusal" );
	const std::string doc = sess->ReadDocument();
	Check( doc.find( "scalar_painter" ) == std::string::npos,
	       "B3: ...and no scalar_painter chunk is in the document at all" );
	Check( doc.find( "reflectance " + r.colorPainter ) != std::string::npos,
	       "B3: the rebind is real, read back out of the document" );
	Check( pJob->GetScene() != nullptr, "B3: the rewritten document still derives" );

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

static void TestDeterminism()
{
	std::printf( "C: determinism -- two runs from the same input produce byte-identical output\n" );
	const std::string tmpA = TempPath( "addwear_c1.RISEscene" );
	const std::string tmpB = TempPath( "addwear_c2.RISEscene" );
	std::string first, second;

	for( int pass = 0; pass < 2; ++pass ) {
		const std::string& tmp = pass ? tmpB : tmpA;
		Job* pJob = LoadScene( SceneThreeMaterials(), tmp );
		Check( pJob != nullptr, "C: fixture derives" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
		const Agent::AgentSession::AgentAddWearResult r = sess->AddWear();
		Check( r.ok && r.applied, "C: applied" );
		( pass ? second : first ) = sess->ReadDocument();
		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}

	Check( !first.empty() && first == second,
	       "C MONEY: the two documents are BYTE-IDENTICAL -- every field scale and the `seed` are "
	       "hashed from the MATERIAL NAME (FNV-1a), so there is no clock and no PRNG state in this "
	       "verb's output" );
}

static void ExpectRefusal( const char* label, const std::string& body,
                           const std::string& expectSubstr,
                           const std::string& material = std::string() )
{
	const std::string tmp = TempPath( ( std::string( "addwear_ref_" ) + label + ".RISEscene" ).c_str() );
	Job* pJob = LoadScene( body, tmp );
	Check( pJob != nullptr, std::string( "D(" ) + label + "): fixture derives" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const std::string before = sess->ReadDocument();
	const Agent::AgentSession::AgentAddWearResult r = sess->AddWear( material );
	Check( !r.ok && !r.applied && r.status.empty(),
	       std::string( "D(" ) + label + "): REFUSED pre-commit (ok=false, empty status)" );
	Check( r.message.find( expectSubstr ) != std::string::npos,
	       std::string( "D(" ) + label + "): the message names THIS rule (\"" + expectSubstr +
	       "\") -- got: " + r.message );
	Check( sess->ReadDocument() == before,
	       std::string( "D(" ) + label + "): the document is BYTE-IDENTICAL afterwards" );
	Check( r.colorPainter.empty() && r.roughnessPainter.empty() && r.colorSlot.empty() &&
	       r.roughnessSlots.empty(),
	       std::string( "D(" ) + label + "): it reports no chunk and no rebind (a caller that "
	       "believed them would go looking for something that does not exist)" );

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

static void TestRefusals()
{
	std::printf( "D: refusals -- each specific, each non-mutating\n" );

	// (1) NOTHING QUALIFIES: the only material's colour already varies.
	{
		std::string body = Preamble();
		body += SdfBlob( "blob" );
		body += "perlin3d_painter\n{\n\tname pnt_noise\n\tcolora pnt_bronze\n\tcolorb pnt_spec\n}\n\n";
		body += Ggx( "mat_v", "pnt_noise", "0.3" );
		body += Obj( "o1", "blob", "mat_v", 0 );
		ExpectRefusal( "nothing-qualifies", body, "no material in this document is a flat, readable colour" );
	}

	// (2) A NAMED material whose colour already varies -- the SPECIFIC rule,
	//     not the generic nothing-qualifies text, because the two need
	//     different corrections.
	{
		std::string body = Preamble();
		body += SdfBlob( "blob" );
		body += "perlin3d_painter\n{\n\tname pnt_noise\n\tcolora pnt_bronze\n\tcolorb pnt_spec\n}\n\n";
		body += Ggx( "mat_v", "pnt_noise", "0.3" );
		body += Ggx( "mat_ok", "pnt_bronze", "0.3" );
		body += Obj( "o1", "blob", "mat_v", -1 );
		body += Obj( "o2", "blob", "mat_ok", 1 );
		ExpectRefusal( "already-varying", body, "its colour already varies", "mat_v" );
	}

	// (3) ALREADY WORN: the material's roughness slot already reads `curv`,
	//     so a second pass would stack two wear layers.
	{
		std::string body = Preamble();
		body += SdfBlob( "blob" );
		body += "scalar_painter\n{\n\tname sp_worn\n\tparam k 3.0\n"
		        "\texpression clamp(-curv*k + 0.5, 0.05, 0.9)\n}\n\n";
		body += Ggx( "mat_worn", "pnt_bronze", "sp_worn" );
		body += Obj( "o1", "blob", "mat_worn", 0 );
		ExpectRefusal( "already-worn", body, "already binds an expression that reads `curv`", "mat_worn" );
		// ...and it is not silently taken by a BARE call either.
		ExpectRefusal( "already-worn-bare", body,
		               "no material in this document is a flat, readable colour" );
	}

	// (4) PLANAR-ONLY GEOMETRY.  Every object bound to the material sits on
	//     a box, where `curv` is 0 everywhere a ray can land -- the whole
	//     composition would render the flat colour it started from, which is
	//     the silent no-op this refusal exists to prevent.
	{
		std::string body = Preamble();
		body += "box_geometry\n{\n\tname slab\n\twidth 1\n\theight 1\n\tdepth 1\n}\n\n";
		body += Ggx( "mat_box", "pnt_bronze", "0.3" );
		body += Obj( "o1", "slab", "mat_box", 0 );
		ExpectRefusal( "planar-only", body, "planar or patch geometry", "mat_box" );
		ExpectRefusal( "planar-only-bare", body,
		               "no material in this document is a flat, readable colour" );
	}

	// (5) A NAMED material that does not exist at all.
	{
		std::string body = SceneThreeMaterials();
		ExpectRefusal( "unknown-name", body, "no chunk named", "not_a_thing" );
	}

	// (6) A NAMED chunk that EXISTS but carries no colour slot this can read
	//     -- a geometry chunk.  Distinct message from (5).
	{
		std::string body = SceneThreeMaterials();
		ExpectRefusal( "wrong-kind", body, "carries no colour slot this can read", "blob" );
	}
}

static void TestUndo()
{
	std::printf( "E: undo -- ONE step restores the pre-verb document EXACTLY\n" );
	const std::string tmp = TempPath( "addwear_undo.RISEscene" );
	Job* pJob = LoadScene( SceneThreeMaterials(), tmp );
	Check( pJob != nullptr, "E: fixture derives" );
	if( !pJob ) return;

	{
		QuietController c( *pJob );
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
		sess->AttachController( &c );

		const std::string before = sess->ReadDocument();
		const Agent::AgentSession::AgentAddWearResult r = sess->AddWear();
		Check( r.ok && r.applied, std::string( "E: applied through the controller -- " ) + r.message );
		const std::string after = sess->ReadDocument();
		Check( after != before, "E: the controller-attached commit really changed the document" );

		c.Undo();
		Check( sess->ReadDocument() == before,
		       "E MONEY: ONE Undo() restores the pre-verb document BYTE-EXACTLY -- TWO painter "
		       "inserts and THREE slot rebinds are one undoable unit, so undo can never strand a "
		       "material pointing at a chunk that is gone" );
		c.Redo();
		Check( sess->ReadDocument() == after, "E: Redo() reinstalls the post-verb document" );

		sess->AttachController( nullptr );
		sess.reset();
	}
	pJob->release();
	std::remove( tmp.c_str() );
}

static void TestAutonomyAndAuthority()
{
	std::printf( "F: autonomy + authority -- Read refuses, Propose refuses with its own message, "
	             "External has no staged form\n" );

	const std::string tmp = TempPath( "addwear_autonomy.RISEscene" );

	// (1) READ autonomy: the dispatcher's deny-by-default gate refuses the
	//     verb BEFORE it reaches AgentSession.
	{
		Job* pJob = LoadScene( SceneThreeMaterials(), tmp );
		Check( pJob != nullptr, "F1: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const std::string before = sess->ReadDocument();
			Agent::AgentRpcDispatcher disp( std::move( sess ), Agent::AgentAutonomy::Read );
			const std::string resp = disp.HandleLine(
				"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"add_wear\",\"params\":{}}" );
			Agent::JsonValue env; std::string perr;
			Check( Agent::JsonParse( resp, env, perr ) && env.isObject(), "F1: response parses" );
			Check( env.has( "error" ),
			       "F1 MONEY: add_wear is REFUSED under Read autonomy -- it is not on the read-safe "
			       "allowlist, so the deny-by-default gate catches it without any per-verb code" );
			const std::string readResp = disp.HandleLine(
				"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"read_document\",\"params\":{}}" );
			Agent::JsonValue readEnv; std::string rerr;
			Check( Agent::JsonParse( readResp, readEnv, rerr ) &&
			       readEnv.get( "result" ).get( "document" ).asString() == before,
			       "F1: the document is BYTE-IDENTICAL after the Read-autonomy refusal" );
		}
	}

	// (2) PROPOSE autonomy: refused too, with the Propose-SPECIFIC message.
	{
		Job* pJob = LoadScene( SceneThreeMaterials(), tmp );
		Check( pJob != nullptr, "F2: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			Agent::AgentRpcDispatcher disp( std::move( sess ), Agent::AgentAutonomy::Propose );
			const std::string resp = disp.HandleLine(
				"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"add_wear\",\"params\":{}}" );
			Agent::JsonValue env; std::string perr;
			Check( Agent::JsonParse( resp, env, perr ) && env.isObject(), "F2: response parses" );
			Check( env.has( "error" ), "F2: refused under Propose autonomy as well" );
			const std::string msg = env.get( "error" ).get( "message" ).asString();
			Check( msg.find( "add_wear" ) != std::string::npos, "F2: the refusal NAMES the verb" );
			Check( msg.find( "--agent-autonomy=propose" ) != std::string::npos &&
			       msg.find( "--agent-autonomy=commit" ) != std::string::npos,
			       "F2 MONEY: the Propose refusal is the verb's OWN message -- truthful about the "
			       "current posture and about commit being the real escape hatch, not the generic "
			       "Read-flavoured fallback" );
		}
	}

	// (3) EXTERNAL authority: refused, document byte-identical, and the
	//     message names the staged steps that DO exist.
	{
		Job* pJob = LoadScene( SceneThreeMaterials(), tmp );
		Check( pJob != nullptr, "F3: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess =
				Agent::AgentSession::WrapJob( pJob, Agent::AgentAuthority::External );
			const std::string before = sess->ReadDocument();
			const Agent::AgentSession::AgentAddWearResult r = sess->AddWear();
			Check( !r.ok && !r.applied, "F3: an External-authority session cannot commit this verb" );
			Check( r.message.find( "no staged-proposal form" ) != std::string::npos,
			       "F3 MONEY: the refusal states the SAME reason vary_material gives -- ONE composite "
			       "document swap is no AgentProposalKind an Owner could approve card-by-card -- "
			       "rather than inventing a different staging shape" );
			Check( r.message.find( "insert_chunk" ) != std::string::npos &&
			       r.message.find( "propose_patch" ) != std::string::npos,
			       "F3: ...and names the staged steps that DO exist, so the refusal is actionable" );
			Check( sess->ReadDocument() == before, "F3: the document is byte-identical" );
			// F3 reaches this refusal AFTER the candidate is fully composed, so
			// it is the case that proves the result fields are published on the
			// COMMIT, not on the plan.
			Check( r.colorPainter.empty() && r.roughnessPainter.empty() && r.colorSlot.empty(),
			       "F3 MONEY: a refusal that happens AFTER the candidate was composed still reports "
			       "no painter and no rebind -- those fields describe the document, never a "
			       "discarded plan" );
			Check( r.material == "mat_b" && r.qualifyingMaterials == 3,
			       "F3: ...while `material`/`qualifying` stay set, because those describe the "
			       "document as it stands and are true either way" );
			sess.reset();
			pJob->release();
		}
	}
	std::remove( tmp.c_str() );
}

static void TestNote()
{
	std::printf( "G: the design note -- fires exactly on the predicate, and NAMES the verb\n" );

	auto hasCode = []( const std::vector<Agent::AgentDiagnostic>& d, const char* code )
		-> const Agent::AgentDiagnostic* {
		for( const Agent::AgentDiagnostic& e : d ) if( e.code == code ) return &e;
		return nullptr;
	};
	const char* const kCode = "DESIGN_UNWORN_MATERIALS";

	// (1) THREE candidates -> FIRES, and names the verb + the material.
	{
		const std::string doc = SceneThreeMaterials();
		const std::vector<Agent::AgentDiagnostic> diags = Agent::AgentSession::ValidateText( doc );
		const Agent::AgentDiagnostic* d = hasCode( diags, kCode );
		Check( d != nullptr, "G1: three flat-colour materials on curved geometry FIRES" );
		if( d ) {
			Check( d->severity == Agent::AgentDiagnostic::Severity::Info,
			       "G1: it is an Info-severity ADVISORY, not a correctness problem" );
			Check( d->message.find( "add_wear" ) != std::string::npos,
			       "G1 MONEY: the clause NAMES THE VERB -- the 2026-08-29 census measured descriptor "
			       "text plus a worked example at 1/6 adoption, so the note's whole job is to state "
			       "a call" );
			Check( d->message.find( "NO ARGUMENTS" ) != std::string::npos,
			       "G1: ...and states the zero-argument form, the lowest-friction one" );
			Check( d->message.find( "mat_b" ) != std::string::npos,
			       "G1 MONEY: it names the SAME material a bare call takes -- note and verb read one "
			       "shared predicate, so the note cannot advertise a call that edits something else" );
			Check( d->message.find( "curv" ) != std::string::npos,
			       "G1: ...and names the signal, so a model that wants to hand-author it can" );
			Check( d->message.find( "REFUSES" ) != std::string::npos,
			       "G1: ...and is honest about the refusal, so trying it is knowably free" );
			Check( d->message.find( "ignore and do not churn" ) != std::string::npos,
			       "G1: ...and carries its own anti-churn escape" );

			const std::string note = Agent::AgentSession::ComputeDesignNote( doc );
			Check( note.find( d->message ) != std::string::npos,
			       "G1 MONEY: the diagnostic message appears BYTE-IDENTICALLY inside the "
			       "render-result note -- one shared formatter, two carriers, so they cannot drift" );
		}
	}

	// (2) TWO candidates -> SILENT.  The gate is three.
	{
		std::string body = Preamble();
		body += SdfBlob( "blob" );
		body += Ggx( "m1", "pnt_bronze", "0.3" );
		body += Ggx( "m2", "pnt_bronze", "0.2" );
		body += Obj( "o1", "blob", "m1", -1 );
		body += Obj( "o2", "blob", "m2", 1 );
		Check( hasCode( Agent::AgentSession::ValidateText( body ), kCode ) == nullptr,
		       "G2 MONEY: TWO candidates does NOT fire -- the gate is three, and a note that fired "
		       "on every small scene would be noise" );
	}

	// (3) PLANAR-ONLY geometry -> SILENT even at three materials.  The note
	//     must not ask for a mask that would render nothing.
	{
		std::string body = Preamble();
		body += "box_geometry\n{\n\tname slab\n\twidth 1\n\theight 1\n\tdepth 1\n}\n\n";
		body += Ggx( "m1", "pnt_bronze", "0.3" );
		body += Ggx( "m2", "pnt_bronze", "0.2" );
		body += Ggx( "m3", "pnt_bronze", "0.4" );
		body += Obj( "o1", "slab", "m1", -1 );
		body += Obj( "o2", "slab", "m2", 0 );
		body += Obj( "o3", "slab", "m3", 1 );
		Check( hasCode( Agent::AgentSession::ValidateText( body ), kCode ) == nullptr,
		       "G3 MONEY: three flat materials on BOX geometry does NOT fire -- `curv` is 0 "
		       "everywhere a ray can land there, so advertising the call would be advertising a "
		       "no-op. The note and the verb share the geometry clause, not just the colour one" );
	}

	// (4) SELF-DISARM: after the verb runs, the document drops below the
	//     gate and the note goes quiet -- the fired-forever failure
	//     materials-realism item 1 found in the OLD binary disarms.
	{
		const std::string tmp = TempPath( "addwear_g4.RISEscene" );
		Job* pJob = LoadScene( SceneThreeMaterials(), tmp );
		Check( pJob != nullptr, "G4: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			Check( hasCode( Agent::AgentSession::ValidateText( sess->ReadDocument() ), kCode ) != nullptr,
			       "G4: the note fires before the call" );
			const Agent::AgentSession::AgentAddWearResult r = sess->AddWear();
			Check( r.ok && r.applied, "G4: the verb applied" );
			Check( hasCode( Agent::AgentSession::ValidateText( sess->ReadDocument() ), kCode ) == nullptr,
			       "G4 MONEY: the note STOPS firing on the document the verb just produced -- the "
			       "worn material leaves the candidate set (clause (d): it now reads `curv`), which "
			       "drops the count below the gate. The predicate self-disarms as work lands rather "
			       "than nagging forever" );
			sess.reset();
			pJob->release();
			std::remove( tmp.c_str() );
		}
	}
}

static void TestWireSurface()
{
	std::printf( "H: wire surface -- RPC dispatch, chat-codec table, MCP advertised AND routable\n" );

	{
		const std::string defs = Agent::ChatToolDefsFingerprint();
		Check( defs.find( "add_wear" ) != std::string::npos,
		       "H: the verb is declared in the shared kToolDefs table (so every provider codec "
		       "carries it -- one table, four formatters)" );
	}

	const std::string tmp = TempPath( "addwear_rpc.RISEscene" );
	Job* pJob = LoadScene( SceneThreeMaterials(), tmp );
	Check( pJob != nullptr, "H: fixture derives" );
	if( !pJob ) return;

	// MCP: ADVERTISED and ROUTABLE.  The 1ed4e7c3 lesson is that the adapter
	// keeps two independent lists (BuildToolsList and IsKnownToolName) and a
	// verb can land in one but not the other -- advertised, and answering
	// -32601 to every call.
	{
		std::unique_ptr<Agent::AgentSession> mcpSess = Agent::AgentSession::WrapJob( pJob );
		Agent::AgentMcpAdapter mcp( std::move( mcpSess ), Agent::AgentAutonomy::Commit );

		Agent::JsonValue listEnv; std::string lerr;
		Check( Agent::JsonParse( mcp.HandleLine(
		           "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/list\",\"params\":{}}" ),
		       listEnv, lerr ), "H: tools/list parses" );
		bool advertised = false;
		const Agent::JsonValue& tools = listEnv.get( "result" ).get( "tools" );
		for( std::size_t i = 0; i < tools.size(); ++i )
			if( tools.at( i ).get( "name" ).asString() == "add_wear" ) advertised = true;
		Check( advertised, "H MONEY: tools/list ADVERTISES add_wear" );

		Agent::JsonValue callEnv; std::string cerr;
		Check( Agent::JsonParse( mcp.HandleLine(
		           "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"tools/call\","
		           "\"params\":{\"name\":\"add_wear\",\"arguments\":{}}}" ),
		       callEnv, cerr ), "H: tools/call parses" );
		const bool disowned = callEnv.has( "error" ) &&
		                      callEnv.get( "error" ).get( "code" ).asNumber( 0 ) == -32601.0;
		Check( !disowned,
		       "H MONEY: tools/call ROUTES add_wear (not -32601) -- the two-list-drift bug 1ed4e7c3 "
		       "fixed for build_element/place_element cannot recur for this verb" );
	}

	pJob->release();
	std::remove( tmp.c_str() );

	// JSON-RPC dispatch, on a FRESH job (the MCP block above owned its own
	// session and mutated the document).
	{
		const std::string tmp2 = TempPath( "addwear_rpc2.RISEscene" );
		Job* pJob2 = LoadScene( SceneThreeMaterials(), tmp2 );
		Check( pJob2 != nullptr, "H: rpc fixture derives" );
		if( pJob2 ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob2 );
			Agent::AgentRpcDispatcher disp( std::move( sess ) );
			const std::string resp = disp.HandleLine(
				"{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"add_wear\",\"params\":{}}" );
			Agent::JsonValue env; std::string perr;
			Check( Agent::JsonParse( resp, env, perr ) && env.isObject(), "H: the response parses" );
			const Agent::JsonValue& result = env.get( "result" );
			Check( result.get( "applied" ).asBool(), "H: the RPC form applied the rewrite" );
			Check( result.get( "material" ).asString() == "mat_b", "H: ...and echoes the material" );
			Check( result.get( "colorSlot" ).asString() == "rd", "H: ...and the colour slot" );
			Check( result.get( "roughSlots" ).isArray() && result.get( "roughSlots" ).size() == 2,
			       "H: ...and the microsurface slots it rebound" );
			Check( !result.get( "painter" ).asString().empty() &&
			       !result.get( "roughPainter" ).asString().empty(),
			       "H: ...and both painters it minted" );
			Check( result.get( "geometry" ).asString() == "sdf_geometry",
			       "H: ...and the geometry family that makes the signal read" );
			Check( result.get( "baseColor" ).isArray() && result.get( "baseColor" ).size() == 3,
			       "H: ...and the base colour it banded around" );
			pJob2->release();
			std::remove( tmp2.c_str() );
		}
	}

	// A REFUSAL comes back as a SUCCESSFUL JSON-RPC response with ok=false --
	// not an error envelope -- because "nothing here qualifies" is an answer.
	{
		std::string body = Preamble();
		body += "box_geometry\n{\n\tname slab\n\twidth 1\n\theight 1\n\tdepth 1\n}\n\n";
		body += Ggx( "mat_box", "pnt_bronze", "0.3" );
		body += Obj( "o1", "slab", "mat_box", 0 );
		const std::string tmp3 = TempPath( "addwear_rpc3.RISEscene" );
		Job* pJob3 = LoadScene( body, tmp3 );
		Check( pJob3 != nullptr, "H-refuse: fixture derives" );
		if( pJob3 ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob3 );
			Agent::AgentRpcDispatcher disp( std::move( sess ) );
			const std::string resp = disp.HandleLine(
				"{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"add_wear\",\"params\":{}}" );
			Agent::JsonValue env; std::string perr;
			Check( Agent::JsonParse( resp, env, perr ) && env.isObject(), "H-refuse: response parses" );
			Check( !env.has( "error" ),
			       "H-refuse MONEY: a pre-commit refusal is a SUCCESSFUL response, not a JSON-RPC "
			       "error -- \"nothing here qualifies\" is an answer, not a malformed call" );
			Check( !env.get( "result" ).get( "ok" ).asBool() &&
			       env.get( "result" ).get( "status" ).asString().empty(),
			       "H-refuse: ok=false with an EMPTY status, so a caller branches on `applied`" );
			Check( !env.get( "result" ).get( "message" ).asString().empty(),
			       "H-refuse: ...and the reason rides in `message`" );
			pJob3->release();
			std::remove( tmp3.c_str() );
		}
	}
}

int main()
{
	std::printf( "AgentAddWearTest -- GEOMETRY_SHADING_SIGNALS sec 11: add_wear\n" );
	TestQualifySelectRewrite();
	TestSelection();
	TestLambertianColourOnly();
	TestDeterminism();
	TestRefusals();
	TestUndo();
	TestAutonomyAndAuthority();
	TestNote();
	TestWireSurface();
	std::printf( "\n%d passed, %d failed\n", g_pass, g_fail );
	return g_fail ? 1 : 0;
}
