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
//    B4 A material authored with a literal roughness of 0.0 (a perfect
//       mirror) is a FOUND constant, not an absent one -- both halves
//       are rewritten, and the emitted band respects VaryBandFor_'s
//       degenerate-band floor rather than a live/inverted band.
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

static void TestRoughnessZeroSentinel()
{
	std::printf( "B4: a material authored with a literal roughness of 0.0 still gets BOTH halves\n" );
	std::string body = Preamble();
	body += SdfBlob( "blob" );
	body += Ggx( "mat_mirror", "pnt_bronze", "0.0" );
	body += Obj( "o1", "blob", "mat_mirror", 0 );
	const std::string tmp = TempPath( "addwear_b4.RISEscene" );
	Job* pJob = LoadScene( body, tmp );
	Check( pJob != nullptr, "B4: fixture derives" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const Agent::AgentSession::AgentAddWearResult r = sess->AddWear();
	Check( r.ok && r.applied, std::string( "B4: applied -- " ) + r.message );
	Check( r.material == "mat_mirror" && r.colorSlot == "rd", "B4: it rebound the colour slot" );
	Check( r.roughnessSlots.size() == 2 &&
	       r.roughnessSlots[0] == "alphax" && r.roughnessSlots[1] == "alphay",
	       "B4 MONEY: a material authored with a literal roughness of 0.0 (a perfect mirror) is a "
	       "FOUND constant, not an absent one -- both ggx roughness slots were rebound, exactly as "
	       "any other flat-roughness material would be" );
	Check( !r.roughnessPainter.empty(),
	       "B4 MONEY: a roughness (scalar_painter) chunk WAS minted -- roughness 0.0 must never be "
	       "conflated with \"no roughness slot found\", which is what a `roughness > 0.0` found-check "
	       "would do" );
	Check( std::fabs( r.previousRoughness - 0.0 ) < 1e-12,
	       "B4: the result struct reports the authored constant, 0.0" );

	const std::string doc = sess->ReadDocument();
	Check( doc.find( "scalar_painter" ) != std::string::npos,
	       "B4: ...and the scalar_painter chunk really is in the document" );

	bool okBase = false, okLo = false, okHi = false;
	const double rBase = ParamValueInChunk( doc, r.roughnessPainter, "rough_base", okBase );
	const double rLo   = ParamValueInChunk( doc, r.roughnessPainter, "rough_polished", okLo );
	const double rHi   = ParamValueInChunk( doc, r.roughnessPainter, "rough_crusted", okHi );
	Check( okBase && okLo && okHi, "B4: the three roughness params are readable out of the chunk" );
	Check( std::fabs( rBase - 0.0 ) < 1e-9, "B4: rough_base is EXACTLY the authored 0.0" );
	// VaryBandFor_'s own degenerate-band fallback (AgentSession.cpp ~33661-33664):
	// the primary band collapses to zero width at roughness 0.0 (0.7x and 1.4x of
	// 0 are both 0), so the function falls back to its floor -- lo=0.001,
	// hi=0.041 -- rather than emitting an inverted or zero-width band.  This is
	// the SAME shared rule vary_material uses; pinned numerically so a future
	// change to it is caught here too.
	Check( rLo > 0.0 && rHi > rLo,
	       "B4 MONEY: the emitted band is neither zero-width nor inverted for a roughness-0.0 "
	       "material -- VaryBandFor_'s floor is doing exactly the job this material needs" );
	Check( std::fabs( rLo - 0.001 ) < 1e-9 && std::fabs( rHi - 0.041 ) < 1e-9,
	       "B4: the degenerate-band floor's exact numbers (0.001 .. 0.041), matching VaryBandFor_'s "
	       "own fallback rule for a roughness of 0.0" );

	Check( pJob->GetScene() != nullptr, "B4: the rewritten document still derives" );

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

//----------------------------------------------------------------------
// Condition M (2026-08-30): the "emissive-on-opaque-shell" translucency
// fake -- DESIGN_ENCLOSED_LIGHT_OPAQUE_SHELL.  A sibling design-note
// condition to L (both ride the SAME ComputeDesignNoteConditionsFromDoc_
// scan and the same ValidateText/ComputeDesignNote carriers this file
// already exercises for L), so its coverage lives in this file too even
// though there is no `add_wear`-style verb for it to test alongside --
// condition M is note/diagnostic-only.
//
// Cases, mirroring condition L's TestNote coverage:
//   (a) opaque pbr-family shell + interior omni_light + a spatially-
//       VARYING `emissive` -> fires, BOTH sentences.
//   (b) same shell + light, no `emissive` bound at all -> fires, first
//       sentence only.
//   (c) SELF-DISARM: the shell is `translucent_material` -> silent.
//   (d) no positional light in the document at all (only the Preamble's
//       directional key light) -> silent.
//   (e) the "shell" is a scene-encompassing box that also contains the
//       camera (a room, not a lantern) -> silent.
//   (f) a small lambertian_luminaire_material "flame" sits at the SAME
//       point as the light, inside a bigger opaque shell -> the flame is
//       never flagged (it is an emitter, excluded from the opaque set by
//       the same rule that excludes it from condition I's), the outer
//       shell is.
//   (g) P1 coverage: a genuine sdf_geometry shell (a single sphere part)
//       with an interior light -> fires, exactly like the analytic-kind
//       shells above.
//   (h) THE P1 REGRESSION: a thin, elongated sdf_geometry cylinder part
//       (radius 0.05, half-height 3) with a light 2 units off its SIDE
//       (laterally, off the thin radius) -> does NOT fire.  Originally a
//       regression against the pre-fix SDFGeometryLocalBounds_, which
//       broadcast the part's isotropic reach radius (~3.0) onto every axis
//       and would have falsely claimed containment.  That hand-rolled
//       reader is GONE (2026-08-30: condition M reads the derived scene),
//       so (h) and (h2) now pin the SAME per-axis / signed-scale behaviour
//       through the REAL SDFGeometry::ComputeBounds -- which is the point:
//       the property is a fact about the engine, not about a port of it.
//   (i) SMALLEST-VOLUME TIE-BREAK: two nested opaque box shells both
//       enclose the light -> the finding names the SMALLER (inner) one,
//       never the outer.
//   (j) an analytic-kind shell beyond box_geometry (sphere_geometry) also
//       fires.
//   (k) THE MEASURED BLIND SPOT (2026-08-30): a multi-piece element -- a
//       container root, an opaque shell with `parent <root>`, and a
//       `shape_light` with `parent <root>` inside it -> fires, names the
//       shell.  Impossible before condition M read the derived scene: the
//       document-side scan excluded every `parent`-bearing object and every
//       parented area light, which is EVERY piece of every element
//       `build_element` writes.  Carries M13 too -- the shape_light's own
//       derived emissive object trivially contains its own centre and must
//       never be named as the shell.  Shell and light carry DIFFERENT
//       non-zero local offsets (not both (0,0,0)) so the fixture actually
//       requires both kinds' parent composition to run with the right sign;
//       (k2) is its companion negative -- mirroring the light's local
//       offset composes to a world point just outside the shell's box, and
//       must NOT fire.
//   (l) THE GEOMETRY-KIND GAP (2026-08-30): a `lathe_geometry` vessel
//       around an interior omni -> fires.  lathe was outside the old
//       analytic allowlist entirely, so no lathe shell could ever be seen.
//   (m)/(n) THE UNBOUNDED-SENTINEL REGRESSION (P2-1, 2026-08-30): an
//       axis-aligned and a rotated `infiniteplane_geometry` (opaque
//       material) with a light above it -> silent in both.  The
//       box-usability filter must reject the plane's +-DBL_MAX bbox via an
//       explicit sentinel-magnitude check, not merely rely on `hi - lo`
//       overflowing to +inf.
//----------------------------------------------------------------------

//! `ggx_material` with an explicit (possibly empty) `emissive` binding.
static std::string GgxEmissive( const std::string& name, const std::string& rd,
                                const std::string& alpha, const std::string& emissive )
{
	std::string s = "ggx_material\n{\n\tname " + name + "\n\trd " + rd + "\n\trs pnt_spec\n"
	                "\talphax " + alpha + "\n\talphay " + alpha + "\n\tior 1.5\n\textinction 0.0\n";
	if( !emissive.empty() ) s += "\temissive " + emissive + "\n";
	s += "}\n\n";
	return s;
}

static std::string Box( const std::string& name, double size )
{
	char buf[64];
	std::snprintf( buf, sizeof( buf ), "%g", size );
	return "box_geometry\n{\n\tname " + name + "\n\twidth " + buf + "\n\theight " + buf +
	       "\n\tdepth " + buf + "\n}\n\n";
}

static std::string OmniAtOrigin( const std::string& name )
{
	return "omni_light\n{\n\tname " + name + "\n\tpower 5.0\n\tcolor 1 1 1\n\tposition 0 0 0\n}\n\n";
}

static std::string OmniAt( const std::string& name, double x, double y, double z )
{
	char buf[128];
	std::snprintf( buf, sizeof( buf ), "%g %g %g", x, y, z );
	return "omni_light\n{\n\tname " + name + "\n\tpower 5.0\n\tcolor 1 1 1\n\tposition " +
	       buf + "\n}\n\n";
}

//! A single-sphere-part sdf_geometry -- a genuine sdf SHELL (as opposed to
//! SdfBlob's dissolved creature), for condition M's (g) coverage: the local-
//! bounds reader must handle sdf_geometry as an enclosure candidate exactly
//! like the analytic allowlist kinds.
static std::string SdfSphereShell( const std::string& name, double radius )
{
	char buf[64];
	std::snprintf( buf, sizeof( buf ), "%g", radius );
	return "sdf_geometry\n{\n\tname " + name + "\n"
	       "\tpart sphere union 0   0 0 0   0 0 0   1 1 1   " + buf + " 0 0   0\n}\n\n";
}

//! P1 regression fixture (review-round M): ONE thin, elongated sdf_geometry
//! part -- a cylinder of radius 0.05 and half-height 3, axis-aligned along
//! local Y, no rotation.  The pre-fix SDFGeometryLocalBounds_ broadcast
//! SDFPartReachRadius_'s ISOTROPIC envelope radius (sqrt(0.05^2+3^2) ~= 3.0)
//! onto every axis, so a point 2 units off the cylinder's THIN side (radius
//! 0.05) still fell inside the resulting ~3x3x3 cube -- a false "enclosed"
//! claim.  The fixed reader keeps radius 0.05 on X/Z and only extends +-3 on
//! Y, so the same point is correctly outside.
static std::string SdfThinCylinderShell( const std::string& name )
{
	return "sdf_geometry\n{\n\tname " + name + "\n"
	       "\tpart cylinder union 0   0 0 0   0 0 0   1 1 1   0.05 3 0   0\n}\n\n";
}

//! P1 regression fixture (convergence round on 49718c7e): a roundcone is the
//! ONE primitive whose local box is asymmetric about the origin on Y
//! (ry0 = min(-a, c-b), ry1 = max(a, c+b) -- here [-0.1, 3.1] for
//! a=b=0.1, c=3), and a NEGATIVE per-part scale component is a legitimate
//! mirroring construct the parser preserves.  scale = (1,-1,1) reflects the
//! true bound to [-3.1, 0.1]; an abs()-scale corner transform leaves it at
//! [-0.1, 3.1] -- shifted to the WRONG side of the origin, capable of a
//! false "enclosed" claim for a light the mirrored geometry never wraps.
static std::string SdfMirroredRoundconeShell( const std::string& name )
{
	return "sdf_geometry\n{\n\tname " + name + "\n"
	       "\tpart roundcone union 0   0 0 0   0 0 0   1 -1 1   0.1 0.1 3   0\n}\n\n";
}

static void TestEnclosedLightShellNote()
{
	std::printf( "M: the design note -- the emissive-on-opaque-shell translucency fake\n" );

	auto hasCode = []( const std::vector<Agent::AgentDiagnostic>& d, const char* code )
		-> const Agent::AgentDiagnostic* {
		for( const Agent::AgentDiagnostic& e : d ) if( e.code == code ) return &e;
		return nullptr;
	};
	const char* const kCode = "DESIGN_ENCLOSED_LIGHT_OPAQUE_SHELL";

	// (a) Opaque shell + interior light + a VARYING emissive -> fires, BOTH
	//     sentences: the containment fact, then the impersonation fact.
	{
		std::string body = Preamble();
		body += Box( "shell", 4.0 );
		body += "expression_painter\n{\n\tname pnt_glow_varying\n\texpr vec3(u,u,u)\n}\n\n";
		body += GgxEmissive( "mat_shell", "pnt_bronze", "0.3", "pnt_glow_varying" );
		body += Obj( "obj_shell", "shell", "mat_shell", 0 );
		body += OmniAtOrigin( "candle" );

		const std::vector<Agent::AgentDiagnostic> diags = Agent::AgentSession::ValidateText( body );
		const Agent::AgentDiagnostic* d = hasCode( diags, kCode );
		Check( d != nullptr, "M1: an omni_light enclosed by an opaque shell FIRES" );
		if( d ) {
			Check( d->severity == Agent::AgentDiagnostic::Severity::Info,
			       "M1: it is an Info-severity ADVISORY" );
			Check( d->message.find( "candle" ) != std::string::npos,
			       "M1: it NAMES the light" );
			Check( d->message.find( "obj_shell" ) != std::string::npos,
			       "M1: ...and the enclosing object" );
			Check( d->message.find( "mat_shell" ) != std::string::npos,
			       "M1: ...and its material" );
			Check( d->message.find( "impersonating" ) != std::string::npos,
			       "M1 MONEY: the shell's OWN varying `emissive` earns the sharper second sentence" );
			Check( d->message.find( "materials-and-media-basics" ) != std::string::npos,
			       "M1: ...and points at the fix (thickness()-driven tau on translucent_material)" );
			Check( d->message.find( "bounding-box" ) != std::string::npos,
			       "M1: ...and hedges the geometric claim as a bbox test, not a watertight one" );

			const std::string note = Agent::AgentSession::ComputeDesignNote( body );
			Check( note.find( d->message ) != std::string::npos,
			       "M1 MONEY: the diagnostic message appears BYTE-IDENTICALLY inside the render-result "
			       "note -- one shared formatter, two carriers" );
		}
	}

	// (b) Same shell + light, NO `emissive` bound at all -> fires, but only
	//     the first sentence -- nothing to call "impersonating".
	{
		std::string body = Preamble();
		body += Box( "shell", 4.0 );
		body += GgxEmissive( "mat_shell", "pnt_bronze", "0.3", "" );
		body += Obj( "obj_shell", "shell", "mat_shell", 0 );
		body += OmniAtOrigin( "candle" );

		const std::vector<Agent::AgentDiagnostic> diags = Agent::AgentSession::ValidateText( body );
		const Agent::AgentDiagnostic* d = hasCode( diags, kCode );
		Check( d != nullptr, "M2: fires without any `emissive` bound" );
		if( d ) {
			Check( d->message.find( "opaque" ) != std::string::npos,
			       "M2: the containment sentence is still there" );
			Check( d->message.find( "impersonating" ) == std::string::npos,
			       "M2 MONEY: ...but the sharper sentence is ABSENT -- there is no painted emissive to "
			       "call out" );
		}
	}

	// (c) SELF-DISARM: the shell is `translucent_material` -- light genuinely
	//     can transmit, so this is not the failure at all.
	{
		std::string body = Preamble();
		body += Box( "shell", 4.0 );
		body += "translucent_material\n{\n\tname mat_shell\n\tref pnt_bronze\n\ttau pnt_bronze\n}\n\n";
		body += Obj( "obj_shell", "shell", "mat_shell", 0 );
		body += OmniAtOrigin( "candle" );

		Check( hasCode( Agent::AgentSession::ValidateText( body ), kCode ) == nullptr,
		       "M3 MONEY: a translucent_material shell does NOT fire -- switching the material kind "
		       "self-disarms the predicate, no state needed" );
	}

	// (d) No positional light in the document at all (only the Preamble's
	//     directional key light) -> silent, even with the same opaque shell.
	{
		std::string body = Preamble();
		body += Box( "shell", 4.0 );
		body += GgxEmissive( "mat_shell", "pnt_bronze", "0.3", "" );
		body += Obj( "obj_shell", "shell", "mat_shell", 0 );

		Check( hasCode( Agent::AgentSession::ValidateText( body ), kCode ) == nullptr,
		       "M4 MONEY: a directional-only document never fires -- a direction carries no world "
		       "point for this condition to place" );
	}

	// (e) A scene-encompassing box (a room) also contains the CAMERA
	//     (Preamble's is at 0 2 9) -- the room-box heuristic skips it even
	//     though it geometrically contains the light too.
	{
		std::string body = Preamble();
		body += Box( "room", 40.0 );
		body += GgxEmissive( "mat_shell", "pnt_bronze", "0.3", "" );
		body += Obj( "obj_room", "room", "mat_shell", 0 );
		body += OmniAtOrigin( "candle" );

		Check( hasCode( Agent::AgentSession::ValidateText( body ), kCode ) == nullptr,
		       "M5 MONEY: an enclosure that ALSO contains the camera is a room/backdrop, not a "
		       "one-light shell -- skipped" );
	}

	// (f) The flame-fixture case: a small lambertian_luminaire_material
	//     "flame" sits at the SAME point as the light, inside a bigger
	//     opaque shell.  The flame is never the finding (it is itself an
	//     emitter, excluded from the opaque set exactly as condition I
	//     excludes a luminaire from its own); the outer shell is.
	{
		std::string body = Preamble();
		body += "sphere_geometry\n{\n\tname flame_sphere\n\tradius 0.1\n}\n\n";
		body += Box( "shell", 4.0 );
		body += "lambertian_luminaire_material\n{\n\tname mat_flame\n\texitance pnt_bronze\n}\n\n";
		body += GgxEmissive( "mat_shell", "pnt_bronze", "0.3", "" );
		body += Obj( "obj_flame", "flame_sphere", "mat_flame", 0 );
		body += Obj( "obj_shell", "shell", "mat_shell", 0 );
		body += OmniAtOrigin( "candle" );

		const std::vector<Agent::AgentDiagnostic> diags = Agent::AgentSession::ValidateText( body );
		const Agent::AgentDiagnostic* d = hasCode( diags, kCode );
		Check( d != nullptr, "M6: fires (the outer shell still encloses the light)" );
		if( d ) {
			Check( d->message.find( "obj_shell" ) != std::string::npos,
			       "M6 MONEY: it names the OUTER shell..." );
			Check( d->message.find( "obj_flame" ) == std::string::npos,
			       "M6 MONEY: ...and never the coincident inner emissive fixture, which IS the light" );
		}
	}

	// (g) P1 coverage: a genuine sdf_geometry shell -- a single sphere part
	//     big enough to enclose the light -- fires exactly like the
	//     analytic-kind shells above.
	{
		std::string body = Preamble();
		body += SdfSphereShell( "sdf_shell", 2.0 );
		body += GgxEmissive( "mat_shell", "pnt_bronze", "0.3", "" );
		body += Obj( "obj_shell", "sdf_shell", "mat_shell", 0 );
		body += OmniAtOrigin( "candle" );

		const std::vector<Agent::AgentDiagnostic> diags = Agent::AgentSession::ValidateText( body );
		const Agent::AgentDiagnostic* d = hasCode( diags, kCode );
		Check( d != nullptr, "M7: a genuine sdf_geometry shell (a sphere part) encloses the light -- FIRES" );
		if( d ) {
			Check( d->message.find( "obj_shell" ) != std::string::npos,
			       "M7: ...and names the sdf shell object" );
		}
	}

	// (h) THE P1 REGRESSION: a thin, elongated sdf cylinder part (radius
	//     0.05, half-height 3, axis-aligned along Y, unrotated) with a
	//     light 2 units off its SIDE -- well outside the true radius-0.05
	//     lateral extent -- must NOT read as enclosed.  Pre-fix,
	//     SDFGeometryLocalBounds_ broadcast SDFPartReachRadius_'s isotropic
	//     envelope (sqrt(0.05^2+3^2) ~= 3.0004) onto EVERY axis, so this
	//     exact light position fell inside the resulting ~3x3x3 cube and
	//     falsely fired -- verified by reasoning against the pre-fix
	//     formula above (and confirmed by briefly reverting
	//     SDFGeometryLocalBounds_ to the old cube-of-SDFPartReachRadius_
	//     form during development, which flips this Check to fail).
	{
		std::string body = Preamble();
		body += SdfThinCylinderShell( "sdf_thin_cyl" );
		body += GgxEmissive( "mat_shell", "pnt_bronze", "0.3", "" );
		body += Obj( "obj_shell", "sdf_thin_cyl", "mat_shell", 0 );
		body += OmniAt( "candle", 2.0, 0.0, 0.0 );

		Check( hasCode( Agent::AgentSession::ValidateText( body ), kCode ) == nullptr,
		       "M8 P1 REGRESSION MONEY: a light 2 units off the SIDE of a thin elongated sdf cylinder "
		       "(radius 0.05, half-height 3) is NOT enclosed -- the per-axis bound correctly rejects it, "
		       "where the old isotropic-radius broadcast (reach ~3 on every axis) would have falsely "
		       "claimed containment" );
	}

	// (h2) SIGNED-SCALE REGRESSION (convergence round): a mirrored roundcone
	//      part (scale 1 -1 1) has its true bound reflected to Y in
	//      [-3.1, 0.1].  A light at y=+2.5 sits inside the UNMIRRORED
	//      bound only -- an abs()-scale transform falsely fires here; the
	//      signed-scale transform correctly does not.  The control at
	//      y=-2.5 sits inside the true mirrored bound and DOES fire,
	//      proving the negative direction isn't a vacuous pass.
	{
		std::string bodyA = Preamble();
		bodyA += SdfMirroredRoundconeShell( "sdf_mirrored_cone" );
		bodyA += GgxEmissive( "mat_shell", "pnt_bronze", "0.3", "" );
		bodyA += Obj( "obj_shell", "sdf_mirrored_cone", "mat_shell", 0 );
		bodyA += OmniAt( "candle", 0.0, 2.5, 0.0 );

		Check( hasCode( Agent::AgentSession::ValidateText( bodyA ), kCode ) == nullptr,
		       "M8b SIGNED-SCALE MONEY: a light at y=+2.5 beside a scale-(1,-1,1) roundcone "
		       "(true bound Y in [-3.1, 0.1]) is NOT enclosed -- an abs()-scale corner "
		       "transform would leave the bound un-mirrored at [-0.1, 3.1] and falsely fire" );

		std::string bodyB = Preamble();
		bodyB += SdfMirroredRoundconeShell( "sdf_mirrored_cone" );
		bodyB += GgxEmissive( "mat_shell", "pnt_bronze", "0.3", "" );
		bodyB += Obj( "obj_shell", "sdf_mirrored_cone", "mat_shell", 0 );
		bodyB += OmniAt( "candle", 0.0, -2.5, 0.0 );

		Check( hasCode( Agent::AgentSession::ValidateText( bodyB ), kCode ) != nullptr,
		       "M8b CONTROL: the same light at y=-2.5 sits inside the true mirrored bound "
		       "and fires -- the negative case above is not a vacuous pass" );
	}

	// (i) SMALLEST-VOLUME TIE-BREAK: two nested opaque box shells both
	//     enclose the light -- the finding names the SMALLER (inner) one,
	//     never the outer.
	{
		std::string body = Preamble();
		body += Box( "shell_outer", 6.0 );
		body += Box( "shell_inner", 2.0 );
		body += GgxEmissive( "mat_outer", "pnt_bronze", "0.3", "" );
		body += GgxEmissive( "mat_inner", "pnt_bronze", "0.3", "" );
		body += Obj( "obj_outer", "shell_outer", "mat_outer", 0 );
		body += Obj( "obj_inner", "shell_inner", "mat_inner", 0 );
		body += OmniAtOrigin( "candle" );

		const std::vector<Agent::AgentDiagnostic> diags = Agent::AgentSession::ValidateText( body );
		const Agent::AgentDiagnostic* d = hasCode( diags, kCode );
		Check( d != nullptr, "M9: two nested opaque shells both enclose the light -- fires" );
		if( d ) {
			Check( d->message.find( "obj_inner" ) != std::string::npos,
			       "M9 MONEY: the SMALLER (inner) shell is named..." );
			Check( d->message.find( "obj_outer" ) == std::string::npos,
			       "M9 MONEY: ...and the larger outer shell is never named" );
		}
	}

	// (j) An analytic-kind shell beyond box_geometry -- sphere_geometry --
	//     also fires.
	{
		std::string body = Preamble();
		body += "sphere_geometry\n{\n\tname sphere_shell\n\tradius 2.0\n}\n\n";
		body += GgxEmissive( "mat_shell", "pnt_bronze", "0.3", "" );
		body += Obj( "obj_shell", "sphere_shell", "mat_shell", 0 );
		body += OmniAtOrigin( "candle" );

		const std::vector<Agent::AgentDiagnostic> diags = Agent::AgentSession::ValidateText( body );
		const Agent::AgentDiagnostic* d = hasCode( diags, kCode );
		Check( d != nullptr, "M10: a sphere_geometry shell (an analytic kind besides box) also fires" );
	}

	// (k) THE MEASURED BLIND SPOT -- the reason condition M was reworked to
	//     read the derived scene at all.  A MULTI-PIECE ELEMENT, exactly as
	//     `build_element` writes one: a container root, then every piece
	//     carrying `parent <root>`.  The shipped document-side scan excluded
	//     any object with a `parent` (it could not resolve a scene-graph
	//     chain by hand) and any rect/shape light with one (their `center` is
	//     LOCAL to the parent), so on a harness-authored scene it saw NOTHING
	//     -- a live p3 altar run rendered an opaque shell around a shape_light
	//     the harness's own light audit measured at ZERO luminance and this
	//     condition stayed silent.
	//
	//     BOTH pieces below are parented to `lantern_root` (world position
	//     5,0,0), and -- unlike the shell and the light sharing local (0,0,0)
	//     in an earlier draft of this fixture -- they carry DIFFERENT,
	//     non-zero local offsets:
	//       shell (obj_shell):     local (-1, 0,   0), half-width 0.8
	//       light (lantern_flame): local (-1, 0.2, 0)
	//     A local (0,0,0) shell coinciding with a local (0,0,0) light is a
	//     degenerate point: parent composition maps a shared local point to
	//     a shared world point NO MATTER what the parent offset is (even 0,
	//     i.e. composition silently skipped), so the light is trivially at
	//     the shell's own centre regardless of whether composition ran at
	//     all -- the pair could pass with parent composition deleted
	//     entirely.  Distinct offsets close that: `obj_shell`'s composed
	//     world box is centred at (5,0,0)+(-1,0,0) = (4,0,0), extents
	//     x[3.2,4.8] y/z[-0.8,0.8]; `lantern_flame`'s composed world centre
	//     is (5,0,0)+(-1,0.2,0) = (4,0.2,0), inside that box ONLY because
	//     BOTH the shell's and the light's own parent-composition ran, with
	//     the correct (+5,0,0) offset, on their own local values.  If either
	//     kind's composition is skipped, or applied with the wrong sign, one
	//     of the two points lands >= 4 units away on X against a box only
	//     1.6 units wide -- well outside -- and the finding does not fire.
	//     (Composition dropped identically, for the SAME numeric value, on
	//     BOTH kinds at once is not distinguishable by any point-in-box test
	//     -- a uniform additive offset cancels out of a containment check
	//     between two points that share it -- but RISE has exactly one
	//     shared `ComposeWorldTransforms` walk for every object-graph node
	//     (`ObjectManager::ComposeWorldTransforms`, which both
	//     `standard_object` and the object a `shape_light` derives to run
	//     through identically), so a regression that broke it wholesale
	//     would fail essentially every other position-dependent test in
	//     this suite, not just this one.)
	//
	//     This case also carries (M13): a shape_light derives to an emissive
	//     OBJECT of its own name, which trivially contains its own centre --
	//     the finding must name the SHELL, never that fixture.
	{
		std::string body = Preamble();
		body += "standard_object\n{\n\tname lantern_root\n\tposition 5 0 0\n}\n\n";
		body += Box( "shell_box", 1.6 );
		body += GgxEmissive( "mat_shell", "pnt_bronze", "0.3", "" );
		body += "standard_object\n{\n\tname obj_shell\n\tgeometry shell_box\n\tmaterial mat_shell\n"
		        "\tparent lantern_root\n\tposition -1 0 0\n}\n\n";
		body += "shape_light\n{\n\tname lantern_flame\n\tparent lantern_root\n\tshape sphere\n"
		        "\tcenter -1 0.2 0\n\tsize 0.2\n\texitance 500\n\tcolor 1 0.8 0.6\n}\n\n";

		const std::vector<Agent::AgentDiagnostic> diags = Agent::AgentSession::ValidateText( body );
		const Agent::AgentDiagnostic* d = hasCode( diags, kCode );
		Check( d != nullptr,
		       "M11 MONEY: a PARENTED opaque shell enclosing a PARENTED shape_light FIRES -- the "
		       "multi-piece-element shape every harness-authored scene has, and the exact case the "
		       "document-side scan was structurally blind to.  The shell and light have DIFFERENT "
		       "local offsets, so this only fires when BOTH kinds' parent composition genuinely ran "
		       "with the correct sign" );
		if( d ) {
			Check( d->message.find( "sits inside `obj_shell`" ) != std::string::npos,
			       "M11: it names the parented SHELL as the enclosure..." );
			Check( d->message.find( "sits inside `lantern_flame`" ) == std::string::npos,
			       "M13 MONEY: ...and NEVER the light's own derived emissive fixture, which trivially "
			       "contains its own centre" );
			Check( d->message.find( "`shape_light lantern_flame`" ) != std::string::npos,
			       "M11: ...and names the light by its authored kind and name" );
		}
	}

	// (k2) M11's COMPANION NEGATIVE.  Identical to (k) -- same root, same
	//     shell at local (-1,0,0) -- except the light's local offset is
	//     mirrored to (+1, 0.2, 0), composing to world
	//     (5,0,0)+(1,0.2,0) = (6, 0.2, 0), which sits 1.2 units past the
	//     shell's world box edge at x=4.8 -- OUTSIDE.  This pins that the
	//     containment test is a real bounded interval check, not a loose
	//     "some positional light is somewhere near this parented shell"
	//     heuristic: (k)'s and (k2)'s shell are byte-identical, and only the
	//     sign of the light's local X offset differs, so a test that fires
	//     on BOTH fixtures would mean the geometry check is not precise
	//     enough to tell -1 from +1 once a parent offset is in play.
	{
		std::string body = Preamble();
		body += "standard_object\n{\n\tname lantern_root\n\tposition 5 0 0\n}\n\n";
		body += Box( "shell_box", 1.6 );
		body += GgxEmissive( "mat_shell", "pnt_bronze", "0.3", "" );
		body += "standard_object\n{\n\tname obj_shell\n\tgeometry shell_box\n\tmaterial mat_shell\n"
		        "\tparent lantern_root\n\tposition -1 0 0\n}\n\n";
		body += "shape_light\n{\n\tname lantern_flame\n\tparent lantern_root\n\tshape sphere\n"
		        "\tcenter 1 0.2 0\n\tsize 0.2\n\texitance 500\n\tcolor 1 0.8 0.6\n}\n\n";

		const std::vector<Agent::AgentDiagnostic> diags = Agent::AgentSession::ValidateText( body );
		const Agent::AgentDiagnostic* d = hasCode( diags, kCode );
		Check( d == nullptr,
		       "M11 NEGATIVE: mirroring the light's local offset to the OTHER side of the shell's "
		       "local centre composes to a world point outside the shell's world box, and must NOT "
		       "fire -- pins that (k)'s fire is a precise bounded-box result, not a loose proximity "
		       "match on the parented pair" );
	}

	// (l) THE GEOMETRY-KIND GAP: a lathe_geometry vessel (a closed profile of
	//     revolution -- r=0 at both ends, so a watertight vase) around an
	//     interior omni.  lathe was OUTSIDE the old scan's analytic allowlist
	//     (box/sphere/ellipsoid/cylinder/torus/sdf), so no lathe shell could
	//     ever fire; the derived scene has a real box for it, as it does for
	//     every other geometry kind.
	{
		std::string body = Preamble();
		body += "lathe_geometry\n{\n\tname vessel\n"
		        "\tprofile_point 0 -2\n"
		        "\tprofile_point 1.5 -1\n"
		        "\tprofile_point 1.8 0\n"
		        "\tprofile_point 1.5 1\n"
		        "\tprofile_point 0 2\n"
		        "\taxis y\n\tsweep_degrees 360\n\tn_radial 24\n}\n\n";
		body += GgxEmissive( "mat_shell", "pnt_bronze", "0.3", "" );
		body += Obj( "obj_vessel", "vessel", "mat_shell", 0 );
		body += OmniAtOrigin( "candle" );

		const std::vector<Agent::AgentDiagnostic> diags = Agent::AgentSession::ValidateText( body );
		const Agent::AgentDiagnostic* d = hasCode( diags, kCode );
		Check( d != nullptr,
		       "M12 MONEY: a lathe_geometry vessel enclosing an omni FIRES -- a geometry kind the old "
		       "analytic allowlist could not size at all" );
		if( d ) {
			Check( d->message.find( "sits inside `obj_vessel`" ) != std::string::npos,
			       "M12: ...and names the lathe object" );
		}
	}

	// (m) THE UNBOUNDED-SENTINEL REGRESSION (P2-1): an axis-aligned
	//     `infiniteplane_geometry` (opaque, non-luminaire material) with a
	//     light above it must NEVER be read as an enclosing shell.  Its
	//     default-constructed BoundingBox is +-RISE_INFINITY == +-DBL_MAX on
	//     every axis, which IS finite by IsFiniteDouble -- the box-usability
	//     filter in CollectDerivedSceneFacts_ must reject it via the
	//     explicit sentinel-magnitude check (>= 1e29), not rely on `hi - lo`
	//     happening to overflow to +inf, since a smaller-but-still-huge
	//     finite sentinel (RISE's own +-1e30 / +-FLT_MAX family) would
	//     satisfy both IsFiniteDouble AND a finite non-overflowing
	//     subtraction.
	{
		std::string body = Preamble();
		body += "infiniteplane_geometry\n{\n\tname inf_plane\n\txtile 1.0\n\tytile 1.0\n}\n\n";
		body += GgxEmissive( "mat_plane", "pnt_bronze", "0.3", "" );
		body += Obj( "obj_plane", "inf_plane", "mat_plane", 0 );
		body += OmniAt( "sun", 0, 0, 5 );

		const std::vector<Agent::AgentDiagnostic> diags = Agent::AgentSession::ValidateText( body );
		const Agent::AgentDiagnostic* d = hasCode( diags, kCode );
		Check( d == nullptr,
		       "M14 (P2-1): an axis-aligned infinite plane is NEVER read as an enclosing shell -- its "
		       "+-DBL_MAX bbox must be rejected as an unbounded sentinel, not merely happen to fail a "
		       "later finite-extent check" );
	}

	// (n) THE UNBOUNDED-SENTINEL REGRESSION, ROTATED (P2-1): the same
	//     opaque infinite plane, but the object also carries a non-trivial
	//     `orientation` -- a rotated instance's world corners are computed
	//     by transforming the +-DBL_MAX local bounds, which the pre-fix
	//     code only rejected because THAT corner transform overflowed to
	//     +-inf.  The explicit sentinel check catches it directly instead,
	//     so this must stay silent exactly like the axis-aligned case.
	{
		std::string body = Preamble();
		body += "infiniteplane_geometry\n{\n\tname inf_plane\n\txtile 1.0\n\tytile 1.0\n}\n\n";
		body += GgxEmissive( "mat_plane", "pnt_bronze", "0.3", "" );
		body += "standard_object\n{\n\tname obj_plane\n\tgeometry inf_plane\n\tmaterial mat_plane\n"
		        "\torientation 30 15 0\n\tposition 0 0 0\n}\n\n";
		body += OmniAt( "sun", 0, 0, 5 );

		const std::vector<Agent::AgentDiagnostic> diags = Agent::AgentSession::ValidateText( body );
		const Agent::AgentDiagnostic* d = hasCode( diags, kCode );
		Check( d == nullptr,
		       "M14 (P2-1), rotated variant: a rotated infinite plane is ALSO never read as an "
		       "enclosing shell -- the explicit sentinel check catches it directly rather than "
		       "depending on the rotated corner transform overflowing" );
	}
}

//----------------------------------------------------------------------
// Condition N (2026-08-30): the DIM HERO LIGHT -- DESIGN_DIM_HERO_LIGHT.
// Condition M's sibling, and here for M's reason: both ride the SAME
// ComputeDesignNoteConditionsFromDoc_ scan and the same carriers this
// file already exercises, and N's own M-SUPPRESSION rule can only be
// tested where M's fixtures are.
//
// WHAT IS DIFFERENT ABOUT IT, and therefore what these cases have to
// pin: N's input is a MEASUREMENT held in SESSION STATE, not a fact
// recomputable from the bytes.  So every case drives the cache through
// the same public seam `light_scene`'s own measurement pass calls
// (AgentSession::RecordLightSoloMeasurements) and then reads the note
// through a SESSION carrier (AgentSession::Validate), never the
// stateless text-only one -- which N1c pins as deliberately SILENT.
//
// Cases:
//   (a) N1  FIRES: a shape_light authored at `exitance 5000` whose
//       cached solo audit measured it at 1.9% of the scene's measured
//       light total -- the motivating trajectory's own numbers (1.3
//       against a 67.9 all-lights frame).  Both numbers in the clause,
//       byte-identical across the two carriers, and SILENT through the
//       cache-less static carrier.
//   (b) N2  VALIDITY: the light's chunk is edited (exitance 5000 ->
//       6000) -> the cached entry is dropped and the note goes silent.
//       A stale measurement never speaks.
//   (c) N3  HEALTHY SHARE: the same light measured at 30% -> silent.
//   (d) N4  AUTHORED FAINT: an `omni_light power 0.1` measured at 0.5%
//       -> silent (a deliberate whisper is not a bug).  N4b is its
//       companion positive: the SAME fixture at `power 5` and the SAME
//       measured share DOES fire, so the only thing separating them is
//       clause (ii).
//   (e) N5  M-SUPPRESSION: one light both enclosed by an opaque shell
//       (M fires) and dim in the cache -> M present, N ABSENT.  M says
//       WHY it is dark; N alone would only say "it is dark".
//   (f) N6  DELETED LIGHT: the cached light's chunk is gone from the
//       document -> silent, no crash (the orphaned entry cannot match
//       a chunk that is not there).
//   (g) N7  INTEGRATION: the REAL `light_scene` path -- enumeration,
//       build, N solo renders -- populates the cache itself, with the
//       right kind and the chunk's verbatim bytes, and records ONLY the
//       four positional kinds.
//----------------------------------------------------------------------

//! A `shape_light` -- the one-chunk area-light form the motivating
//! trajectory's lantern candle took, and condition N's headline case.
static std::string ShapeLightAt( const std::string& name, double exitance, const char* center )
{
	char buf[64];
	std::snprintf( buf, sizeof( buf ), "%g", exitance );
	return "shape_light\n{\n\tname " + name + "\n\tshape sphere\n\tcenter " + center +
	       "\n\tsize 0.15\n\texitance " + buf + "\n\tcolor 1 0.9 0.7\n}\n\n";
}

//! An `omni_light` with an explicit `power`, for clause (ii)'s pair.
static std::string OmniPowerAt( const std::string& name, double power, const char* position )
{
	char buf[64];
	std::snprintf( buf, sizeof( buf ), "%g", power );
	return "omni_light\n{\n\tname " + name + "\n\tpower " + buf +
	       "\n\tcolor 1 1 1\n\tposition " + position + "\n}\n\n";
}

//! A lit slab, so every fixture has something for the lights to fall on
//! and derives/renders like a real scene.
static std::string DimLightSlab()
{
	return Box( "slab", 1.0 ) + Ggx( "mat_slab", "pnt_bronze", "0.3" ) +
	       Obj( "obj_slab", "slab", "mat_slab", 0 );
}

//! Drive ONE solo audit into `sess` through the SAME public seam
//! `light_scene`'s measurement pass calls.  `dim*` is the light under
//! test; `bright*` is the rest of the scene's light, present so the
//! soloed total is positive (an all-black audit records nothing at all,
//! by design) and so the share under test is a real fraction of
//! something.  The bright entry is deliberately the Preamble's
//! DIRECTIONAL key: it is soloable and it feeds the total, but it is not
//! one of the four positional kinds, so it must get no cache record.
static void RecordSoloAudit( Agent::AgentSession& sess, const std::string& dimName,
                             double dimLuma, double dimShare )
{
	Agent::AgentSession::AgentLightSceneResult r;
	r.ok                = true;
	r.allLightsMeanLuma = 67.9;

	Agent::AgentSession::AgentLightContribution dim;
	dim.name               = dimName;
	dim.kind               = "emissive object";
	dim.soloed             = true;
	dim.meanLuma           = dimLuma;
	dim.shareOfSoloedTotal = dimShare;
	r.contributions.push_back( dim );

	Agent::AgentSession::AgentLightContribution bright;
	bright.name               = "key";
	bright.kind               = "light";
	bright.soloed             = true;
	bright.meanLuma           = 66.6;
	bright.shareOfSoloedTotal = 1.0 - dimShare;
	r.contributions.push_back( bright );

	sess.RecordLightSoloMeasurements( r );
	r.contributions.clear();
}

//! A canned two-answer completer, the AgentChunkCrudTest arc-81 helper's
//! shape: answer 0 is the source enumeration, answer 1 the build, and the
//! last answer repeats if a repair retry asks again.
static Agent::AgentSession::AgentTextCompleter MakeCannedCompleter( std::vector<std::string> answers )
{
	Agent::AgentSession::AgentTextCompleter c;
	c.supported    = true;
	c.providerName = "mock";
	c.modelId      = "mock-lighting-1";
	auto shared = std::make_shared<std::vector<std::string> >( std::move( answers ) );
	auto count  = std::make_shared<int>( 0 );
	c.complete = [shared, count]( const std::string& ) -> Agent::AgentSession::AgentTextCompletionOutcome
	{
		Agent::AgentSession::AgentTextCompletionOutcome o;
		if( shared->empty() ) { o.error = "no canned answer"; return o; }
		const std::size_t idx = ( static_cast<std::size_t>( *count ) < shared->size() )
			? static_cast<std::size_t>( *count ) : shared->size() - 1;
		++( *count );
		o.ok   = true;
		o.text = ( *shared )[idx];
		return o;
	};
	return c;
}

static void TestDimHeroLightNote()
{
	std::printf( "N: the design note -- the dim hero light (authored bright, measured dark)\n" );

	auto hasCode = []( const std::vector<Agent::AgentDiagnostic>& d, const char* code )
		-> const Agent::AgentDiagnostic* {
		for( const Agent::AgentDiagnostic& e : d ) if( e.code == code ) return &e;
		return nullptr;
	};
	const char* const kCode  = "DESIGN_DIM_HERO_LIGHT";
	const char* const kCodeM = "DESIGN_ENCLOSED_LIGHT_OPAQUE_SHELL";

	// The motivating trajectory's own figures: a 1.3 mean-luma solo against
	// a 67.9 all-lights frame, i.e. 1.9% of the measured light total.
	const double kDimLuma  = 1.3;
	const double kDimShare = 1.3 / 67.9;

	// (a) N1: FIRES, with BOTH numbers, on both carriers, and only with a
	//     cache.
	{
		const std::string body = Preamble() + DimLightSlab() +
			ShapeLightAt( "lantern_candle", 5000.0, "0 3 0" );
		const std::string tmp = TempPath( "addwear_dim_n1.RISEscene" );
		Job* pJob = LoadScene( body, tmp );
		Check( pJob != nullptr, "N1 fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

			Check( hasCode( sess->Validate( body ), kCode ) == nullptr,
			       "N1 BEFORE any measurement the note is silent -- nothing has been measured, so "
			       "there is nothing for it to speak from" );

			RecordSoloAudit( *sess, "lantern_candle", kDimLuma, kDimShare );

			const std::vector<Agent::AgentDiagnostic> diags = sess->Validate( body );
			const Agent::AgentDiagnostic* d = hasCode( diags, kCode );
			Check( d != nullptr,
			       "N1 MONEY: a light authored at exitance 5000 whose own solo audit measured it at "
			       "1.9% of the scene's light total FIRES -- the measurement rides the note carrier "
			       "instead of dying in one turn's result text" );
			if( d ) {
				Check( d->severity == Agent::AgentDiagnostic::Severity::Info,
				       "N1 it is an Info-severity ADVISORY" );
				Check( d->message.find( "lantern_candle" ) != std::string::npos,
				       "N1 it NAMES the light" );
				Check( d->message.find( "exitance 5000" ) != std::string::npos,
				       "N1 MONEY: ...quotes what the author WROTE" );
				Check( d->message.find( "1.9%" ) != std::string::npos,
				       "N1 MONEY: ...and what the audit MEASURED -- the mismatch between the two IS "
				       "the finding, and either number alone reads as an opinion" );
				Check( d->message.find( "not reaching the scene" ) != std::string::npos,
				       "N1 ...states the consequence" );
				Check( d->message.find( "re-run light_scene" ) != std::string::npos,
				       "N1 ...and the action, including the re-measure" );
				Check( d->message.find( "edits ELSEWHERE" ) != std::string::npos,
				       "N1 MONEY: ...and HEDGES honestly -- the cached figure tracks this light's own "
				       "chunk and nothing else, which is the one thing the validity key cannot cover" );

				const std::string note = Agent::AgentSession::ComputeDesignNote(
					body, false, &sess->LightSoloMeasurements() );
				Check( note.find( d->message ) != std::string::npos,
				       "N1 MONEY: the diagnostic message appears BYTE-IDENTICALLY inside the "
				       "render-result note -- one shared formatter, two carriers" );
			}

			// N1c: the documented limitation, pinned rather than assumed.
			Check( hasCode( Agent::AgentSession::ValidateText( body ), kCode ) == nullptr,
			       "N1c MONEY: the STATELESS text-only carrier is SILENT on N -- it holds no session "
			       "and therefore no measurement, and a condition whose input the caller does not "
			       "have must be absent, never guessed" );
			Check( Agent::AgentSession::ComputeDesignNote( body ).find( "light_scene's own solo" ) ==
			       std::string::npos,
			       "N1c ...and so is the stateless note wrapper, on the same bytes" );

			pJob->release();
			std::remove( tmp.c_str() );
		}
	}

	// (b) N2: VALIDITY.  The measurement was taken against `exitance 5000`;
	//     retune the light and the entry is dropped SILENTLY rather than
	//     quoting a number the document no longer says.
	{
		const std::string body = Preamble() + DimLightSlab() +
			ShapeLightAt( "lantern_candle", 5000.0, "0 3 0" );
		const std::string tmp = TempPath( "addwear_dim_n2.RISEscene" );
		Job* pJob = LoadScene( body, tmp );
		Check( pJob != nullptr, "N2 fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			RecordSoloAudit( *sess, "lantern_candle", kDimLuma, kDimShare );
			Check( hasCode( sess->Validate( body ), kCode ) != nullptr,
			       "N2 the un-edited document still fires (the control for the case below)" );

			const std::string edited = Preamble() + DimLightSlab() +
				ShapeLightAt( "lantern_candle", 6000.0, "0 3 0" );
			Check( hasCode( sess->Validate( edited ), kCode ) == nullptr,
			       "N2 MONEY: editing the light's own chunk DROPS the cached measurement -- a stale "
			       "figure never speaks, and the note goes silent until light_scene re-measures" );
			pJob->release();
			std::remove( tmp.c_str() );
		}
	}

	// (c) N3: a HEALTHY share is not a finding.
	{
		const std::string body = Preamble() + DimLightSlab() +
			ShapeLightAt( "lantern_candle", 5000.0, "0 3 0" );
		const std::string tmp = TempPath( "addwear_dim_n3.RISEscene" );
		Job* pJob = LoadScene( body, tmp );
		Check( pJob != nullptr, "N3 fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			RecordSoloAudit( *sess, "lantern_candle", 20.0, 0.30 );
			Check( hasCode( sess->Validate( body ), kCode ) == nullptr,
			       "N3 MONEY: a light measured at 30% of the scene's light total is WORKING -- a note "
			       "that fires on a working light is how this family loses the right to be read" );
			pJob->release();
			std::remove( tmp.c_str() );
		}
	}

	// (d) N4/N4b: clause (ii), both directions.  Same geometry, same
	//     measured share -- only the AUTHORED intensity differs.
	{
		const std::string faint = Preamble() + DimLightSlab() +
			OmniPowerAt( "candle", 0.1, "0 3 0" );
		const std::string tmpF = TempPath( "addwear_dim_n4.RISEscene" );
		Job* pJobF = LoadScene( faint, tmpF );
		Check( pJobF != nullptr, "N4 faint fixture derives" );
		if( pJobF ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJobF );
			RecordSoloAudit( *sess, "candle", 0.3, 0.005 );
			Check( hasCode( sess->Validate( faint ), kCode ) == nullptr,
			       "N4 MONEY: a light AUTHORED at power 0.1 measuring 0.5% is a deliberate whisper "
			       "doing exactly what it was asked to -- never a finding" );
			pJobF->release();
			std::remove( tmpF.c_str() );
		}

		const std::string bright = Preamble() + DimLightSlab() +
			OmniPowerAt( "candle", 5.0, "0 3 0" );
		const std::string tmpB = TempPath( "addwear_dim_n4b.RISEscene" );
		Job* pJobB = LoadScene( bright, tmpB );
		Check( pJobB != nullptr, "N4b bright fixture derives" );
		if( pJobB ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJobB );
			RecordSoloAudit( *sess, "candle", 0.3, 0.005 );
			// NAMED, not a temporary: `hasCode` hands back a pointer INTO the
			// vector, so binding the call inline would leave `d` dangling the
			// moment the full expression ends.
			const std::vector<Agent::AgentDiagnostic> diags = sess->Validate( bright );
			const Agent::AgentDiagnostic* d = hasCode( diags, kCode );
			Check( d != nullptr,
			       "N4b MONEY: the SAME fixture at power 5 and the SAME 0.5% share DOES fire -- the "
			       "only thing separating N4 from N4b is the authored intensity, which is exactly "
			       "what clause (ii) claims to test" );
			if( d ) Check( d->message.find( "power 5" ) != std::string::npos,
			               "N4b ...and the clause quotes the omni's `power`, not an `exitance` it "
			               "does not have" );
			pJobB->release();
			std::remove( tmpB.c_str() );
		}
	}

	// (e) N5: M-SUPPRESSION.  One light, both enclosed and dim.
	{
		std::string body = Preamble();
		body += Box( "shell", 4.0 );
		body += GgxEmissive( "mat_shell", "pnt_bronze", "0.3", "" );
		body += Obj( "obj_shell", "shell", "mat_shell", 0 );
		body += ShapeLightAt( "lantern_candle", 5000.0, "0 0 0" );
		const std::string tmp = TempPath( "addwear_dim_n5.RISEscene" );
		Job* pJob = LoadScene( body, tmp );
		Check( pJob != nullptr, "N5 fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			RecordSoloAudit( *sess, "lantern_candle", kDimLuma, kDimShare );
			const std::vector<Agent::AgentDiagnostic> diags = sess->Validate( body );
			Check( hasCode( diags, kCodeM ) != nullptr,
			       "N5 condition M fires on the enclosed light" );
			Check( hasCode( diags, kCode ) == nullptr,
			       "N5 MONEY: condition N is SUPPRESSED for the same light -- M already names it AND "
			       "says why it is dark, and N alone would add only \"it is dark, find out why\"" );
			pJob->release();
			std::remove( tmp.c_str() );
		}
	}

	// (f) N6: the cached light's chunk is GONE.
	{
		const std::string body = Preamble() + DimLightSlab() +
			ShapeLightAt( "lantern_candle", 5000.0, "0 3 0" );
		const std::string tmp = TempPath( "addwear_dim_n6.RISEscene" );
		Job* pJob = LoadScene( body, tmp );
		Check( pJob != nullptr, "N6 fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			RecordSoloAudit( *sess, "lantern_candle", kDimLuma, kDimShare );

			const std::string deleted = Preamble() + DimLightSlab();
			const std::vector<Agent::AgentDiagnostic> diags = sess->Validate( deleted );
			Check( hasCode( diags, kCode ) == nullptr,
			       "N6 MONEY: a cache entry whose light chunk no longer exists is SILENT, not a crash "
			       "and not a note about a light that is not there -- the orphan simply never matches" );
			pJob->release();
			std::remove( tmp.c_str() );
		}
	}

	// (g) N7: THE REAL PATH.  `light_scene` -- enumeration, build, its own
	//     solo renders -- must be what populates the cache; a seam only the
	//     tests call would be a condition that never fires in production.
	{
		const std::string body = Preamble() + DimLightSlab() +
			ShapeLightAt( "lantern_candle", 5000.0, "0 3 0" );
		const std::string tmp = TempPath( "addwear_dim_n7.RISEscene" );
		Job* pJob = LoadScene( body, tmp );
		Check( pJob != nullptr, "N7 fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			sess->SetTextCompleter( MakeCannedCompleter( {
				"a warm candle burning inside the lantern\n",
				"rect_light\n{\n\tname lit_panel\n\tcenter 0 3 1\n\tsize 2 1\n"
				"\tfacing 0 -1 0\n\texitance 40\n\tcolor 1 1 1\n}\n" } ) );

			const Agent::AgentSession::AgentLightSceneResult r = sess->LightScene();
			Check( r.ok, "N7 light_scene completes" );
			Check( r.soloedCount >= 1, "N7 and it soloed at least one light" );

			const Agent::AgentSession::AgentLightSoloMeasurementMap& cache =
				sess->LightSoloMeasurements();
			const Agent::AgentSession::AgentLightSoloMeasurementMap::const_iterator it =
				cache.find( "lantern_candle" );
			Check( it != cache.end(),
			       "N7 MONEY: the REAL light_scene measurement pass wrote the cache itself -- the "
			       "seam the cases above drive is the one production calls, not a test-only door" );
			if( it != cache.end() ) {
				Check( it->second.kind == "shape_light",
				       "N7 ...with the AUTHORED chunk keyword, joined by name to the emissive object "
				       "the shape_light derives to" );
				Check( it->second.chunkText.find( "exitance 5000" ) != std::string::npos,
				       "N7 ...and the chunk's VERBATIM bytes as the validity key" );
				Check( it->second.intensityParam == "exitance" &&
				       it->second.authoredIntensity == 5000.0,
				       "N7 ...and the authored intensity it was measured against" );
			}
			Check( cache.find( "key" ) == cache.end(),
			       "N7 MONEY: the Preamble's DIRECTIONAL light is soloed and reported exactly as "
			       "before but gets NO record -- condition N's scope is the four positional kinds, "
			       "and nothing outside it is cached for a condition that would never read it" );
			pJob->release();
			std::remove( tmp.c_str() );
		}
	}
}

int main()
{
	std::printf( "AgentAddWearTest -- GEOMETRY_SHADING_SIGNALS sec 11: add_wear\n" );
	TestQualifySelectRewrite();
	TestSelection();
	TestLambertianColourOnly();
	TestRoughnessZeroSentinel();
	TestDeterminism();
	TestRefusals();
	TestUndo();
	TestAutonomyAndAuthority();
	TestNote();
	TestWireSurface();
	TestEnclosedLightShellNote();
	TestDimHeroLightNote();
	std::printf( "\n%d passed, %d failed\n", g_pass, g_fail );
	return g_fail ? 1 : 0;
}
