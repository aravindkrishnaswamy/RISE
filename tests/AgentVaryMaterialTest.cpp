//////////////////////////////////////////////////////////////////////
//
//  AgentVaryMaterialTest.cpp - 88 S5 (2026-08-20): vary_material, the
//    VERB half of design-note condition D.
//
//  WHAT THIS VERB HAS TO GET RIGHT, and therefore what is measured here.
//  Its contract is "band around the number that is already there": the
//  material it touches must stay the material the author wrote, with the
//  ONE difference that its microsurface stops being flat.  So every
//  positive case asserts three things a weaker test would miss --
//    (a) the minted painter is GENUINELY spatially varying, evaluated
//        directly through IScalarPainter::GetValuesAt at distinct world
//        points (a render-pixel check is vacuous: Monte-Carlo noise
//        varies every pixel whether or not the material does);
//    (b) every sample it produces lands INSIDE the band the emitted
//        `param` metadata advertises -- an unclamped mix would walk a
//        roughness slot toward zero and silently mirror the surface, and
//        the params would be lying;
//    (c) the material chunk's roughness slot really points at it, read
//        back out of the DOCUMENT, and the whole scene still derives.
//
//  Cases:
//    A  QUALIFY + SELECT + REWRITE.  Three constant-roughness ggx
//       materials; the bare call takes the one bound to the MOST objects
//       (not the first in the document), mints ONE scalar_painter, rebinds
//       BOTH alphax and alphay, and the document still derives + renders.
//    A2 The band, numerically: every probe of the minted painter is inside
//       [rough_lo, rough_hi] and the spread is real.
//    B  SELECTION: `material` overrides the auto-pick; the lexicographic
//       tie-break decides between two equally-used materials; a material
//       bound through a `source` + `count_u` instancing chunk counts as
//       ONE chunk toward prominence, never the minted copies (B3).
//    C  DETERMINISM: two runs from the SAME input document produce
//       BYTE-IDENTICAL output.  No clock, no PRNG state.
//    D  REFUSALS, each with the document BYTE-IDENTICAL afterwards: no
//       qualifying material, an already-varying material, an unknown
//       name, a material whose kind has no microsurface slot, and a
//       roughness of zero (a deliberate mirror).
//    E  UNDO through a live SceneEditController restores the pre-verb
//       document EXACTLY -- the whole rewrite is ONE undo step, so a
//       Cmd-Z can never leave a material pointing at a chunk that is no
//       longer there.
//    F  AUTONOMY: refused under Read; refused with a Propose-specific
//       message under Propose; refused (document byte-identical) under
//       External authority, with the SAME no-staged-form shape
//       collapse_to_instances uses.
//    G  THE NOTE fires exactly on the predicate (>= 3 qualifying AND 0
//       varying), NAMES the verb, and is byte-identical across its two
//       carriers.  Must NOT fire at 2 qualifying; must NOT fire when one
//       spatially-varying microsurface binding already exists; must STILL
//       fire when a fourth, non-qualifying OPAQUE material is present
//       (TestOpaqueTripleDisqualifies); must STOP firing on the document
//       vary_material itself just mutated (TestPostVaryDisarmsCensus).
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
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../src/Library/Job.h"
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/Interfaces/IScene.h"
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
// Undo() the GUI's Cmd-Z drives (EditHistory lives on the controller;
// the headless direct-Job path has no undo stack at all, so an
// undo-restores-the-document claim can only be tested here).  No
// Start() -- the test never needs a render thread, and not spawning one
// keeps the case deterministic.
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
// THE OBSERVABLE: evaluate a minted scalar_painter through the same
// accessor the renderer uses, at distinct WORLD points (the expression
// bodies this verb writes read `P`), and report both the spread and the
// extremes.
//----------------------------------------------------------------------
struct ScalarProbe
{
	double lo = 1e30;
	double hi = -1e30;
	bool   found = false;
	double Spread() const { return found ? ( hi - lo ) : -1.0; }
};

static ScalarProbe ProbeScalarPainter( Job& j, const std::string& name )
{
	ScalarProbe out;
	IScalarPainterManager* mgr = j.GetScalarPainters();
	IScalarPainter* p = mgr ? mgr->GetItem( name.c_str() ) : nullptr;
	if( !p ) return out;
	out.found = true;
	for( int i = 0; i < 24; ++i ) {
		RayIntersectionGeometric ri( Ray(), nullRasterizerState );
		ri.bHit = true;
		ri.ptCoord = Point2( ( i % 7 ) / 7.0, ( ( i * 3 ) % 5 ) / 5.0 );
		ri.ptIntersection = Point3( i * 0.41 - 2.0, i * 0.73 - 1.3, i * 1.17 + 0.5 );
		const double v = p->GetValuesAt( ri ).v[0];
		out.lo = std::min( out.lo, v );
		out.hi = std::max( out.hi, v );
	}
	return out;
}

//! The value on the FIRST line whose first token is `param` and whose
//! SECOND token is `which`, searched from chunk `chunkName`'s `name` line.
static double ParamValueInChunk( const std::string& doc, const std::string& chunkName,
                                 const std::string& which, bool& ok )
{
	ok = false;
	const std::size_t namePos = doc.find( chunkName );
	if( namePos == std::string::npos ) return 0.0;
	const std::string marker = "param";
	std::size_t pos = namePos;
	while( true ) {
		pos = doc.find( marker, pos );
		if( pos == std::string::npos ) return 0.0;
		const std::size_t eol = doc.find( '\n', pos );
		const std::string line = doc.substr( pos, eol - pos );
		// line looks like: param<ws>which<ws>value<ws>min ...
		std::size_t w = line.find( which );
		if( w != std::string::npos ) {
			const std::size_t vs = line.find_first_not_of( " \t", w + which.size() );
			if( vs != std::string::npos ) {
				ok = true;
				return std::strtod( line.c_str() + vs, nullptr );
			}
		}
		pos = eol == std::string::npos ? doc.size() : eol;
		if( pos >= doc.size() ) return 0.0;
	}
}

//----------------------------------------------------------------------
// Fixtures.  Each derives on its own -- shader, rasterizer, film, camera,
// painters, materials, geometry -- so a failure is never "the scene did
// not load".
//----------------------------------------------------------------------
static std::string Preamble()
{
	return
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
		"film\n{\n\twidth 24\n\theight 24\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 2 9\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 45.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_dark\n\tcolor 0.1 0.1 0.1\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_spec\n\tcolor 0.6 0.6 0.6\n}\n\n"
		"sphere_geometry\n{\n\tname sph\n\tradius 0.6\n}\n\n"
		"directional_light\n{\n\tname key\n\tpower 3.0\n\tcolor 1 1 1\n\tdirection 0.4 0.5 0.9\n}\n\n";
}

static std::string Ggx( const std::string& name, const std::string& alpha )
{
	return "ggx_material\n{\n\tname " + name + "\n\trd pnt_dark\n\trs pnt_spec\n"
	       "\talphax " + alpha + "\n\talphay " + alpha + "\n\tior 1.5\n\textinction 0.0\n}\n\n";
}

static std::string Obj( const std::string& name, const std::string& mat, double x )
{
	char buf[64];
	std::snprintf( buf, sizeof( buf ), "%g", x );
	return "standard_object\n{\n\tname " + name + "\n\tgeometry sph\n\tmaterial " + mat +
	       "\n\tposition " + buf + " 0 0\n}\n\n";
}

