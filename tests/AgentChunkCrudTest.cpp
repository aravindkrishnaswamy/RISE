//////////////////////////////////////////////////////////////////////
//
//  AgentChunkCrudTest.cpp - Model-B F5 slice S2: insert_chunk /
//    remove_chunk -- the agent can BUILD scenes (chunk-level CRUD), not
//    just edit parameters.
//
//  Coverage:
//    H1  Headless insert: a new painter + material + geometry + object +
//        light land in the retained Document AND the derived live scene
//        (managers grew), and the scene still renders.
//    H2  Insert rejections: empty / stray-header / multi-chunk / bare
//        word / unknown chunk type (dry-run diagnostic surfaces) /
//        duplicate (kind,name).  Head byte-identical across ALL of them.
//    H3  Remove: an existing light goes away (Document + managers);
//        removing a still-REFERENCED material is refused with the
//        dry-run diagnostic; unknown target and ambiguous bare name are
//        refused with specific messages; kind narrowing resolves the
//        ambiguous case.
//    H4  Conflict gate + revision semantics: a stale baseHeadVersion ->
//        status="conflict", head byte-identical; the SAME edit without a
//        base APPLIES (the red-prove that the gate is what rejected it);
//        revision bumps on success only.
//    T1  TRIVIA-PRESERVING erase byte-contracts (Cst::DocEraseChunkTidy,
//        the mechanism Job::ApplyCstRemoveChunk routes through):
//        chunk between two comment blocks; first chunk; last chunk with
//        AND without a trailing newline; two adjacent removals.  Each
//        case asserts the result equals the byte-exact concatenation of
//        the UNTOUCHED items (only the chunk + at most its OWN
//        pure-whitespace separator may go).
//    T2  RED-PROVE: the OLD clone-undo-only Job::ApplyCstRemoveCameraChunk
//        CORRUPTS a file-authored middle chunk's neighbourhood (glues the
//        previous chunk's `}` onto the next keyword); the NEW erase on
//        the identical document does not.
//    T3  Insert -> remove symmetric case through the REAL Job verbs:
//        insert appends exactly [\n][chunk][\n]; remove drops exactly
//        the chunk + its own trailing separator.
//    L1  LIVE-controller path (mirrors AgentLiveCommitTest's harness):
//        an insert during a running render cancel-and-parks, applies,
//        flips HasUnsavedChanges, and kicks a fresh viewport pass; the
//        mTxnOpen refusal is retriable and non-mutating; remove through
//        the controller works and a stale base conflicts.
//    L2  The FULL LIVE DISPATCHER path: raw insert_chunk / remove_chunk
//        JSON-RPC lines through AgentRpcDispatcher::HandleLine against
//        the attached controller mutate the real managers.
//    G5  Round-3 red-prove: the taught camera-SWAP recipe is TRUE --
//        the sole unnamed camera removes via kind="camera" (positional
//        fallback), the camera-less document derives, and the inserted
//        thinlens replacement renders.  (G3 additionally asserts the
//        render result's `integrator` field tracks a rasterizer insert;
//        H2 asserts the reserved-name `none` refusal message.)
//    G9  gltf_import hardening: distinct-name_prefix multi-import still
//        applies (sponza_new_ivy idiom); a REPEATED prefix is refused at
//        derive time with a diagnostic naming it; a non-prefix entity-name
//        collision (hand-authored chunk vs. a generated name) is also
//        refused; read_schema advertises unnamedRepeatable:true.
//    U1  Unresolved-reference WARNING on insert_chunk (non-blocking): the
//        motivating real-world case (a reference names a chunk one
//        character off from the one actually defined) still APPLIES and
//        surfaces the near-miss suggestion -- via directlighting_shaderop's
//        `bsdf` param, the reachable vehicle for this on today's tree (see
//        the VEHICLE NOTE above TestUnresolvedReferenceWarning: an ordinary
//        material/object reference hard-fails the WHOLE insert instead,
//        by design); a forward reference (shaderop before its material)
//        warns then resolves cleanly once the material lands; the
//        `material none` idiom and inline numerics are NOT false-flagged;
//        a clean insert omits the wire key entirely; a PRE-EXISTING
//        dangling reference elsewhere never leaks into an unrelated
//        insert's report.
//    R1  Actionable REJECTED insert_chunk diagnostics (the same `issues`
//        shape as U1, now ALSO populated on a rejection): three REAL
//        insert_chunk failures reproduced verbatim (a near-miss dangling
//        reference / a numeric literal in a Painter reference slot / an
//        undeclared parameter name with the real one a near-miss typo
//        away) each get a specific {param,value,reason,suggestions} issue
//        plus an actionable sentence appended to `message`; a genuinely
//        unrelated `material none` idiom and numeric Double slot in the
//        SAME rejected chunk are not false-flagged; and a rejection the
//        descriptor-only analyser cannot explain (a semantic cross-param
//        constraint) honestly returns no issues rather than implying it
//        exonerated the chunk.
//    R2  The propose_patch sibling of R1: an unknown target (near-miss
//        suggestion), an undeclared param (full valid-parameter list), a
//        dangling reference retarget (near-miss suggestion), a numeric
//        literal in a reference slot, and a non-numeric value in a Double
//        slot each get a specific issue plus an ACTIONABLE clause; a
//        genuinely clean patch carries no `issues` key at all.
//    R3  The remove_chunk sibling of R1/R2: removing a still-referenced
//        material NAMES the blocking referrer (via the reference graph's
//        reverse adjacency) instead of the engine's own hedged "likely
//        still REFERENCED... or the document no longer derives in order"
//        message; HONESTY red-prove -- a remove refused because a
//        DYNAMIC reference (a timeline `element`, outside any declared
//        Reference param) still targets the chunk emits NO invented
//        issue, since the static reference graph cannot see it.
//
//  Self-contained: no RISE_MEDIA_PATH, inline native-v7 scenes, OIDN off.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <cmath>

#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/Interfaces/IMaterialManager.h"
#include "../src/Library/Interfaces/ILightManager.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/ICameraManager.h"
#include "../src/Library/Interfaces/IAnimator.h"
#include "../src/Library/SceneEditor/CameraIntrospection.h"
#include "../src/Library/SceneEditor/SceneEditController.h"
#include "../src/Library/Agent/AgentSession.h"
#include "../src/Library/Agent/AgentRpc.h"
#include "../src/Library/Agent/Json.h"

using namespace RISE;
using namespace RISE::Implementation;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const std::string& w )
{
	if( c ) ++g_pass;
	else { ++g_fail; std::printf( "  FAIL: %s\n", w.c_str() ); }
}

//----------------------------------------------------------------------
// The renderable shapes fixture (the known-good lit-sphere + area-emitter
// body the other agent tests use; renders non-black at 24x24 / 8 spp).
//----------------------------------------------------------------------
static const char* const kScene =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 8\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 24\n\theight 24\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_albedo\n\tcolor 0.5 0.5 0.5\n}\n\n"
	"lambertian_material\n{\n\tname mat_diffuse\n\treflectance pnt_albedo\n}\n\n"
	"sphere_geometry\n{\n\tname sph\n\tradius 0.8\n}\n\n"
	"standard_object\n{\n\tname obj_sph\n\tgeometry sph\n\tmaterial mat_diffuse\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1.0 1.0 1.0\n}\n\n"
	"lambertian_luminaire_material\n{\n\tname mat_emit\n\texitance pnt_emit\n\tscale 30.0\n\tmaterial none\n}\n\n"
	"clippedplane_geometry\n{\n\tname quad_emit\n\tpta -0.6 0.6 3.5\n\tptb 0.6 0.6 3.5\n\tptc 0.6 -0.6 3.5\n\tptd -0.6 -0.6 3.5\n}\n\n"
	"standard_object\n{\n\tname obj_emit\n\tgeometry quad_emit\n\tmaterial mat_emit\n}\n";

//----------------------------------------------------------------------
// A derivable two-NAMED-camera fixture: camB is authored LAST, so
// "last-added wins" makes camB the ACTIVE camera and camA a non-active
// named camera -- the setup that distinguishes named camera-timeline
// targeting from the active-camera fallback.
//----------------------------------------------------------------------
static const char* const kTwoCamScene =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 8\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 24\n\theight 24\n}\n\n"
	"pinhole_camera\n{\n\tname camA\n\tlocation 0 0 5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n"
	"pinhole_camera\n{\n\tname camB\n\tlocation 0 0 9\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_albedo\n\tcolor 0.5 0.5 0.5\n}\n\n"
	"lambertian_material\n{\n\tname mat_diffuse\n\treflectance pnt_albedo\n}\n\n"
	"sphere_geometry\n{\n\tname sph\n\tradius 0.8\n}\n\n"
	"standard_object\n{\n\tname obj_sph\n\tgeometry sph\n\tmaterial mat_diffuse\n}\n";

static std::string TempPath( const char* name )
{
	const char* base = std::getenv( "TMPDIR" );
	std::string dir = base ? base : "/tmp";
	if( !dir.empty() && dir.back() != '/' ) dir += '/';
	return dir + name;
}

static Job* LoadScene( const char* text, const std::string& path )
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
// CST item helpers for the byte-exact trivia assertions: serialize one
// green node (same contract as Cst.cpp's internal Serialize) and collect
// every top-level item's bytes.
//----------------------------------------------------------------------
static void SerializeNode( const RISE::Cst::NodeRef& n, std::string& out )
{
	if( !n ) return;
	if( n->kids.empty() ) out += n->text;
	else for( const auto& k : n->kids ) SerializeNode( k, out );
}

static std::vector<std::string> ItemBytes( const RISE::Cst::Document& doc )
{
	std::vector<std::string> out;
	const int n = RISE::Cst::DocItemCount( doc );
	for( int i = 0; i < n; ++i ) {
		const RISE::Cst::NodeRef it =
			RISE::Cst::DocResolveNodeId( doc, RISE::Cst::DocNodeIdAt( doc, i ) );
		std::string s;
		SerializeNode( it, s );
		out.push_back( s );
	}
	return out;
}

// The top-level index of the chunk whose "keyword/name" path is `namePath`
// (-1 if absent/ambiguous).
static int ChunkIndexByPath( const RISE::Cst::Document& doc, const std::string& namePath )
{
	const RISE::Cst::NodeId id = RISE::Cst::DocFindByName( doc, namePath );
	if( !id ) return -1;
	return RISE::Cst::DocIndexOfNodeId( doc, id, nullptr );
}

// THE byte-exact trivia contract for DocEraseChunkTidy: the result must be
// the concatenation of every item EXCEPT the erased chunk, with AT MOST the
// single item that immediately FOLLOWED the chunk also gone -- and that item
// may only go if it was PURE WHITESPACE (a comment or any other content must
// survive byte-exact).  Returns which form matched (0 = neither -> FAIL,
// 1 = all neighbours kept, 2 = the chunk's own whitespace separator
// collapsed too).
static int MatchEraseContract( const std::vector<std::string>& items, int chunkIdx,
                               const std::string& after )
{
	std::string keepAll, keepMinusSep;
	for( int i = 0; i < (int)items.size(); ++i ) {
		if( i == chunkIdx ) continue;
		keepAll += items[i];
		if( i == chunkIdx + 1 ) continue;
		keepMinusSep += items[i];
	}
	if( after == keepAll ) return 1;
	if( after == keepMinusSep ) {
		// The collapsed item must have been pure whitespace.
		if( chunkIdx + 1 < (int)items.size() &&
		    items[chunkIdx + 1].find_first_not_of( " \t\r\n" ) == std::string::npos )
			return 2;
	}
	return 0;
}

