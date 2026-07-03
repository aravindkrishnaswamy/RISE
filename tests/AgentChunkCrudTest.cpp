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
	Check( !rmRef.message.empty() && rmRef.message.find( ": " ) != std::string::npos,
	       "the refusal carries the dry-run diagnostic detail" );
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
	Check( rC.message.find( "stale baseHeadVersion" ) != std::string::npos,
	       "conflict message reports the stale base" );
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

	std::printf( "AgentChunkCrudTest: %d passed, %d failed\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