//! pbr_metallic_roughness_material -- 88 S6.  `roughness` resolves through
//! the COLOUR painter manager (Job::AddPBRMetallicRoughnessMaterial calls
//! pPntManager->GetItem() on it), unlike every other qualifying kind's
//! primary roughness slot.
static std::string Pbr( const std::string& name, const std::string& roughness )
{
	return "pbr_metallic_roughness_material\n{\n\tname " + name + "\n\tbase_color pnt_dark\n"
	       "\tmetallic 0.0\n\troughness " + roughness + "\n}\n\n";
}

//! Three qualifying ggx materials.  `mat_b` is bound to THREE objects,
//! `mat_a` to two and `mat_c` to one, so the bare call has a unique
//! most-prominent answer that is NOT the first in document order -- a
//! test whose fixture made document order and prominence agree could not
//! tell the two selection rules apart.
static std::string SceneThreeMaterials()
{
	std::string s = Preamble();
	s += Ggx( "mat_a", "0.3" );
	s += Ggx( "mat_b", "0.25" );
	s += Ggx( "mat_c", "0.4" );
	s += Obj( "o1", "mat_a", -3 );
	s += Obj( "o2", "mat_a", -2 );
	s += Obj( "o3", "mat_b", -1 );
	s += Obj( "o4", "mat_b", 0 );
	s += Obj( "o5", "mat_b", 1 );
	s += Obj( "o6", "mat_c", 2 );
	return s;
}

//----------------------------------------------------------------------