//----------------------------------------------------------------------
// H1: headless insert -- build the scene up (painter -> material ->
// geometry -> object -> light), verify Document + managers + render.
//----------------------------------------------------------------------
static void TestHeadlessInsert()
{
	std::printf( "H1: headless insert (painter -> material -> geometry -> object -> light)...\n" );
	const std::string tmp = TempPath( "agentcrud_h1.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "fixture loads via the CST path" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	Check( sess != nullptr, "AgentSession wraps the Job (headless: no controller)" );

	const RISE::Cst::CstHeadVersion v0 = sess->HeadVersion();

	// (1) A painter the new material will reference.
	Agent::AgentChunkResult r1 = sess->InsertChunk(
		"uniformcolor_painter\n{\n\tname pnt_new\n\tcolor 0.1 0.8 0.2\n}" );
	Check( r1.applied, "insert painter applied" );
	Check( r1.status == "applied", "insert painter status is \"applied\"" );
	Check( r1.rawCode == 2, "an insert is ALWAYS a D2 full re-derive (rawCode 2, never 1)" );
	Check( r1.kind == "uniformcolor_painter" && r1.name == "pnt_new",
	       "insert result echoes the parsed chunk kind + name" );
	Check( r1.headVersion.revision > v0.revision, "successful insert bumped the revision" );

	// (2) A material referencing it (declare-before-use held: painter is in).
	Agent::AgentChunkResult r2 = sess->InsertChunk(
		"lambertian_material\n{\n\tname mat_new\n\treflectance pnt_new\n}" );
	Check( r2.applied, "insert material (referencing the new painter) applied" );

	// (3) Geometry + object + light.
	Agent::AgentChunkResult r3 = sess->InsertChunk(
		"sphere_geometry\n{\n\tname sph2\n\tradius 0.3\n}" );
	Check( r3.applied, "insert geometry applied" );
	Agent::AgentChunkResult r4 = sess->InsertChunk(
		"standard_object\n{\n\tname obj_new\n\tgeometry sph2\n\tmaterial mat_new\n\tposition 1.2 0 0\n}" );
	Check( r4.applied, "insert object applied" );
	Agent::AgentChunkResult r5 = sess->InsertChunk(
		"omni_light\n{\n\tname key\n\tposition 0 4 2\n\tcolor 1 1 1\n\tpower 2.0\n}" );
	Check( r5.applied, "insert omni_light applied" );
	Check( r5.kind == "omni_light" && r5.name == "key", "light insert echoes kind + name" );

	// The DERIVED live scene grew: managers resolve the new entities.
	Check( pJob->GetMaterials() && pJob->GetMaterials()->GetItem( "mat_new" ) != nullptr,
	       "derived scene has the new material (managers grew)" );
	Check( pJob->GetObjects() && pJob->GetObjects()->GetItem( "obj_new" ) != nullptr,
	       "derived scene has the new object" );
	Check( pJob->GetLights() && pJob->GetLights()->GetItem( "key" ) != nullptr,
	       "derived scene has the new light" );

	// The retained Document carries the chunks and still round-trips.
	const std::string doc = sess->ReadDocument();
	Check( doc.find( "pnt_new" ) != std::string::npos &&
	       doc.find( "mat_new" ) != std::string::npos &&
	       doc.find( "obj_new" ) != std::string::npos &&
	       doc.find( "omni_light" ) != std::string::npos,
	       "ReadDocument carries every inserted chunk" );
	{
		RISE::Cst::Document rt = RISE::Cst::ParseToCst( doc );
		Check( RISE::Cst::SerializeCst( rt ) == doc,
		       "post-insert head round-trips through the CST parser byte-identically" );
	}

	// And the grown scene RENDERS.
	Agent::AgentRenderResult rr = sess->Render();
	Check( rr.ok, "the grown scene renders" );
	Check( rr.meanR + rr.meanG + rr.meanB > 0.0, "the render is non-black" );

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

//----------------------------------------------------------------------
// H2: insert rejections -- each refusal is specific and non-mutating.
//----------------------------------------------------------------------
static void TestInsertRejections()
{
	std::printf( "H2: insert rejections (parse / multi-chunk / stray text / unknown kind / collision)...\n" );
	const std::string tmp = TempPath( "agentcrud_h2.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

	const std::string headBefore = sess->ReadDocument();
	const RISE::Cst::CstHeadVersion vBefore = sess->HeadVersion();

	// Empty text.
	Agent::AgentChunkResult rEmpty = sess->InsertChunk( "" );
	Check( !rEmpty.applied && rEmpty.status == "rejected", "empty chunkText rejected" );

	// A scene header / directive around the chunk (stray tokens).
	Agent::AgentChunkResult rHdr = sess->InsertChunk(
		"RISE ASCII SCENE 7\nsphere_geometry\n{\n\tname hx\n\tradius 1\n}" );
	Check( !rHdr.applied && rHdr.status == "rejected", "header + chunk rejected (stray text)" );
	Check( rHdr.message.find( "exactly ONE" ) != std::string::npos,
	       "stray-text rejection message teaches the one-chunk contract" );

	// Two chunks in one call.
	Agent::AgentChunkResult rTwo = sess->InsertChunk(
		"sphere_geometry\n{\n\tname a2\n\tradius 1\n}\nsphere_geometry\n{\n\tname b2\n\tradius 1\n}" );
	Check( !rTwo.applied && rTwo.status == "rejected", "two chunks in one call rejected" );
	Check( rTwo.message.find( "2 chunks" ) != std::string::npos,
	       "multi-chunk rejection message reports the count" );

	// A bare word (no chunk at all).
	Agent::AgentChunkResult rBare = sess->InsertChunk( "hello" );
	Check( !rBare.applied && rBare.status == "rejected", "bare word rejected (no chunk found)" );
	Check( rBare.message.find( "no chunk" ) != std::string::npos,
	       "no-chunk rejection message says so" );

	// An UNCLOSED chunk (review round 1 P1 -- the truncated-LLM-output shape):
	// ParseChunk tolerates EOF mid-chunk, so this parses as exactly one Chunk
	// and would even DERIVE cleanly -- but the retained head would serialize
	// without the `}` and the next save+reload would swallow every following
	// chunk into the unclosed body.  Must be refused up front.
	Agent::AgentChunkResult rOpen = sess->InsertChunk(
		"sphere_geometry\n{\n\tname unclosed\n\tradius 2\n" );   // no closing }
	Check( !rOpen.applied && rOpen.status == "rejected", "UNCLOSED chunk rejected" );
	Check( rOpen.message.find( "not closed" ) != std::string::npos ||
	       rOpen.message.find( "missing `}`" ) != std::string::npos,
	       "unclosed-chunk rejection names the missing brace" );

	// An unknown chunk type parses as ONE chunk but fails the dry-run derive:
	// the FIRST dry-run diagnostic must surface in the message.
	Agent::AgentChunkResult rUnk = sess->InsertChunk( "bogus_chunk_kind\n{\n\tname zz\n}" );
	Check( !rUnk.applied && rUnk.status == "rejected", "unknown chunk kind rejected" );
	Check( rUnk.message.find( "would not derive" ) != std::string::npos,
	       "unknown-kind rejection is the would-not-derive class" );
	Check( rUnk.message.find( "unknown chunk type" ) != std::string::npos,
	       "the dry-run diagnostic (unknown chunk type) surfaces in the message" );

	// Duplicate (kind,name): sph already exists as a sphere_geometry.
	Agent::AgentChunkResult rDup = sess->InsertChunk(
		"sphere_geometry\n{\n\tname sph\n\tradius 2\n}" );
	Check( !rDup.applied && rDup.status == "rejected", "duplicate (kind,name) rejected" );
	Check( rDup.message.find( "already exists" ) != std::string::npos,
	       "collision rejection message says the name already exists" );
	Check( rDup.kind == "sphere_geometry" && rDup.name == "sph",
	       "collision rejection still echoes the attempted kind + name" );

	// Duplicate UNNAMED singleton (review round 1 P2): a second `film` chunk
	// would be last-wins-masked on derive yet persisted by save, and the
	// bare-name-addressed remove_chunk could never delete it -- refused.
	Agent::AgentChunkResult rFilm = sess->InsertChunk( "film\n{\n\twidth 32\n\theight 32\n}" );
	Check( !rFilm.applied && rFilm.status == "rejected", "duplicate unnamed film chunk rejected" );
	Check( rFilm.message.find( "already exists" ) != std::string::npos,
	       "unnamed-singleton rejection says the chunk already exists" );
	Check( rFilm.kind == "film", "unnamed-singleton rejection echoes the kind" );

	// RESERVED name `none` (round 3 message precision): the unbind sentinel
	// the managers pre-register -- must be refused EARLY with a message that
	// names the real cause, not the generic would-not-derive / "apply failed
	// (e.g. unresolved reference)" it used to fold into.
	Agent::AgentChunkResult rNone = sess->InsertChunk(
		"sphere_geometry\n{\n\tname none\n\tradius 1\n}" );
	Check( !rNone.applied && rNone.status == "rejected", "`name none` insert rejected" );
	Check( rNone.message.find( "reserved name" ) != std::string::npos &&
	       rNone.message.find( "none" ) != std::string::npos,
	       "the `name none` rejection names the reserved-name cause" );
	Check( rNone.message.find( "already exists" ) == std::string::npos,
	       "the `name none` rejection does NOT claim a chunk collision" );

	// NON-MUTATION: the head is byte-identical and the revision unmoved
	// across ALL of the refusals above.
	Check( sess->ReadDocument() == headBefore,
	       "head byte-identical across every insert rejection" );
	Check( sess->HeadVersion() == vBefore,
	       "revision unmoved across every insert rejection" );

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

//----------------------------------------------------------------------
// H3: remove -- an existing light; a referenced material refused with the
// diagnostic; unknown + ambiguous targets; kind narrowing.
//----------------------------------------------------------------------
static void TestRemove()
{
	std::printf( "H3: remove (light / referenced material refused / unknown / ambiguous + kind narrowing)...\n" );
	const std::string tmp = TempPath( "agentcrud_h3.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

	// Seed a light to remove, plus an AMBIGUOUS bare name: a geometry and a
	// material both named "dup" (different (kind,name) paths, so both insert).
	Check( sess->InsertChunk( "omni_light\n{\n\tname key\n\tposition 0 4 2\n\tcolor 1 1 1\n\tpower 2.0\n}" ).applied,
	       "seed light inserted" );
	Check( sess->InsertChunk( "sphere_geometry\n{\n\tname dup\n\tradius 0.1\n}" ).applied,
	       "seed geometry 'dup' inserted" );
	Check( sess->InsertChunk( "lambertian_material\n{\n\tname dup\n\treflectance pnt_albedo\n}" ).applied,
	       "seed material 'dup' inserted (cross-kind bare-name clash is legal)" );
	Check( pJob->GetLights()->GetItem( "key" ) != nullptr, "seed light is live" );

	// Remove the light.
	Agent::AgentChunkResult rmLight = sess->RemoveChunk( "key" );
	Check( rmLight.applied && rmLight.status == "applied", "remove existing light applied" );
	Check( rmLight.rawCode == 2, "a remove is ALWAYS a D2 full re-derive (rawCode 2)" );
	Check( rmLight.kind == "omni_light" && rmLight.name == "key",
	       "remove result echoes the resolved chunk kind + the target name" );
	Check( pJob->GetLights()->GetItem( "key" ) == nullptr,
	       "the light is GONE from the derived managers" );
	Check( sess->ReadDocument().find( "omni_light" ) == std::string::npos,
	       "the light chunk is GONE from the Document" );

	// Removing a still-REFERENCED material is refused with the dry-run
	// diagnostic and mutates nothing (mat_diffuse is bound by obj_sph).
	const std::string headBefore = sess->ReadDocument();
	const RISE::Cst::CstHeadVersion vBefore = sess->HeadVersion();
	Agent::AgentChunkResult rmRef = sess->RemoveChunk( "mat_diffuse" );
	Check( !rmRef.applied && rmRef.status == "rejected",
	       "removing a still-referenced material is REFUSED" );
	Check( rmRef.message.find( "would not derive" ) != std::string::npos &&
	       rmRef.message.find( "REFERENCED" ) != std::string::npos,
	       "the refusal explains the likely-still-referenced cause" );
	// The dry-run diagnostic DETAIL must ride along (round-2 P3: match a
	// diagnostic-specific substring, not the tautological ": " separator --
	// the fold's own prefix contains one).  The consumer's apply failure
	// names the failing chunk keyword ("standard_object: apply failed
	// (e.g. unresolved reference); see log").
	Check( rmRef.message.find( "apply failed" ) != std::string::npos ||
	       rmRef.message.find( "unresolved reference" ) != std::string::npos,
	       "the refusal carries the dry-run diagnostic detail (apply-failed / unresolved-reference)" );
	Check( sess->ReadDocument() == headBefore && sess->HeadVersion() == vBefore,
	       "refused remove left the head byte-identical" );
	Check( pJob->GetMaterials()->GetItem( "mat_diffuse" ) != nullptr,
	       "the referenced material is still live" );

	// Unknown target.
	Agent::AgentChunkResult rmUnk = sess->RemoveChunk( "does_not_exist" );
	Check( !rmUnk.applied && rmUnk.status == "rejected", "unknown target rejected" );
	Check( rmUnk.message.find( "no chunk named" ) != std::string::npos,
	       "unknown-target message names the miss" );

	// Ambiguous bare name -> rejected with the disambiguation hint; kind
	// narrowing (the SAME suffix rules as propose_patch) then resolves it.
	Agent::AgentChunkResult rmAmb = sess->RemoveChunk( "dup" );
	Check( !rmAmb.applied && rmAmb.status == "rejected", "ambiguous bare name rejected" );
	Check( rmAmb.message.find( "ambiguous" ) != std::string::npos &&
	       rmAmb.message.find( "kind" ) != std::string::npos,
	       "ambiguity message suggests passing kind" );
	Agent::AgentChunkResult rmNarrow = sess->RemoveChunk( "dup", "material" );
	Check( rmNarrow.applied, "kind narrowing (suffix \"material\") resolves the clash" );
	Check( rmNarrow.kind == "lambertian_material", "the MATERIAL 'dup' was the one removed" );
	Check( sess->ReadDocument().find( "sphere_geometry\n{\n\tname dup" ) != std::string::npos,
	       "the geometry 'dup' survives" );

	// KIND VERIFICATION (review round 1 P2): a UNIQUELY-named target whose
	// kind does NOT match the requested one must be REFUSED, not removed --
	// DocFindByNameAnyRole's single-match path ignores the suffix, so without
	// the post-resolve role check this would delete the geometry.
	const std::string headKind = sess->ReadDocument();
	Agent::AgentChunkResult rmWrongKind = sess->RemoveChunk( "sph", "material" );
	Check( !rmWrongKind.applied && rmWrongKind.status == "rejected",
	       "remove of a uniquely-named target under the WRONG kind is refused" );
	Check( rmWrongKind.message.find( "sphere_geometry" ) != std::string::npos,
	       "the wrong-kind refusal names the actual kind" );
	Check( sess->ReadDocument() == headKind, "wrong-kind refusal mutated nothing" );

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

//----------------------------------------------------------------------
// H4: conflict gate + revision semantics (with the red-prove).
//----------------------------------------------------------------------
static void TestConflictGate()
{
	std::printf( "H4: conflict gate (stale baseHeadVersion) + revision semantics...\n" );
	const std::string tmp = TempPath( "agentcrud_h4.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

	const char* lightChunk = "omni_light\n{\n\tname key\n\tposition 0 4 2\n\tcolor 1 1 1\n\tpower 2.0\n}";

	// Build a STALE base (the head as if it had moved on).
	RISE::Cst::CstHeadVersion stale = sess->HeadVersion();
	stale.revision += 100;

	const std::string headBefore = sess->ReadDocument();
	const RISE::Cst::CstHeadVersion vBefore = sess->HeadVersion();
	Agent::AgentChunkResult rC = sess->InsertChunk( lightChunk, &stale );
	Check( !rC.applied && rC.status == "conflict", "stale-base insert -> status=\"conflict\"" );
	// Round-3 message precision: the head did NOT "move" here (the caller's
	// base is a fabricated FUTURE revision) -- the message must claim only
	// the true fact: the base does not match the current head.
	Check( rC.message.find( "does not match the current head" ) != std::string::npos,
	       "conflict message reports the mismatch (not a false 'head moved' claim)" );
	Check( sess->ReadDocument() == headBefore, "conflict left the head byte-identical" );
	Check( sess->HeadVersion() == vBefore, "conflict did not bump the revision" );
	Check( rC.headVersion == vBefore, "conflict result carries the CURRENT head" );

	// RED-PROVE the gate: the IDENTICAL insert WITHOUT a base APPLIES --
	// so the stale rejection above really was the precondition's doing,
	// not some other refusal.
	Agent::AgentChunkResult rOk = sess->InsertChunk( lightChunk );
	Check( rOk.applied, "the identical insert WITHOUT a base applies (red-prove: the gate rejected it)" );
	Check( rOk.headVersion.revision == vBefore.revision + 1,
	       "success bumped the revision by exactly one" );

	// A FRESH base gates cleanly too (the happy path).
	RISE::Cst::CstHeadVersion fresh = sess->HeadVersion();
	Agent::AgentChunkResult rm = sess->RemoveChunk( "key", "", &fresh );
	Check( rm.applied, "remove with the FRESH base applies" );
	Check( rm.headVersion.revision == fresh.revision + 1, "remove bumped the revision" );

	// And a stale REMOVE conflicts without mutating.
	Check( sess->InsertChunk( lightChunk ).applied, "re-seed the light" );
	RISE::Cst::CstHeadVersion stale2 = sess->HeadVersion();
	stale2.revision += 7;
	const std::string head2 = sess->ReadDocument();
	Agent::AgentChunkResult rm2 = sess->RemoveChunk( "key", "", &stale2 );
	Check( !rm2.applied && rm2.status == "conflict", "stale-base remove -> conflict" );
	Check( sess->ReadDocument() == head2, "stale remove mutated nothing" );

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

//----------------------------------------------------------------------
// T1: the trivia-preserving erase byte-contracts (Cst level -- the exact
// mechanism Job::ApplyCstRemoveChunk routes through).
//----------------------------------------------------------------------
static void TestTriviaContracts()
{
	std::printf( "T1: trivia-preserving erase byte-contracts (DocEraseChunkTidy)...\n" );

	// (a) A chunk between two COMMENT blocks: neither comment may be eaten.
	{
		const std::string text =
			"RISE ASCII SCENE 7\n"
			"# comment A -- describes s\n"
			"sphere_geometry\n{\n\tname s\n\tradius 1\n}\n"
			"# comment B -- describes b\n"
			"sphere_geometry\n{\n\tname b\n\tradius 2\n}\n";
		RISE::Cst::Document doc = RISE::Cst::ParseToCst( text );
		Check( RISE::Cst::SerializeCst( doc ) == text, "(a) fixture round-trips" );
		const std::vector<std::string> items = ItemBytes( doc );
		const int idx = ChunkIndexByPath( doc, "sphere_geometry/s" );
		Check( idx >= 0, "(a) chunk resolves" );
		RISE::Cst::Document d1 = RISE::Cst::DocEraseChunkTidy( doc, idx );
		const std::string after = RISE::Cst::SerializeCst( d1 );
		const int form = MatchEraseContract( items, idx, after );
		Check( form != 0, "(a) result is the byte-exact remainder (chunk + at most its own whitespace separator gone)" );
		Check( after.find( "# comment A" ) != std::string::npos &&
		       after.find( "# comment B" ) != std::string::npos,
		       "(a) BOTH comment blocks survive byte-exact" );
		Check( after.find( "name s" ) == std::string::npos, "(a) the chunk is gone" );
		Check( RISE::Cst::SerializeCst( RISE::Cst::ParseToCst( after ) ) == after,
		       "(a) the remainder round-trips (well-formed)" );
	}

	// (b) FIRST chunk (the document-start case: index 0, no header).
	{
		const std::string text =
			"sphere_geometry\n{\n\tname s\n\tradius 1\n}\n"
			"\n"
			"sphere_geometry\n{\n\tname b\n\tradius 2\n}\n";
		RISE::Cst::Document doc = RISE::Cst::ParseToCst( text );
		const std::vector<std::string> items = ItemBytes( doc );
		const int idx = ChunkIndexByPath( doc, "sphere_geometry/s" );
		Check( idx == 0, "(b) the first chunk is item 0" );
		RISE::Cst::Document d1 = RISE::Cst::DocEraseChunkTidy( doc, idx );
		const std::string after = RISE::Cst::SerializeCst( d1 );
		Check( MatchEraseContract( items, idx, after ) != 0,
		       "(b) first-chunk removal is the byte-exact remainder" );
		Check( after.find( "name b" ) != std::string::npos, "(b) the sibling chunk survives" );
		Check( after.rfind( "sphere_geometry", 0 ) == 0 || after[0] == '\n' || after[0] == '#',
		       "(b) no leading garbage at document start" );
		Check( RISE::Cst::SerializeCst( RISE::Cst::ParseToCst( after ) ) == after,
		       "(b) the remainder round-trips" );
	}

	// (c) LAST chunk WITH a trailing newline.
	{
		const std::string text =
			"RISE ASCII SCENE 7\n"
			"sphere_geometry\n{\n\tname a\n\tradius 1\n}\n"
			"sphere_geometry\n{\n\tname z\n\tradius 2\n}\n";
		RISE::Cst::Document doc = RISE::Cst::ParseToCst( text );
		const std::vector<std::string> items = ItemBytes( doc );
		const int idx = ChunkIndexByPath( doc, "sphere_geometry/z" );
		Check( idx >= 0, "(c) last chunk resolves" );
		RISE::Cst::Document d1 = RISE::Cst::DocEraseChunkTidy( doc, idx );
		const std::string after = RISE::Cst::SerializeCst( d1 );
		Check( MatchEraseContract( items, idx, after ) != 0,
		       "(c) last-chunk (trailing-newline) removal is the byte-exact remainder" );
		Check( !after.empty() && after.back() == '\n',
		       "(c) the file keeps a final newline" );
		Check( after.find( "name a" ) != std::string::npos, "(c) the preceding chunk survives" );
	}

	// (d) LAST chunk WITHOUT a trailing newline (the chunk IS the last item).
	{
		const std::string text =
			"RISE ASCII SCENE 7\n"
			"sphere_geometry\n{\n\tname a\n\tradius 1\n}\n"
			"sphere_geometry\n{\n\tname z\n\tradius 2\n}";   // no final \n
		RISE::Cst::Document doc = RISE::Cst::ParseToCst( text );
		const std::vector<std::string> items = ItemBytes( doc );
		const int idx = ChunkIndexByPath( doc, "sphere_geometry/z" );
		Check( idx == (int)items.size() - 1, "(d) the chunk is the LAST item (no trailing trivia)" );
		RISE::Cst::Document d1 = RISE::Cst::DocEraseChunkTidy( doc, idx );
		const std::string after = RISE::Cst::SerializeCst( d1 );
		Check( MatchEraseContract( items, idx, after ) != 0,
		       "(d) last-chunk (no-trailing-newline) removal is the byte-exact remainder" );
		Check( after.find( "name a" ) != std::string::npos, "(d) the preceding chunk survives" );
	}

	// (e) TWO ADJACENT removals: erase s then (the shifted) b; every other
	// byte survives.
	{
		const std::string text =
			"RISE ASCII SCENE 7\n"
			"# keep me 1\n"
			"sphere_geometry\n{\n\tname s\n\tradius 1\n}\n"
			"sphere_geometry\n{\n\tname b\n\tradius 2\n}\n"
			"# keep me 2\n"
			"sphere_geometry\n{\n\tname c\n\tradius 3\n}\n";
		RISE::Cst::Document doc = RISE::Cst::ParseToCst( text );
		const int idxS = ChunkIndexByPath( doc, "sphere_geometry/s" );
		Check( idxS >= 0, "(e) first target resolves" );
		std::vector<std::string> items1 = ItemBytes( doc );
		RISE::Cst::Document d1 = RISE::Cst::DocEraseChunkTidy( doc, idxS );
		const std::string mid = RISE::Cst::SerializeCst( d1 );
		Check( MatchEraseContract( items1, idxS, mid ) != 0,
		       "(e) first adjacent removal is the byte-exact remainder" );
		const int idxB = ChunkIndexByPath( d1, "sphere_geometry/b" );
		Check( idxB >= 0, "(e) second target resolves after the first erase" );
		std::vector<std::string> items2 = ItemBytes( d1 );
		RISE::Cst::Document d2 = RISE::Cst::DocEraseChunkTidy( d1, idxB );
		const std::string after = RISE::Cst::SerializeCst( d2 );
		Check( MatchEraseContract( items2, idxB, after ) != 0,
		       "(e) second adjacent removal is the byte-exact remainder" );
		Check( after.find( "# keep me 1" ) != std::string::npos &&
		       after.find( "# keep me 2" ) != std::string::npos &&
		       after.find( "name c" ) != std::string::npos,
		       "(e) both comments + the surviving chunk are byte-intact" );
		Check( after.find( "name s" ) == std::string::npos &&
		       after.find( "name b" ) == std::string::npos,
		       "(e) both removed chunks are gone" );
		Check( RISE::Cst::SerializeCst( RISE::Cst::ParseToCst( after ) ) == after,
		       "(e) the remainder round-trips" );
	}
}

//----------------------------------------------------------------------
// T2: RED-PROVE the landmine -- the OLD clone-undo-only remove CORRUPTS a
// file-authored middle chunk's neighbourhood; the NEW erase does not.
//----------------------------------------------------------------------
static void TestCloneOnlyRemoveRedProve()
{
	std::printf( "T2: red-prove -- the OLD clone-undo remove corrupts file-authored trivia...\n" );

	// A file-authored document: camB sits BETWEEN camA and a geometry chunk,
	// each chunk separated by the file's own "\n\n" trivia.
	const char* text =
		"RISE ASCII SCENE 7\n"
		"pinhole_camera\n{\n\tname camA\n\tlocation 0 0 5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 30\n}\n"
		"\n"
		"pinhole_camera\n{\n\tname camB\n\tlocation 0 0 8\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40\n}\n"
		"\n"
		"sphere_geometry\n{\n\tname s\n\tradius 1\n}\n";

	// (1) The OLD path: Job::ApplyCstRemoveCameraChunk (Document-only, so a
	// bare CST-loaded Job suffices).  Its unconditional idx-1 drop eats the
	// PREVIOUS chunk's real trailing separator AND the chunk's own trailing
	// separator -- gluing camA's `}` straight onto `sphere_geometry`.
	{
		const std::string tmp = TempPath( "agentcrud_t2_old.RISEscene" );
		Job* pJob = LoadScene( text, tmp );
		Check( pJob != nullptr, "red-prove fixture loads" );
		if( !pJob ) return;
		const int rc = pJob->ApplyCstRemoveCameraChunk( "camB" );
		Check( rc == 1, "the OLD clone-undo remove reports success on the file-authored camera" );
		const std::string corrupted = RISE::Cst::SerializeCst( *pJob->GetCstDocument() );
		Check( corrupted.find( "}sphere_geometry" ) != std::string::npos,
		       "RED-PROVE: the OLD remove GLUED `}` onto `sphere_geometry` (the documented corruption)" );
		pJob->release();
		std::remove( tmp.c_str() );
	}

	// (2) The NEW path on the IDENTICAL document: Job::ApplyCstRemoveChunk
	// (trivia-preserving DocEraseChunkTidy + the D2 re-derive).  No glue;
	// the byte-diff is exactly camB + at most its own whitespace separator.
	{
		const std::string tmp = TempPath( "agentcrud_t2_new.RISEscene" );
		Job* pJob = LoadScene( text, tmp );
		Check( pJob != nullptr, "red-prove fixture reloads" );
		if( !pJob ) return;
		const std::vector<std::string> items = ItemBytes( *pJob->GetCstDocument() );
		const int idx = ChunkIndexByPath( *pJob->GetCstDocument(), "pinhole_camera/camB" );
		Check( idx >= 0, "camB resolves" );
		char kw[64]; char diag[256];
		const int rc = pJob->ApplyCstRemoveChunk( "camB", "camera", kw, sizeof( kw ), diag, sizeof( diag ) );
		Check( rc == 2, "the NEW remove applies via the D2 full re-derive" );
		Check( std::string( kw ) == "pinhole_camera", "the NEW remove echoes the resolved keyword" );
		const std::string after = RISE::Cst::SerializeCst( *pJob->GetCstDocument() );
		Check( after.find( "}sphere_geometry" ) == std::string::npos,
		       "the NEW erase produces NO glue" );
		Check( MatchEraseContract( items, idx, after ) != 0,
		       "the NEW erase is the byte-exact remainder (neighbouring trivia intact)" );
		Check( RISE::Cst::SerializeCst( RISE::Cst::ParseToCst( after ) ) == after,
		       "the NEW erase result round-trips" );
		pJob->release();
		std::remove( tmp.c_str() );
	}
}

//----------------------------------------------------------------------
// T3: insert -> remove symmetric case through the REAL Job verbs.
//----------------------------------------------------------------------
static void TestInsertRemoveSymmetry()
{
	std::printf( "T3: insert -> remove symmetry (the [\\n][chunk][\\n] triple)...\n" );
	const std::string tmp = TempPath( "agentcrud_t3.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

	const std::string before = sess->ReadDocument();
	const char* chunkText = "omni_light\n{\n\tname key\n\tposition 0 4 2\n\tcolor 1 1 1\n\tpower 2.0\n}";

	// INSERT appends exactly [leadSep "\n"][chunk][trailSep "\n"].
	Check( sess->InsertChunk( chunkText ).applied, "insert applies" );
	const std::string inserted = sess->ReadDocument();
	Check( inserted == before + "\n" + chunkText + "\n",
	       "insert appended exactly [\\n][chunk][\\n] (anti-glue triple)" );

	// REMOVE drops exactly the chunk + its OWN trailing separator: the
	// difference vs the post-insert head is chunk+\n, and vs the ORIGINAL
	// head one residual blank line (the lead separator -- correctness over
	// minimality, documented).
	Check( sess->RemoveChunk( "key" ).applied, "remove applies" );
	const std::string after = sess->ReadDocument();
	Check( after == before + "\n",
	       "remove dropped exactly the chunk + its own separator (one residual lead \\n vs the original)" );
	Check( RISE::Cst::SerializeCst( RISE::Cst::ParseToCst( after ) ) == after,
	       "the symmetric-case result round-trips" );

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

//----------------------------------------------------------------------
// L1/L2 harness: a controller whose DoOneRenderPass simulates render work
// in cancel-checked slices (mirrors AgentLiveCommitTest).
//----------------------------------------------------------------------
class TestController : public SceneEditController
{
public:
	TestController( IJobPriv& job, unsigned int simulatedRenderMs = 20 )
	: SceneEditController( job, /*interactiveRasterizer*/0 )
	, mSimulatedRenderMs( simulatedRenderMs )
	{}

protected:
	void DoOneRenderPass() override
	{
		const unsigned int sliceMs = 2;
		const unsigned int slices  = ( mSimulatedRenderMs + sliceMs - 1 ) / sliceMs;
		for( unsigned int i = 0; i < slices; ++i )
		{
			if( IsCancelRequested() ) return;
			std::this_thread::sleep_for( std::chrono::milliseconds( sliceMs ) );
		}
	}

private:
	unsigned int mSimulatedRenderMs;
};

// The same base scene AgentLiveCommitTest uses (fast incremental edits,
// uniquely-named entities, no rasterizer chunk -- the mock render loop
// stands in for it).
static const char* kLiveScene =
	"RISE ASCII SCENE 7\n"
	"uniformcolor_painter\n{\nname white\ncolor 1 1 1\n}\n"
	"lambertian_luminaire_material\n{\nname lum\nexitance white\nscale 5.0\nmaterial none\n}\n"
	"sphere_geometry\n{\nname s\nradius 1\n}\n"
	"standard_object\n{\nname obj\ngeometry s\nmaterial lum\n}\n";

//----------------------------------------------------------------------
// L1: the LIVE controller path.
//----------------------------------------------------------------------
static void TestLiveControllerPath()
{
	std::printf( "L1: live controller path (park + apply + dirty + kick; mTxnOpen refusal)...\n" );
	const std::string tmp = TempPath( "agentcrud_l1.RISEscene" );
	Job* pJob = LoadScene( kLiveScene, tmp );
	Check( pJob != nullptr, "live fixture loads" );
	if( !pJob ) return;

	{
		// A LONG simulated pass so the insert reliably lands mid-render.
		TestController c( *pJob, /*simulatedRenderMs*/300 );
		c.Start();
		std::this_thread::sleep_for( std::chrono::milliseconds( 40 ) );

		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
		sess->AttachController( &c );
		Check( sess->HasController(), "session attached to the running controller" );

		// (1) INSERT during a running render: parked (cancel count advances),
		// applied, dirty flips, and a fresh viewport pass fires (the kick).
		Check( !c.HasUnsavedChanges(), "scene is CLEAN before the live insert" );
		const unsigned int cancelsBefore = c.ForTest_GetCancelCount();
		const unsigned int rendersBefore = c.ForTest_GetRenderCount();
		Agent::AgentChunkResult r = sess->InsertChunk(
			"omni_light\n{\nname livekey\nposition 0 4 2\ncolor 1 1 1\npower 2.0\n}" );
		Check( r.applied && r.rawCode == 2, "live insert applied via the controller (D2)" );
		Check( r.kind == "omni_light" && r.name == "livekey", "live insert echoes kind + name" );
		Check( c.ForTest_GetCancelCount() > cancelsBefore,
		       "the insert cancel-and-PARKED the in-flight pass (did not race it)" );
		Check( c.HasUnsavedChanges(), "the live insert marked the editor DIRTY (Save enables)" );
		Check( c.ForTest_WaitForRenders( rendersBefore + 1, 3000 ),
		       "the insert KICKED a fresh viewport pass" );
		Check( pJob->GetLights() && pJob->GetLights()->GetItem( "livekey" ) != nullptr,
		       "the live managers carry the inserted light" );
		Check( c.IsRunning(), "render thread alive after the live insert" );

		// (2) mTxnOpen refusal: retriable + non-mutating; retry succeeds
		// after the gesture completes.
		Check( c.BeginTransaction(), "editor transaction opens" );
		const std::string headBefore = sess->ReadDocument();
		Agent::AgentChunkResult rTxn = sess->InsertChunk(
			"omni_light\n{\nname fillkey\nposition -2 3 1\ncolor 1 1 1\npower 1.0\n}" );
		Check( !rTxn.applied && rTxn.status == "rejected", "mid-transaction insert refused" );
		Check( rTxn.retriable, "the transaction refusal is RETRIABLE (transient)" );
		Check( sess->ReadDocument() == headBefore, "the refusal mutated nothing" );
		Check( c.EndTransaction(), "editor transaction closes" );
		Agent::AgentChunkResult rRetry = sess->InsertChunk(
			"omni_light\n{\nname fillkey\nposition -2 3 1\ncolor 1 1 1\npower 1.0\n}" );
		Check( rRetry.applied, "the identical insert succeeds after the gesture completes" );

		// (3) Conflict through the live path.
		RISE::Cst::CstHeadVersion stale = sess->HeadVersion();
		stale.revision += 50;
		Agent::AgentChunkResult rC = sess->RemoveChunk( "livekey", "", &stale );
		Check( !rC.applied && rC.status == "conflict", "stale-base remove conflicts through the controller" );

		// (4) Remove through the live path.
		Agent::AgentChunkResult rRm = sess->RemoveChunk( "livekey" );
		Check( rRm.applied, "live remove applied via the controller" );
		Check( pJob->GetLights()->GetItem( "livekey" ) == nullptr,
		       "the live managers dropped the removed light" );

		c.Stop();
		Check( !c.IsRunning(), "controller stops + joins cleanly" );
	}
	pJob->release();
	std::remove( tmp.c_str() );
}

//----------------------------------------------------------------------
// L2: raw JSON-RPC insert_chunk / remove_chunk through the LIVE dispatcher
// (the exact wiring the GUI's agentHandleLine drives).
//----------------------------------------------------------------------
static bool JsonResultObj( const std::string& line, Agent::JsonValue& outResult )
{
	Agent::JsonValue root;
	std::string err;
	if( !Agent::JsonParse( line, root, err ) || !root.isObject() ) return false;
	const Agent::JsonValue* r = root.find( "result" );
	if( !r || !r->isObject() ) return false;
	outResult = *r;
	return true;
}

static void TestLiveDispatcherChunkCrud()
{
	std::printf( "L2: live dispatcher chunk CRUD (HandleLine -> controller -> managers)...\n" );
	const std::string tmp = TempPath( "agentcrud_l2.RISEscene" );
	Job* pJob = LoadScene( kLiveScene, tmp );
	Check( pJob != nullptr, "live fixture loads" );
	if( !pJob ) return;

	{
		TestController c( *pJob, /*simulatedRenderMs*/20 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
		sess->AttachController( &c );
		Agent::AgentRpcDispatcher disp( std::move( sess ) );

		// insert_chunk over the wire.
		const std::string insResp = disp.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"insert_chunk\",\"params\":"
			"{\"chunkText\":\"omni_light\\n{\\nname wirekey\\nposition 0 4 2\\ncolor 1 1 1\\npower 2.0\\n}\"}}" );
		Agent::JsonValue insResult;
		Check( JsonResultObj( insResp, insResult ), "insert_chunk returns a JSON-RPC result object" );
		const Agent::JsonValue* applied = insResult.find( "applied" );
		Check( applied && applied->isBool() && applied->asBool(), "wire insert applied" );
		const Agent::JsonValue* nm = insResult.find( "name" );
		const Agent::JsonValue* kd = insResult.find( "kind" );
		Check( nm && nm->isString() && nm->asString() == "wirekey", "wire result echoes name" );
		Check( kd && kd->isString() && kd->asString() == "omni_light", "wire result echoes kind" );
		Check( pJob->GetLights() && pJob->GetLights()->GetItem( "wirekey" ) != nullptr,
		       "the wire insert reached the live managers" );

		// remove_chunk over the wire.
		const std::string rmResp = disp.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"remove_chunk\",\"params\":"
			"{\"target\":\"wirekey\",\"kind\":\"omni_light\"}}" );
		Agent::JsonValue rmResult;
		Check( JsonResultObj( rmResp, rmResult ), "remove_chunk returns a JSON-RPC result object" );
		const Agent::JsonValue* rmApplied = rmResult.find( "applied" );
		Check( rmApplied && rmApplied->isBool() && rmApplied->asBool(), "wire remove applied" );
		Check( pJob->GetLights()->GetItem( "wirekey" ) == nullptr,
		       "the wire remove reached the live managers" );

		// Param validation: a missing chunkText is a -32602, not a crash.
		const std::string badResp = disp.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"insert_chunk\",\"params\":{}}" );
		Check( badResp.find( "-32602" ) != std::string::npos,
		       "insert_chunk without chunkText -> -32602 invalid params" );

		// Eval-harness hardening (local-model shootout, 2026-07-12): the
		// observed llama3.3 mistake was sending the chunk body under the
		// wrong key ('chunk' instead of 'chunkText') and then repeating the
		// SAME wrong key on retry after seeing "'chunkText' (string) is
		// required" -- the message named what was MISSING but not what was
		// actually SENT. AgentRpc.cpp's insert_chunk handler now also names
		// the offending key(s) so a model reading its own tool error has
		// something to diff against and can self-correct in one round.
		const std::string wrongKeyResp = disp.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"insert_chunk\",\"params\":"
			"{\"chunk\":\"omni_light { name wrongkey }\"}}" );
		Check( wrongKeyResp.find( "-32602" ) != std::string::npos,
		       "insert_chunk with wrong key 'chunk' -> -32602 invalid params" );
		Check( wrongKeyResp.find( "'chunkText'" ) != std::string::npos,
		       "insert_chunk wrong-key error still names the required field" );
		Check( wrongKeyResp.find( "'chunk'" ) != std::string::npos,
		       "insert_chunk wrong-key error names the offending key actually sent" );
		Check( pJob->GetLights() == nullptr || pJob->GetLights()->GetItem( "wrongkey" ) == nullptr,
		       "the wrong-key insert never reached the live managers" );

		c.Stop();
	}
	pJob->release();
	std::remove( tmp.c_str() );
}

//----------------------------------------------------------------------
// G1: the AGENT full-derivability gate (review round 2 P1-A root gate).
// An agent param edit that would leave the head unable to derive in
// DOCUMENT ORDER (a forward reference: consumer chunk before the
// declaration it names) must be REFUSED with the head untouched -- the
// incremental fast path validates against the LIVE managers (where the
// entity exists), so without the gate it commits a head whose bytes no
// longer reload (silent save-time data loss).
//----------------------------------------------------------------------
static void TestAgentEditDerivabilityGate()
{
	std::printf( "G1: agent full-derivability gate (forward-reference retarget refused; head reloads)...\n" );

	// (0) PERMANENT RED-PROVE of the wedge SHAPE on the UNCHECKED path
	// (Job::ApplyCstParamEdit -- the GUI-internal fast path, intentionally
	// untouched by the gate): retargeting mat_diffuse's reflectance to
	// pnt_emit -- a painter declared LATER in the document -- commits
	// incrementally (the live managers have the painter), yet the
	// committed head FAILS to reload (DeriveToJob applies in document
	// order).  This is exactly the state the agent gate exists to refuse.
	{
		const std::string tmp = TempPath( "agentcrud_g1_wedge.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "wedge fixture loads" );
		if( !pJob ) return;
		const int code = pJob->ApplyCstParamEdit( "mat_diffuse", "material", "reflectance", 0, "pnt_emit" );
		Check( code == 1, "RED-PROVE: the UNCHECKED path commits the forward reference incrementally (rawCode 1)" );
		const std::string bytes = RISE::Cst::SerializeCst( *pJob->GetCstDocument() );
		const std::string tmp2 = TempPath( "agentcrud_g1_wedge_reload.RISEscene" );
		Job* fresh = LoadScene( bytes.c_str(), tmp2 );
		Check( fresh == nullptr, "RED-PROVE: the wedged head's bytes FAIL to reload (the save-time data loss)" );
		if( fresh ) fresh->release();
		pJob->release();
		std::remove( tmp.c_str() );
		std::remove( tmp2.c_str() );
	}

	// (1) HEADLESS agent path: the IDENTICAL edit through ProposePatch is
	// REFUSED cleanly (code 0), head byte-identical, revision unmoved, and
	// the head still reloads.
	{
		const std::string tmp = TempPath( "agentcrud_g1_headless.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "headless gate fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

		const std::string headBefore = sess->ReadDocument();
		const RISE::Cst::CstHeadVersion vBefore = sess->HeadVersion();
		Agent::AgentSetPatch p;
		p.target = "mat_diffuse";
		p.kind   = "material";
		p.param  = "reflectance";
		p.value  = "pnt_emit";   // declared AFTER mat_diffuse -> forward reference
		Agent::AgentPatchResult r = sess->ProposePatch( p );
		Check( !r.applied && r.status == "rejected",
		       "the agent gate REFUSES the forward-reference retarget (headless)" );
		Check( r.rawCode == 0, "the refusal is code 0 (head + live scene untouched)" );
		Check( sess->ReadDocument() == headBefore, "the refused edit left the head byte-identical" );
		Check( sess->HeadVersion() == vBefore, "the refused edit did not bump the revision" );
		{
			const std::string tmp2 = TempPath( "agentcrud_g1_headless_reload.RISEscene" );
			Job* fresh = LoadScene( sess->ReadDocument().c_str(), tmp2 );
			Check( fresh != nullptr, "the head still RELOADS after the refusal (no wedge)" );
			if( fresh ) fresh->release();
			std::remove( tmp2.c_str() );
		}

		// The gate must NOT over-block: an ORDER-VALID retarget (pnt_albedo
		// is declared BEFORE mat_emit) still applies on the incremental
		// fast path (rawCode 1 -- the gate adds a dry-run, not a D2).
		Agent::AgentSetPatch pOk;
		pOk.target = "mat_emit";
		pOk.kind   = "material";
		pOk.param  = "exitance";
		pOk.value  = "pnt_albedo";
		Agent::AgentPatchResult rOk = sess->ProposePatch( pOk );
		Check( rOk.applied && rOk.rawCode == 1,
		       "an ORDER-VALID retarget still applies incrementally through the gate" );

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}

	// (2) LIVE-controller agent path (the second entry point): the same
	// forward-reference retarget through the attached controller is refused
	// and mutates nothing.
	{
		const std::string tmp = TempPath( "agentcrud_g1_live.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "live gate fixture loads" );
		if( !pJob ) return;
		{
			TestController c( *pJob, /*simulatedRenderMs*/20 );
			c.Start();
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			sess->AttachController( &c );

			const std::string headBefore = sess->ReadDocument();
			Agent::AgentSetPatch p;
			p.target = "mat_diffuse";
			p.kind   = "material";
			p.param  = "reflectance";
			p.value  = "pnt_emit";
			Agent::AgentPatchResult r = sess->ProposePatch( p );
			Check( !r.applied && r.status == "rejected",
			       "the agent gate REFUSES the forward-reference retarget (live controller)" );
			Check( sess->ReadDocument() == headBefore, "the live refusal mutated nothing" );

			c.Stop();
		}
		pJob->release();
		std::remove( tmp.c_str() );
	}
}

//----------------------------------------------------------------------
// G2: the RENAME RECIPE end-to-end through the dispatcher (review round 2
// P1-A teaching): insert the renamed material -> retarget the consumer ->
// remove the old material -> every step applies and the final head
// reloads cleanly.  Also asserts the declaration-class insert POSITIONING
// (the new material lands BEFORE the first object chunk).
//----------------------------------------------------------------------
static void TestRenameRecipeEndToEnd()
{
	std::printf( "G2: rename recipe end-to-end via the dispatcher (insert -> retarget -> remove -> reload)...\n" );
	const std::string tmp = TempPath( "agentcrud_g2.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "recipe fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	Agent::AgentRpcDispatcher disp( std::move( sess ) );

	// (1) Insert the renamed material.
	const std::string insResp = disp.HandleLine(
		"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"insert_chunk\",\"params\":"
		"{\"chunkText\":\"lambertian_material\\n{\\n\\tname matRenamed\\n\\treflectance pnt_albedo\\n}\"}}" );
	Agent::JsonValue insResult;
	Check( JsonResultObj( insResp, insResult ), "step 1 (insert matRenamed) returns a result" );
	const Agent::JsonValue* insApplied = insResult.find( "applied" );
	Check( insApplied && insApplied->isBool() && insApplied->asBool(), "step 1 (insert matRenamed) applied" );

	// POSITIONING: the declaration-class chunk landed BEFORE the first
	// object chunk, so the retarget below derives in document order.
	{
		const std::string doc = RISE::Cst::SerializeCst( *pJob->GetCstDocument() );
		const std::size_t posMat = doc.find( "name matRenamed" );
		const std::size_t posObj = doc.find( "standard_object" );
		Check( posMat != std::string::npos && posObj != std::string::npos && posMat < posObj,
		       "the inserted material is POSITIONED before the first object chunk" );
	}

	// (2) Retarget the consumer.
	const std::string patchResp = disp.HandleLine(
		"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"propose_patch\",\"params\":"
		"{\"target\":\"obj_sph\",\"param\":\"material\",\"value\":\"matRenamed\"}}" );
	Agent::JsonValue patchResult;
	Check( JsonResultObj( patchResp, patchResult ), "step 2 (retarget obj_sph) returns a result" );
	const Agent::JsonValue* patchApplied = patchResult.find( "applied" );
	Check( patchApplied && patchApplied->isBool() && patchApplied->asBool(),
	       "step 2 (retarget obj_sph.material -> matRenamed) applied" );

	// (3) Remove the old material.
	const std::string rmResp = disp.HandleLine(
		"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"remove_chunk\",\"params\":"
		"{\"target\":\"mat_diffuse\",\"kind\":\"material\"}}" );
	Agent::JsonValue rmResult;
	Check( JsonResultObj( rmResp, rmResult ), "step 3 (remove mat_diffuse) returns a result" );
	const Agent::JsonValue* rmApplied = rmResult.find( "applied" );
	Check( rmApplied && rmApplied->isBool() && rmApplied->asBool(),
	       "step 3 (remove the old material) applied -- the FULL recipe lands" );

	// (4) The final head RELOADS cleanly and carries the rename.
	{
		const std::string bytes = RISE::Cst::SerializeCst( *pJob->GetCstDocument() );
		const std::string tmp2 = TempPath( "agentcrud_g2_reload.RISEscene" );
		Job* fresh = LoadScene( bytes.c_str(), tmp2 );
		Check( fresh != nullptr, "the post-recipe head reloads cleanly (no data loss)" );
		if( fresh ) {
			Check( fresh->GetMaterials() && fresh->GetMaterials()->GetItem( "matRenamed" ) != nullptr,
			       "the reloaded scene has the renamed material" );
			Check( fresh->GetMaterials()->GetItem( "mat_diffuse" ) == nullptr,
			       "the reloaded scene no longer has the old material" );
			Check( fresh->GetObjects() && fresh->GetObjects()->GetItem( "obj_sph" ) != nullptr,
			       "the reloaded scene keeps the retargeted object" );
			fresh->release();
		}
		std::remove( tmp2.c_str() );
	}

	pJob->release();
	std::remove( tmp.c_str() );
}

//----------------------------------------------------------------------
// G3: rasterizer insert ACTIVATION (review round 2 P1-B): inserting a
// different-keyword rasterizer makes it ACTIVE live, and the serialized
// head reloaded into a fresh Job agrees (live == reload; last-wins).
//----------------------------------------------------------------------
static void TestRasterizerInsertActivation()
{
	std::printf( "G3: rasterizer insert activates live AND matches reload (P1-B)...\n" );
	const std::string tmp = TempPath( "agentcrud_g3.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "rasterizer fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

	Check( pJob->GetActiveRasterizerName() == "pathtracing_pel_rasterizer",
	       "the authored pathtracing rasterizer is active on load" );

	// Round-3 additive wire field: the render result carries the ACTIVE
	// integrator (the rasterizer's chunk keyword) -- pre-insert it must be
	// the authored pathtracing one.
	{
		Agent::AgentRenderResult rrPre = sess->Render();
		Check( rrPre.ok, "the pre-insert render succeeds" );
		Check( rrPre.integrator == "pathtracing_pel_rasterizer",
		       "the pre-insert render reports integrator=pathtracing_pel_rasterizer" );
	}

	Agent::AgentChunkResult r = sess->InsertChunk(
		"bdpt_pel_rasterizer\n{\n\tsamples 4\n}" );
	Check( r.applied, "inserting a different-keyword rasterizer applies" );
	Check( pJob->GetActiveRasterizerName() == "bdpt_pel_rasterizer",
	       "the inserted rasterizer is ACTIVE live (activation restore skipped)" );

	// ... and post-insert the render result OBSERVES the switch -- the agent
	// no longer has to take activation on faith.
	{
		Agent::AgentRenderResult rrPost = sess->Render();
		Check( rrPost.ok, "the post-insert render succeeds" );
		Check( rrPost.integrator == "bdpt_pel_rasterizer",
		       "the post-insert render reports integrator=bdpt_pel_rasterizer (the field reflects the insert)" );
	}

	// Live == reload: derive the serialized bytes into a FRESH Job and
	// compare active-rasterizer names (last-wins on load).
	{
		const std::string bytes = sess->ReadDocument();
		const std::string tmp2 = TempPath( "agentcrud_g3_reload.RISEscene" );
		Job* fresh = LoadScene( bytes.c_str(), tmp2 );
		Check( fresh != nullptr, "the post-insert head reloads" );
		if( fresh ) {
			Check( fresh->GetActiveRasterizerName() == "bdpt_pel_rasterizer",
			       "the reloaded head's active rasterizer AGREES with live (no divergence)" );
			Check( fresh->GetActiveRasterizerName() == pJob->GetActiveRasterizerName(),
			       "live and reload name the SAME active rasterizer" );
			fresh->release();
		}
		std::remove( tmp2.c_str() );
	}

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

//----------------------------------------------------------------------
// G4: round-2 P3 bundle -- (a) an EXACT-duplicate (kind,name,variant)
// overlay insert is refused while a DIFFERENT-variant overlay stays
// allowed; (b) the ambiguity refusal message is conditional on whether
// `kind` was passed.
//----------------------------------------------------------------------
static void TestVariantOverlayAndAmbiguityMessages()
{
	std::printf( "G4: exact-duplicate variant overlay refused + conditional ambiguity message...\n" );
	const std::string tmp = TempPath( "agentcrud_g4.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

	// (a) Variant overlays: first insert applies; the EXACT (kind,name,
	// variant) duplicate is refused; a DIFFERENT variant still applies.
	const char* overlay =
		"lambertian_material\n{\n\tname mat_diffuse\n\tvariant nightMode\n\treflectance pnt_albedo\n}";
	Agent::AgentChunkResult r1 = sess->InsertChunk( overlay );
	Check( r1.applied, "a variant overlay sharing the base (kind,name) inserts" );
	Agent::AgentChunkResult r2 = sess->InsertChunk( overlay );
	Check( !r2.applied && r2.status == "rejected",
	       "the EXACT (kind,name,variant) duplicate overlay is REFUSED" );
	Check( r2.message.find( "already exists" ) != std::string::npos,
	       "the duplicate-overlay refusal says it already exists" );
	Agent::AgentChunkResult r3 = sess->InsertChunk(
		"lambertian_material\n{\n\tname mat_diffuse\n\tvariant dayMode\n\treflectance pnt_albedo\n}" );
	Check( r3.applied, "a DIFFERENT-variant overlay of the same base still inserts" );

	// (b) Ambiguity messages: the base + its overlays share the (kind,name)
	// path, so 'mat_diffuse' is now ambiguous BOTH bare and under the exact
	// keyword -- exercising both message branches (materials are name-unique
	// within their manager, so two same-name chunks of different material
	// kinds cannot exist; overlays are the one legitimate same-name shape).
	Agent::AgentChunkResult noKind = sess->RemoveChunk( "mat_diffuse" );
	Check( !noKind.applied && noKind.status == "rejected", "bare ambiguous remove refused" );
	Check( noKind.message.find( "pass `kind` to narrow" ) != std::string::npos,
	       "WITHOUT kind the message suggests passing kind" );
	Agent::AgentChunkResult withKind = sess->RemoveChunk( "mat_diffuse", "lambertian_material" );
	Check( !withKind.applied && withKind.status == "rejected", "with-kind ambiguous remove refused" );
	Check( withKind.message.find( "more specific kind" ) != std::string::npos,
	       "WITH a kind the message asks for a MORE SPECIFIC kind (not the misleading 'pass kind')" );
	Check( withKind.message.find( "match" ) != std::string::npos,
	       "the with-kind ambiguity message reports how many chunks match" );

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}


//----------------------------------------------------------------------
// G5: the taught camera-SWAP recipe is TRUE (round 3 P1 red-prove).
// The round-2 teaching claimed an unnamed camera can NEVER be removed;
// in fact ApplyCstRemoveChunk's kind="camera" positional fallback
// resolves the SOLE camera-kind chunk even unnamed.  Drive the exact
// recipe the prompts now teach: remove the sole unnamed camera FIRST
// (the camera-less document derives), THEN insert the replacement --
// and the derived head carries the thinlens camera and renders.
//----------------------------------------------------------------------
static void TestCameraSwapRecipe()
{
	std::printf( "G5: camera SWAP -- remove the sole unnamed camera (kind=\"camera\"), then insert the replacement...\n" );
	const std::string tmp = TempPath( "agentcrud_g5.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "camera-swap fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

	// Step 1: the sole UNNAMED camera IS removable via the kind="camera"
	// positional fallback (the target is a bare name that matches nothing;
	// resolution is by position because exactly ONE camera-kind chunk exists).
	Agent::AgentChunkResult rm = sess->RemoveChunk( "pinhole_camera", "camera" );
	Check( rm.applied && rm.status == "applied",
	       "the sole unnamed camera is REMOVABLE via kind=\"camera\" (the teaching is true)" );
	Check( rm.kind == "pinhole_camera", "the remove echoes the resolved camera keyword" );
	Check( sess->ReadDocument().find( "pinhole_camera" ) == std::string::npos,
	       "the camera chunk is gone from the head (camera-less document derived)" );

	// Step 2: insert the replacement -- the taught remove-FIRST order means
	// exactly one camera exists again afterwards (no wedged pair).
	Agent::AgentChunkResult ins = sess->InsertChunk(
		"thinlens_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n"
		"\tsensor_size 36.0\n\tfocal_length 35.0\n\tfstop 2.8\n\tfocus_distance 3.5\n}" );
	Check( ins.applied && ins.status == "applied", "the replacement thinlens camera inserts" );
	Check( ins.kind == "thinlens_camera", "the insert echoes the camera keyword" );
	Check( sess->ReadDocument().find( "thinlens_camera" ) != std::string::npos,
	       "the head carries the thinlens camera" );

	// The DERIVED scene really has the swapped camera: it renders non-black.
	Agent::AgentRenderResult rr = sess->Render();
	Check( rr.ok, "the swapped head renders" );
	Check( rr.meanR + rr.meanG + rr.meanB > 0.0, "the post-swap render is non-black" );

	// And the swapped camera stays removable -- it is again the SOLE camera.
	Agent::AgentChunkResult rm2 = sess->RemoveChunk( "thinlens_camera", "camera" );
	Check( rm2.applied, "the swapped-in camera is itself removable via kind=\"camera\"" );

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

//----------------------------------------------------------------------
// G6: APPEND-class unnamed chunks (descriptor `unnamedRepeatable`).
//
// A `timeline` chunk carries no `name` param and derives by APPENDING an
// independent effect (Job::AddKeyframeToAnimation), so a scene legitimately
// carries MANY unnamed timelines (sdf_morph_torture has ~14).  Regression for
// the false-rejection bug root-caused from a live gemini-3.5-flash trajectory:
// the pre-fix unnamed-singleton rule refused ANY second unnamed chunk of a
// keyword, wrongly rejecting a schema-conformant second timeline.
//
// RED-PROVE (pre-fix): before the descriptor `unnamedRepeatable` gate, the
// UNNAMED branch of Job::ApplyCstInsertChunk ran an UNCONDITIONAL top-level
// scan and returned -2 ("an unnamed `timeline` chunk already exists ...") for
// the SECOND insert below -- so `r2.applied` was FALSE and this test failed on
// the "second unnamed timeline applied" Check.  With `timeline.unnamedRepeatable
// = true` the scan is skipped and the insert derives.
//----------------------------------------------------------------------
static void TestUnnamedRepeatableTimeline()
{
	std::printf( "G6: unnamedRepeatable -- a SECOND unnamed timeline is accepted (append-class), not a singleton...\n" );
	const std::string tmp = TempPath( "agentcrud_g6.RISEscene" );

	// --- Scenario A: two unnamed timelines coexist; ambiguous kind-only removal refuses. ---
	{
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "G6 fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

		const RISE::Cst::CstHeadVersion v0 = sess->HeadVersion();

		// Two schema-conformant, DERIVABLE unnamed timelines (object position animation of the two
		// existing objects).  Both are unnamed (no `name` param on `timeline`).
		Agent::AgentChunkResult r1 = sess->InsertChunk(
			"timeline\n{\n\telement_type object\n\telement obj_sph\n\tparam position\n"
			"\ttime 0\n\tvalue 0 0 0\n\ttime 1\n\tvalue 1 0 0\n}" );
		Check( r1.applied && r1.status == "applied", "first unnamed timeline applied" );
		Check( r1.kind == "timeline", "first timeline insert echoes the keyword" );

		Agent::AgentChunkResult r2 = sess->InsertChunk(
			"timeline\n{\n\telement_type object\n\telement obj_emit\n\tparam position\n"
			"\ttime 0\n\tvalue 0 0 0\n\ttime 1\n\tvalue 0 1 0\n}" );
		Check( r2.applied && r2.status == "applied",
		       "SECOND unnamed timeline applied (the false-rejection bug is fixed)" );
		Check( r2.rawCode == 2, "the second timeline insert is a full re-derive (rawCode 2)" );
		Check( r2.headVersion.revision > v0.revision, "the second insert advanced the head revision" );

		// BOTH timelines are present in the head, and it round-trips + still derives.
		const std::string doc = sess->ReadDocument();
		{
			size_t first = doc.find( "timeline" );
			size_t second = ( first == std::string::npos ) ? std::string::npos : doc.find( "timeline", first + 1 );
			Check( first != std::string::npos && second != std::string::npos,
			       "ReadDocument carries BOTH unnamed timeline chunks" );
			RISE::Cst::Document rt = RISE::Cst::ParseToCst( doc );
			Check( RISE::Cst::SerializeCst( rt ) == doc,
			       "the two-timeline head round-trips through the CST parser byte-identically" );
		}
		// applied==true above already proves the dry-run derive passed; the render confirms end-to-end.
		Agent::AgentRenderResult rr = sess->Render();
		Check( rr.ok && rr.meanR + rr.meanG + rr.meanB > 0.0,
		       "the scene with two appended timelines derives and renders non-black" );

		// remove_chunk by kind alone is now AMBIGUOUS (2 unnamed timelines) -> honest refusal, head intact.
		const std::string headBeforeRm = sess->ReadDocument();
		Agent::AgentChunkResult rmAmb = sess->RemoveChunk( "timeline", "timeline" );
		Check( !rmAmb.applied && rmAmb.status == "rejected",
		       "kind-only removal of one of two unnamed timelines is REFUSED" );
		Check( rmAmb.message.find( "ambiguous" ) != std::string::npos,
		       "the refusal message names the ambiguity" );
		Check( sess->ReadDocument() == headBeforeRm,
		       "the ambiguity refusal left the head byte-identical" );

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}

	// --- Scenario B: exactly ONE unnamed timeline removes fine (positional fallback). ---
	{
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "G6 scenario-B fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

		Check( sess->InsertChunk(
			"timeline\n{\n\telement_type object\n\telement obj_sph\n\tparam position\n"
			"\ttime 0\n\tvalue 0 0 0\n\ttime 1\n\tvalue 1 0 0\n}" ).applied,
			"scenario-B: the sole unnamed timeline inserts" );

		Agent::AgentChunkResult rmOne = sess->RemoveChunk( "timeline", "timeline" );
		Check( rmOne.applied && rmOne.status == "applied",
		       "the SOLE unnamed timeline removes cleanly (positional unique-in-kind fallback)" );
		Check( rmOne.kind == "timeline", "the sole-timeline remove echoes the keyword" );
		Check( sess->ReadDocument().find( "timeline" ) == std::string::npos,
		       "the timeline chunk is gone from the head" );

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}

	// --- Scenario C: schema exposes the flag for timeline but NOT for a singleton kind. ---
	{
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "G6 scenario-C fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

		const std::string tlSchema = sess->ReadSchema( "timeline" );
		Check( tlSchema.find( "\"unnamedRepeatable\":true" ) != std::string::npos,
		       "read_schema(timeline) advertises unnamedRepeatable:true" );
		const std::string filmSchema = sess->ReadSchema( "film" );
		Check( filmSchema.find( "unnamedRepeatable" ) == std::string::npos,
		       "read_schema(film) carries NO unnamedRepeatable key (singleton, unbloated)" );

		// The singleton rule is UNCHANGED for non-repeatable kinds: a second unnamed film still refuses.
		Agent::AgentChunkResult rFilm = sess->InsertChunk( "film\n{\n\twidth 32\n\theight 32\n}" );
		Check( !rFilm.applied && rFilm.status == "rejected",
		       "a second unnamed film is STILL rejected (singleton rule intact)" );
		Check( rFilm.message.find( "already exists" ) != std::string::npos,
		       "the film-singleton refusal keeps its existing message" );

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}
}

//----------------------------------------------------------------------
// G7: remove_chunk ambiguity guard must fire ONLY on the KEYWORD-interpretation path.
//
// Regression for the coincidence bug: the guard used to resolve
// `DescriptorForKeyword((kind && kind[0]) ? kind : target)` -- with `kind`
// omitted (the normal remove-by-name path), a chunk of ANY OTHER kind that
// happens to be NAMED the same as a repeatable keyword (e.g. an omni_light
// literally named "timeline") could never be removed by bare name while 2+
// unnamed timelines coexist: the code resolved `target` as the KEYWORD
// "timeline" and refused as ambiguous, even though DocFindByNameAnyRole
// would have uniquely resolved the NAME "timeline" to the light.
//
// Fixed by trying plain name resolution FIRST when `kind` is omitted; only
// when that finds NOTHING does `target` fall through to keyword
// interpretation and the ambiguity guard.
//----------------------------------------------------------------------
static void TestRemoveChunkNameKeywordCoincidence()
{
	std::printf( "G7: remove_chunk name/keyword coincidence -- a chunk NAMED \"timeline\" resolves by NAME, not keyword...\n" );
	const std::string tmp = TempPath( "agentcrud_g7.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "G7 fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

	// Two unnamed timelines (so the repeatable-keyword ambiguity guard is IN PLAY for kind-only removal)...
	Check( sess->InsertChunk(
		"timeline\n{\n\telement_type object\n\telement obj_sph\n\tparam position\n"
		"\ttime 0\n\tvalue 0 0 0\n\ttime 1\n\tvalue 1 0 0\n}" ).applied,
		"G7: first unnamed timeline inserts" );
	Check( sess->InsertChunk(
		"timeline\n{\n\telement_type object\n\telement obj_emit\n\tparam position\n"
		"\ttime 0\n\tvalue 0 0 0\n\ttime 1\n\tvalue 0 1 0\n}" ).applied,
		"G7: second unnamed timeline inserts" );

	// ...plus an omni_light literally NAMED "timeline" (a coincidental name/keyword clash).
	Agent::AgentChunkResult rLight = sess->InsertChunk(
		"omni_light\n{\n\tname timeline\n\tposition 2 2 2\n\tcolor 1 1 1\n\tpower 5.0\n}" );
	Check( rLight.applied && rLight.status == "applied", "G7: the light named `timeline` inserts" );

	const RISE::Cst::CstHeadVersion vBefore = sess->HeadVersion();
	const std::string docBefore = sess->ReadDocument();
	Check( docBefore.find( "omni_light" ) != std::string::npos, "G7 precondition: the light is present before removal" );

	// remove_chunk(target="timeline", NO kind) must resolve the NAME first and remove THE LIGHT --
	// not refuse as an ambiguous keyword address, even though "timeline" also names a repeatable kind
	// with 2 unnamed instances.
	Agent::AgentChunkResult rmByName = sess->RemoveChunk( "timeline" );
	Check( rmByName.applied && rmByName.status == "applied",
	       "remove_chunk(target=\"timeline\", no kind) REMOVES THE LIGHT (name wins over keyword coincidence)" );
	Check( rmByName.kind == "omni_light", "the removal echoes the LIGHT's keyword, not `timeline`" );
	Check( rmByName.headVersion.revision > vBefore.revision, "the removal advanced the head revision" );

	const std::string docAfter = sess->ReadDocument();
	Check( docAfter.find( "omni_light" ) == std::string::npos, "the light is gone from the head" );
	{
		size_t first = docAfter.find( "timeline" );
		size_t second = ( first == std::string::npos ) ? std::string::npos : docAfter.find( "timeline", first + 1 );
		Check( first != std::string::npos && second != std::string::npos,
		       "BOTH unnamed timelines are still intact after removing the coincidentally-named light" );
	}

	// With `kind="timeline"` explicitly provided, the keyword-interpretation path IS in play and the
	// ambiguity guard still fires exactly as before (2 unnamed timelines -> refuse).
	Agent::AgentChunkResult rmByKeyword = sess->RemoveChunk( "timeline", "timeline" );
	Check( !rmByKeyword.applied && rmByKeyword.status == "rejected",
	       "remove_chunk(target=\"timeline\", kind=\"timeline\") is STILL refused as ambiguous (2 unnamed instances)" );
	Check( rmByKeyword.message.find( "ambiguous" ) != std::string::npos,
	       "the refusal message names the ambiguity" );
	Check( sess->ReadDocument() == docAfter,
	       "the ambiguity refusal left the head byte-identical (unaffected by the earlier name-resolved removal)" );

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

//----------------------------------------------------------------------
// G8: element_type camera resolves `element` as the TARGET camera's name.
//   (a) two named cameras -> a timeline element=<non-active camera> animates
//       THAT camera, proven by evaluating the keyframe at t=1;
//   (b) a name that matches no camera is a LOUD derive rejection (NOT a
//       silent active-camera fallback), head unchanged, and the rejection
//       MESSAGE (not just the log) names the missing camera;
//   (c) an empty element with a single unnamed camera falls back to the
//       ACTIVE camera and applies (the eval-fixture's own golden path);
//   (d) (TestReservedCameraNameNoneAtDerive, below) a HAND-AUTHORED scene
//       naming a camera `none` -- which never goes through the AGENT-insert
//       gate (Job::ApplyCstInsertChunk's own "reserved name" early check) --
//       is still refused, at the chunk-parser/derive layer, because a
//       camera literally named "none" would collide with the unbind
//       sentinel (a) above and (c) above both rely on to distinguish
//       named-target vs active-camera-fallback.
//----------------------------------------------------------------------
static Point3 ReadCamLocation( ICamera* cam )
{
	double x = 0, y = 0, z = 0;
	if( cam ) {
		const String v = CameraIntrospection::GetPropertyValue( *cam, String( "location" ) );
		std::sscanf( v.c_str(), "%lf %lf %lf", &x, &y, &z );
	}
	return Point3( x, y, z );
}

static void TestCameraTimelineNamedTargeting()
{
	std::printf( "G8: element_type camera -- named targeting / loud miss / active fallback...\n" );
	const std::string tmp = TempPath( "agentcrud_g7.RISEscene" );

	// --- (a) named NON-active camera is the one animated. ---
	{
		Job* pJob = LoadScene( kTwoCamScene, tmp );
		Check( pJob != nullptr, "(a) two-named-camera fixture loads" );
		if( !pJob ) return;
		Check( pJob->GetActiveCameraName() == "camB",
		       "(a) camB (authored last) is the active camera; camA is non-active" );
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

		Agent::AgentChunkResult rA = sess->InsertChunk(
			"timeline\n{\n\telement_type camera\n\telement camA\n\tparam location\n"
			"\ttime 0\n\tvalue 0 0 5\n\ttime 1\n\tvalue 7 0 5\n}" );
		Check( rA.applied && rA.status == "applied",
		       "(a) timeline targeting the NAMED non-active camera camA applies + derives" );

		IAnimator* anim = pJob->GetScene() ? pJob->GetScene()->GetAnimator() : nullptr;
		Check( anim != nullptr, "(a) the derived scene exposes an animator" );
		if( anim ) anim->EvaluateAtTime( 1.0 );

		ICameraManager* cams = pJob->GetCameras();
		ICamera* camA = cams ? cams->GetItem( "camA" ) : nullptr;
		ICamera* camB = cams ? cams->GetItem( "camB" ) : nullptr;
		Check( camA && camB, "(a) both named cameras resolve in the derived scene" );
		const Point3 aLoc = ReadCamLocation( camA );
		const Point3 bLoc = ReadCamLocation( camB );
		Check( std::fabs( aLoc.x - 7.0 ) < 1e-6,
		       "(a) camA (the NAMED target) moved to the keyframed x=7 at t=1" );
		Check( std::fabs( bLoc.x - 0.0 ) < 1e-6 && std::fabs( bLoc.z - 9.0 ) < 1e-6,
		       "(a) camB (the ACTIVE camera) was NOT animated -- still at authored 0 0 9" );

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}

	// --- (b) a name that matches no camera is a LOUD derive rejection. ---
	{
		Job* pJob = LoadScene( kTwoCamScene, tmp );
		Check( pJob != nullptr, "(b) fixture reloads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

		const std::string headBefore = sess->ReadDocument();
		const RISE::Cst::CstHeadVersion vBefore = sess->HeadVersion();

		Agent::AgentChunkResult rMiss = sess->InsertChunk(
			"timeline\n{\n\telement_type camera\n\telement no_such_cam\n\tparam location\n"
			"\ttime 0\n\tvalue 0 0 5\n\ttime 1\n\tvalue 7 0 5\n}" );
		Check( !rMiss.applied && rMiss.status == "rejected",
		       "(b) naming a non-existent camera is REJECTED (no silent active-camera fallback)" );
		Check( rMiss.message.find( "would not derive" ) != std::string::npos,
		       "(b) the rejection is the derive-gate class" );
		Check( rMiss.message.find( "no_such_cam" ) != std::string::npos,
		       "(b) the missing camera's name is named IN THE RESPONSE MESSAGE (via g_cstFinalizeDiagSink -- "
		       "not just the log), so the agent can see the specific reason without reading the server log" );
		Check( sess->ReadDocument() == headBefore,
		       "(b) the rejected insert left the head byte-identical" );
		Check( sess->HeadVersion() == vBefore,
		       "(b) the rejected insert did not advance the revision" );

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}

	// --- (c) empty element + single unnamed camera -> active-camera fallback. ---
	{
		Job* pJob = LoadScene( kScene, tmp );   // kScene has ONE unnamed camera
		Check( pJob != nullptr, "(c) single-unnamed-camera fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

		Agent::AgentChunkResult rC = sess->InsertChunk(
			"timeline\n{\n\telement_type camera\n\tparam location\n"
			"\ttime 0\n\tvalue 0 0 3.5\n\ttime 1\n\tvalue 2 0 3.5\n}" );
		Check( rC.applied && rC.status == "applied",
		       "(c) a camera timeline with NO element falls back to the active camera + derives" );

		IAnimator* anim = pJob->GetScene() ? pJob->GetScene()->GetAnimator() : nullptr;
		if( anim ) anim->EvaluateAtTime( 1.0 );
		ICameraManager* cams = pJob->GetCameras();
		ICamera* active = cams ? cams->GetItem( pJob->GetActiveCameraName().c_str() ) : nullptr;
		Check( active != nullptr, "(c) the active (unnamed -> auto-named) camera resolves" );
		const Point3 loc = ReadCamLocation( active );
		Check( std::fabs( loc.x - 2.0 ) < 1e-6,
		       "(c) the fallback animated the active camera to x=2 at t=1" );

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}
}

//----------------------------------------------------------------------
// G8(d): a camera explicitly named `none` collides with the scene
// language's universal unbind sentinel -- Job::AddKeyframeToAnimation's
// camera branch (exercised by (a)/(b)/(c) above) treats element=="none"
// as "target the ACTIVE camera", never a named lookup, so a camera
// actually named "none" could never be targeted by name.
//
// Job::ApplyCstInsertChunk already refuses `name none` for AGENT-driven
// inserts (the "reserved name" early check, red-proved at line ~376
// above via `sphere_geometry { name none }`), but that gate sits ABOVE
// the parser and never runs for a scene parsed/derived directly -- e.g.
// a hand-authored .RISEscene file loaded via LoadAsciiSceneViaCst. This
// test goes DIRECTLY through RISE::Cst::ParseToCst + DeriveToJob (NOT
// AgentSession::InsertChunk) specifically so it bypasses that early
// gate and actually exercises the derive-layer refusal (Chunk-
// ParserRegistry.cpp's RejectReservedCameraName, called from every
// camera chunk's Finalize before AllocateCameraName) -- going through
// InsertChunk here would silently test the WRONG gate.
//----------------------------------------------------------------------
static void TestReservedCameraNameNoneAtDerive()
{
	std::printf( "G8(d): a hand-authored `thinlens_camera { name none }` is refused at derive...\n" );

	const std::string text =
		"RISE ASCII SCENE 7\n"
		"film\n{\n\twidth 24\n\theight 24\n}\n\n"
		"thinlens_camera\n{\n\tname none\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n"
		"\tfocus_distance 3.5\n}\n";

	RISE::Cst::Document doc = RISE::Cst::ParseToCst( text );
	Job* j = new Job();
	std::vector<std::string> diags;
	RISE::Cst::DeriveToJob( doc, *j, &diags );

	Check( !diags.empty(), "deriving a scene with `thinlens_camera { name none }` emits a diagnostic" );
	if( !diags.empty() ) {
		Check( diags[0].find( "thinlens_camera" ) != std::string::npos,
		       "the diagnostic names the offending chunk `thinlens_camera`" );
		Check( diags[0].find( "reserved name" ) != std::string::npos,
		       "the diagnostic uses the same \"reserved name\" wording as the agent-insert gate "
		       "(Job::ApplyCstInsertChunk)" );
		Check( diags[0].find( "none" ) != std::string::npos,
		       "the diagnostic names the reserved sentinel `none`" );
	}
	// The camera must never have been registered under the reserved name -- neither via the
	// chunk-parser-level refusal above, nor (defense-in-depth) had it slipped past that, via
	// Scene::AddCamera's own independent `none` refusal.
	ICameraManager* cams = j->GetCameras();
	Check( !cams || !cams->GetItem( "none" ), "no camera is ever registered under the reserved name `none`" );

	j->release();
}

//----------------------------------------------------------------------
// G9: gltf_import hardening -- entity-name collisions fail loudly (2026-07-11),
// THEN gltf_import is flagged unnamedRepeatable (same append-class idiom as
// timeline/keyframe -- see G6).
//
// Background: GLTFSceneImporter::ImportScene used to unconditionally `return true`
// and silently swallow every duplicate-name GenericManager::AddItem failure, so TWO
// unnamed `gltf_import` chunks sharing the SAME (defaulted) name_prefix passed the
// dry-run derive with the second import's ~10+ entities (gltf.pnt.*, gltf.mat.0,
// gltf.geom.m0.p0, __pbrmr_* ...) silently masked.  This blocked flagging gltf_import
// as unnamedRepeatable (deferred in 436f604a pending exactly this fix).  Fixed via
// two layers: (1) Job::ImportGLTFScene refuses a REPEATED name_prefix within the same
// derive up front (a per-Job `mGltfImportPrefixes` record, reset each derive) with a
// diagnostic naming the prefix; (2) GLTFSceneImporter::ImportScene itself now
// propagates any entity-registration failure (material / geometry / object) as a hard
// `false` instead of discarding it -- belt-and-suspenders for a NON-prefix collision
// (a hand-authored entity that happens to collide with a generated name).  Distinct
// prefixes remain fully supported -- the multi-import idiom
// scenes/FeatureBased/Geometry/sponza_new_ivy.RISEscene relies on (2+ imports, an
// in-file comment blessing it).
//
// RED-PROVE (pre-fix): Scenario B's second-same-prefix-insert rejection is the
// load-bearing regression -- pre-fix, ImportScene's unconditional `return true` meant
// that insert passed the dry-run derive cleanly (r2.applied would be TRUE, not the
// FALSE asserted below).  Scenario D's hand-authored-collision rejection is likewise
// pre-fix-failing: ImportScene discarded CreateMaterial/AddPrebuiltTriangleMeshGeometry/
// AddObjectMatrix's bool returns, so a colliding geometry name silently dropped that
// one primitive instead of failing the import.  Both were verified by temporarily
// reverting the Job::ImportGLTFScene prefix guard and the ImportScene
// `anyRegistrationFailure` propagation (restoring the old unconditional `return true`)
// -- both rejections flipped to false-positive "applied" passes, confirming the
// Checks below actually exercise the new code.  Scenario A and C are NOT expected to
// fail pre-fix (A predates this change entirely -- distinct prefixes always worked;
// C is the flag flip itself, gated on this whole fix landing).
//----------------------------------------------------------------------
static void TestGltfImportPrefixCollision()
{
	std::printf( "G9: gltf_import hardening -- prefix + entity collisions fail loudly...\n" );
	const std::string tmp = TempPath( "agentcrud_g9.RISEscene" );
	const std::string kBoxAsset = "scenes/Tests/Geometry/assets/Box.glb";

	// --- Scenario A: two DISTINCT prefixes -- both apply (the supported multi-import idiom). ---
	{
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "G9(A) fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
		const RISE::Cst::CstHeadVersion v0 = sess->HeadVersion();

		Agent::AgentChunkResult rA = sess->InsertChunk(
			"gltf_import\n{\n\tfile " + kBoxAsset + "\n\tname_prefix boxA\n"
			"\timport_lights FALSE\n\timport_cameras FALSE\n}" );
		Check( rA.applied && rA.status == "applied", "G9(A) first gltf_import (prefix boxA) applies" );
		const RISE::Cst::CstHeadVersion v1 = sess->HeadVersion();
		Check( v1.revision > v0.revision, "G9(A) first import advanced the head revision" );

		Agent::AgentChunkResult rB = sess->InsertChunk(
			"gltf_import\n{\n\tfile " + kBoxAsset + "\n\tname_prefix boxB\n"
			"\timport_lights FALSE\n\timport_cameras FALSE\n}" );
		Check( rB.applied && rB.status == "applied",
		       "G9(A) SECOND gltf_import (DISTINCT prefix boxB) also applies" );
		const RISE::Cst::CstHeadVersion v2 = sess->HeadVersion();
		Check( v2.revision > v1.revision, "G9(A) second import advanced the head revision AGAIN (head advances twice)" );

		Check( pJob->GetGeometries() && pJob->GetGeometries()->GetItem( "boxA.geom.m0.p0" ) != nullptr,
		       "G9(A) boxA's geometry is present" );
		Check( pJob->GetGeometries() && pJob->GetGeometries()->GetItem( "boxB.geom.m0.p0" ) != nullptr,
		       "G9(A) boxB's geometry is present (BOTH imports' entities survive)" );

		// applied==true above already proves the dry-run derive passed; the render confirms end-to-end
		// (matches the discipline G6 uses for its own two-unnamed-chunk scenario).
		Agent::AgentRenderResult rr = sess->Render();
		Check( rr.ok, "G9(A) the scene with two distinct-prefix gltf_import chunks renders" );

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}

	// --- Scenario B: SAME (defaulted) prefix -- the second insert is REJECTED at derive. ---
	{
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "G9(B) fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

		const std::string chunkText =
			"gltf_import\n{\n\tfile " + kBoxAsset + "\n\timport_lights FALSE\n\timport_cameras FALSE\n}";
		Agent::AgentChunkResult r1 = sess->InsertChunk( chunkText );
		Check( r1.applied && r1.status == "applied", "G9(B) first unnamed gltf_import (default prefix `gltf`) applies" );
		const std::string headAfterFirst = sess->ReadDocument();
		const RISE::Cst::CstHeadVersion vAfterFirst = sess->HeadVersion();

		// Second insert, SAME default prefix ("gltf") -- must be REJECTED, not silently masked.
		Agent::AgentChunkResult r2 = sess->InsertChunk( chunkText );
		Check( !r2.applied && r2.status == "rejected",
		       "G9(B) SECOND same-prefix gltf_import is REJECTED (red-prove: this FAILS pre-fix)" );
		Check( r2.message.find( "gltf" ) != std::string::npos && r2.message.find( "collides" ) != std::string::npos,
		       "G9(B) the rejection message names the colliding prefix and says 'collides'" );
		Check( sess->ReadDocument() == headAfterFirst,
		       "G9(B) the rejected second import left the head byte-identical" );
		Check( sess->HeadVersion() == vAfterFirst,
		       "G9(B) the rejected second import did not advance the head revision" );

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}

	// --- Scenario C: read_schema advertises unnamedRepeatable for gltf_import (matches G6/timeline). ---
	{
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "G9(C) fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

		const std::string gltfSchema = sess->ReadSchema( "gltf_import" );
		Check( gltfSchema.find( "\"unnamedRepeatable\":true" ) != std::string::npos,
		       "G9(C) read_schema(gltf_import) advertises unnamedRepeatable:true" );

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}

	// --- Scenario D: BELT-AND-SUSPENDERS -- a hand-authored entity collides with a
	// gltf_import-GENERATED name.  This is the FIRST (and only) gltf_import in the
	// derive, so the name_prefix guard (Scenario B's mechanism) does NOT fire -- the
	// collision is caught by ImportScene's own AddItem-failure propagation instead. ---
	{
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "G9(D) fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

		// Box.glb's sole primitive derives the geometry name "gltf.geom.m0.p0" under
		// the default (unspecified) name_prefix "gltf" -- hand-author a geometry
		// chunk under exactly that name BEFORE the import.
		Agent::AgentChunkResult rPre = sess->InsertChunk(
			"sphere_geometry\n{\n\tname gltf.geom.m0.p0\n\tradius 0.1\n}" );
		Check( rPre.applied && rPre.status == "applied", "G9(D) the colliding hand-authored geometry inserts" );
		const std::string headAfterPre = sess->ReadDocument();

		Agent::AgentChunkResult r = sess->InsertChunk(
			"gltf_import\n{\n\tfile " + kBoxAsset + "\n\timport_lights FALSE\n\timport_cameras FALSE\n}" );
		Check( !r.applied && r.status == "rejected",
		       "G9(D) the gltf_import is REJECTED -- entity-name collision, not a prefix collision "
		       "(red-prove: this FAILS pre-fix)" );
		Check( sess->ReadDocument() == headAfterPre,
		       "G9(D) the rejected import left the head byte-identical (the pre-authored geometry survives alone)" );

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}
}

//----------------------------------------------------------------------
// U1: unresolved-reference WARNING on insert_chunk. The CST resolver
//     (RISE::Cst::BuildReferenceGraph) already DETECTS a dangling
//     reference (a param value naming a chunk that is not defined
//     anywhere in the document) -- it just used to throw that
//     information away past a log-only string diagnostic. insert_chunk
//     now surfaces it as a STRUCTURED, NON-BLOCKING warning
//     (AgentChunkResult::issues / wire key
//     "issues") so a model gets same-turn signal instead
//     of silently shipping a broken material.
//
//     Motivating case (a real observed failure): a local model built a
//     scene that inserted `lambertian_material { reflectance
//     uniform_wall_pink }` where the painter it had actually just
//     created was named `_wall_pink` (NOT `uniform_wall_pink`).
//     insert_chunk returned SUCCESS with no diagnostic -- the model got
//     no signal, silently left the broken material in the document, and
//     only much later inserted a corrected duplicate.
//
//     VEHICLE NOTE (read before changing this test): the LITERAL
//     `lambertian_material.reflectance` scenario above cannot be
//     reproduced through insert_chunk on today's tree -- verified by
//     running it. `Job::AddLambertianMaterial` (Job.cpp) hard-fails
//     (`return false`) when its painter name does not resolve, and
//     `Job::ApplyCstInsertChunk`'s dry-run-guarded FULL re-derive (its
//     documented "no half-applied state" contract) refuses the WHOLE
//     insert when any chunk's Finalize fails -- so `applied` can never
//     be `true` for that exact chunk/param pairing; this is a stronger,
//     pre-existing, deliberately-tested safety net (see H2/G1) that this
//     feature must NOT weaken. That hard gate is universal for
//     `standard_object.material/geometry/modifier/shader` and every
//     ordinary material's painter slot audited for this test. It is
//     NOT universal, though: `Job::AddDirectLightingShaderOp` (bound to
//     `directlighting_shaderop`'s `bsdf` param, ValueKind::Reference /
//     ChunkCategory::Material) resolves the name with NO null check at
//     all -- an unresolved `bsdf` silently becomes "no BSDF override"
//     and the shaderop still registers successfully. That gap is this
//     test's VEHICLE: same resolver, same AgentSession/AgentRpc
//     plumbing, same near-miss suggestion algorithm, just a param the
//     engine happens not to hard-validate -- so `applied==true` with a
//     genuine unresolvedRef is actually reachable. The bad/right name
//     pair (`uniform_wall_pink` / `_wall_pink`) is kept verbatim from
//     the motivating report even though the category is Material here,
//     not Painter -- the mechanism under test doesn't care which.
//----------------------------------------------------------------------
static void TestUnresolvedReferenceWarning()
{
	std::printf( "U1: insert_chunk surfaces unresolved-reference WARNINGS (non-blocking)...\n" );

	// (a) THE MOTIVATING CASE (adapted vehicle -- see the note above).
	{
		const std::string tmp = TempPath( "agentcrud_u1a.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "U1(a) fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

		// The material the local model ACTUALLY created (name `_wall_pink`),
		// standing in for the motivating bug's painter.
		Agent::AgentChunkResult rMat = sess->InsertChunk(
			"lambertian_material\n{\n\tname _wall_pink\n\treflectance pnt_albedo\n}" );
		Check( rMat.applied, "U1(a) the (correctly-named) material inserts" );

		// A shaderop's `bsdf` override references the WRONG name
		// (`uniform_wall_pink` -- the bug: guessing a name instead of using
		// the one actually created). Job::AddDirectLightingShaderOp does
		// NOT hard-validate `bsdf`, so this insert still lands.
		Agent::AgentChunkResult rOp = sess->InsertChunk(
			"directlighting_shaderop\n{\n\tname dlop_test\n\tbsdf uniform_wall_pink\n}" );
		Check( rOp.applied, "U1(a) the shaderop insert STILL APPLIES (a warning, not a rejection)" );
		Check( rOp.status == "applied", "U1(a) status stays \"applied\"" );
		Check( rOp.issues.size() == 1, "U1(a) exactly ONE issue" );
		if( rOp.issues.size() == 1 ) {
			const Agent::AgentChunkIssue& u = rOp.issues[0];
			Check( u.param == "bsdf", "U1(a) issue param is \"bsdf\"" );
			Check( u.value == "uniform_wall_pink", "U1(a) issue value is the bad name" );
			Check( u.reason == "unresolved_reference", "U1(a) issue reason is \"unresolved_reference\"" );
			bool sawIt = false;
			std::string suggList;
			for( const std::string& s : u.suggestions ) {
				if( s == "_wall_pink" ) sawIt = true;
				suggList += "'" + s + "' ";
			}
			Check( sawIt, "U1(a) suggestions include the ACTUAL material name '_wall_pink'" );
			std::printf( "  U1(a) suggestions for 'uniform_wall_pink': %s\n", suggList.c_str() );
		}
		Check( rOp.message.find( "bsdf" ) != std::string::npos,
		       "U1(a) message names the offending param" );

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}

	// (b) NON-BLOCKING FORWARD REFERENCE: a shaderop references a material that
	// does not exist YET (warned, not refused); inserting that material next
	// succeeds cleanly and the document as a whole now resolves with no
	// dangling entry left for that shaderop.
	{
		const std::string tmp = TempPath( "agentcrud_u1b.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "U1(b) fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

		Agent::AgentChunkResult rOp = sess->InsertChunk(
			"directlighting_shaderop\n{\n\tname dlop_fwd\n\tbsdf mat_notyet\n}" );
		Check( rOp.applied, "U1(b) forward-referencing shaderop insert APPLIES (not refused)" );
		Check( rOp.issues.size() == 1 &&
		       rOp.issues[0].param == "bsdf" &&
		       rOp.issues[0].value == "mat_notyet" &&
		       rOp.issues[0].reason == "unresolved_reference",
		       "U1(b) warned about the not-yet-defined material" );

		Agent::AgentChunkResult rMat = sess->InsertChunk(
			"lambertian_material\n{\n\tname mat_notyet\n\treflectance pnt_albedo\n}" );
		Check( rMat.applied, "U1(b) the material insert applies" );
		Check( rMat.issues.empty(), "U1(b) the material's OWN insert reports no issues of its own" );

		// The document as a whole resolves cleanly now.
		const std::string doc = sess->ReadDocument();
		RISE::Cst::Document parsed = RISE::Cst::ParseToCst( doc );
		std::vector<RISE::Cst::UnresolvedReference> unresolved;
		RISE::Cst::BuildReferenceGraph( parsed, nullptr, &unresolved );
		bool stillDangling = false;
		for( const RISE::Cst::UnresolvedReference& u : unresolved )
			if( u.chunkKeyword == "directlighting_shaderop" && u.param == "bsdf" && u.value == "mat_notyet" )
				stillDangling = true;
		Check( !stillDangling, "U1(b) dlop_fwd.bsdf now resolves cleanly (no dangling entry left)" );

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}

	// (c) NO FALSE POSITIVES: the explicit `material none` idiom and an inline
	// numeric literal in a reference-ish slot must NOT be reported.
	{
		const std::string tmp = TempPath( "agentcrud_u1c.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "U1(c) fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

		Agent::AgentChunkResult rPnt = sess->InsertChunk(
			"uniformcolor_painter\n{\n\tname pnt_glow\n\tcolor 1 1 1\n}" );
		Check( rPnt.applied, "U1(c) glow painter inserts" );

		// lambertian_luminaire_material's `material` slot accepts the
		// explicit-none idiom (an underlying, optional wrapped material) --
		// must not be flagged as a dangling reference.
		Agent::AgentChunkResult rLum = sess->InsertChunk(
			"lambertian_luminaire_material\n{\n\tname mat_glow\n\texitance pnt_glow\n\tscale 10.0\n\tmaterial none\n}" );
		Check( rLum.applied, "U1(c) luminaire material (material none idiom) applies" );
		Check( rLum.issues.empty(), "U1(c) the explicit-none idiom produces NO issues" );

		// standard_object.position is a plain DoubleVec3 slot elsewhere in this
		// suite, but exercise a numeric-literal-in-a-reference-ish-looking-tuple
		// path directly against the resolver here too: `rs` (radiance scale on
		// this fixture's rasterizer) is a plain numeric, never reference-typed,
		// so re-confirm via a second geometry insert with an inline numeric
		// tuple value in an otherwise ordinary param -- must not spuriously warn.
		Agent::AgentChunkResult rGeo = sess->InsertChunk(
			"sphere_geometry\n{\n\tname sph_numeric\n\tradius 0.42\n}" );
		Check( rGeo.applied, "U1(c) plain-numeric-param geometry insert applies" );
		Check( rGeo.issues.empty(), "U1(c) a plain numeric param produces NO issues" );

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}

	// (d) A fully clean insert produces no `issues` key at all in
	// the JSON-RPC response (back-compat: an existing caller's key set is
	// unchanged on the common case).
	{
		const std::string tmp = TempPath( "agentcrud_u1d.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "U1(d) fixture loads" );
		if( !pJob ) return;

		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
		Agent::AgentRpcDispatcher disp( std::move( sess ) );

		const std::string resp = disp.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"insert_chunk\",\"params\":"
			"{\"chunkText\":\"sphere_geometry\\n{\\nname sph_clean\\nradius 0.4\\n}\"}}" );
		Agent::JsonValue result;
		Check( JsonResultObj( resp, result ), "U1(d) insert_chunk returns a JSON-RPC result object" );
		const Agent::JsonValue* applied = result.find( "applied" );
		Check( applied && applied->isBool() && applied->asBool(), "U1(d) clean insert applied" );
		Check( result.find( "issues" ) == nullptr,
		       "U1(d) a clean insert OMITS the issues key entirely" );

		pJob->release();
		std::remove( tmp.c_str() );
	}

	// (e) SCOPING: a PRE-EXISTING dangling reference elsewhere in the document
	// is NOT reported by an unrelated clean insert -- only the just-inserted
	// chunk's own refs are.
	{
		const std::string tmp = TempPath( "agentcrud_u1e.RISEscene" );
		// kScene plus one EXTRA, already-broken shaderop (bsdf names a
		// material that was never defined) authored directly into the
		// fixture text -- bypassing insert_chunk's own gate so the
		// pre-existing dangling reference is genuinely already IN the head
		// when this test begins. Uses the same reachable vehicle as (a)/(b)
		// (see the VEHICLE NOTE above TestUnresolvedReferenceWarning): a
		// dangling `lambertian_material.reflectance` would fail to LOAD at
		// all (Job::AddLambertianMaterial hard-fails), so it can't stand in
		// for "already in a loadable head" here either.
		const std::string kSceneWithDangling = std::string( kScene ) +
			"\ndirectlighting_shaderop\n{\n\tname dlop_dangling\n\tbsdf nonexistent_material\n}\n";
		Job* pJob = LoadScene( kSceneWithDangling.c_str(), tmp );
		Check( pJob != nullptr, "U1(e) fixture (with a pre-existing dangling reference) loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

		// Sanity: the pre-existing dangling reference really is there.
		{
			RISE::Cst::Document parsed = RISE::Cst::ParseToCst( sess->ReadDocument() );
			std::vector<RISE::Cst::UnresolvedReference> unresolved;
			RISE::Cst::BuildReferenceGraph( parsed, nullptr, &unresolved );
			bool sawPreexisting = false;
			for( const RISE::Cst::UnresolvedReference& u : unresolved )
				if( u.chunkKeyword == "directlighting_shaderop" && u.value == "nonexistent_material" ) sawPreexisting = true;
			Check( sawPreexisting, "U1(e) sanity: dlop_dangling's dangling bsdf is really in the head" );
		}

		// An UNRELATED clean insert must report NOTHING about dlop_dangling.
		Agent::AgentChunkResult rGeo = sess->InsertChunk(
			"sphere_geometry\n{\n\tname sph_unrelated\n\tradius 0.55\n}" );
		Check( rGeo.applied, "U1(e) the unrelated geometry insert applies" );
		Check( rGeo.issues.empty(),
		       "U1(e) the unrelated insert's own issues is empty -- the PRE-EXISTING "
		       "dangling reference elsewhere in the document does not leak into it" );

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}
}

//----------------------------------------------------------------------
// R1: actionable REJECTED insert_chunk diagnostics (Model-B F5 slice S3).
//     Reproduces THREE real insert_chunk rejections observed from a live
//     GUI trajectory of qwen3.6:27b building a scene -- every one of them
//     was rejected with an unactionable, hedged message ("apply failed
//     (e.g. unresolved reference); see log" / "invalid parameter(s) (see
//     log)") that named no param, no value, and pointed at a log an agent
//     cannot read. AnalyzeRejectedInsert (AgentSession.cpp) is a
//     descriptor-based pre-flight pass that pins the EXACT cause for all
//     three, without changing engine derive diagnostics or `applied`/
//     `status`/`retriable` semantics one bit.
//   (a) lambertian_material.reflectance names `uniform_wall_pink` when the
//       painter actually defined is `_wall_pink` -> unresolved_reference.
//   (b) ggx_material.rs is given an inline RGB triple (`0.95 0.9 0.7`)
//       where a Painter NAME belongs -> numeric_in_reference_slot.
//   (c) scalar_painter is given `constant 0.45` where the real parameter
//       is `value` -> unknown_param, with the FULL valid-parameter list
//       surfaced in `message`.
//   (d) NO FALSE POSITIVES on a REJECTED insert: the explicit `material
//       none` idiom and a genuinely numeric Double slot (`scale`) must NOT
//       be flagged alongside a real unknown_param elsewhere in the SAME
//       rejected chunk.
//   (e) HONESTY: a rejection the analyser genuinely cannot explain (a
//       semantic cross-param constraint no descriptor field encodes --
//       `ggx_material fresnel_mode thinfilm` without `film_ior`+
//       `film_thickness`) returns EMPTY issues and does NOT append an
//       "ACTIONABLE" sentence that would misleadingly imply the analyser
//       exonerated the chunk.
//----------------------------------------------------------------------
static void TestRejectedInsertDiagnostics()
{
	std::printf( "R1: actionable REJECTED insert_chunk diagnostics (real repro cases)...\n" );

	// (a) unresolved_reference: reflectance names the wrong (near-miss) painter.
	{
		const std::string tmp = TempPath( "agentcrud_r1a.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "R1(a) fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

		Agent::AgentChunkResult rPaint = sess->InsertChunk(
			"uniformcolor_painter\n{\n\tname _wall_pink\n\tcolor 0.9 0.6 0.7\n}" );
		Check( rPaint.applied, "R1(a) the painter '_wall_pink' inserts" );

		Agent::AgentChunkResult rMat = sess->InsertChunk(
			"lambertian_material\n{\n\tname wall_pink\n\treflectance uniform_wall_pink\n}" );
		Check( !rMat.applied && rMat.status == "rejected",
		       "R1(a) the material insert is REJECTED (the engine hard-fails an unresolved painter)" );
		Check( rMat.issues.size() == 1, "R1(a) exactly ONE issue" );
		if( rMat.issues.size() == 1 ) {
			const Agent::AgentChunkIssue& u = rMat.issues[0];
			Check( u.param == "reflectance", "R1(a) issue param is \"reflectance\"" );
			Check( u.value == "uniform_wall_pink", "R1(a) issue value is the bad name" );
			Check( u.reason == "unresolved_reference", "R1(a) issue reason is \"unresolved_reference\"" );
			bool sawIt = false;
			for( const std::string& s : u.suggestions ) if( s == "_wall_pink" ) sawIt = true;
			Check( sawIt, "R1(a) suggestions include the ACTUAL painter name '_wall_pink'" );
		}
		Check( rMat.message.find( "ACTIONABLE" ) != std::string::npos,
		       "R1(a) message carries the actionable sentence" );
		std::printf( "  R1(a) message: %s\n", rMat.message.c_str() );

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}

	// (b) numeric_in_reference_slot: rs given an inline RGB triple.
	{
		const std::string tmp = TempPath( "agentcrud_r1b.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "R1(b) fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

		Agent::AgentChunkResult rGgx = sess->InsertChunk(
			"ggx_material\n{\n\tname gold_mat\n\trd pnt_albedo\n\trs 0.95 0.9 0.7\n"
			"\talphax pnt_albedo\n\talphay pnt_albedo\n\tior pnt_albedo\n\textinction pnt_albedo\n}" );
		Check( !rGgx.applied && rGgx.status == "rejected",
		       "R1(b) the ggx_material insert is REJECTED (a numeric literal in a Painter reference slot)" );
		Check( rGgx.issues.size() == 1, "R1(b) exactly ONE issue" );
		if( rGgx.issues.size() == 1 ) {
			const Agent::AgentChunkIssue& u = rGgx.issues[0];
			Check( u.param == "rs", "R1(b) issue param is \"rs\"" );
			Check( u.value == "0.95 0.9 0.7", "R1(b) issue value is the numeric triple" );
			Check( u.reason == "numeric_in_reference_slot", "R1(b) issue reason is \"numeric_in_reference_slot\"" );
			Check( u.suggestions.empty(), "R1(b) a literal has no near-miss NAME to suggest" );
		}
		Check( rGgx.message.find( "painter" ) != std::string::npos,
		       "R1(b) message names the declared reference category (painter)" );
		std::printf( "  R1(b) message: %s\n", rGgx.message.c_str() );

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}

	// (c) unknown_param: `constant` typed for the real param `value`.
	{
		const std::string tmp = TempPath( "agentcrud_r1c.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "R1(c) fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

		Agent::AgentChunkResult rScalar = sess->InsertChunk(
			"scalar_painter\n{\n\tname _gold_ior\n\tconstant 0.45\n}" );
		Check( !rScalar.applied && rScalar.status == "rejected",
		       "R1(c) the scalar_painter insert is REJECTED (an undeclared parameter name)" );
		Check( rScalar.issues.size() == 1, "R1(c) exactly ONE issue" );
		if( rScalar.issues.size() == 1 ) {
			const Agent::AgentChunkIssue& u = rScalar.issues[0];
			Check( u.param == "constant", "R1(c) issue param is \"constant\"" );
			Check( u.reason == "unknown_param", "R1(c) issue reason is \"unknown_param\"" );
			bool sawValue = false;
			for( const std::string& s : u.suggestions ) if( s == "value" ) sawValue = true;
			Check( sawValue, "R1(c) suggestions contain the real parameter name 'value'" );
		}
		Check( rScalar.message.find( "value" ) != std::string::npos,
		       "R1(c) message names the real parameter 'value'" );
		Check( rScalar.message.find( "valid parameters are" ) != std::string::npos,
		       "R1(c) message carries the FULL valid-parameter list" );
		std::printf( "  R1(c) message: %s\n", rScalar.message.c_str() );

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}

	// (d) NO FALSE POSITIVES on a rejected insert: `material none` and a
	// genuinely numeric Double slot (`scale`) must not be flagged alongside
	// a real unknown_param (`bogus_param`) in the SAME chunk.
	{
		const std::string tmp = TempPath( "agentcrud_r1d.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "R1(d) fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

		Agent::AgentChunkResult rLum = sess->InsertChunk(
			"lambertian_luminaire_material\n{\n\tname mat_bad\n\texitance pnt_albedo\n"
			"\tscale 5.0\n\tmaterial none\n\tbogus_param 1\n}" );
		Check( !rLum.applied && rLum.status == "rejected",
		       "R1(d) the insert is REJECTED (the one genuinely undeclared parameter)" );
		Check( rLum.issues.size() == 1, "R1(d) exactly ONE issue -- no false positives on `material none` or `scale`" );
		if( rLum.issues.size() == 1 ) {
			Check( rLum.issues[0].param == "bogus_param", "R1(d) the single issue is the real culprit" );
			Check( rLum.issues[0].reason == "unknown_param", "R1(d) the single issue's reason is unknown_param" );
		}

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}

	// (e) HONESTY: a rejection the analyser cannot explain (a semantic
	// cross-param constraint -- fresnel_mode thinfilm requires film_ior +
	// film_thickness together -- no descriptor field encodes that) returns
	// EMPTY issues and does NOT append a misleading "ACTIONABLE" sentence.
	{
		const std::string tmp = TempPath( "agentcrud_r1e.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "R1(e) fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

		// alphax/alphay/ior/extinction need an ACTUAL scalar_painter (a
		// physical-scalar IScalarPainter slot, per docs/ISCALARPAINTER_REFACTOR.md
		// -- a colour IPainter like pnt_albedo would trip a DIFFERENT rejection
		// here); rd/rs stay ordinary colour painters (tints), which is correct
		// for those slots. That isolates the rejection to EXACTLY the thinfilm
		// presence contract this case is about.
		Agent::AgentChunkResult rScal = sess->InsertChunk(
			"scalar_painter\n{\n\tname _r1e_scalar\n\tvalue 0.3\n}" );
		Check( rScal.applied, "R1(e) helper scalar_painter inserts" );

		Agent::AgentChunkResult rGgx = sess->InsertChunk(
			"ggx_material\n{\n\tname film_mat\n\trd pnt_albedo\n\trs pnt_albedo\n"
			"\talphax _r1e_scalar\n\talphay _r1e_scalar\n\tior _r1e_scalar\n\textinction _r1e_scalar\n"
			"\tfresnel_mode thinfilm\n}" );
		Check( !rGgx.applied && rGgx.status == "rejected",
		       "R1(e) the insert is REJECTED (thinfilm without film_ior+film_thickness)" );
		Check( rGgx.issues.empty(),
		       "R1(e) HONESTY: the analyser cannot see this semantic constraint -- issues stays empty" );
		Check( rGgx.message.find( "ACTIONABLE" ) == std::string::npos,
		       "R1(e) an empty analysis does NOT append a misleading ACTIONABLE sentence" );
		std::printf( "  R1(e) message: %s\n", rGgx.message.c_str() );

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}
}

//----------------------------------------------------------------------
// R2: actionable REJECTED propose_patch diagnostics -- the propose_patch
// sibling of R1 (same {param,value,reason,suggestions} shape, closing the
// gap that only insert_chunk explained ITS rejections).  (target, kind,
// param, value) are already in hand from the call, so this resolves
// directly against the head document rather than parsing candidate text.
//----------------------------------------------------------------------
static void TestActionablePatchDiagnostics()
{
	std::printf( "R2: actionable REJECTED propose_patch diagnostics (real repro cases)...\n" );

	// (a) unknown_target: a near-miss typo of the real geometry name "sph".
	{
		const std::string tmp = TempPath( "agentcrud_r2a.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "R2(a) fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

		Agent::AgentSetPatch p;
		p.target = "sph_x";
		p.kind   = "sphere_geometry";
		p.param  = "radius";
		p.value  = "1.0";
		Agent::AgentPatchResult r = sess->ProposePatch( p );
		Check( !r.applied && r.status == "rejected",
		       "R2(a) an unknown target is REJECTED" );
		Check( r.issues.size() == 1, "R2(a) exactly ONE issue" );
		if( r.issues.size() == 1 ) {
			const Agent::AgentChunkIssue& u = r.issues[0];
			Check( u.reason == "unknown_target", "R2(a) issue reason is \"unknown_target\"" );
			Check( u.value == "sph_x", "R2(a) issue value is the bad target name" );
			bool sawIt = false;
			for( const std::string& s : u.suggestions ) if( s == "sph" ) sawIt = true;
			Check( sawIt, "R2(a) suggestions include the ACTUAL geometry name 'sph'" );
		}
		Check( r.message.find( "ACTIONABLE" ) != std::string::npos,
		       "R2(a) message carries the actionable sentence" );
		std::printf( "  R2(a) message: %s\n", r.message.c_str() );

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}

	// (b) unknown_param: a valid target, an undeclared param.
	{
		const std::string tmp = TempPath( "agentcrud_r2b.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "R2(b) fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

		Agent::AgentSetPatch p;
		p.target = "mat_diffuse";
		p.kind   = "material";
		p.param  = "bogus_param";
		p.value  = "1.0";
		Agent::AgentPatchResult r = sess->ProposePatch( p );
		Check( !r.applied && r.status == "rejected",
		       "R2(b) an undeclared param is REJECTED" );
		Check( r.issues.size() == 1, "R2(b) exactly ONE issue" );
		if( r.issues.size() == 1 ) {
			const Agent::AgentChunkIssue& u = r.issues[0];
			Check( u.param == "bogus_param", "R2(b) issue param is \"bogus_param\"" );
			Check( u.reason == "unknown_param", "R2(b) issue reason is \"unknown_param\"" );
		}
		Check( r.message.find( "valid parameters are" ) != std::string::npos &&
		       r.message.find( "reflectance" ) != std::string::npos,
		       "R2(b) message lists the valid parameters (including 'reflectance')" );
		std::printf( "  R2(b) message: %s\n", r.message.c_str() );

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}

	// (c) unresolved_reference: a near-miss typo of the real painter "pnt_emit".
	{
		const std::string tmp = TempPath( "agentcrud_r2c.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "R2(c) fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

		Agent::AgentSetPatch p;
		p.target = "mat_diffuse";
		p.kind   = "material";
		p.param  = "reflectance";
		p.value  = "pnt_emitt";
		Agent::AgentPatchResult r = sess->ProposePatch( p );
		Check( !r.applied && r.status == "rejected",
		       "R2(c) a dangling reference retarget is REJECTED" );
		Check( r.issues.size() == 1, "R2(c) exactly ONE issue" );
		if( r.issues.size() == 1 ) {
			const Agent::AgentChunkIssue& u = r.issues[0];
			Check( u.param == "reflectance", "R2(c) issue param is \"reflectance\"" );
			Check( u.value == "pnt_emitt", "R2(c) issue value is the bad name" );
			Check( u.reason == "unresolved_reference", "R2(c) issue reason is \"unresolved_reference\"" );
			bool sawIt = false;
			for( const std::string& s : u.suggestions ) if( s == "pnt_emit" ) sawIt = true;
			Check( sawIt, "R2(c) suggestions include the ACTUAL painter name 'pnt_emit'" );
		}
		Check( r.message.find( "ACTIONABLE" ) != std::string::npos,
		       "R2(c) message carries the actionable sentence" );
		std::printf( "  R2(c) message: %s\n", r.message.c_str() );

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}

	// (d) numeric_in_reference_slot: reflectance given an inline RGB triple.
	{
		const std::string tmp = TempPath( "agentcrud_r2d.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "R2(d) fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

		Agent::AgentSetPatch p;
		p.target = "mat_diffuse";
		p.kind   = "material";
		p.param  = "reflectance";
		p.value  = "0.9 0.6 0.7";
		Agent::AgentPatchResult r = sess->ProposePatch( p );
		Check( !r.applied && r.status == "rejected",
		       "R2(d) a numeric literal in a reference slot is REJECTED" );
		Check( r.issues.size() == 1, "R2(d) exactly ONE issue" );
		if( r.issues.size() == 1 ) {
			const Agent::AgentChunkIssue& u = r.issues[0];
			Check( u.param == "reflectance", "R2(d) issue param is \"reflectance\"" );
			Check( u.value == "0.9 0.6 0.7", "R2(d) issue value is the numeric triple" );
			Check( u.reason == "numeric_in_reference_slot", "R2(d) issue reason is \"numeric_in_reference_slot\"" );
		}
		Check( r.message.find( "painter" ) != std::string::npos,
		       "R2(d) message names the declared reference category (painter)" );
		std::printf( "  R2(d) message: %s\n", r.message.c_str() );

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}

	// (e) invalid_value: a non-numeric token in a Double slot (sphere radius).
	{
		const std::string tmp = TempPath( "agentcrud_r2e.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "R2(e) fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

		Agent::AgentSetPatch p;
		p.target = "sph";
		p.kind   = "sphere_geometry";
		p.param  = "radius";
		p.value  = "not_a_number";
		Agent::AgentPatchResult r = sess->ProposePatch( p );
		Check( !r.applied && r.status == "rejected",
		       "R2(e) a non-numeric value in a Double slot is REJECTED" );
		Check( r.issues.size() == 1, "R2(e) exactly ONE issue" );
		if( r.issues.size() == 1 ) {
			const Agent::AgentChunkIssue& u = r.issues[0];
			Check( u.param == "radius", "R2(e) issue param is \"radius\"" );
			Check( u.value == "not_a_number", "R2(e) issue value is the bad token" );
			Check( u.reason == "invalid_value", "R2(e) issue reason is \"invalid_value\"" );
		}
		Check( r.message.find( "ACTIONABLE" ) != std::string::npos,
		       "R2(e) message carries the actionable sentence" );
		std::printf( "  R2(e) message: %s\n", r.message.c_str() );

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}

	// (f) a genuinely CLEAN patch carries no issues key at all (empty vector).
	{
		const std::string tmp = TempPath( "agentcrud_r2f.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "R2(f) fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

		Agent::AgentSetPatch p;
		p.target = "sph";
		p.kind   = "sphere_geometry";
		p.param  = "radius";
		p.value  = "1.2";
		Agent::AgentPatchResult r = sess->ProposePatch( p );
		Check( r.applied && r.status == "applied", "R2(f) a clean patch applies" );
		Check( r.issues.empty(), "R2(f) a clean patch carries NO issues" );

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}
}

//----------------------------------------------------------------------
// R3: actionable REJECTED remove_chunk diagnostics -- the remove_chunk
// sibling of R1/R2. The reference graph's reverse adjacency NAMES the
// blocking referrer(s) instead of the engine's own hedged "likely still
// REFERENCED... or the document no longer derives in order" message.
//----------------------------------------------------------------------
static void TestActionableRemoveDiagnostics()
{
	std::printf( "R3: actionable REJECTED remove_chunk diagnostics (real repro cases)...\n" );

	// (h) still_referenced: mat_diffuse is bound by obj_sph's `material` param.
	{
		const std::string tmp = TempPath( "agentcrud_r3h.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "R3(h) fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

		Agent::AgentChunkResult r = sess->RemoveChunk( "mat_diffuse" );
		Check( !r.applied && r.status == "rejected",
		       "R3(h) removing a still-referenced material is REFUSED" );
		Check( r.issues.size() == 1, "R3(h) exactly ONE issue" );
		if( r.issues.size() == 1 ) {
			const Agent::AgentChunkIssue& u = r.issues[0];
			Check( u.value == "mat_diffuse", "R3(h) issue value is the remove target's own name" );
			Check( u.reason == "still_referenced", "R3(h) issue reason is \"still_referenced\"" );
			bool sawIt = false;
			for( const std::string& s : u.suggestions ) if( s == "obj_sph" ) sawIt = true;
			Check( sawIt, "R3(h) suggestions NAME the blocking referrer 'obj_sph'" );
		}
		Check( r.message.find( "ACTIONABLE" ) != std::string::npos &&
		       r.message.find( "obj_sph" ) != std::string::npos,
		       "R3(h) message NAMES the blocking referrer, not just a hedge" );
		std::printf( "  R3(h) message: %s\n", r.message.c_str() );

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}

	// (j) HONESTY: a remove that fails for the OTHER (non-reference) reason --
	// camA is targeted by a timeline's `element` param, a DYNAMIC reference
	// outside any declared Reference param, so the static reference graph
	// shows NO dependents for camA even though removing it demonstrably
	// breaks the derive.  No issue must be invented; the honest hedged
	// message must survive untouched.
	{
		const std::string tmp = TempPath( "agentcrud_r3j.RISEscene" );
		Job* pJob = LoadScene( kTwoCamScene, tmp );
		Check( pJob != nullptr, "R3(j) two-camera fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

		Agent::AgentChunkResult rTl = sess->InsertChunk(
			"timeline\n{\n\telement_type camera\n\telement camA\n\tparam location\n"
			"\ttime 0\n\tvalue 0 0 5\n\ttime 1\n\tvalue 7 0 5\n}" );
		Check( rTl.applied, "R3(j) the camA-targeting timeline inserts" );

		Agent::AgentChunkResult r = sess->RemoveChunk( "camA" );
		Check( !r.applied && r.status == "rejected",
		       "R3(j) removing camA (still targeted by the timeline) is REFUSED" );
		Check( r.issues.empty(),
		       "R3(j) HONESTY: the static reference graph does not see the DYNAMIC timeline "
		       "reference -- issues stays EMPTY rather than inventing a referrer" );
		Check( r.message.find( "ACTIONABLE" ) == std::string::npos,
		       "R3(j) an empty analysis does NOT append a misleading ACTIONABLE sentence" );
		Check( r.message.find( "would not derive" ) != std::string::npos,
		       "R3(j) the engine's own hedged message survives untouched" );
		std::printf( "  R3(j) message: %s\n", r.message.c_str() );

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}
}

//----------------------------------------------------------------------
// IC1: insert_chunks -- the BATCH form of insert_chunk (headless, direct
// AgentSession::InsertChunks API).  Covers the ALL-APPLY case (a) and the
// INTRA-BATCH DEPENDENCY case (b): a painter at index 0 resolves cleanly
// against a material at index 1 that references it, because index 0 has
// already landed by the time index 1 is applied -- exactly as if the two
// had been separate insert_chunk calls in the same order.
//----------------------------------------------------------------------
static void TestInsertChunksBatchAllApply()
{
	std::printf( "IC1: insert_chunks batch -- ALL-APPLY + intra-batch dependency (headless)...\n" );
	const std::string tmp = TempPath( "agentcrud_ic1.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "IC1 fixture loads" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const RISE::Cst::CstHeadVersion v0 = sess->HeadVersion();

	std::vector<std::string> chunks;
	chunks.push_back( "uniformcolor_painter\n{\n\tname pnt_batch\n\tcolor 0.1 0.9 0.3\n}" );
	chunks.push_back( "lambertian_material\n{\n\tname mat_batch\n\treflectance pnt_batch\n}" );
	chunks.push_back( "sphere_geometry\n{\n\tname sph_batch\n\tradius 0.3\n}" );
	chunks.push_back( "standard_object\n{\n\tname obj_batch\n\tgeometry sph_batch\n\tmaterial mat_batch\n\tposition 1.5 0 0\n}" );

	const std::vector<Agent::AgentChunkResult> results = sess->InsertChunks( chunks );
	Check( results.size() == 4, "IC1 returns exactly one result per input chunk, same order" );
	if( results.size() == 4 ) {
		Check( results[0].applied && results[0].status == "applied" &&
		       results[0].kind == "uniformcolor_painter" && results[0].name == "pnt_batch",
		       "IC1(a) element 0 (painter) applied, echoes kind/name" );
		Check( results[1].applied && results[1].status == "applied" &&
		       results[1].kind == "lambertian_material" && results[1].name == "mat_batch",
		       "IC1(a) element 1 (material referencing element 0) applied" );
		Check( results[1].issues.empty(),
		       "IC1(b) INTRA-BATCH DEPENDENCY: the material's reflectance reference resolves cleanly "
		       "-- no unresolved_reference issue -- because the painter already landed earlier in "
		       "THIS SAME BATCH before the material was applied" );
		Check( results[2].applied && results[2].kind == "sphere_geometry" && results[2].name == "sph_batch",
		       "IC1(a) element 2 (geometry) applied" );
		Check( results[3].applied && results[3].kind == "standard_object" && results[3].name == "obj_batch",
		       "IC1(a) element 3 (object binding elements 1+2) applied" );
		Check( results[3].headVersion.revision > v0.revision,
		       "IC1(a) the final element's headVersion reflects the accumulated batch revisions" );
	}

	// The DERIVED live scene grew (managers resolve every batch-inserted entity).
	Check( pJob->GetMaterials() && pJob->GetMaterials()->GetItem( "mat_batch" ) != nullptr,
	       "IC1(a) derived scene has the batch-inserted material" );
	Check( pJob->GetObjects() && pJob->GetObjects()->GetItem( "obj_batch" ) != nullptr,
	       "IC1(a) derived scene has the batch-inserted object" );

	// The retained Document carries every chunk.
	const std::string doc = sess->ReadDocument();
	Check( doc.find( "pnt_batch" ) != std::string::npos &&
	       doc.find( "mat_batch" ) != std::string::npos &&
	       doc.find( "sph_batch" ) != std::string::npos &&
	       doc.find( "obj_batch" ) != std::string::npos,
	       "IC1(a) ReadDocument carries every batch-inserted chunk" );

	// Empty input is a documented no-op.
	{
		const std::vector<std::string> none;
		const std::vector<Agent::AgentChunkResult> emptyResults = sess->InsertChunks( none );
		Check( emptyResults.empty(), "IC1 an empty chunks vector returns an empty results vector (no-op)" );
	}

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

//----------------------------------------------------------------------
// IC2: insert_chunks BEST-EFFORT / PER-CHUNK RESULTS (headless) -- a
// batch whose SECOND element is rejected (references a painter defined
// NEITHER in the batch NOR the document) does not stop the batch: the
// other three elements still apply, and `applied` == total-1.
//----------------------------------------------------------------------
static void TestInsertChunksBestEffort()
{
	std::printf( "IC2: insert_chunks batch -- BEST-EFFORT per-chunk results (headless)...\n" );
	const std::string tmp = TempPath( "agentcrud_ic2.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "IC2 fixture loads" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

	std::vector<std::string> chunks;
	chunks.push_back( "uniformcolor_painter\n{\n\tname pnt_c0\n\tcolor 0.4 0.4 0.4\n}" );
	// References a painter that exists NEITHER earlier in this batch NOR
	// anywhere in the document -- a hard REJECT (not a forward-reference
	// warning; see U1's VEHICLE NOTE for why an ordinary material reference
	// hard-fails the whole insert).
	chunks.push_back( "lambertian_material\n{\n\tname mat_c1\n\treflectance nope_painter_xyz\n}" );
	chunks.push_back( "sphere_geometry\n{\n\tname sph_c2\n\tradius 0.2\n}" );
	// Binds the NEW geometry to the PRE-EXISTING mat_diffuse (from kScene)
	// so it does NOT depend on the rejected mat_c1 -- isolating the
	// best-effort assertion from a chained-dependency failure.
	chunks.push_back( "standard_object\n{\n\tname obj_c3\n\tgeometry sph_c2\n\tmaterial mat_diffuse\n\tposition -1.5 0 0\n}" );

	const std::vector<Agent::AgentChunkResult> results = sess->InsertChunks( chunks );
	Check( results.size() == 4, "IC2 returns one result per input chunk" );
	if( results.size() == 4 ) {
		Check( results[0].applied && results[0].status == "applied",
		       "IC2(c) element 0 (painter) applied" );
		Check( !results[1].applied && results[1].status == "rejected",
		       "IC2(c) element 1 (dangling reference) is REJECTED" );
		Check( results[1].issues.size() == 1, "IC2(c) the rejected element carries exactly one issue" );
		if( results[1].issues.size() == 1 ) {
			const Agent::AgentChunkIssue& u = results[1].issues[0];
			Check( u.param == "reflectance", "IC2(c) issue param is \"reflectance\"" );
			Check( u.value == "nope_painter_xyz", "IC2(c) issue value is the undefined name" );
			Check( u.reason == "unresolved_reference", "IC2(c) issue reason is \"unresolved_reference\"" );
		}
		Check( results[2].applied && results[2].status == "applied",
		       "IC2(c) BEST-EFFORT: element 2 (geometry, independent of the rejected element 1) still applied" );
		Check( results[3].applied && results[3].status == "applied",
		       "IC2(c) BEST-EFFORT: element 3 (object, does not depend on the rejected element) still applied" );
	}

	// applied == total - 1: exactly one rejection, nothing else affected.
	int appliedCount = 0;
	for( const Agent::AgentChunkResult& r : results ) if( r.applied ) ++appliedCount;
	Check( appliedCount == 3 && (int)results.size() == 4,
	       "IC2 applied == total-1 (3 of 4) -- the one rejection is isolated, not cascaded" );

	// The rejected element never reached the managers; the applied ones did.
	Check( pJob->GetMaterials() == nullptr || pJob->GetMaterials()->GetItem( "mat_c1" ) == nullptr,
	       "IC2(c) the rejected material never reached the live managers" );
	Check( pJob->GetObjects() && pJob->GetObjects()->GetItem( "obj_c3" ) != nullptr,
	       "IC2(c) the surviving object DID reach the live managers" );

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

//----------------------------------------------------------------------
// IC3: insert_chunks VALIDATION -- driven through the REAL wire
// (AgentRpcDispatcher::HandleLine, headless: WrapJob with no controller
// attached), mirroring L2's raw-JSON-RPC style.  `chunks` missing / not an
// array / an empty array / containing a non-string element must ALL
// return -32602 (invalid params), never crash, and never mutate the
// document.
//----------------------------------------------------------------------
static void TestInsertChunksValidation()
{
	std::printf( "IC3: insert_chunks wire validation (missing/non-array/empty/non-string -> -32602)...\n" );
	const std::string tmp = TempPath( "agentcrud_ic3.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "IC3 fixture loads" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const std::string headBefore = sess->ReadDocument();
	Agent::AgentRpcDispatcher disp( std::move( sess ) );

	// (1) Missing `chunks` entirely.
	{
		const std::string resp = disp.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"insert_chunks\",\"params\":{}}" );
		Check( resp.find( "-32602" ) != std::string::npos,
		       "IC3(1) insert_chunks without 'chunks' -> -32602 invalid params" );
	}

	// (2) `chunks` present but not an array (a string, the insert_chunk-style mistake).
	{
		const std::string resp = disp.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"insert_chunks\",\"params\":"
			"{\"chunks\":\"omni_light { name x }\"}}" );
		Check( resp.find( "-32602" ) != std::string::npos,
		       "IC3(2) insert_chunks with a non-array 'chunks' -> -32602 invalid params" );
	}

	// (3) `chunks` is an empty array.
	{
		const std::string resp = disp.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"insert_chunks\",\"params\":{\"chunks\":[]}}" );
		Check( resp.find( "-32602" ) != std::string::npos,
		       "IC3(3) insert_chunks with an EMPTY 'chunks' array -> -32602 invalid params" );
	}

	// (4) `chunks` contains a non-string element.
	{
		const std::string resp = disp.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"insert_chunks\",\"params\":"
			"{\"chunks\":[\"uniformcolor_painter\\n{\\nname ok_one\\ncolor 1 1 1\\n}\", 42]}}" );
		Check( resp.find( "-32602" ) != std::string::npos,
		       "IC3(4) insert_chunks with a non-string element -> -32602 invalid params" );
		Check( resp.find( "chunks[1]" ) != std::string::npos,
		       "IC3(4) the error names WHICH element (index 1) was the offender" );
	}

	// None of the above touched the document (every rejection was a pure
	// param-validation refusal before AgentSession::InsertChunks was ever
	// called).
	Check( disp.Session() && disp.Session()->ReadDocument() == headBefore,
	       "IC3 none of the 4 validation refusals mutated the document" );

	pJob->release();
	std::remove( tmp.c_str() );
}

//----------------------------------------------------------------------
// IC4: insert_chunks WIRE SHAPE -- the ALL-APPLY (a) and BEST-EFFORT (c)
// cases driven through the REAL JSON-RPC transport
// (AgentRpcDispatcher::HandleLine), asserting the {applied,total,results:
// [...]} envelope IC1/IC2 could not see (those two exercise the C++
// AgentSession::InsertChunks API directly, not the wire serialization).
// Prints the raw response line for both cases.
//----------------------------------------------------------------------
static void TestInsertChunksWireShape()
{
	std::printf( "IC4: insert_chunks wire shape ({applied,total,results}) via HandleLine...\n" );

	// (a) ALL-APPLY over the wire.
	{
		const std::string tmp = TempPath( "agentcrud_ic4a.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "IC4(a) fixture loads" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			Agent::AgentRpcDispatcher disp( std::move( sess ) );

			const std::string resp = disp.HandleLine(
				"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"insert_chunks\",\"params\":{\"chunks\":["
				"\"uniformcolor_painter\\n{\\nname pnt_wire\\ncolor 0.1 0.9 0.3\\n}\","
				"\"lambertian_material\\n{\\nname mat_wire\\nreflectance pnt_wire\\n}\","
				"\"sphere_geometry\\n{\\nname sph_wire\\nradius 0.3\\n}\","
				"\"standard_object\\n{\\nname obj_wire\\ngeometry sph_wire\\nmaterial mat_wire\\nposition 1.5 0 0\\n}\""
				"]}}" );
			std::printf( "  IC4(a) all-apply raw response:\n  %s\n", resp.c_str() );

			Agent::JsonValue result;
			Check( JsonResultObj( resp, result ), "IC4(a) insert_chunks returns a JSON-RPC result object" );
			const Agent::JsonValue* applied = result.find( "applied" );
			const Agent::JsonValue* total   = result.find( "total" );
			const Agent::JsonValue* results = result.find( "results" );
			Check( applied && applied->isNumber() && applied->asNumber() == 4.0,
			       "IC4(a) wire 'applied' == 4" );
			Check( total && total->isNumber() && total->asNumber() == 4.0,
			       "IC4(a) wire 'total' == 4" );
			Check( results && results->isArray() && results->size() == 4,
			       "IC4(a) wire 'results' is a 4-element array" );
			if( results && results->isArray() && results->size() == 4 ) {
				for( std::size_t i = 0; i < 4; ++i ) {
					const Agent::JsonValue& e = results->at( i );
					const Agent::JsonValue* st = e.find( "status" );
					Check( st && st->isString() && st->asString() == "applied",
					       "IC4(a) every wire result element has status \"applied\"" );
				}
			}
			Check( pJob->GetObjects() && pJob->GetObjects()->GetItem( "obj_wire" ) != nullptr,
			       "IC4(a) the wire batch reached the live managers" );

			pJob->release();
		}
		std::remove( tmp.c_str() );
	}

	// (c) BEST-EFFORT over the wire: element 1 rejected, others applied.
	{
		const std::string tmp = TempPath( "agentcrud_ic4c.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "IC4(c) fixture loads" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			Agent::AgentRpcDispatcher disp( std::move( sess ) );

			const std::string resp = disp.HandleLine(
				"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"insert_chunks\",\"params\":{\"chunks\":["
				"\"uniformcolor_painter\\n{\\nname pnt_wire_c0\\ncolor 0.4 0.4 0.4\\n}\","
				"\"lambertian_material\\n{\\nname mat_wire_c1\\nreflectance nope_painter_wire\\n}\","
				"\"sphere_geometry\\n{\\nname sph_wire_c2\\nradius 0.2\\n}\","
				"\"standard_object\\n{\\nname obj_wire_c3\\ngeometry sph_wire_c2\\nmaterial mat_diffuse\\nposition -1.5 0 0\\n}\""
				"]}}" );
			std::printf( "  IC4(c) best-effort raw response:\n  %s\n", resp.c_str() );

			Agent::JsonValue result;
			Check( JsonResultObj( resp, result ), "IC4(c) insert_chunks returns a JSON-RPC result object" );
			const Agent::JsonValue* applied = result.find( "applied" );
			const Agent::JsonValue* total   = result.find( "total" );
			const Agent::JsonValue* results = result.find( "results" );
			Check( applied && applied->isNumber() && applied->asNumber() == 3.0,
			       "IC4(c) wire 'applied' == 3 (one rejection out of 4)" );
			Check( total && total->isNumber() && total->asNumber() == 4.0,
			       "IC4(c) wire 'total' == 4" );
			Check( results && results->isArray() && results->size() == 4,
			       "IC4(c) wire 'results' is a 4-element array" );
			if( results && results->isArray() && results->size() == 4 ) {
				const Agent::JsonValue& e0 = results->at( 0 );
				const Agent::JsonValue& e1 = results->at( 1 );
				const Agent::JsonValue& e2 = results->at( 2 );
				const Agent::JsonValue& e3 = results->at( 3 );
				const Agent::JsonValue* s0 = e0.find( "status" );
				const Agent::JsonValue* s1 = e1.find( "status" );
				const Agent::JsonValue* s2 = e2.find( "status" );
				const Agent::JsonValue* s3 = e3.find( "status" );
				Check( s0 && s0->asString() == "applied", "IC4(c) wire element 0 status \"applied\"" );
				Check( s1 && s1->asString() == "rejected", "IC4(c) wire element 1 status \"rejected\"" );
				Check( s2 && s2->asString() == "applied", "IC4(c) wire element 2 status \"applied\"" );
				Check( s3 && s3->asString() == "applied", "IC4(c) wire element 3 status \"applied\"" );
				const Agent::JsonValue* issues1 = e1.find( "issues" );
				Check( issues1 && issues1->isArray() && issues1->size() == 1,
				       "IC4(c) wire element 1 carries exactly one issue" );
			}

			pJob->release();
		}
		std::remove( tmp.c_str() );
	}
}

// A small helper for the propose_patches battery: build one patch element.
static Agent::AgentSetPatch Patch( const char* target, const char* param, const char* value )
{
	Agent::AgentSetPatch p;
	p.target = target;
	p.param  = param;
	p.value  = value;
	return p;
}

//----------------------------------------------------------------------
// PP1: propose_patches -- the BATCH form of propose_patch, over the wire.
//   Every target below EXISTS in kScene: an element naming an absent
//   entity is a REJECT, not an apply, so a happy-path assertion must not
//   smuggle one in (PP2 covers the reject path deliberately).
//----------------------------------------------------------------------
static void TestProposePatchesBatch()
{
	std::printf( "PP1: propose_patches batch -- multi-patch applied via RPC...\n" );

	const std::string tmp = TempPath( "agentcrud_pp1.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "PP1 fixture scene loads" );
	if( pJob )
	{
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
		Agent::AgentRpcDispatcher rpc( std::move( sess ) );

		const std::string req =
			"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"propose_patches\",\"params\":{"
			"\"patches\":["
			"{\"target\":\"sph\",\"param\":\"radius\",\"value\":\"0.5\"},"
			"{\"target\":\"mat_emit\",\"param\":\"scale\",\"value\":\"12.0\"}"
			"]}}";

		const std::string resp = rpc.HandleLine( req );
		Agent::JsonValue result;
		Check( JsonResultObj( resp, result ), "PP1 propose_patches returns a JSON-RPC result object" );
		const Agent::JsonValue* applied = result.find( "applied" );
		const Agent::JsonValue* total   = result.find( "total" );
		const Agent::JsonValue* results = result.find( "results" );
		Check( applied && applied->isNumber() && applied->asNumber() == 2.0, "PP1 applied == 2" );
		Check( total && total->isNumber() && total->asNumber() == 2.0, "PP1 total == 2" );
		Check( results && results->isArray() && results->size() == 2, "PP1 results has 2 elements" );
		// results[i] must be the SAME shape propose_patch returns -- the
		// documented wire contract the model's tool schema promises.
		if( results && results->isArray() && results->size() == 2 ) {
			const Agent::JsonValue& r0 = results->at( 0 );
			Check( r0.find( "applied" ) && r0.find( "applied" )->isBool(),
			       "PP1 results[0].applied is a BOOL (per-element), not the batch COUNT" );
			Check( r0.find( "status" ) && r0.find( "status" )->asString() == "applied",
			       "PP1 results[0].status == \"applied\"" );
			Check( r0.find( "rawCode" ) && r0.find( "headVersion" ) && r0.find( "message" ),
			       "PP1 results[0] carries the full propose_patch result shape" );
		}

		pJob->release();
	}
	std::remove( tmp.c_str() );
}

//----------------------------------------------------------------------
// PP2: BEST-EFFORT -- a REJECTED element does not stop the batch.
//----------------------------------------------------------------------
static void TestProposePatchesBestEffort()
{
	std::printf( "PP2: propose_patches -- BEST-EFFORT per-patch results (headless)...\n" );
	const std::string tmp = TempPath( "agentcrud_pp2.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "PP2 fixture loads" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

	std::vector<Agent::AgentSetPatch> patches;
	patches.push_back( Patch( "sph", "radius", "0.5" ) );
	// Names an entity that does not exist anywhere in the document -- a
	// hard REJECT that must NOT abort the elements after it.
	patches.push_back( Patch( "no_such_entity_xyz", "radius", "1.0" ) );
	patches.push_back( Patch( "mat_emit", "scale", "12.0" ) );

	const std::vector<Agent::AgentPatchResult> results = sess->ProposePatches( patches );
	Check( results.size() == 3, "PP2 returns one result per input patch" );
	if( results.size() == 3 ) {
		Check( results[0].applied && results[0].status == "applied", "PP2 element 0 applied" );
		Check( !results[1].applied && results[1].status == "rejected",
		       "PP2 element 1 (unknown target) is REJECTED" );
		Check( results[2].applied && results[2].status == "applied",
		       "PP2 BEST-EFFORT: element 2 still applied after the rejected element 1" );
		// The rejection must be ACTIONABLE, exactly as the single-item verb is.
		Check( !results[1].issues.empty(), "PP2 the rejected element carries an actionable issue" );
		if( !results[1].issues.empty() ) {
			Check( results[1].issues[0].reason == "unknown_target",
			       "PP2 issue reason is \"unknown_target\"" );
		}
	}

	int appliedCount = 0;
	for( const Agent::AgentPatchResult& r : results ) if( r.applied ) ++appliedCount;
	Check( appliedCount == 2, "PP2 applied == total - 1 (exactly one rejection)" );

	std::remove( tmp.c_str() );
}

//----------------------------------------------------------------------
// PP3: wire validation -- malformed `patches` is -32602 and applies NOTHING.
//----------------------------------------------------------------------
static void TestProposePatchesValidation()
{
	std::printf( "PP3: propose_patches wire validation (missing/non-array/empty/bad item -> -32602)...\n" );
	const std::string tmp = TempPath( "agentcrud_pp3.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "PP3 fixture loads" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const std::string headBefore = sess->ReadDocument();
	Agent::AgentRpcDispatcher disp( std::move( sess ) );

	struct Case { const char* body; const char* what; };
	static const Case kCases[] = {
		{ "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"propose_patches\",\"params\":{}}",
		  "PP3(1) without 'patches' -> -32602" },
		{ "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"propose_patches\",\"params\":"
		  "{\"patches\":\"sph\"}}",
		  "PP3(2) non-array 'patches' -> -32602" },
		{ "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"propose_patches\",\"params\":"
		  "{\"patches\":[]}}",
		  "PP3(3) empty 'patches' array -> -32602" },
		{ "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"propose_patches\",\"params\":"
		  "{\"patches\":[\"sph\"]}}",
		  "PP3(4) a non-object element -> -32602" },
		{ "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"propose_patches\",\"params\":"
		  "{\"patches\":[{\"target\":\"sph\",\"param\":\"radius\"}]}}",
		  "PP3(5) an element missing 'value' -> -32602" },
		{ "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"propose_patches\",\"params\":"
		  "{\"patches\":[{\"target\":\"sph\",\"param\":\"radius\",\"value\":\"0.5\",\"kind\":7}]}}",
		  "PP3(6) an element with a non-string 'kind' -> -32602" },
	};
	for( const Case& c : kCases ) {
		const std::string resp = disp.HandleLine( c.body );
		Check( resp.find( "-32602" ) != std::string::npos, c.what );
	}

	// A WRONG KEY names itself in the same round-trip (the insert_chunks
	// guidance posture), so a model can fix it without a schema re-read.
	{
		const std::string resp = disp.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"propose_patches\",\"params\":"
			"{\"edits\":[{\"target\":\"sph\",\"param\":\"radius\",\"value\":\"0.5\"}]}}" );
		Check( resp.find( "-32602" ) != std::string::npos, "PP3(7) a wrong key -> -32602" );
		Check( resp.find( "edits" ) != std::string::npos,
		       "PP3(7) the error NAMES the key that was sent instead" );
	}

	// VALIDATE-BEFORE-APPLY: every rejection above left the head untouched,
	// including case (5)/(6) whose FIRST element was perfectly well-formed.
	Check( disp.Session() && disp.Session()->ReadDocument() == headBefore,
	       "PP3 a -32602 batch applied NOTHING (head byte-identical)" );

	std::remove( tmp.c_str() );
}

//----------------------------------------------------------------------
// PP4: a STALE-BASE CONFLICT is BATCH-FATAL -- the lost-update guard.
//   This is the ONE place propose_patches deliberately diverges from
//   insert_chunks: an insert is additive, a patch OVERWRITES, so
//   continuing past a stale base would blind-clobber a co-editor.
//----------------------------------------------------------------------
static void TestProposePatchesConflictIsBatchFatal()
{
	std::printf( "PP4: propose_patches -- a stale base is BATCH-FATAL (no partial clobber)...\n" );
	const std::string tmp = TempPath( "agentcrud_pp4.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "PP4 fixture loads" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

	std::vector<Agent::AgentSetPatch> patches;
	patches.push_back( Patch( "sph", "radius", "0.5" ) );
	patches.push_back( Patch( "mat_emit", "scale", "12.0" ) );
	patches.push_back( Patch( "pnt_albedo", "color", "0.2 0.3 0.4" ) );

	RISE::Cst::CstHeadVersion stale = sess->HeadVersion();
	stale.revision += 100;

	const std::string headBefore = sess->ReadDocument();
	const RISE::Cst::CstHeadVersion vBefore = sess->HeadVersion();

	const std::vector<Agent::AgentPatchResult> results = sess->ProposePatches( patches, &stale );
	Check( results.size() == 3,
	       "PP4 results still has ONE entry per input patch (results[i] <-> patches[i])" );
	int appliedCount = 0;
	for( const Agent::AgentPatchResult& r : results ) if( r.applied ) ++appliedCount;
	Check( appliedCount == 0, "PP4 NOTHING applied -- the batch precondition failed" );
	if( results.size() == 3 ) {
		Check( results[0].status == "conflict", "PP4 element 0 reports the conflict" );
		Check( results[1].status == "conflict" && results[2].status == "conflict",
		       "PP4 the unattempted tail is reported as \"conflict\", not silently applied" );
		Check( results[2].message.find( "not attempted" ) != std::string::npos,
		       "PP4 the tail's message says it was NOT ATTEMPTED (and why)" );
	}
	Check( sess->ReadDocument() == headBefore, "PP4 the head is byte-identical" );
	Check( sess->HeadVersion() == vBefore, "PP4 the revision did not move" );

	// RED-PROVE the gate: the IDENTICAL batch WITHOUT a base applies all
	// three -- so the abort above really was the precondition's doing and
	// not some unrelated rejection of these patches.
	const std::vector<Agent::AgentPatchResult> ok = sess->ProposePatches( patches );
	int okCount = 0;
	for( const Agent::AgentPatchResult& r : ok ) if( r.applied ) ++okCount;
	Check( okCount == 3, "PP4 RED-PROVE: the same batch with NO base applies all 3" );
	Check( sess->ReadDocument() != headBefore, "PP4 RED-PROVE: that batch really did mutate the head" );

	std::remove( tmp.c_str() );
}

int main()
{
	std::printf( "=== AgentChunkCrudTest (Model-B F5 slice S2: insert_chunk / remove_chunk) ===\n" );

	TestHeadlessInsert();
	TestInsertRejections();
	TestRemove();
	TestConflictGate();
	TestTriviaContracts();
	TestCloneOnlyRemoveRedProve();
	TestInsertRemoveSymmetry();
	TestLiveControllerPath();
	TestLiveDispatcherChunkCrud();
	TestAgentEditDerivabilityGate();
	TestRenameRecipeEndToEnd();
	TestRasterizerInsertActivation();
	TestVariantOverlayAndAmbiguityMessages();
	TestCameraSwapRecipe();
	TestUnnamedRepeatableTimeline();
	TestRemoveChunkNameKeywordCoincidence();
	TestCameraTimelineNamedTargeting();
	TestReservedCameraNameNoneAtDerive();
	TestGltfImportPrefixCollision();
	TestUnresolvedReferenceWarning();
	TestRejectedInsertDiagnostics();
	TestActionablePatchDiagnostics();
	TestActionableRemoveDiagnostics();
	TestInsertChunksBatchAllApply();
	TestInsertChunksBestEffort();
	TestInsertChunksValidation();
	TestInsertChunksWireShape();
	TestProposePatchesBatch();
	TestProposePatchesBestEffort();
	TestProposePatchesValidation();
	TestProposePatchesConflictIsBatchFatal();

	std::printf( "AgentChunkCrudTest: %d passed, %d failed\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
