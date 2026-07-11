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

#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/Interfaces/IMaterialManager.h"
#include "../src/Library/Interfaces/ILightManager.h"
#include "../src/Library/Interfaces/IObjectManager.h"
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

	std::printf( "AgentChunkCrudTest: %d passed, %d failed\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