static void TestQualifySelectRewrite()
{
	std::printf( "A: qualify + select + rewrite -- the bare call takes the most-bound material\n" );
	const std::string tmp = TempPath( "varymat_a.RISEscene" );
	Job* pJob = LoadScene( SceneThreeMaterials(), tmp );
	Check( pJob != nullptr, "A: fixture derives" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const std::string docBefore = sess->ReadDocument();

	const Agent::AgentSession::AgentVaryMaterialResult r = sess->VaryMaterial();
	Check( r.ok && r.applied, std::string( "A: the no-argument call APPLIED -- " ) + r.message );
	Check( r.status == "applied", "A: status is \"applied\"" );
	Check( r.material == "mat_b",
	       "A MONEY: it took `mat_b` -- the material bound to the MOST objects (3), not `mat_a`, "
	       "which is FIRST in the document (2 objects)" );
	Check( r.materialKind == "ggx_material", "A: the result names the material's kind" );
	Check( r.qualifyingMaterials == 3, "A: all three constant-roughness materials qualified" );
	Check( r.boundObjects == 3, "A: it reports how many objects bind the chosen material" );
	Check( std::fabs( r.previousRoughness - 0.25 ) < 1e-12,
	       "A: it reports the constant it banded around, exactly as authored" );
	Check( r.reboundSlots.size() == 2 &&
	       r.reboundSlots[0] == "alphax" && r.reboundSlots[1] == "alphay",
	       "A: BOTH ggx roughness slots were rebound (a ggx with only alphax varying would be "
	       "anisotropic by accident)" );
	Check( !r.painterChunk.empty(), "A: it names the painter chunk it minted" );

	const std::string docAfter = sess->ReadDocument();
	Check( docBefore != docAfter, "A: the document really changed" );
	Check( docAfter.find( "scalar_painter" ) != std::string::npos, "A: a scalar_painter chunk landed" );
	Check( docAfter.find( "expression" ) != std::string::npos, "A: ...in the `expression` form" );
	Check( docAfter.find( "alphax " + r.painterChunk ) != std::string::npos,
	       "A MONEY: the DOCUMENT's alphax names the minted chunk (the rebind is real, read back "
	       "out of the text rather than assumed from the result struct)" );
	Check( docAfter.find( "alphay " + r.painterChunk ) != std::string::npos,
	       "A: ...and so does alphay" );
	// THE HUMAN-EDITABILITY CONTRACT: this verb's own output must obey the
	// rule the skills teach, or the worked example a model copies is wrong.
	Check( docAfter.find( "min " ) != std::string::npos && docAfter.find( "max " ) != std::string::npos &&
	       docAfter.find( "step " ) != std::string::npos && docAfter.find( "label " ) != std::string::npos,
	       "A MONEY: every emitted `param` carries min/max/step/label metadata -- the verb's own "
	       "output follows the human-editability contract it exists to teach" );
	Check( docAfter.find( "\tseed" ) != std::string::npos, "A: ...and a `seed` line for per-instance jitter" );
	// The two untouched materials are byte-identically untouched.
	Check( docAfter.find( "name mat_a\n" ) != std::string::npos &&
	       docAfter.find( "name mat_c\n" ) != std::string::npos,
	       "A: the other two materials survive" );
	Check( docAfter.find( "alphax 0.3" ) != std::string::npos &&
	       docAfter.find( "alphax 0.4" ) != std::string::npos,
	       "A: ...with their own roughness untouched -- ONE material was varied, not all three" );

	// The scene still derives (the chunk landed BEFORE its consumer) and
	// the live scalar-painter manager resolved it.
	Check( pJob->GetScene() != nullptr, "A: the rewritten document still derives" );

	// ---- A2: THE BAND, numerically --------------------------------------
	{
		const ScalarProbe p = ProbeScalarPainter( *pJob, r.painterChunk );
		Check( p.found, "A2: the minted painter resolved in the live scalar-painter manager" );
		bool okLo = false, okHi = false;
		const double lo = ParamValueInChunk( docAfter, r.painterChunk, "rough_lo", okLo );
		const double hi = ParamValueInChunk( docAfter, r.painterChunk, "rough_hi", okHi );
		Check( okLo && okHi, "A2: both band params are readable out of the emitted chunk" );
		std::printf( "    A2: band [%g, %g], observed [%g, %g], spread %g\n",
		             lo, hi, p.lo, p.hi, p.Spread() );
		Check( p.Spread() > 0.01,
		       "A2 MONEY: the minted painter is GENUINELY spatially varying (spread " +
		       std::to_string( p.Spread() ) + " over 24 distinct world points, evaluated through "
		       "IScalarPainter::GetValuesAt -- NOT a render-pixel check)" );
		Check( p.lo >= lo - 1e-9 && p.hi <= hi + 1e-9,
		       "A2 MONEY: every sample lands INSIDE the band the emitted params advertise -- the "
		       "clamp before the mix is what makes the min/max metadata true rather than "
		       "decorative (an unclamped fbm mix walks a roughness slot toward zero)" );
		Check( lo > 0.0 && hi > lo,
		       "A2: the band is well-formed (positive, non-inverted) around the authored 0.25" );
	}

	// It renders.
	{
		Agent::AgentRenderParams rp;
		rp.width = 32; rp.height = 32; rp.samples = 4;
		const Agent::AgentRenderResult rr = sess->Render( rp );
		Check( rr.ok, "A: the rewritten scene renders" );
		Check( rr.meanR + rr.meanG + rr.meanB > 0.0, "A: ...and is non-black" );
	}

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

//! A2b: an authored roughness ABOVE 1 must keep real headroom above itself.
//! `VaryBandFor_`'s earlier `cap = roughness` collapsed `hi` to EXACTLY the
//! authored value for this regime -- zero headroom -- while every
//! model-facing description of this verb (AgentChatCodecs kToolDefs,
//! AgentMcpAdapter's tool description, this file's own AgentSession.h doc
//! comment) claimed "roughly 0.7x .. 1.4x" for every regime.  1.6 is deep
//! enough into the >1 branch that 1.4x (2.24) would also have exceeded the
//! old `cap == roughness` ceiling, so a regression back to the old formula
//! fails this the same way it fails the model-facing claim.
static void TestBandAboveOne()
{
	std::printf( "A3: an authored roughness above 1 keeps real headroom -- hi > roughness, never == it\n" );

	std::string body = Preamble();
	body += Ggx( "mat_hi", "1.6" );
	body += Obj( "o1", "mat_hi", 0 );
	const std::string tmp = TempPath( "varymat_a3.RISEscene" );
	Job* pJob = LoadScene( body, tmp );
	Check( pJob != nullptr, "A3: fixture derives" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const Agent::AgentSession::AgentVaryMaterialResult r = sess->VaryMaterial();
	Check( r.ok && r.applied, std::string( "A3: applied -- " ) + r.message );
	Check( std::fabs( r.previousRoughness - 1.6 ) < 1e-12, "A3: it banded around the authored 1.6" );

	const std::string doc = sess->ReadDocument();
	bool okLo = false, okHi = false;
	const double lo = ParamValueInChunk( doc, r.painterChunk, "rough_lo", okLo );
	const double hi = ParamValueInChunk( doc, r.painterChunk, "rough_hi", okHi );
	Check( okLo && okHi, "A3: both band params are readable out of the emitted chunk" );
	std::printf( "    A3: authored 1.6, band [%g, %g]\n", lo, hi );

	Check( hi > r.previousRoughness,
	       "A3 MONEY: hi (" + std::to_string( hi ) + ") is STRICTLY GREATER than the authored "
	       "roughness (1.6) -- the earlier `cap = roughness` collapsed hi to exactly the authored "
	       "value here, zero headroom, contradicting the \"roughly 0.7x .. 1.4x\" every model-facing "
	       "description of this verb claims" );
	Check( lo < r.previousRoughness && lo > 0.0,
	       "A3: lo stays below the authored value and positive, unaffected by the >1 headroom fix" );
	// The fixed ceiling is 1.15x rather than 1.4x above 1 (deliberately
	// smaller -- see VaryBandFor_'s own comment) -- pin the actual number
	// rather than only the inequality, so a silent formula change elsewhere
	// (e.g. reverting to 1.4x, which would ALSO satisfy hi > roughness) is
	// still caught.
	Check( std::fabs( hi - 1.6 * 1.15 ) < 1e-9,
	       "A3 MONEY: hi is exactly 1.6 * 1.15 (" + std::to_string( 1.6 * 1.15 ) + ") -- the specific "
	       "above-1 ceiling this fix chose, not merely SOME value greater than 1.6" );
	Check( std::fabs( lo - 1.6 * 0.7 ) < 1e-9, "A3: lo is exactly 1.6 * 0.7, the unchanged floor rule" );

	// And the minted painter's live samples really do land inside that band
	// (the same clamp-before-mix property A2 pins at roughness < 1), so the
	// headroom fix did not disturb the clamp.
	{
		const ScalarProbe p = ProbeScalarPainter( *pJob, r.painterChunk );
		Check( p.found, "A3: the minted painter resolved in the live scalar-painter manager" );
		Check( p.lo >= lo - 1e-9 && p.hi <= hi + 1e-9,
		       "A3: every sample still lands inside the (now wider) band" );
	}

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

static void TestSelection()
{
	std::printf( "B: selection -- explicit `material`, and the lexicographic tie-break\n" );

	// (1) Explicit override.
	{
		const std::string tmp = TempPath( "varymat_b1.RISEscene" );
		Job* pJob = LoadScene( SceneThreeMaterials(), tmp );
		Check( pJob != nullptr, "B1: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const Agent::AgentSession::AgentVaryMaterialResult r = sess->VaryMaterial( "mat_c" );
			Check( r.ok && r.applied, std::string( "B1: the explicit call applied -- " ) + r.message );
			Check( r.material == "mat_c",
			       "B1 MONEY: `material` OVERRIDES the auto-pick (mat_c has ONE object; the bare "
			       "call would have taken mat_b)" );
			Check( std::fabs( r.previousRoughness - 0.4 ) < 1e-12, "B1: it banded around mat_c's own 0.4" );
			sess.reset();
			pJob->release();
			std::remove( tmp.c_str() );
		}
	}

	// (2) TIE-BREAK.  Two materials, one object each: lexicographic by name
	//     decides, so re-ordering the two chunks cannot silently change the
	//     answer.  `zed_mat` is FIRST in the document precisely so document
	//     order and the lexicographic rule disagree.
	{
		std::string body = Preamble();
		body += Ggx( "zed_mat", "0.2" );
		body += Ggx( "alpha_mat", "0.2" );
		body += Obj( "oz", "zed_mat", -1 );
		body += Obj( "oa", "alpha_mat", 1 );
		const std::string tmp = TempPath( "varymat_b2.RISEscene" );
		Job* pJob = LoadScene( body, tmp );
		Check( pJob != nullptr, "B2: tie fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const Agent::AgentSession::AgentVaryMaterialResult r = sess->VaryMaterial();
			Check( r.ok && r.applied, std::string( "B2: applied -- " ) + r.message );
			Check( r.material == "alpha_mat",
			       "B2 MONEY: a tie on object count breaks LEXICOGRAPHICALLY, not by document "
			       "position -- `alpha_mat` wins although `zed_mat` is declared first" );
			sess.reset();
			pJob->release();
			std::remove( tmp.c_str() );
		}
	}

	// (3) INSTANCED CHUNK COUNTS AS ONE.  `mat_b` is bound via a `source` +
	//     `count_u 5` instancing chunk (plus the source object itself, which
	//     `source` COPIES rather than moves or hides -- 88's own condition-C
	//     comment) -- TWO standard_object CHUNKS, minting FIVE more world
	//     copies through the source.  `mat_a` is bound to THREE plain
	//     objects.  Prominence here is "how much of the frame is this
	//     material", proxied by CHUNK count, not by expanded copies -- so
	//     `mat_a` (3 chunks) must beat `mat_b` (2 chunks) even though mat_b's
	//     material paints far more geometry (6 copies) once the instancing
	//     chunk expands.  A selector that counted copies instead of chunks
	//     would take `mat_b` here (6 > 3) and this case would fail.
	{
		std::string body = Preamble();
		body += Ggx( "mat_a", "0.3" );
		body += Ggx( "mat_b", "0.25" );
		body += Obj( "o1", "mat_a", -3 );
		body += Obj( "o2", "mat_a", -2 );
		body += Obj( "o3", "mat_a", -1 );
		body += "standard_object\n{\n\tname src\n\tgeometry sph\n\tmaterial mat_b\n"
		        "\tposition 0 5 0\n}\n\n";
		body += "standard_object\n{\n\tname grid\n\tsource src\n\tcount_u 5\n\tmaterial mat_b\n"
		        "\tposition expr(u*1.4-2.8) 7 0\n}\n\n";
		const std::string tmp = TempPath( "varymat_b3.RISEscene" );
		Job* pJob = LoadScene( body, tmp );
		Check( pJob != nullptr, "B3: instanced fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const Agent::AgentSession::AgentVaryMaterialResult r = sess->VaryMaterial();
			Check( r.ok && r.applied, std::string( "B3: applied -- " ) + r.message );
			Check( r.material == "mat_a",
			       "B3 MONEY: the bare call takes `mat_a` (3 chunks) over `mat_b` (2 chunks, `src` + "
			       "`grid`) even though mat_b's count_u expansion paints twice as many world copies -- "
			       "prominence counts the CHUNK, not the minted copies" );

			// And selected EXPLICITLY, `boundObjects` itself pins the exact
			// number: 2 (the source chunk plus the one instancing chunk), not
			// 6 (source plus five expanded copies) and not 1 (missing the
			// still-visible source).
			std::unique_ptr<Agent::AgentSession> sess2;
			{
				Job* pJob2 = LoadScene( body, TempPath( "varymat_b3b.RISEscene" ) );
				Check( pJob2 != nullptr, "B3b: second instanced fixture derives" );
				if( pJob2 ) {
					sess2 = Agent::AgentSession::WrapJob( pJob2 );
					const Agent::AgentSession::AgentVaryMaterialResult r2 = sess2->VaryMaterial( "mat_b" );
					Check( r2.ok && r2.applied, std::string( "B3b: explicit mat_b applied -- " ) + r2.message );
					Check( r2.boundObjects == 2,
					       "B3b MONEY: `boundObjects` reports 2 -- the `src` chunk and the `grid` "
					       "instancing chunk, ONE credit each, never the 5 copies `count_u` mints and "
					       "never omitting the still-visible source" );
					sess2.reset();
					pJob2->release();
					std::remove( TempPath( "varymat_b3b.RISEscene" ).c_str() );
				}
			}

			sess.reset();
			pJob->release();
			std::remove( tmp.c_str() );
		}
	}
}

static void TestDeterminism()
{
	std::printf( "C: determinism -- two runs on the same document are BYTE-IDENTICAL\n" );

	auto run = [&]( const char* tag ) -> std::string {
		const std::string tmp = TempPath( ( std::string( "varymat_c_" ) + tag + ".RISEscene" ).c_str() );
		Job* pJob = LoadScene( SceneThreeMaterials(), tmp );
		if( !pJob ) return std::string();
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
		const Agent::AgentSession::AgentVaryMaterialResult r = sess->VaryMaterial();
		Check( r.applied, std::string( "C(" ) + tag + "): applied" );
		const std::string doc = sess->ReadDocument();
		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
		return doc;
	};

	// A real wall-clock gap between the two runs, so a clock-seeded jitter
	// (the failure mode this case exists to exclude) would have time to
	// produce a different number rather than passing by being too fast.
	const std::string one = run( "one" );
	std::this_thread::sleep_for( std::chrono::milliseconds( 25 ) );
	const std::string two = run( "two" );

	Check( !one.empty() && !two.empty(), "C: both runs produced a document" );
	Check( one == two,
	       "C MONEY: two runs on the SAME input document produce BYTE-IDENTICAL output -- the "
	       "field scale, contrast and seed are hashed from the material NAME, never from a clock "
	       "or a PRNG" );

	// And a DIFFERENT material name really does jitter different constants,
	// so the determinism above is not "the same constant every time".
	{
		std::string body = Preamble();
		body += Ggx( "other_mat", "0.25" );
		body += Obj( "oo", "other_mat", 0 );
		const std::string tmp = TempPath( "varymat_c_other.RISEscene" );
		Job* pJob = LoadScene( body, tmp );
		Check( pJob != nullptr, "C: second-name fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const Agent::AgentSession::AgentVaryMaterialResult r = sess->VaryMaterial();
			Check( r.applied, "C: second-name run applied" );
			const std::string doc = sess->ReadDocument();
			bool okA = false, okB = false;
			const double scaleA = ParamValueInChunk( one, "mat_b_roughfield", "field_scale", okA );
			const double scaleB = ParamValueInChunk( doc, "other_mat_roughfield", "field_scale", okB );
			Check( okA && okB, "C: field_scale readable from both documents" );
			Check( scaleA != scaleB,
			       "C: a DIFFERENT material name jitters a DIFFERENT field_scale (the constants "
			       "really vary with the name, not just the chunk labels)" );
			sess.reset();
			pJob->release();
			std::remove( tmp.c_str() );
		}
	}
}

//! Every refusal: the document must come back BYTE-IDENTICAL and the
//! message must carry a RULE-SPECIFIC substring, so no refusal can be
//! satisfied by a different rule firing.
static void ExpectRefusal( const char* label, const std::string& body,
                           const std::string& expectSubstr,
                           const std::string& material = std::string() )
{
	const std::string tmp = TempPath( ( std::string( "varymat_ref_" ) + label + ".RISEscene" ).c_str() );
	Job* pJob = LoadScene( body, tmp );
	Check( pJob != nullptr, std::string( "D(" ) + label + "): fixture derives" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const std::string before = sess->ReadDocument();
	const Agent::AgentSession::AgentVaryMaterialResult r = sess->VaryMaterial( material );
	Check( !r.ok && !r.applied && r.status.empty(),
	       std::string( "D(" ) + label + "): REFUSED pre-commit (ok=false, empty status)" );
	Check( r.message.find( expectSubstr ) != std::string::npos,
	       std::string( "D(" ) + label + "): the message names THIS rule (\"" + expectSubstr +
	       "\") -- got: " + r.message );
	Check( sess->ReadDocument() == before,
	       std::string( "D(" ) + label + "): the document is BYTE-IDENTICAL afterwards" );
	Check( r.painterChunk.empty() && r.reboundSlots.empty(),
	       std::string( "D(" ) + label + "): it reports no chunk and no rebind (a caller that "
	       "believed them would go looking for something that does not exist)" );

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

static void TestRefusals()
{
	std::printf( "D: refusals -- each specific, each non-mutating\n" );

	// (1) NOTHING QUALIFIES: only lambertians, which have no microsurface
	//     scalar slot at all.
	{
		std::string body = Preamble();
		body += "lambertian_material\n{\n\tname flat\n\treflectance pnt_dark\n}\n\n";
		body += Obj( "o1", "flat", 0 );
		ExpectRefusal( "nothing-qualifies", body, "no material in this document has a microsurface" );
	}

	// (2) ALREADY VARYING: the only ggx's roughness is already an
	//     expression, so there is no constant to band around.
	{
		std::string body = Preamble();
		body += "scalar_painter\n{\n\tname sp_var\n\tparam f 3.0\n\texpression clamp(fbm(P*f, 3, 0.5, 2.0)+0.5, 0.05, 0.9)\n}\n\n";
		body += Ggx( "mat_v", "sp_var" );
		body += Obj( "o1", "mat_v", 0 );
		ExpectRefusal( "already-varying", body, "no material in this document has a microsurface" );
	}

	// (3) A NAMED material that does not exist at all.  The message must
	//     say so specifically -- "no chunk named" -- rather than reporting
	//     the generic nothing-qualifies reason, because the two need
	//     different corrections.
	{
		std::string body = Preamble();
		body += Ggx( "mat_a", "0.3" );
		body += Obj( "o1", "mat_a", 0 );
		ExpectRefusal( "unknown-name", body, "no chunk named", "not_a_thing" );
	}

	// (4) A NAMED chunk that EXISTS but has no readable microsurface -- a
	//     lambertian.  Distinct message from (3).
	{
		std::string body = Preamble();
		body += "lambertian_material\n{\n\tname flat\n\treflectance pnt_dark\n}\n\n";
		body += Ggx( "mat_a", "0.3" );
		body += Obj( "o1", "mat_a", 0 );
		ExpectRefusal( "wrong-kind", body, "exists but its microsurface is not a readable constant", "flat" );
	}

	// (5) A ROUGHNESS OF ZERO is a deliberate mirror-specular surface;
	//     banding it would change what the material IS.
	{
		std::string body = Preamble();
		body += Ggx( "mat_mirror", "0" );
		body += Obj( "o1", "mat_mirror", 0 );
		ExpectRefusal( "zero-roughness", body, "deliberate mirror-specular surface" );
	}
}

static void TestUndo()
{
	std::printf( "E: undo -- ONE step restores the pre-verb document EXACTLY\n" );
	const std::string tmp = TempPath( "varymat_undo.RISEscene" );
	Job* pJob = LoadScene( SceneThreeMaterials(), tmp );
	Check( pJob != nullptr, "E: fixture derives" );
	if( !pJob ) return;

	{
		QuietController c( *pJob );
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
		sess->AttachController( &c );

		const std::string before = sess->ReadDocument();
		const Agent::AgentSession::AgentVaryMaterialResult r = sess->VaryMaterial();
		Check( r.ok && r.applied, std::string( "E: applied through the controller -- " ) + r.message );
		const std::string after = sess->ReadDocument();
		Check( after != before, "E: the controller-attached commit really changed the document" );

		// ONE Undo(), not two: the inserted painter and the rebound slot are
		// ONE composite swap, so a single Cmd-Z must restore both.  If they
		// were two ops, one Undo would leave `alphax` pointing at a chunk
		// that no longer exists.
		c.Undo();
		const std::string restored = sess->ReadDocument();
		Check( restored == before,
		       "E MONEY: ONE Undo() restores the pre-verb document BYTE-EXACTLY -- the painter "
		       "insert and the slot rebind are one undoable unit, so undo can never strand a "
		       "material pointing at a chunk that is gone" );

		// And redo puts it back, so the step is a real history entry rather
		// than a discarded document.
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

	const std::string tmp = TempPath( "varymat_autonomy.RISEscene" );

	// (1) READ autonomy: the dispatcher's deny-by-default gate refuses the
	//     verb BEFORE it reaches AgentSession, exactly like every other
	//     mutating verb.
	{
		Job* pJob = LoadScene( SceneThreeMaterials(), tmp );
		Check( pJob != nullptr, "F1: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const std::string before = sess->ReadDocument();
			Agent::AgentRpcDispatcher disp( std::move( sess ), Agent::AgentAutonomy::Read );
			const std::string resp = disp.HandleLine(
				"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"vary_material\",\"params\":{}}" );
			Agent::JsonValue env; std::string perr;
			Check( Agent::JsonParse( resp, env, perr ) && env.isObject(), "F1: response parses" );
			Check( env.has( "error" ),
			       "F1 MONEY: vary_material is REFUSED under Read autonomy -- it is not on the "
			       "read-safe allowlist, so the deny-by-default gate catches it without any "
			       "per-verb code" );
			// The dispatcher owns the session now, so the document is read
			// back through the dispatcher's own read_document rather than
			// through the (moved-from) session handle.
			const std::string readResp = disp.HandleLine(
				"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"read_document\",\"params\":{}}" );
			Agent::JsonValue readEnv; std::string rerr;
			Check( Agent::JsonParse( readResp, readEnv, rerr ) &&
			       readEnv.get( "result" ).get( "document" ).asString() == before,
			       "F1: the document is BYTE-IDENTICAL after the Read-autonomy refusal" );
		}
	}

	// (2) PROPOSE autonomy: refused too, and with the Propose-SPECIFIC
	//     message shape (a truthful data.autonomy, not the generic Read
	//     fallback's hardcoded "read") -- the same treatment
	//     collapse_to_instances gets, for the same reason: a composite
	//     whole-document swap has no AgentProposalKind to stage.
	{
		Job* pJob = LoadScene( SceneThreeMaterials(), tmp );
		Check( pJob != nullptr, "F2: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			Agent::AgentRpcDispatcher disp( std::move( sess ), Agent::AgentAutonomy::Propose );
			const std::string resp = disp.HandleLine(
				"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"vary_material\",\"params\":{}}" );
			Agent::JsonValue env; std::string perr;
			Check( Agent::JsonParse( resp, env, perr ) && env.isObject(), "F2: response parses" );
			Check( env.has( "error" ), "F2: refused under Propose autonomy as well" );
			const std::string msg = env.get( "error" ).get( "message" ).asString();
			Check( msg.find( "vary_material" ) != std::string::npos,
			       "F2: the refusal NAMES the verb" );
			Check( msg.find( "--agent-autonomy=propose" ) != std::string::npos &&
			       msg.find( "--agent-autonomy=commit" ) != std::string::npos,
			       "F2 MONEY: the Propose refusal is the verb's OWN message -- truthful about the "
			       "current posture and about commit being the real escape hatch, not the generic "
			       "Read-flavoured fallback" );
		}
	}

	// (3) EXTERNAL authority: refused, document byte-identical, and the
	//     message names the staged steps that DO exist -- collapse_to_
	//     instances' exact shape, deliberately not a new one.
	{
		Job* pJob = LoadScene( SceneThreeMaterials(), tmp );
		Check( pJob != nullptr, "F3: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess =
				Agent::AgentSession::WrapJob( pJob, Agent::AgentAuthority::External );
			const std::string before = sess->ReadDocument();
			const Agent::AgentSession::AgentVaryMaterialResult r = sess->VaryMaterial();
			Check( !r.ok && !r.applied,
			       "F3: an External-authority session cannot commit this verb" );
			Check( r.message.find( "no staged-proposal form" ) != std::string::npos,
			       "F3 MONEY: the refusal states the SAME reason collapse_to_instances gives -- ONE "
			       "composite document swap is no AgentProposalKind an Owner could approve "
			       "card-by-card -- rather than inventing a different staging shape" );
			Check( r.message.find( "insert_chunk" ) != std::string::npos &&
			       r.message.find( "propose_patch" ) != std::string::npos,
			       "F3: ...and names the two staged steps that DO exist, so the refusal is actionable" );
			Check( sess->ReadDocument() == before, "F3: the document is byte-identical" );
			// F3 reaches this refusal AFTER the candidate is fully composed --
			// the field chunk was named and the slots were rebound in the
			// CANDIDATE -- so it is the case that proves the result fields are
			// published on the COMMIT, not on the plan.  A caller that read
			// them here and believed them would go looking for a chunk that
			// was never written.
			Check( r.painterChunk.empty() && r.reboundSlots.empty(),
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

	auto hasCode = []( const std::vector<Agent::AgentDiagnostic>& d, const char* code ) -> const Agent::AgentDiagnostic* {
		for( const Agent::AgentDiagnostic& e : d ) if( e.code == code ) return &e;
		return nullptr;
	};
	const char* const kCode = "DESIGN_CONSTANT_MICROSURFACE";

	// (1) THREE qualifying, ZERO varying -> FIRES.
	{
		const std::string doc = SceneThreeMaterials();
		const std::vector<Agent::AgentDiagnostic> diags = Agent::AgentSession::ValidateText( doc );
		const Agent::AgentDiagnostic* d = hasCode( diags, kCode );
		Check( d != nullptr, "G1: three constant-microsurface materials + zero varying bindings FIRES" );
		if( d ) {
			Check( d->severity == Agent::AgentDiagnostic::Severity::Info,
			       "G1: it is an Info-severity ADVISORY, not a correctness problem" );
			Check( d->message.find( "vary_material" ) != std::string::npos,
			       "G1 MONEY: the clause NAMES THE VERB -- advice asking for the hand rewrite is "
			       "measured dead (0/24), so the note's whole job is to state a call" );
			Check( d->message.find( "NO ARGUMENTS" ) != std::string::npos,
			       "G1: ...and states the zero-argument form, the lowest-friction one" );
			Check( d->message.find( "mat_b" ) != std::string::npos,
			       "G1 MONEY: it names the SAME material a bare call takes -- note and verb read one "
			       "shared predicate, so the note cannot advertise a call that edits something else" );
			Check( d->message.find( "REFUSES" ) != std::string::npos,
			       "G1: ...and is honest about the refusal, so trying it is knowably free" );
			Check( d->message.find( "ignore and do not churn" ) != std::string::npos,
			       "G1: ...and carries its own anti-churn escape" );

			// THE VERBATIM-COPY INVARIANT, by construction (one shared
			// formatter, two carriers).
			const std::string note = Agent::AgentSession::ComputeDesignNote( doc );
			Check( note.find( d->message ) != std::string::npos,
			       "G1 MONEY: the diagnostic message appears BYTE-IDENTICALLY inside the "
			       "render-result note -- one shared formatter, two carriers, so they cannot drift" );
		}
	}

	// (2) TWO qualifying -> SILENT.  The gate is three.
	{
		std::string body = Preamble();
		body += Ggx( "m1", "0.3" );
		body += Ggx( "m2", "0.2" );
		body += Obj( "o1", "m1", -1 );
		body += Obj( "o2", "m2", 1 );
		Check( hasCode( Agent::AgentSession::ValidateText( body ), kCode ) == nullptr,
		       "G2 MONEY: TWO qualifying materials does NOT fire -- the gate is three, and a note "
		       "that fired on every small scene would be noise" );
	}

	// (3) THREE qualifying but ONE spatially-varying microsurface binding
	//     already in the document -> SILENT.  The note prices an unreached
	//     affordance; one varying binding proves the author has reached it.
	{
		std::string body = Preamble();
		body += "scalar_painter\n{\n\tname sp_var\n\tparam f 3.0\n\texpression clamp(fbm(P*f, 3, 0.5, 2.0)+0.5, 0.05, 0.9)\n}\n\n";
		body += Ggx( "m1", "0.3" );
		body += Ggx( "m2", "0.2" );
		body += Ggx( "m3", "0.4" );
		body += Ggx( "m4", "sp_var" );
		body += Obj( "o1", "m1", -2 );
		body += Obj( "o2", "m2", -1 );
		body += Obj( "o3", "m3", 1 );
		body += Obj( "o4", "m4", 2 );
		const std::vector<Agent::AgentDiagnostic> diags = Agent::AgentSession::ValidateText( body );
		Check( hasCode( diags, kCode ) == nullptr,
		       "G3 MONEY: three qualifying materials but ONE already-varying microsurface binding "
		       "DISARMS the note -- it prices an unreached affordance, and one varying binding "
		       "proves the author has already reached it" );
		// ...and the VERB still works on that document, because a scene that
		// textured one material may still be shipping three flat ones.
		{
			const std::string tmp = TempPath( "varymat_g3.RISEscene" );
			Job* pJob = LoadScene( body, tmp );
			Check( pJob != nullptr, "G3: disarmed-note fixture derives" );
			if( pJob ) {
				std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
				const Agent::AgentSession::AgentVaryMaterialResult r = sess->VaryMaterial();
				Check( r.ok && r.applied,
				       "G3 MONEY: the VERB does NOT inherit the note's disarm -- it still varies one "
				       "of the three flat materials, because a scene that textured its floor may "
				       "still be shipping plastic props" );
				sess.reset();
				pJob->release();
				std::remove( tmp.c_str() );
			}
		}
	}
}

//! An UNEQUAL scalar_painter{values} triple on a PRIMARY microsurface slot
//! -- a per-channel dispersive binding authored on purpose
//! (MicrosurfaceParseUniformNumber_'s own doc) -- disqualifies its material
//! via the OPAQUE path: the three components disagree, so it is not
//! MicrosurfaceBinding_::Constant, and a `values` triple is not one of the
//! four forms the scalar_painter descriptor documents as spatially varying,
//! so it is not MicrosurfaceBinding_::Varying either.  It must therefore
//! NOT increment the census (`varyingMicrosurfaceBindings`) that disarms
//! condition D wholesale -- an Opaque slot on an otherwise-unrelated fourth
//! material must not silence the note about the three OTHER materials that
//! genuinely qualify.
static void TestOpaqueTripleDisqualifies()
{
	std::printf( "D6/G4a: an UNEQUAL scalar_painter{values} triple on a primary slot disqualifies via "
	             "the OPAQUE path (neither Constant nor Varying), and does not disarm the note\n" );

	auto hasCode = []( const std::vector<Agent::AgentDiagnostic>& d, const char* code ) -> const Agent::AgentDiagnostic* {
		for( const Agent::AgentDiagnostic& e : d ) if( e.code == code ) return &e;
		return nullptr;
	};
	const char* const kCode = "DESIGN_CONSTANT_MICROSURFACE";

	std::string body = Preamble();
	body += Ggx( "mat_a", "0.3" );
	body += Ggx( "mat_b", "0.25" );
	body += Ggx( "mat_c", "0.4" );
	body += Obj( "o1", "mat_a", -3 );
	body += Obj( "o2", "mat_b", -2 );
	body += Obj( "o3", "mat_c", -1 );
	// The fourth material: alphax bound to an UNEQUAL per-channel triple.
	// alphay is left unspelled (descriptor default, itself a Constant) so
	// the ONLY disqualifying slot is the Opaque one.
	body += "scalar_painter\n{\n\tname sp_triple\n\tvalues 0.2 0.3 0.4\n}\n\n";
	body += "ggx_material\n{\n\tname mat_bad\n\trd pnt_dark\n\trs pnt_spec\n"
	        "\talphax sp_triple\n\tior 1.5\n\textinction 0.0\n}\n\n";
	body += Obj( "obad", "mat_bad", 5 );

	// (1) Named explicitly: refused via the SAME "not a readable constant"
	//     message a wrong-kind material gets, not "no chunk named" (it DOES
	//     exist) and not silently treated as qualifying.
	ExpectRefusal( "opaque-triple", body, "exists but its microsurface is not a readable constant", "mat_bad" );

	// (2) THE CENSUS: condition D still FIRES on this document (three real
	//     qualifying materials, mat_bad excluded) -- if the Opaque slot had
	//     been miscounted as Varying, `varyingMicrosurfaceBindings` would be
	//     nonzero and the note's disarm clause would wrongly silence it.
	const std::vector<Agent::AgentDiagnostic> diags = Agent::AgentSession::ValidateText( body );
	const Agent::AgentDiagnostic* d = hasCode( diags, kCode );
	Check( d != nullptr,
	       "G4a MONEY: condition D still FIRES with the opaque-triple material present -- an Opaque "
	       "slot does not disarm the note the way a genuinely Varying one would" );

	// (3) THE BARE CALL: qualifyingMaterials is 3, never 4 -- mat_bad is
	//     excluded from the qualifying set, not silently included as a
	//     fourth candidate.
	{
		const std::string tmp = TempPath( "varymat_opaque.RISEscene" );
		Job* pJob = LoadScene( body, tmp );
		Check( pJob != nullptr, "D6: opaque-triple fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const Agent::AgentSession::AgentVaryMaterialResult r = sess->VaryMaterial();
			Check( r.ok && r.applied, std::string( "D6: the bare call applied -- " ) + r.message );
			Check( r.qualifyingMaterials == 3,
			       "D6 MONEY: qualifyingMaterials is 3 -- `mat_bad` (Opaque alphax) never counts as a "
			       "fourth qualifying material" );
			Check( r.material != "mat_bad", "D6: the bare call never picks the disqualified material" );
			sess.reset();
			pJob->release();
			std::remove( tmp.c_str() );
		}
	}
}

//! After vary_material runs, the SAME condition-D census re-run on the
//! mutated document must no longer fire: the chosen material's roughness
//! slot(s) are now bound to the minted (Varying) scalar_painter, which both
//! drops the constant-microsurface count below the gate AND directly trips
//! the varying-binding disarm -- either alone would already silence it.
static void TestPostVaryDisarmsCensus()
{
	std::printf( "G4b: after vary_material runs, re-running the census on the MUTATED document -- D "
	             "no longer fires\n" );

	auto hasCode = []( const std::vector<Agent::AgentDiagnostic>& d, const char* code ) -> const Agent::AgentDiagnostic* {
		for( const Agent::AgentDiagnostic& e : d ) if( e.code == code ) return &e;
		return nullptr;
	};
	const char* const kCode = "DESIGN_CONSTANT_MICROSURFACE";

	const std::string before = SceneThreeMaterials();
	Check( hasCode( Agent::AgentSession::ValidateText( before ), kCode ) != nullptr,
	       "G4b: the PRE-verb document fires condition D (three qualifying, zero varying) -- the "
	       "baseline this case's MONEY assertion needs to be meaningful" );

	const std::string tmp = TempPath( "varymat_g4b.RISEscene" );
	Job* pJob = LoadScene( before, tmp );
	Check( pJob != nullptr, "G4b: fixture derives" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const Agent::AgentSession::AgentVaryMaterialResult r = sess->VaryMaterial();
	Check( r.ok && r.applied, std::string( "G4b: vary_material applied -- " ) + r.message );
	const std::string after = sess->ReadDocument();

	Check( hasCode( Agent::AgentSession::ValidateText( after ), kCode ) == nullptr,
	       "G4b MONEY: re-running the SAME census on the document vary_material just mutated -- "
	       "condition D no longer fires.  `mat_b`'s alphax/alphay are now bound to the minted "
	       "scalar_painter (Varying), which drops the constant-microsurface count to 2 (below the "
	       "gate of 3) AND trips the varying-binding disarm directly -- the new binding really does "
	       "disarm the census it used to fire" );

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

//----------------------------------------------------------------------
// I: pbr_metallic_roughness_material -- 88 S6 (2026-08-20).  The live
// census log caught this: `roughness` resolves through the COLOUR painter
// manager (`Job::AddPBRMetallicRoughnessMaterial` calls
// `pPntManager->GetItem()` on it, then bridges the composed IPainter graph
// back to the material's scalar alpha slots via `PainterToScalarAdapter`),
// unlike every other qualifying kind's primary roughness slot, which lives
// in the SCALAR painter manager.  Before the fix, this verb always minted a
// `scalar_painter` -- a name invisible to `pPntManager` -- so
// `Job::AddPBRMetallicRoughnessMaterial` fell through to `atof()` on a
// non-numeric name and refused ("`roughness` must be a finite scalar or a
// painter name"), and the whole candidate document failed to derive.
//----------------------------------------------------------------------
static ScalarProbe ProbeColourPainterAsScalar( Job& j, const std::string& name )
{
	ScalarProbe out;
	IPainterManager* mgr = j.GetPainters();
	IPainter* p = mgr ? mgr->GetItem( name.c_str() ) : nullptr;
	if( !p ) return out;
	out.found = true;
	for( int i = 0; i < 24; ++i ) {
		RayIntersectionGeometric ri( Ray(), nullRasterizerState );
		ri.bHit = true;
		ri.ptCoord = Point2( ( i % 7 ) / 7.0, ( ( i * 3 ) % 5 ) / 5.0 );
		ri.ptIntersection = Point3( i * 0.41 - 2.0, i * 0.73 - 1.3, i * 1.17 + 0.5 );
		const double v = p->GetColor( ri )[0];
		out.lo = std::min( out.lo, v );
		out.hi = std::max( out.hi, v );
	}
	return out;
}

static void TestPbrColourPipe()
{
	std::printf( "I: pbr_metallic_roughness_material -- roughness resolves through the COLOUR "
	             "painter manager, so this verb must mint an expression_painter, never a "
	             "scalar_painter\n" );

	std::string body = Preamble();
	body += Pbr( "pbr_mat", "0.35" );
	body += Obj( "o1", "pbr_mat", 0 );
	const std::string tmp = TempPath( "varymat_i.RISEscene" );
	Job* pJob = LoadScene( body, tmp );
	Check( pJob != nullptr, "I: pbr fixture derives" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const std::string docBefore = sess->ReadDocument();

	const Agent::AgentSession::AgentVaryMaterialResult r = sess->VaryMaterial();
	Check( r.ok && r.applied,
	       std::string( "I MONEY: the no-argument call APPLIED on a pbr-only scene (the live census "
	       "log's derive-refusal is fixed) -- " ) + r.message );
	Check( r.status == "applied", "I: status is \"applied\"" );
	Check( r.material == "pbr_mat", "I: it took the sole qualifying pbr material" );
	Check( r.materialKind == "pbr_metallic_roughness_material", "I: kind reported correctly" );
	Check( r.reboundSlots.size() == 1 && r.reboundSlots[0] == "roughness",
	       "I: only the primary `roughness` slot was rebound (metallic is left alone)" );
	Check( !r.painterChunk.empty(), "I: it names the painter chunk it minted" );

	const std::string docAfter = sess->ReadDocument();
	Check( docBefore != docAfter, "I: the document really changed" );
	Check( docAfter.find( "expression_painter" ) != std::string::npos,
	       "I MONEY: an expression_painter chunk landed -- NOT a scalar_painter, which pbr's "
	       "roughness slot cannot see" );
	Check( docAfter.find( "expr\t\t\tmix(" ) != std::string::npos,
	       "I: ...using the `expr` field (expression_painter's final-value keyword, distinct from "
	       "scalar_painter's `expression`) -- so it actually parses" );
	Check( docAfter.find( "scalar_painter" ) == std::string::npos,
	       "I: no scalar_painter chunk landed anywhere in this pbr-only fixture" );
	Check( docAfter.find( "roughness " + r.painterChunk ) != std::string::npos,
	       "I MONEY: the DOCUMENT's roughness slot names the minted chunk, read back out of the "
	       "text rather than assumed from the result struct" );
	// The human-editability contract still holds on the colour-pipe form.
	Check( docAfter.find( "min " ) != std::string::npos && docAfter.find( "max " ) != std::string::npos &&
	       docAfter.find( "step " ) != std::string::npos && docAfter.find( "label " ) != std::string::npos,
	       "I: every emitted `param` still carries min/max/step/label metadata" );

	// THE ROOT-CAUSE REGRESSION, through the REAL parser path (not just CST
	// text manipulation): before the fix, AddPBRMetallicRoughnessMaterial
	// rejected the rebound `roughness` before the painter-existence check
	// ever ran, and the whole candidate document failed to derive.
	Check( pJob->GetScene() != nullptr,
	       "I MONEY: the rewritten document DERIVES through the real parser path -- "
	       "Job::AddPBRMetallicRoughnessMaterial actually resolved the minted chunk" );

	// The minted chunk lives in the COLOUR painter manager (GetPainters()),
	// not the scalar one, and is genuinely spatially varying there, inside
	// the advertised band -- the same three properties case A pins for the
	// native scalar_painter form.
	{
		const ScalarProbe p = ProbeColourPainterAsScalar( *pJob, r.painterChunk );
		Check( p.found, "I: the minted painter resolved in the live COLOUR painter manager" );
		bool okLo = false, okHi = false;
		const double lo = ParamValueInChunk( docAfter, r.painterChunk, "rough_lo", okLo );
		const double hi = ParamValueInChunk( docAfter, r.painterChunk, "rough_hi", okHi );
		Check( okLo && okHi, "I: both band params are readable out of the emitted chunk" );
		std::printf( "    I: band [%g, %g], observed [%g, %g], spread %g\n",
		             lo, hi, p.lo, p.hi, p.Spread() );
		Check( p.Spread() > 0.01,
		       "I MONEY: the minted painter is GENUINELY spatially varying (spread " +
		       std::to_string( p.Spread() ) + " over 24 distinct world points, evaluated through "
		       "IPainter::GetColor -- the SAME accessor PainterToScalarAdapter reads, so no JH "
		       "spectral uplift reaches it: that adapter calls ONLY GetColor, never GetColorNM)" );
		Check( p.lo >= lo - 1e-9 && p.hi <= hi + 1e-9,
		       "I MONEY: every sample lands INSIDE the band the emitted params advertise" );
		Check( lo > 0.0 && hi > lo,
		       "I: the band is well-formed (positive, non-inverted) around the authored 0.35" );
	}

	// It renders, with the roughness genuinely varying across the sphere.
	{
		Agent::AgentRenderParams rp;
		rp.width = 32; rp.height = 32; rp.samples = 4;
		const Agent::AgentRenderResult rr = sess->Render( rp );
		Check( rr.ok, "I: the rewritten pbr scene renders" );
		Check( rr.meanR + rr.meanG + rr.meanB > 0.0, "I: ...and is non-black" );
	}

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

//! I2: DETERMINISM for the pbr colour-pipe form specifically -- the same
//! byte-identical guarantee case C pins for the scalar_painter form, so the
//! new emission branch is not exempt from it.
static void TestPbrDeterminism()
{
	std::printf( "I2: pbr colour-pipe determinism -- two runs on the same document are "
	             "BYTE-IDENTICAL\n" );

	auto run = [&]( const char* tag ) -> std::string {
		const std::string tmp = TempPath( ( std::string( "varymat_i2_" ) + tag + ".RISEscene" ).c_str() );
		std::string body = Preamble();
		body += Pbr( "pbr_mat", "0.35" );
		body += Obj( "o1", "pbr_mat", 0 );
		Job* pJob = LoadScene( body, tmp );
		if( !pJob ) return std::string();
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
		const Agent::AgentSession::AgentVaryMaterialResult r = sess->VaryMaterial();
		Check( r.applied, std::string( "I2(" ) + tag + "): applied" );
		const std::string doc = sess->ReadDocument();
		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
		return doc;
	};

	const std::string one = run( "one" );
	std::this_thread::sleep_for( std::chrono::milliseconds( 25 ) );
	const std::string two = run( "two" );

	Check( !one.empty() && !two.empty(), "I2: both runs produced a document" );
	Check( one == two,
	       "I2 MONEY: two runs on the SAME input document produce BYTE-IDENTICAL output on the "
	       "colour-pipe emission branch too" );
}

static void TestWireSurface()
{
	std::printf( "H: wire surface -- RPC dispatch, chat-codec table, MCP advertised AND routable\n" );

	{
		const std::string defs = Agent::ChatToolDefsFingerprint();
		Check( defs.find( "vary_material" ) != std::string::npos,
		       "H: the verb is declared in the shared kToolDefs table (so every provider codec "
		       "carries it -- one table, four formatters)" );
	}

	const std::string tmp = TempPath( "varymat_rpc.RISEscene" );
	Job* pJob = LoadScene( SceneThreeMaterials(), tmp );
	Check( pJob != nullptr, "H: fixture derives" );
	if( !pJob ) return;

	// MCP: ADVERTISED and ROUTABLE.  The 1ed4e7c3 lesson is that the adapter
	// keeps two independent lists (BuildToolsList and IsKnownToolName) and a
	// verb can land in one but not the other -- advertised, and answering
	// -32601 to every call.  AgentMcpAdapterTest's structural sweep covers
	// every tool generically; this pins THIS verb by name so a reader
	// grepping vary_material finds a test rather than only a loop.
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
			if( tools.at( i ).get( "name" ).asString() == "vary_material" ) advertised = true;
		Check( advertised, "H MONEY: tools/list ADVERTISES vary_material" );

		Agent::JsonValue callEnv; std::string cerr;
		Check( Agent::JsonParse( mcp.HandleLine(
		           "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"tools/call\","
		           "\"params\":{\"name\":\"vary_material\",\"arguments\":{}}}" ),
		       callEnv, cerr ), "H: tools/call parses" );
		const bool disowned = callEnv.has( "error" ) &&
		                      callEnv.get( "error" ).get( "code" ).asNumber( 0 ) == -32601.0;
		Check( !disowned,
		       "H MONEY: tools/call ROUTES vary_material (not -32601) -- the two-list-drift bug "
		       "1ed4e7c3 fixed for build_element/place_element cannot recur for this verb" );
	}

	// JSON-RPC dispatch, on a FRESH job (the MCP block above owned its own
	// session and may have mutated the document).
	{
		const std::string tmp2 = TempPath( "varymat_rpc2.RISEscene" );
		Job* pJob2 = LoadScene( SceneThreeMaterials(), tmp2 );
		Check( pJob2 != nullptr, "H: rpc fixture derives" );
		if( pJob2 ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob2 );
			Agent::AgentRpcDispatcher disp( std::move( sess ) );
			const std::string resp = disp.HandleLine(
				"{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"vary_material\",\"params\":{}}" );
			Agent::JsonValue env; std::string perr;
			Check( Agent::JsonParse( resp, env, perr ) && env.isObject(), "H: the response parses" );
			const Agent::JsonValue& result = env.get( "result" );
			Check( result.get( "applied" ).asBool(), "H: the RPC form applied the rewrite" );
			Check( result.get( "material" ).asString() == "mat_b", "H: ...and echoes the material" );
			Check( result.get( "slots" ).isArray() && result.get( "slots" ).size() == 2,
			       "H: ...and the slots it rebound" );
			Check( !result.get( "painter" ).asString().empty(), "H: ...and the painter it minted" );
			pJob2->release();
			std::remove( tmp2.c_str() );
		}
	}

	pJob->release();
	std::remove( tmp.c_str() );
}

int main()
{
	std::printf( "AgentVaryMaterialTest -- 88 S5: vary_material\n" );
	TestQualifySelectRewrite();
	TestBandAboveOne();
	TestSelection();
	TestDeterminism();
	TestRefusals();
	TestUndo();
	TestAutonomyAndAuthority();
	TestNote();
	TestOpaqueTripleDisqualifies();
	TestPostVaryDisarmsCensus();
	TestPbrColourPipe();
	TestPbrDeterminism();
	TestWireSurface();
	std::printf( "\n%d passed, %d failed\n", g_pass, g_fail );
	return g_fail ? 1 : 0;
}
