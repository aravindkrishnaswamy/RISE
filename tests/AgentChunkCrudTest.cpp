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
//    E1  Post-arc enforcement: the LUMINAIRE_NULL_GEOMETRY Warning gets a
//        CREATION-TIME BLOCK.  insert_chunk of an emissive-bound
//        csg_object without `allow_non_sampling_emitter TRUE` is REFUSED
//        (consequence + both escapes named, head unchanged); the SAME
//        insert WITH the flag applies and Validate goes silent;
//        propose_patch re-pointing an existing csg_object's `material` to
//        an emitter is refused the same way; an insert_chunks batch with
//        one offending + one clean element splits per-item (BEST-EFFORT);
//        a scene FILE carrying the unacknowledged construct still only
//        WARNS (R1/T6d's pre-existing contract, unchanged), and the
//        acknowledged file-loaded construct is silent.  Fix rounds:
//        editing a referenced MATERIAL's own emissive-capable param in
//        place (never touching the csg_object) is refused too, naming
//        the referencing csg; removing an acknowledgment RE-creates the
//        construct and is refused; a proposal staged while innocent that
//        becomes dangerous before it is approved is refused at RESOLVE
//        time.  Round-2 fix: the material-side check is DELTA-based, not
//        state-based -- a pre-existing unacknowledged construct a scene
//        FILE already carries is Validate's job to keep Warning about, so
//        an edit to that material UNRELATED to emission (e.g. alphax)
//        still APPLIES; only an edit that CREATES or WORSENS the
//        unacknowledged state is refused.
//
//  Self-contained: no RISE_MEDIA_PATH, inline native-v7 scenes, OIDN off.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <atomic>
#include <cctype>
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
#include "../src/Library/Interfaces/IPainter.h"
#include "../src/Library/Interfaces/IPainterManager.h"
#include "../src/Library/Interfaces/IScalarPainter.h"
#include "../src/Library/Interfaces/IScalarPainterManager.h"
#include "../src/Library/Interfaces/IGeometry.h"
#include "../src/Library/Interfaces/IGeometryManager.h"
#include "../src/Library/Interfaces/IRasterImageReader.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/SceneEditor/CameraIntrospection.h"
#include "../src/Library/SceneEditor/SceneEditController.h"
#include "../src/Library/Utilities/MemoryBuffer.h"
#include "../src/Library/Utilities/Color/Color.h"
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

	// KIND VERIFICATION: kind is now a hard lookup constraint even for a
	// unique bare name, so a mismatched target resolves as not found.
	const std::string headKind = sess->ReadDocument();
	Agent::AgentChunkResult rmWrongKind = sess->RemoveChunk( "sph", "material" );
	Check( !rmWrongKind.applied && rmWrongKind.status == "rejected",
	       "remove of a uniquely-named target under the WRONG kind is refused" );
	Check( !rmWrongKind.message.empty(),
	       "the wrong-kind refusal carries a diagnostic" );
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

		// (3) A UI scrub is also an open EditHistory composite.  Agent
		// commits must not land inside it: the UI thread would otherwise
		// absorb the agent record into "Scrub", breaking both undo grouping
		// and the transaction boundary.
		c.OnTimeScrubBegin();
		const std::string headBeforeScrub = sess->ReadDocument();
		Agent::AgentChunkResult rScrub = sess->InsertChunk(
			"omni_light\n{\nname gesturekey\nposition 2 3 1\ncolor 1 1 1\npower 1.0\n}" );
		Check( !rScrub.applied && rScrub.status == "rejected",
		       "mid-scrub insert refused" );
		Check( rScrub.retriable, "the scrub refusal is RETRIABLE (transient)" );
		Check( sess->ReadDocument() == headBeforeScrub, "the scrub refusal mutated nothing" );
		c.OnTimeScrubEnd();
		Agent::AgentChunkResult rScrubRetry = sess->InsertChunk(
			"omni_light\n{\nname gesturekey\nposition 2 3 1\ncolor 1 1 1\npower 1.0\n}" );
		Check( rScrubRetry.applied, "the identical insert succeeds after the scrub completes" );

		// (4) Conflict through the live path.
		RISE::Cst::CstHeadVersion stale = sess->HeadVersion();
		stale.revision += 50;
		Agent::AgentChunkResult rC = sess->RemoveChunk( "livekey", "", &stale );
		Check( !rC.applied && rC.status == "conflict", "stale-base remove conflicts through the controller" );

		// (5) Remove through the live path.
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
// (the exact wiring BOTH GUI agent entry points drive -- agentHandleToolCall
// for chat tool calls, agentHandleLine for the raw JSON-RPC debug panel;
// separate AgentSessions, same dispatcher shape over the same controller).
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
		SceneEditController controller( *pJob, /*interactiveRasterizer*/0 );
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
		sess->AttachController( &controller );

		Check( sess->InsertChunk(
			"timeline\n{\n\telement_type object\n\telement obj_sph\n\tparam position\n"
			"\ttime 0\n\tvalue 0 0 0\n\ttime 1\n\tvalue 1 0 0\n}" ).applied,
			"scenario-B: the sole unnamed timeline inserts" );
		const std::string documentWithTimeline = sess->ReadDocument();

		Agent::AgentChunkResult rmOne = sess->RemoveChunk( "timeline", "timeline" );
		Check( rmOne.applied && rmOne.status == "applied",
		       "the SOLE unnamed timeline removes cleanly (positional unique-in-kind fallback)" );
		Check( rmOne.kind == "timeline", "the sole-timeline remove echoes the keyword" );
		Check( sess->ReadDocument().find( "timeline" ) == std::string::npos,
		       "the timeline chunk is gone from the head" );
		controller.Undo();
		Check( sess->ReadDocument() == documentWithTimeline,
		       "Undo restores the sole unnamed timeline byte-for-byte" );
		controller.Redo();
		Check( sess->ReadDocument().find( "timeline" ) == std::string::npos,
		       "Redo removes the sole unnamed timeline again" );

		sess->AttachController( nullptr );
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
// E1: post-arc enforcement (docs/agentic-redesign/75-expressive-surface-
// arc.md sec 7 / 76-...-log.md sec 3's mechanism law -- blocking facts
// act, a Warning gets skimmed): the LUMINAIRE_NULL_GEOMETRY Warning
// (R1(a)-adjacent, T6d in AgentEvalCheckTest.cpp) is now paired with a
// CREATION-TIME BLOCK.  A csg_object has no directly-owned geometry, so
// an emissive material bound to it is never NEE-light-sampled -- glow-
// only-on-direct-view.  insert_chunk / propose_patch REFUSE the edit that
// would CREATE that binding unless the csg_object chunk carries
// `allow_non_sampling_emitter TRUE`; a scene FILE loaded with the
// unacknowledged construct still only WARNS (R1/T6d's PRE-EXISTING
// contract, unchanged); the same flag silences that Warning too.
//
// Fix round (fresh review): THREE more ways to land the SAME construct --
// (P1-1) editing the MATERIAL's own emissive-capable param in place while
// a csg_object already references it (never touching the csg_object
// chunk at all); (P1-2) removing `allow_non_sampling_emitter` from an
// already-acknowledged emissive csg (a two-call bypass: insert
// acknowledged, then patch the flag away); (P1-3) a proposal staged while
// INNOCENT that becomes dangerous by the time it is APPROVED, because the
// world moved underneath it while it sat in the queue.  (g)-(j) cover the
// first two; (k) covers the third via SceneEditController's stage/
// resolve seam.  The refusal wording (P2a fix) is the PRECISE, VERIFIED
// claim ValidateText's Warning uses -- NEE light-sampling is where the
// gap is; direct-view AND a BSDF-sampled hit both still contribute.
//
// Round-2 fix round: (g)'s material-side check was STATE-based (does the
// candidate come back unacknowledged?), which refused ANY edit -- even
// alphax/roughness, nothing to do with emission -- on a material a
// PRE-EXISTING unacknowledged construct already references, forever,
// once that construct existed.  (l) red-proves the DELTA-based
// replacement: only an edit that CREATES or WORSENS the unacknowledged
// state is refused; a pre-existing one (Validate's job) does not freeze
// unrelated edits.
//----------------------------------------------------------------------
static const char* const kCsgReadyScene =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 16\n\theight 16\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 6\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 50.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname albedo\n\tcolor 0.8 0.8 0.8\n}\n\n"
	"lambertian_material\n{\n\tname matte\n\treflectance albedo\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_glow\n\tcolor 3.0 2.5 1.5\n}\n\n"
	"lambertian_luminaire_material\n{\n\tname mat_glow\n\texitance pnt_glow\n\tmaterial matte\n\tscale 3.0\n}\n\n"
	// P1-1/P1-3 vehicle: a MATERIAL-category chunk with its OWN inline
	// `emissive` param (default "none" -- non-emissive as authored here),
	// distinct from mat_glow's separate-wrapper-chunk idiom above.  Not
	// referenced by anything in the base fixture; each sub-test below
	// binds it (or not) as its scenario needs.
	"ggx_material\n{\n\tname mat_ggx\n\trd albedo\n\trs albedo\n\talphax 0.2\n\talphay 0.2\n\tior 1.5\n\textinction 0.0\n}\n\n"
	"sphere_geometry\n{\n\tname sph_a\n\tradius 0.6\n}\n\n"
	"sphere_geometry\n{\n\tname sph_b\n\tradius 0.6\n}\n\n"
	"standard_object\n{\n\tname csg_opA\n\tgeometry sph_a\n\tmaterial matte\n}\n\n"
	"standard_object\n{\n\tname csg_opB\n\tgeometry sph_b\n\tposition 0.35 0 0\n\tmaterial matte\n}\n";

static void TestNonSamplingEmitterGate()
{
	std::printf( "E1: non-sampling-emitter creation gate (insert/patch refuse; ack flag escapes)...\n" );

	// (a) insert_chunk: a csg_object bound to the emissive material, no
	// acknowledgement flag -> REFUSED, message names the PRECISE
	// consequence (NEE light-sampling specifically -- P2a fix: NOT the
	// overclaiming "will never illuminate" / "only... direct camera view"
	// text an earlier round shipped) and BOTH escapes (real geometry, or
	// the acknowledgement flag).  Head byte-identical, revision unmoved.
	//
	// (b) the SAME insert, WITH the flag -> applies; a subsequent Validate
	// is SILENT (no LUMINAIRE_NULL_GEOMETRY at all -- an acknowledged
	// choice must not nag).
	{
		const std::string tmp = TempPath( "agentcrud_e1a.RISEscene" );
		Job* pJob = LoadScene( kCsgReadyScene, tmp );
		Check( pJob != nullptr, "E1(a) fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

		const std::string headBefore = sess->ReadDocument();
		const RISE::Cst::CstHeadVersion vBefore = sess->HeadVersion();

		Agent::AgentChunkResult r = sess->InsertChunk(
			"csg_object\n{\n\tname csg_glow\n\tobja csg_opA\n\tobjb csg_opB\n\toperation union\n\tmaterial mat_glow\n}" );
		Check( !r.applied && r.status == "rejected",
		       "E1(a) inserting an emissive-bound csg_object WITHOUT the flag is REFUSED" );
		Check( r.message.find( "NOT act as an area light for next-event estimation" ) != std::string::npos,
		       "E1(a) message states the PRECISE consequence (NEE light-sampling specifically)" );
		Check( r.message.find( "BSDF-sampled hit" ) != std::string::npos,
		       "E1(a) message honestly names the paths that STILL contribute (P2a: not overclaiming total invisibility)" );
		Check( r.message.find( "allow_non_sampling_emitter" ) != std::string::npos,
		       "E1(a) message states the ESCAPE (acknowledgement flag)" );
		Check( r.message.find( "standard_object" ) != std::string::npos,
		       "E1(a) message ALSO states the other fix (real-geometry object)" );
		Check( sess->ReadDocument() == headBefore, "E1(a) the refusal leaves the head byte-identical" );
		Check( sess->HeadVersion() == vBefore, "E1(a) the refusal leaves the revision unmoved" );
		std::printf( "  E1(a) message: %s\n", r.message.c_str() );

		Agent::AgentChunkResult r2 = sess->InsertChunk(
			"csg_object\n{\n\tname csg_glow\n\tobja csg_opA\n\tobjb csg_opB\n\toperation union\n"
			"\tmaterial mat_glow\n\tallow_non_sampling_emitter TRUE\n}" );
		Check( r2.applied && r2.status == "applied",
		       "E1(b) the SAME insert WITH `allow_non_sampling_emitter TRUE` APPLIES" );
		if( r2.applied ) {
			const std::vector<Agent::AgentDiagnostic> diags =
				Agent::AgentSession::ValidateText( sess->ReadDocument() );
			bool sawCode = false;
			for( const Agent::AgentDiagnostic& d : diags )
				if( d.code == "LUMINAIRE_NULL_GEOMETRY" ) sawCode = true;
			Check( !sawCode, "E1(b) validate is SILENT on the acknowledged construct -- no nag" );
		}

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}

	// (c) propose_patch: an EXISTING csg_object (bound to a non-emissive
	// material) has its `material` re-pointed to the emissive one WITHOUT
	// the flag -> REFUSED.  P2b fix: pin the revision (not just the byte-
	// identical document) before/after, matching E1(a)'s asymmetry.
	{
		const std::string tmp = TempPath( "agentcrud_e1c.RISEscene" );
		std::string scene = kCsgReadyScene;
		scene += "csg_object\n{\n\tname csg_plain\n\tobja csg_opA\n\tobjb csg_opB\n"
		         "\toperation union\n\tmaterial matte\n}\n";
		Job* pJob = LoadScene( scene.c_str(), tmp );
		Check( pJob != nullptr, "E1(c) fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

		const std::string headBefore = sess->ReadDocument();
		const RISE::Cst::CstHeadVersion vBefore = sess->HeadVersion();

		Agent::AgentSetPatch p;
		p.target = "csg_plain";
		p.kind   = "csg_object";
		p.param  = "material";
		p.value  = "mat_glow";
		Agent::AgentPatchResult r = sess->ProposePatch( p );
		Check( !r.applied && r.status == "rejected",
		       "E1(c) patching a csg_object's material to an emitter WITHOUT the flag is REFUSED" );
		Check( r.message.find( "NOT act as an area light for next-event estimation" ) != std::string::npos,
		       "E1(c) message states the precise consequence" );
		Check( r.message.find( "BSDF-sampled hit" ) != std::string::npos,
		       "E1(c) message honestly names the paths that still contribute" );
		Check( r.message.find( "allow_non_sampling_emitter" ) != std::string::npos,
		       "E1(c) message states the escape" );
		Check( sess->ReadDocument() == headBefore, "E1(c) the refusal leaves the head byte-identical" );
		Check( sess->HeadVersion() == vBefore, "E1(c) [P2b] the refusal leaves the revision unmoved" );

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}

	// (d) insert_chunks BATCH: one offending element (emissive csg_object,
	// no flag) alongside one clean, unrelated element -> PER-ITEM split;
	// the clean element still applies (BEST-EFFORT, same contract as IC2).
	{
		const std::string tmp = TempPath( "agentcrud_e1d.RISEscene" );
		Job* pJob = LoadScene( kCsgReadyScene, tmp );
		Check( pJob != nullptr, "E1(d) fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

		std::vector<std::string> chunks;
		chunks.push_back(
			"csg_object\n{\n\tname csg_glow_batch\n\tobja csg_opA\n\tobjb csg_opB\n\toperation union\n\tmaterial mat_glow\n}" );
		chunks.push_back(
			"sphere_geometry\n{\n\tname sph_clean\n\tradius 0.2\n}" );

		const std::vector<Agent::AgentChunkResult> results = sess->InsertChunks( chunks );
		Check( results.size() == 2, "E1(d) one result per input chunk" );
		if( results.size() == 2 ) {
			Check( !results[0].applied && results[0].status == "rejected",
			       "E1(d) the offending element is REFUSED" );
			Check( results[0].message.find( "allow_non_sampling_emitter" ) != std::string::npos,
			       "E1(d) the refusal names the escape" );
			Check( results[1].applied && results[1].status == "applied",
			       "E1(d) BEST-EFFORT: the unrelated clean element still applies" );
		}
		Check( pJob->GetObjects() && pJob->GetObjects()->GetItem( "csg_glow_batch" ) == nullptr,
		       "E1(d) the refused csg_object never reached the live managers" );
		Check( pJob->GetGeometry( "sph_clean" ) != nullptr,
		       "E1(d) the clean element DID reach the live managers" );

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}

	// (e) scene-FILE load of the UNACKNOWLEDGED construct -- unchanged
	// behaviour: loads fine (the derive-side gate is agent-edit-only, the
	// construct DERIVES legally), and Validate still WARNS (R1/T6d's
	// pre-existing contract, untouched).
	{
		const std::string tmp = TempPath( "agentcrud_e1e.RISEscene" );
		std::string scene = kCsgReadyScene;
		scene += "csg_object\n{\n\tname csg_glow\n\tobja csg_opA\n\tobjb csg_opB\n"
		         "\toperation union\n\tmaterial mat_glow\n}\n";
		Job* pJob = LoadScene( scene.c_str(), tmp );
		Check( pJob != nullptr, "E1(e) a scene FILE carrying the unacknowledged construct loads cleanly" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const std::vector<Agent::AgentDiagnostic> diags =
				Agent::AgentSession::ValidateText( sess->ReadDocument() );
			bool sawWarning = false;
			for( const Agent::AgentDiagnostic& d : diags )
				if( d.code == "LUMINAIRE_NULL_GEOMETRY" && d.severity == Agent::AgentDiagnostic::Severity::Warning )
					sawWarning = true;
			Check( sawWarning, "E1(e) an unacknowledged file-loaded construct still WARNS (unchanged)" );
			sess.reset();
			pJob->release();
		}
		std::remove( tmp.c_str() );
	}

	// (f) scene-FILE load of the ACKNOWLEDGED construct -- loads AND
	// validate is silent (the flag suppresses the Warning too, not just
	// the agent-edit gate).
	{
		const std::string tmp = TempPath( "agentcrud_e1f.RISEscene" );
		std::string scene = kCsgReadyScene;
		scene += "csg_object\n{\n\tname csg_glow\n\tobja csg_opA\n\tobjb csg_opB\n\toperation union\n"
		         "\tmaterial mat_glow\n\tallow_non_sampling_emitter TRUE\n}\n";
		Job* pJob = LoadScene( scene.c_str(), tmp );
		Check( pJob != nullptr, "E1(f) a scene FILE carrying the ACKNOWLEDGED construct loads cleanly" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const std::vector<Agent::AgentDiagnostic> diags =
				Agent::AgentSession::ValidateText( sess->ReadDocument() );
			bool sawCode = false;
			for( const Agent::AgentDiagnostic& d : diags )
				if( d.code == "LUMINAIRE_NULL_GEOMETRY" ) sawCode = true;
			Check( !sawCode, "E1(f) an acknowledged file-loaded construct is SILENT" );
			sess.reset();
			pJob->release();
		}
		std::remove( tmp.c_str() );
	}

	// (g) [P1-1] MATERIAL-side creation: mat_ggx starts non-emissive and a
	// csg_object (csg_ggx) already references it; patching mat_ggx's OWN
	// `emissive` param (never touching csg_ggx at all) is REFUSED, and the
	// message NAMES the referencing csg_object -- actionable from the
	// material side, where the fix ("csg_ggx has no directly-owned
	// geometry...") is not obvious from the material chunk alone.
	{
		const std::string tmp = TempPath( "agentcrud_e1g.RISEscene" );
		std::string scene = kCsgReadyScene;
		scene += "csg_object\n{\n\tname csg_ggx\n\tobja csg_opA\n\tobjb csg_opB\n"
		         "\toperation union\n\tmaterial mat_ggx\n}\n";
		Job* pJob = LoadScene( scene.c_str(), tmp );
		Check( pJob != nullptr, "E1(g) fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

		const std::string headBefore = sess->ReadDocument();

		Agent::AgentSetPatch p;
		p.target = "mat_ggx";
		p.kind   = "ggx_material";
		p.param  = "emissive";
		p.value  = "pnt_glow";
		Agent::AgentPatchResult r = sess->ProposePatch( p );
		Check( !r.applied && r.status == "rejected",
		       "E1(g) [P1-1] editing a MATERIAL's own emissive param while a csg references it is REFUSED" );
		Check( r.message.find( "csg_ggx" ) != std::string::npos,
		       "E1(g) message NAMES the referencing csg_object (actionable from the material side)" );
		Check( r.message.find( "NOT act as an area light for next-event estimation" ) != std::string::npos,
		       "E1(g) message states the precise consequence" );
		Check( sess->ReadDocument() == headBefore, "E1(g) the refusal leaves the head byte-identical" );
		std::printf( "  E1(g) message: %s\n", r.message.c_str() );

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}

	// (h) [P1-1] the SAME material-side edit, but the referencing csg is
	// ALREADY acknowledged -> APPLIES (the material-side gate is scoped to
	// UNACKNOWLEDGED referencing csg_objects only).
	{
		const std::string tmp = TempPath( "agentcrud_e1h.RISEscene" );
		std::string scene = kCsgReadyScene;
		scene += "csg_object\n{\n\tname csg_ggx_ack\n\tobja csg_opA\n\tobjb csg_opB\n\toperation union\n"
		         "\tmaterial mat_ggx\n\tallow_non_sampling_emitter TRUE\n}\n";
		Job* pJob = LoadScene( scene.c_str(), tmp );
		Check( pJob != nullptr, "E1(h) fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

		Agent::AgentSetPatch p;
		p.target = "mat_ggx";
		p.kind   = "ggx_material";
		p.param  = "emissive";
		p.value  = "pnt_glow";
		Agent::AgentPatchResult r = sess->ProposePatch( p );
		Check( r.applied && r.status == "applied",
		       "E1(h) [P1-1] the SAME material-side edit APPLIES when the referencing csg is already acknowledged" );

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}

	// (i) [P1-2] removing the acknowledgment from an ALREADY-emissive,
	// already-acknowledged csg_object -> REFUSED (would RECREATE the
	// construct -- the two-call bypass the fresh review found).
	{
		const std::string tmp = TempPath( "agentcrud_e1i.RISEscene" );
		std::string scene = kCsgReadyScene;
		scene += "csg_object\n{\n\tname csg_ack\n\tobja csg_opA\n\tobjb csg_opB\n\toperation union\n"
		         "\tmaterial mat_glow\n\tallow_non_sampling_emitter TRUE\n}\n";
		Job* pJob = LoadScene( scene.c_str(), tmp );
		Check( pJob != nullptr, "E1(i) fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

		// RED-PROVE the fixture: silent BEFORE the removal attempt (the
		// acknowledgment is doing real work) -- else the refusal below
		// would be trivially unfalsifiable.
		{
			const std::vector<Agent::AgentDiagnostic> diags =
				Agent::AgentSession::ValidateText( sess->ReadDocument() );
			bool sawCode = false;
			for( const Agent::AgentDiagnostic& d : diags ) if( d.code == "LUMINAIRE_NULL_GEOMETRY" ) sawCode = true;
			Check( !sawCode, "E1(i) RED-PROVE: the fixture starts silent (genuinely acknowledged)" );
		}

		Agent::AgentSetPatch p;
		p.target = "csg_ack";
		p.kind   = "csg_object";
		p.param  = "allow_non_sampling_emitter";
		p.value  = "FALSE";
		Agent::AgentPatchResult r = sess->ProposePatch( p );
		Check( !r.applied && r.status == "rejected",
		       "E1(i) [P1-2] removing the acknowledgment from an emissive-bound csg is REFUSED" );
		Check( r.message.find( "RECREATE" ) != std::string::npos,
		       "E1(i) message frames this as RECREATING the already-refused construct" );
		Check( r.message.find( "keep the" ) != std::string::npos,
		       "E1(i) message's escape is 'keep the flag' (distinct framing from the 'add the flag' creation-arm message)" );
		std::printf( "  E1(i) message: %s\n", r.message.c_str() );

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}

	// (j) [P1-2 negation] removing the flag from a csg bound to a NON-
	// emissive material APPLIES -- the flag alone is not load-bearing;
	// only removing it FROM AN EMISSIVE BINDING is refused.
	{
		const std::string tmp = TempPath( "agentcrud_e1j.RISEscene" );
		std::string scene = kCsgReadyScene;
		scene += "csg_object\n{\n\tname csg_vacuous_ack\n\tobja csg_opA\n\tobjb csg_opB\n\toperation union\n"
		         "\tmaterial matte\n\tallow_non_sampling_emitter TRUE\n}\n";
		Job* pJob = LoadScene( scene.c_str(), tmp );
		Check( pJob != nullptr, "E1(j) fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

		Agent::AgentSetPatch p;
		p.target = "csg_vacuous_ack";
		p.kind   = "csg_object";
		p.param  = "allow_non_sampling_emitter";
		p.value  = "FALSE";
		Agent::AgentPatchResult r = sess->ProposePatch( p );
		Check( r.applied && r.status == "applied",
		       "E1(j) [P1-2 negation] removing the flag from a csg bound to a NON-emissive material APPLIES" );

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}

	// (k) [P1-3] a proposal staged while INNOCENT becomes dangerous by the
	// time it is APPROVED: External stages a csg_object material re-point
	// while the target material is still non-emissive (stages cleanly);
	// an Owner direct edit then makes that material emissive (applies
	// cleanly -- nothing references it live yet, only the PENDING
	// proposal would); approving the now-stale proposal must be REFUSED,
	// not silently land the construct.
	{
		const std::string tmp = TempPath( "agentcrud_e1k.RISEscene" );
		std::string scene = kCsgReadyScene;
		scene += "csg_object\n{\n\tname csg_stale\n\tobja csg_opA\n\tobjb csg_opB\n"
		         "\toperation union\n\tmaterial matte\n}\n";
		Job* pJob = LoadScene( scene.c_str(), tmp );
		Check( pJob != nullptr, "E1(k) fixture loads" );
		if( !pJob ) return;

		TestController c( *pJob, /*simulatedRenderMs*/ 0 );
		c.Start();

		std::unique_ptr<Agent::AgentSession> owner = Agent::AgentSession::WrapJob( pJob, Agent::AgentAuthority::Owner );
		std::unique_ptr<Agent::AgentSession> ext   = Agent::AgentSession::WrapJob( pJob, Agent::AgentAuthority::External );
		owner->AttachController( &c );
		ext->AttachController( &c );

		Agent::AgentSetPatch stagePatch;
		stagePatch.target = "csg_stale";
		stagePatch.kind   = "csg_object";
		stagePatch.param  = "material";
		stagePatch.value  = "mat_ggx";
		Agent::AgentPatchResult staged = ext->ProposePatch( stagePatch );
		Check( staged.status == "staged",
		       "E1(k) the re-point stages CLEANLY -- innocent at stage time (mat_ggx is not yet emissive)" );
		std::uint64_t id = 0;
		for( const auto& p : owner->ListProposals() ) if( p.status == "pending" ) id = p.id;
		Check( id != 0, "E1(k) the proposal is pending" );

		// The world moves: an OWNER direct edit makes mat_ggx emissive.
		// No csg currently references mat_ggx yet -- csg_stale still
		// points at `matte` LIVE; the re-point is only PENDING -- so this
		// edit is not itself refused by the material-side gate.
		Agent::AgentSetPatch makeEmissive;
		makeEmissive.target = "mat_ggx";
		makeEmissive.kind   = "ggx_material";
		makeEmissive.param  = "emissive";
		makeEmissive.value  = "pnt_glow";
		Agent::AgentPatchResult em = owner->ProposePatch( makeEmissive );
		Check( em.applied, "E1(k) the intervening edit (nobody references mat_ggx yet) applies cleanly" );

		// Approve the now-dangerous stale proposal.
		Agent::AgentSession::AgentResolveResult rr = owner->ResolveProposal( id, /*approve=*/true );
		Check( rr.ok, "E1(k) resolve runs (the id is found)" );
		Check( rr.status == "rejected",
		       "E1(k) RED-PROVE: the stale-but-now-dangerous approve is REFUSED at resolve time" );
		Check( rr.message.find( "resolve refused" ) != std::string::npos,
		       "E1(k) message carries the resolve-refusal marker" );
		Check( rr.message.find( "csg_stale" ) != std::string::npos,
		       "E1(k) message names the affected csg_object" );
		std::printf( "  E1(k) message: %s\n", rr.message.c_str() );

		// The live document is unchanged -- the stale re-point never
		// landed (nothing else in this fixture ever writes this token).
		const std::string liveDoc = owner->ReadDocument();
		Check( liveDoc.find( "material mat_ggx" ) == std::string::npos,
		       "E1(k) the live document NEVER received the stale re-point" );

		c.Stop();
		pJob->release();
		std::remove( tmp.c_str() );
	}

	// (l) [round-2 fix] DELTA, not state, in Arm C: a scene FILE (loaded,
	// not agent-edited) already carries an UNACKNOWLEDGED emissive csg --
	// `mat_ggx_glow` is emissive from the moment it's authored, so
	// `csg_ggx_preexisting` is ALREADY the refused construct before any
	// agent edit runs (Validate is already Warning about it, exactly like
	// E1(e)).  Patching an UNRELATED param on that SAME material
	// (`alphax` -- nothing to do with emission) must APPLY: the edit did
	// not create or worsen the construct, so Arm C's state-based
	// predecessor (which the round-2 review found: ANY edit to a
	// referenced material was refused, forever, once a csg went
	// unacknowledged) would wrongly freeze it.  The Warning is Validate's
	// job to keep nagging about, not this gate's job to block on.
	{
		const std::string tmp = TempPath( "agentcrud_e1l.RISEscene" );
		std::string scene = kCsgReadyScene;
		scene += "ggx_material\n{\n\tname mat_ggx_glow\n\trd albedo\n\trs albedo\n\talphax 0.2\n\talphay 0.2\n"
		         "\tior 1.5\n\textinction 0.0\n\temissive pnt_glow\n}\n";
		scene += "csg_object\n{\n\tname csg_ggx_preexisting\n\tobja csg_opA\n\tobjb csg_opB\n"
		         "\toperation union\n\tmaterial mat_ggx_glow\n}\n";
		Job* pJob = LoadScene( scene.c_str(), tmp );
		Check( pJob != nullptr, "E1(l) fixture (pre-existing unacknowledged emissive csg) loads cleanly" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

		// RED-PROVE the fixture: the construct is ALREADY there, unrelated
		// to anything this test will do -- Validate Warns before any edit.
		auto sawLuminaireWarning = [&]() {
			const std::vector<Agent::AgentDiagnostic> diags =
				Agent::AgentSession::ValidateText( sess->ReadDocument() );
			for( const Agent::AgentDiagnostic& d : diags )
				if( d.code == "LUMINAIRE_NULL_GEOMETRY" && d.severity == Agent::AgentDiagnostic::Severity::Warning )
					return true;
			return false;
		};
		Check( sawLuminaireWarning(), "E1(l) RED-PROVE: the fixture is ALREADY unacknowledged+emissive before any edit" );

		Agent::AgentSetPatch p;
		p.target = "mat_ggx_glow";
		p.kind   = "ggx_material";
		p.param  = "alphax";
		p.value  = "0.4";
		Agent::AgentPatchResult r = sess->ProposePatch( p );
		Check( r.applied && r.status == "applied",
		       "E1(l) [round-2 fix] an UNRELATED param edit on the referenced material APPLIES -- "
		       "it neither created nor worsened the pre-existing construct" );
		Check( sawLuminaireWarning(),
		       "E1(l) the Warning is STILL present after the edit -- unchanged posture, "
		       "Validate keeps nagging, the gate did not silently \"fix\" anything" );

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

//----------------------------------------------------------------------
// MS1-MS6: Arc-75 slice S2.1 -- insert_material_scaffold.
//----------------------------------------------------------------------

//! Every generated chunk is named tmpl_<name>_<role>; helper for the
//! per-family expected-shape table below.
static std::string TmplName( const std::string& name, const char* role )
{
	return "tmpl_" + name + "_" + role;
}

//! Arc-75 S2.1 fix-round P2a: PIN the tool against the "resolves to a
//! painter but is actually spatially CONSTANT" decoy landmine (a real
//! bug class -- the 3D-solid noise painters dual-register into the
//! Function2D manager and PARSE/RENDER fine when wrapped in
//! scalar_painter{function2d}, but silently evaluate at a fixed point
//! every time; see ScaffoldExprFunction2DText's doc in AgentSession.cpp).
//! Render-pixel non-black checks are VACUOUS here (Monte Carlo noise
//! varies every pixel regardless of whether the material itself is
//! spatially varying) -- so these two helpers EVALUATE the resolved
//! live painter DIRECTLY, through the SAME accessor the renderer uses
//! (IPainter::GetColor / IScalarPainter::GetValuesAt), at several
//! distinct synthetic points, and report the max spread observed.  A
//! genuinely-varying painter spreads well past any honest epsilon; a
//! spatially-constant one (the decoy) returns EXACTLY the same value at
//! every point, spread == 0.0.
//!
//! Colour-pipe (world-space 3D noise painters): distinct `ptIntersection`
//! points, mirroring how Perlin3DPainter::GetColor etc. actually sample.
static double ColorPainterMaxSpread( IPainterManager* mgr, const std::string& name )
{
	IPainter* p = mgr ? mgr->GetItem( name.c_str() ) : nullptr;
	if( !p ) return -1.0;   // sentinel: not found in this manager
	double lo[3] = { 1e30, 1e30, 1e30 }, hi[3] = { -1e30, -1e30, -1e30 };
	for( int i = 0; i < 8; ++i ) {
		RayIntersectionGeometric ri( Ray(), nullRasterizerState );
		ri.bHit = true;
		ri.ptIntersection = Point3( i * 0.37, i * 0.71 - 1.3, i * 1.9 + 0.5 );
		const RISEPel c = p->GetColor( ri );
		for( int ch = 0; ch < 3; ++ch ) { lo[ch] = std::min( lo[ch], c[ch] ); hi[ch] = std::max( hi[ch], c[ch] ); }
	}
	double spread = 0.0;
	for( int ch = 0; ch < 3; ++ch ) spread = std::max( spread, hi[ch] - lo[ch] );
	return spread;
}

//! Scalar-pipe (scalar_painter{function2d expression_function2d}): distinct
//! `ptCoord` (u,v) points -- mirroring Function2DScalarPainter::GetValuesAt,
//! which calls `pFunc->Evaluate(ri.ptCoord.x, ri.ptCoord.y)` for real.
static double ScalarPainterMaxSpread( IScalarPainterManager* mgr, const std::string& name )
{
	IScalarPainter* p = mgr ? mgr->GetItem( name.c_str() ) : nullptr;
	if( !p ) return -1.0;   // sentinel: not found in this manager
	double lo = 1e30, hi = -1e30;
	for( int i = 0; i < 8; ++i ) {
		RayIntersectionGeometric ri( Ray(), nullRasterizerState );
		ri.bHit = true;
		ri.ptCoord = Point2( ( i % 8 ) / 8.0, ( ( i * 3 ) % 8 ) / 8.0 );
		const double v = p->GetValuesAt( ri ).v[0];
		lo = std::min( lo, v ); hi = std::max( hi, v );
	}
	return hi - lo;
}

//! MS1: each of the 5 families, at a FIXED name -- the expansion
//! applies, the document contains the expected chunk set, the
//! microsurface slot resolves to a real painter (checked via the
//! DOCUMENT and the LIVE managers, not the generator string), EVERY
//! bound slot is a GENUINELY spatially-varying painter (evaluated
//! directly at several points through the renderer's own accessor --
//! see ColorPainterMaxSpread/ScalarPainterMaxSpread's doc for why this,
//! not a render-pixel check, is what actually pins the decoy landmine),
//! and the scene derive+renders clean and non-black once an object is
//! bound to the new material.
static void TestMaterialScaffoldFamilies()
{
	std::printf( "MS1: insert_material_scaffold -- all 5 families, applies + binds + renders...\n" );

	struct FamilyCase
	{
		const char* family;
		const char* materialKind;
		std::size_t expectedChunkCount;
		// Honest-against-amplitude epsilons for the spread checks below
		// (see each family's own comment in AgentSession.cpp's
		// BuildXxx functions for the designed bias/scale ranges this is
		// derived from) -- deliberately well BELOW the smallest
		// expected spread so the check has real headroom, and well
		// ABOVE 0.0 so the decoy (spread identically 0.0) still fails
		// it by a wide margin.
		double colorEps;    // for boundSlots resolved in the colour-pipe manager
		double scalarEps;   // for boundSlots resolved in the scalar-pipe manager
	};
	const FamilyCase cases[] = {
		// weathered_wood: colour-pipe only (roughness+base_color share
		// the grain painter); dark/light tone endpoints differ by a
		// 0.35-0.60 factor, easily > 0.01 spread.
		{ "weathered_wood",  "pbr_metallic_roughness_material", 4, 0.01,  0.0   },
		// rough_stone: rd (colour, worley pebble) + facets (scalar,
		// bias 0.04-0.09 + scale 0.05-0.35 at wear=0.6 -> span ~0.23).
		{ "rough_stone",     "cooktorrance_material",           5, 0.01,  0.01  },
		// brushed_metal: alphax/alphay (scalar only) -- the NARROWEST
		// amplitude family by design (alphax span ~0.02 at wear=0.6).
		{ "brushed_metal",   "ward_anisotropic_material",       5, 0.0,   0.002 },
		// aged_bronze: rd (colour, reaction-diffusion patina) + facets
		// (scalar, span ~0.15 at wear=0.6).
		{ "aged_bronze",     "cooktorrance_material",           5, 0.01,  0.01  },
		// glazed_ceramic: alphax/alphay (scalar only) -- DELIBERATELY
		// "low-alpha with SUBTLE scalar variation" (span ~0.014 at
		// wear=0.6) -- the tightest epsilon of the five, honestly so.
		{ "glazed_ceramic",  "ggx_material",                     5, 0.0,   0.001 },
	};

	for( const FamilyCase& fc : cases ) {
		const std::string tmp = TempPath( ( std::string( "agentcrud_ms1_" ) + fc.family + ".RISEscene" ).c_str() );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, std::string( "MS1(" ) + fc.family + ") fixture loads" );
		if( !pJob ) continue;

		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
		const RISE::Cst::CstHeadVersion v0 = sess->HeadVersion();

		const Agent::AgentSession::AgentScaffoldResult sr = sess->InsertMaterialScaffold(
			fc.family, "deskA", "0.55 0.42 0.30", 0.6, 1.5, &v0 );

		Check( sr.ok, std::string( "MS1(" ) + fc.family + ") call itself is well-formed (ok==true): " + sr.message );
		Check( sr.chunkResults.size() == fc.expectedChunkCount,
		       std::string( "MS1(" ) + fc.family + ") generated the expected chunk count" );
		Check( sr.materialKind == fc.materialKind,
		       std::string( "MS1(" ) + fc.family + ") material chunk kind matches the family design" );
		Check( sr.materialName == TmplName( "deskA", "mat" ),
		       std::string( "MS1(" ) + fc.family + ") material chunk is named tmpl_deskA_mat" );
		Check( !sr.boundSlots.empty(),
		       std::string( "MS1(" ) + fc.family + ") at least one microsurface slot is bound (the tool's reason to exist)" );

		bool allApplied = true;
		for( const Agent::AgentChunkResult& cr : sr.chunkResults ) if( !cr.applied ) allApplied = false;
		Check( allApplied, std::string( "MS1(" ) + fc.family + ") every generated chunk applied" );

		// The microsurface slot resolves to a painter -- via the DOCUMENT
		// (the material's own chunk literally names the bound painter) AND
		// via the LIVE MANAGERS (Job::Add*Material only succeeds, and the
		// chunk only comes back applied==true, when every referenced
		// painter/scalar_painter name actually resolved -- an unresolved
		// slot would have rejected the material chunk).
		const std::string doc = sess->ReadDocument();
		for( const auto& kv : sr.boundSlots ) {
			Check( doc.find( kv.first + " " + kv.second ) != std::string::npos,
			       std::string( "MS1(" ) + fc.family + ") document binds `" + kv.first + "` to `" + kv.second + "`" );
		}
		Check( pJob->GetMaterials() && pJob->GetMaterials()->GetItem( sr.materialName.c_str() ) != nullptr,
		       std::string( "MS1(" ) + fc.family + ") the live material manager resolved the whole graph" );

		// P2a: EVERY bound slot is a GENUINELY spatially-varying painter,
		// not just a name that happens to resolve.  Try the scalar-pipe
		// manager first (scalar_painter chunks register ONLY there, never
		// dual-registered), then the colour-pipe manager -- exactly one
		// must resolve (a boundSlot painter that resolves in NEITHER, or a
		// spread <= its family's honest epsilon, both fail loudly).
		for( const auto& kv : sr.boundSlots ) {
			IScalarPainter* asScalar = pJob->GetScalarPainters() ? pJob->GetScalarPainters()->GetItem( kv.second.c_str() ) : nullptr;
			if( asScalar ) {
				const double spread = ScalarPainterMaxSpread( pJob->GetScalarPainters(), kv.second );
				Check( spread > fc.scalarEps,
				       std::string( "MS1(" ) + fc.family + ") scalar slot `" + kv.first + "` -> `" + kv.second +
				       "` is GENUINELY spatially varying (spread " + std::to_string( spread ) +
				       " > epsilon " + std::to_string( fc.scalarEps ) + ", evaluated directly via IScalarPainter::GetValuesAt "
				       "at distinct UVs -- NOT a render-pixel check)" );
				continue;
			}
			IPainter* asColor = pJob->GetPainters() ? pJob->GetPainters()->GetItem( kv.second.c_str() ) : nullptr;
			Check( asColor != nullptr,
			       std::string( "MS1(" ) + fc.family + ") bound painter `" + kv.second +
			       "` resolves in EITHER the scalar or colour painter manager" );
			if( asColor ) {
				const double spread = ColorPainterMaxSpread( pJob->GetPainters(), kv.second );
				Check( spread > fc.colorEps,
				       std::string( "MS1(" ) + fc.family + ") colour slot `" + kv.first + "` -> `" + kv.second +
				       "` is GENUINELY spatially varying (spread " + std::to_string( spread ) +
				       " > epsilon " + std::to_string( fc.colorEps ) + ", evaluated directly via IPainter::GetColor "
				       "at distinct world points -- NOT a render-pixel check)" );
			}
		}

		// Bind an object to the new material and render a small non-black
		// check -- "derive+render clean" per the family's own binding, not
		// a hand-typed sanity material.
		std::vector<std::string> objChunks;
		objChunks.push_back( "sphere_geometry\n{\nname sph_" + std::string( fc.family ) + "\nradius 0.5\n}" );
		objChunks.push_back( "standard_object\n{\nname obj_" + std::string( fc.family ) +
			"\ngeometry sph_" + fc.family + "\nmaterial " + sr.materialName + "\nposition 1.6 0 0\n}" );
		const std::vector<Agent::AgentChunkResult> objResults = sess->InsertChunks( objChunks );
		Check( objResults.size() == 2 && objResults[0].applied && objResults[1].applied,
		       std::string( "MS1(" ) + fc.family + ") the follow-up object binding the scaffold material applied" );

		Agent::AgentRenderParams rp;
		rp.width = 32; rp.height = 32; rp.samples = 4;
		const Agent::AgentRenderResult rr = sess->Render( rp );
		Check( rr.ok, std::string( "MS1(" ) + fc.family + ") the scene renders" );
		Check( rr.meanR + rr.meanG + rr.meanB > 0.0,
		       std::string( "MS1(" ) + fc.family + ") the render is non-black" );

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}
}

//! Extract the value on the FIRST line starting with `param ` that
//! appears AFTER chunk `chunkName`'s own `name <chunkName>` line --
//! good enough for these single-chunk-per-name fixtures (no nested
//! braces to worry about).
static std::string ExtractParamAfter( const std::string& doc, const std::string& chunkName, const std::string& param )
{
	const std::string nameMarker = "name " + chunkName + "\n";
	const std::size_t namePos = doc.find( nameMarker );
	if( namePos == std::string::npos ) return std::string();
	const std::string paramMarker = "\n" + param + " ";
	const std::size_t paramPos = doc.find( paramMarker, namePos );
	if( paramPos == std::string::npos ) return std::string();
	const std::size_t valStart = paramPos + paramMarker.size();
	const std::size_t valEnd = doc.find( '\n', valStart );
	return doc.substr( valStart, valEnd - valStart );
}

//! MS2: determinism -- the SAME name, in TWO FRESH documents, produces
//! BYTE-IDENTICAL generated chunk text (no RNG, no clock); a DIFFERENT
//! name visibly differs in its jittered constants (not just the renamed
//! chunk tokens).
static void TestMaterialScaffoldDeterminism()
{
	std::printf( "MS2: insert_material_scaffold -- determinism (same name twice byte-identical; different name differs)...\n" );

	auto expandFresh = [&]( const std::string& name ) -> std::string {
		const std::string tmp = TempPath( ( "agentcrud_ms2_" + name + ".RISEscene" ).c_str() );
		Job* pJob = LoadScene( kScene, tmp );
		if( !pJob ) return std::string();
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
		const Agent::AgentSession::AgentScaffoldResult sr = sess->InsertMaterialScaffold(
			"weathered_wood", name, "0.5 0.4 0.3", 0.5, 1.0 );
		Check( sr.ok, "MS2 expansion for `" + name + "` is well-formed" );
		const std::string doc = sess->ReadDocument();
		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
		return doc;
	};

	const std::string docA1 = expandFresh( "scafA" );
	const std::string docA2 = expandFresh( "scafA" );
	Check( !docA1.empty() && !docA2.empty(), "MS2 both `scafA` fixtures produced a document" );
	Check( docA1 == docA2,
	       "MS2 SAME name, two FRESH documents -> BYTE-IDENTICAL generated chunk text (deterministic, no RNG/clock)" );

	const std::string docB = expandFresh( "scafB" );
	Check( !docB.empty(), "MS2 `scafB` fixture produced a document" );
	Check( docA1 != docB, "MS2 a DIFFERENT name produces a different document (trivially true from the renamed chunks alone)" );

	// The STRONGER claim: the JITTERED NUMERIC CONSTANTS differ, not just
	// the renamed chunk tokens -- extract the grain painter's own
	// `persistence` value for each name and require them to differ.
	const std::string persA = ExtractParamAfter( docA1, TmplName( "scafA", "grain" ), "persistence" );
	const std::string persB = ExtractParamAfter( docB,  TmplName( "scafB", "grain" ), "persistence" );
	Check( !persA.empty() && !persB.empty(), "MS2 extracted a `persistence` value from both fixtures' grain painter" );
	Check( persA != persB,
	       "MS2 a DIFFERENT name jitters a DIFFERENT `persistence` value (the internal constants really do vary with `name`, not just the labels)" );
}

//! MS3: each of the 5 required params, omitted in turn, is a BLOCKING
//! error naming the missing param -- driven through the REAL wire
//! (AgentRpcDispatcher::HandleLine), mirroring IC3's style.
static void TestMaterialScaffoldMissingParams()
{
	std::printf( "MS3: insert_material_scaffold -- each missing required param -> blocking -32602...\n" );
	const std::string tmp = TempPath( "agentcrud_ms3.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "MS3 fixture loads" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const std::string headBefore = sess->ReadDocument();
	Agent::AgentRpcDispatcher disp( std::move( sess ) );

	struct Case { const char* id; const char* paramsJson; const char* missing; };
	const Case cases[] = {
		{ "1", "{\"name\":\"x1\",\"tone\":\"0.5 0.5 0.5\",\"wear\":0.5,\"scale\":1.0}", "family" },
		{ "2", "{\"family\":\"weathered_wood\",\"tone\":\"0.5 0.5 0.5\",\"wear\":0.5,\"scale\":1.0}", "name" },
		{ "3", "{\"family\":\"weathered_wood\",\"name\":\"x3\",\"wear\":0.5,\"scale\":1.0}", "tone" },
		{ "4", "{\"family\":\"weathered_wood\",\"name\":\"x4\",\"tone\":\"0.5 0.5 0.5\",\"scale\":1.0}", "wear" },
		{ "5", "{\"family\":\"weathered_wood\",\"name\":\"x5\",\"tone\":\"0.5 0.5 0.5\",\"wear\":0.5}", "scale" },
	};
	int id = 10;
	for( const Case& c : cases ) {
		const std::string req = std::string( "{\"jsonrpc\":\"2.0\",\"id\":" ) + std::to_string( id++ ) +
			",\"method\":\"insert_material_scaffold\",\"params\":" + c.paramsJson + "}";
		const std::string resp = disp.HandleLine( req );
		Check( resp.find( "-32602" ) != std::string::npos,
		       std::string( "MS3(" ) + c.id + ") missing `" + c.missing + "` -> -32602 invalid params" );
		Check( resp.find( c.missing ) != std::string::npos,
		       std::string( "MS3(" ) + c.id + ") the error message NAMES the missing param `" + c.missing + "`" );
	}

	Check( disp.Session() && disp.Session()->ReadDocument() == headBefore,
	       "MS3 none of the 5 missing-param refusals mutated the document" );

	pJob->release();
	std::remove( tmp.c_str() );
}

//! MS4: an unrecognized `family` is refused with a message listing the
//! valid families -- document unchanged.
static void TestMaterialScaffoldBadFamily()
{
	std::printf( "MS4: insert_material_scaffold -- unknown family -> error listing valid families...\n" );
	const std::string tmp = TempPath( "agentcrud_ms4.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "MS4 fixture loads" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const std::string headBefore = sess->ReadDocument();

	const Agent::AgentSession::AgentScaffoldResult sr = sess->InsertMaterialScaffold(
		"rusty_chrome", "x", "0.5 0.5 0.5", 0.5, 1.0 );
	Check( !sr.ok, "MS4 an unknown family refuses the call (ok==false)" );
	Check( sr.chunkResults.empty(), "MS4 no chunks were generated for an unknown family" );
	const char* families[] = { "weathered_wood", "rough_stone", "brushed_metal", "aged_bronze", "glazed_ceramic" };
	for( const char* f : families ) {
		Check( sr.message.find( f ) != std::string::npos,
		       std::string( "MS4 the error message lists valid family `" ) + f + "`" );
	}
	Check( sess->ReadDocument() == headBefore, "MS4 the refusal mutated nothing" );

	pJob->release();
	std::remove( tmp.c_str() );
}

//! MS4b (fix-round P3): `name` past the sane length cap (kScaffoldMaxNameLength
//! == 64 in AgentSession.cpp) is refused, same as any other invalid `name`.
static void TestMaterialScaffoldNameLengthCap()
{
	std::printf( "MS4b: insert_material_scaffold -- `name` past the length cap is refused...\n" );
	const std::string tmp = TempPath( "agentcrud_ms4b.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "MS4b fixture loads" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const std::string tooLongName( 65, 'a' );   // one past the 64-char cap
	const Agent::AgentSession::AgentScaffoldResult sr = sess->InsertMaterialScaffold(
		"weathered_wood", tooLongName, "0.5 0.5 0.5", 0.5, 1.0 );
	Check( !sr.ok, "MS4b a 65-char `name` (one past the cap) is refused" );

	pJob->release();
	std::remove( tmp.c_str() );
}

//! MS5: a name collision -- a family/name expanded once, then the SAME
//! family+name expanded again -- refuses the WHOLE second call cleanly
//! (document unchanged), rather than landing a partial second graph.
static void TestMaterialScaffoldNameCollision()
{
	std::printf( "MS5: insert_material_scaffold -- name collision -> clean refusal, document unchanged...\n" );
	const std::string tmp = TempPath( "agentcrud_ms5.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "MS5 fixture loads" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

	const Agent::AgentSession::AgentScaffoldResult sr1 = sess->InsertMaterialScaffold(
		"rough_stone", "dup1", "0.5 0.5 0.5", 0.5, 1.0 );
	Check( sr1.ok && !sr1.chunkResults.empty() && sr1.chunkResults.back().applied,
	       "MS5 the FIRST expansion under this name applies cleanly" );

	const std::string headAfterFirst = sess->ReadDocument();
	const RISE::Cst::CstHeadVersion vAfterFirst = sess->HeadVersion();

	// A SECOND expansion, same family AND name -- every generated chunk
	// name collides with the first expansion's.
	const Agent::AgentSession::AgentScaffoldResult sr2 = sess->InsertMaterialScaffold(
		"rough_stone", "dup1", "0.9 0.1 0.1", 0.9, 2.0 );
	Check( !sr2.ok, "MS5 the SECOND expansion (same family+name) is refused (ok==false)" );
	Check( sr2.chunkResults.empty(), "MS5 the refused expansion generated NO chunk results (refused before InsertChunks ran)" );
	Check( sr2.message.find( "dup1" ) != std::string::npos, "MS5 the refusal message names the colliding `name`" );

	Check( sess->ReadDocument() == headAfterFirst, "MS5 the document is BYTE-IDENTICAL to before the collision (no partial graph landed)" );
	Check( sess->HeadVersion() == vAfterFirst, "MS5 the head revision did not move" );

	// A collision against a DIFFERENT family sharing the same `name` also
	// refuses cleanly (rough_stone and aged_bronze both emit tmpl_<name>_tone
	// and tmpl_<name>_mat).
	const Agent::AgentSession::AgentScaffoldResult sr3 = sess->InsertMaterialScaffold(
		"aged_bronze", "dup1", "0.5 0.5 0.3", 0.4, 1.0 );
	Check( !sr3.ok, "MS5 a collision against a DIFFERENT family sharing the same `name` is also refused" );
	Check( sess->ReadDocument() == headAfterFirst, "MS5 that cross-family collision ALSO left the document byte-identical" );

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

//! MS6: under External authority with a live controller attached (the
//! same Secure-MCP staging posture insert_chunk/insert_chunks use), the
//! expansion STAGES proposals rather than committing -- every generated
//! chunk comes back status=="staged", applied==false, and the document
//! is untouched until an Owner resolves them.
static void TestMaterialScaffoldProposalMode()
{
	std::printf( "MS6: insert_material_scaffold -- External authority STAGES, does not commit...\n" );
	const std::string tmp = TempPath( "agentcrud_ms6.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "MS6 fixture loads" );
	if( !pJob ) return;

	TestController c( *pJob, /*simulatedRenderMs*/ 0 );
	c.Start();

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob, Agent::AgentAuthority::External );
	sess->AttachController( &c );
	Check( sess->Authority() == Agent::AgentAuthority::External, "MS6 session reports External authority" );

	const std::string headBefore = sess->ReadDocument();
	const Agent::AgentSession::AgentScaffoldResult sr = sess->InsertMaterialScaffold(
		"glazed_ceramic", "extA", "0.8 0.8 0.75", 0.3, 1.0 );

	Check( sr.ok, "MS6 the call itself is well-formed under External authority (ok==true -- generation + collision-precheck succeeded)" );
	Check( !sr.chunkResults.empty(), "MS6 the expansion produced per-chunk results" );
	bool allStaged = true;
	for( const Agent::AgentChunkResult& cr : sr.chunkResults ) {
		if( cr.applied || cr.status != "staged" ) allStaged = false;
	}
	Check( allStaged, "MS6 EVERY generated chunk is staged (applied==false, status==\"staged\"), none committed directly" );
	Check( sess->ReadDocument() == headBefore, "MS6 the document is UNCHANGED -- nothing committed, only proposals queued" );

	c.Stop();
	pJob->release();
	std::remove( tmp.c_str() );
}

//----------------------------------------------------------------------
// GS1-GS6: Arc-75 slice S3b -- insert_geometry_scaffold.  The geometry
// sibling of MS1-MS6 above; same fixture/helper conventions
// (TempPath/LoadScene/TmplName reused verbatim).
//----------------------------------------------------------------------

//! Local-space bbox extents (width,height,depth) for a named geometry
//! chunk, resolved through the LIVE geometry manager -- exactly what
//! every spatial-effect assertion below needs: the scaffold's
//! `size`/`aspect` params are baked into the chunk's own authored
//! numbers, not into any `standard_object` binding transform, so no
//! object needs to exist for this to be meaningful.
static bool GeometryBBoxExtents( Job* pJob, const std::string& name, double& ex, double& ey, double& ez )
{
	IGeometry* g = ( pJob && pJob->GetGeometries() ) ? pJob->GetGeometries()->GetItem( name.c_str() ) : nullptr;
	if( !g ) return false;
	// displaced_geometry defers its tessellation bake to Realize() (normally
	// called from RayCaster::AttachScene at render time) -- an un-rendered
	// scene's DisplacedGeometry has a null internal mesh and reports a
	// degenerate (0,0,0) bbox until baked.  Realize() is idempotent/no-op
	// for every other geometry kind, so calling it unconditionally here is
	// always safe.
	g->Realize();
	const BoundingBox bb = g->GenerateBoundingBox();
	ex = bb.ur.x - bb.ll.x;
	ey = bb.ur.y - bb.ll.y;
	ez = bb.ur.z - bb.ll.z;
	return std::isfinite( ex ) && std::isfinite( ey ) && std::isfinite( ez );
}

//! P2a fix-round: the Nth (0-indexed) repeatable `part` line belonging to
//! `sdf_geometry` chunk `chunkName` -- text found after that chunk's own
//! "name <chunkName>" line, one `part ` line per iteration, stopping at
//! `maxsteps` or the closing brace.  Returns everything AFTER the "part "
//! keyword (the `<prim> <op> <k> ...` fields); empty on any lookup miss.
static std::string ExtractNthPartLine( const std::string& doc, const std::string& chunkName, std::size_t n )
{
	const std::string nameMarker = "name " + chunkName + "\n";
	std::size_t pos = doc.find( nameMarker );
	if( pos == std::string::npos ) return std::string();
	pos += nameMarker.size();
	std::size_t found = 0;
	while( pos < doc.size() ) {
		std::size_t lineEnd = doc.find( '\n', pos );
		if( lineEnd == std::string::npos ) lineEnd = doc.size();
		const std::string line = doc.substr( pos, lineEnd - pos );
		if( line.rfind( "part ", 0 ) == 0 ) {
			if( found == n ) return line.substr( 5 );
			++found;
		} else if( line == "}" || line.rfind( "maxsteps", 0 ) == 0 ) {
			break;
		}
		pos = ( lineEnd >= doc.size() ) ? doc.size() : lineEnd + 1;
	}
	return std::string();
}

//! P2a fix-round: the `idx`-th (0-indexed) whitespace-separated token of
//! `line` -- used to pull the `k` field (index 2: `<prim> <op> <k> ...`)
//! out of one ExtractNthPartLine result without pulling in <sstream> for
//! a single-purpose tokenizer.
static std::string NthWhitespaceToken( const std::string& line, std::size_t idx )
{
	std::size_t pos = 0;
	for( std::size_t i = 0; ; ++i ) {
		while( pos < line.size() && std::isspace( static_cast<unsigned char>( line[pos] ) ) ) ++pos;
		if( pos >= line.size() ) return std::string();
		const std::size_t start = pos;
		while( pos < line.size() && !std::isspace( static_cast<unsigned char>( line[pos] ) ) ) ++pos;
		if( i == idx ) return line.substr( start, pos - start );
	}
}

//! Per-pixel luma decode of a rendered PNG, through RISE's OWN PNGReader
//! (the same decode idiom AgentObjectMapTest.cpp's DecodePng uses) --
//! needed for GS1b's flat-vs-bumpy comparison, which needs the SPREAD of
//! shading across the image, not just its mean (meanR/G/B alone cannot
//! tell "uniformly lit flat face" apart from "genuinely varying bumpy
//! face" when both variants sit in the same base scene under the same
//! light).
struct DecodedLuma
{
	unsigned int        w = 0, h = 0;
	std::vector<double> luma;   // row-major, linear
};

static bool DecodeRenderLuma( const std::vector<unsigned char>& png, DecodedLuma& out )
{
	if( png.empty() ) return false;
	Implementation::MemoryBuffer* buf = new Implementation::MemoryBuffer(
		const_cast<char*>( reinterpret_cast<const char*>( png.data() ) ),
		(unsigned int)png.size(), /*bTakeOwnership*/false );
	IRasterImageReader* reader = nullptr;
	if( !RISE_API_CreatePNGReader( &reader, *buf, eColorSpace_sRGB ) || !reader ) {
		safe_release( buf );
		return false;
	}
	unsigned int w = 0, h = 0;
	if( !reader->BeginRead( w, h ) ) { safe_release( reader ); safe_release( buf ); return false; }
	out.w = w; out.h = h;
	out.luma.resize( (std::size_t)w * h );
	for( unsigned int y = 0; y < h; ++y ) {
		for( unsigned int x = 0; x < w; ++x ) {
			RISEColor c;
			reader->ReadColor( c, x, y );
			out.luma[ (std::size_t)y * w + x ] = 0.2126 * c.base.r + 0.7152 * c.base.g + 0.0722 * c.base.b;
		}
	}
	reader->EndRead();
	safe_release( reader );
	safe_release( buf );
	return true;
}

static double LumaStdDev( const DecodedLuma& d )
{
	if( d.luma.empty() ) return 0.0;
	double mean = 0.0;
	for( double v : d.luma ) mean += v;
	mean /= static_cast<double>( d.luma.size() );
	double var = 0.0;
	for( double v : d.luma ) var += ( v - mean ) * ( v - mean );
	var /= static_cast<double>( d.luma.size() );
	return std::sqrt( var );
}

//! A tiny dedicated scene (no pre-existing objects/lights beyond camera +
//! one directional key) for GS1b's flat-vs-bumpy comparison.  Camera
//! looks STRAIGHT DOWN (-Y) from close range at a NARROW fov chosen so
//! the frame's covered footprint (2 * camHeight * tan(fov/2) ~= 1.07) is
//! well INSIDE the box's 1.2x1.2 footprint (size=1.2, aspect=1.0 in the
//! call below) -- every pixel is the object's top face, no background,
//! no silhouette/side-face edges.  That matters because a 3/4 view's
//! edge pixels (object-vs-background, top-vs-side-face) contribute
//! stddev unrelated to bumpiness, common to BOTH variants and large
//! enough to swamp the genuinely-bumpy signal at this render's modest
//! sample count -- this framing removes that confound entirely rather
//! than trying to out-margin it.
static const char* const kGeoRenderScene =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 16\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 48\n\theight 48\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 2.0 0\n\tlookat 0 0 0\n\tup 0 0 -1\n\tfov 30.0\n}\n\n"
	"directional_light\n{\n\tname key\n\tpower 3.0\n\tcolor 1 1 1\n\tdirection 0.3 0.6 0.7\n}\n";

//! GS1: each of the 4 families, at a FIXED name -- the expansion
//! applies, the document contains the expected chunk set, the ONE
//! geometry chunk resolves through the live geometry manager, its bbox
//! has genuinely nonzero extent on all three axes, and the scene
//! derive+renders clean once an object (+ a plain material) is bound to
//! the new geometry.
static void TestGeometryScaffoldFamilies()
{
	std::printf( "GS1: insert_geometry_scaffold -- all 4 families, applies + realizes + renders...\n" );

	struct FamilyCase
	{
		const char* family;
		const char* geometryKind;
		const char* roleSuffix;      // tmpl_<name>_<roleSuffix> is the hero geometry chunk
		std::size_t expectedChunkCount;
	};
	const FamilyCase cases[] = {
		{ "displaced_slab", "displaced_geometry", "disp",   3 },
		{ "sweep_rail",     "sweep_geometry",      "rail",   1 },
		{ "blended_vessel", "sdf_geometry",        "vessel", 1 },
		{ "sdf_column",     "sdf_geometry",        "col",    1 },
	};

	for( const FamilyCase& fc : cases ) {
		const std::string tmp = TempPath( ( std::string( "agentcrud_gs1_" ) + fc.family + ".RISEscene" ).c_str() );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, std::string( "GS1(" ) + fc.family + ") fixture loads" );
		if( !pJob ) continue;

		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
		const RISE::Cst::CstHeadVersion v0 = sess->HeadVersion();

		const Agent::AgentSession::AgentGeometryScaffoldResult sr = sess->InsertGeometryScaffold(
			fc.family, "geoA", 1.4, 0.6, 1.3, std::string(), 0.0, std::string(), &v0 );

		Check( sr.ok, std::string( "GS1(" ) + fc.family + ") call itself is well-formed (ok==true): " + sr.message );
		Check( sr.chunkResults.size() == fc.expectedChunkCount,
		       std::string( "GS1(" ) + fc.family + ") generated the expected chunk count" );
		Check( sr.geometryKind == fc.geometryKind,
		       std::string( "GS1(" ) + fc.family + ") geometry chunk kind matches the family design" );
		Check( sr.geometryName == TmplName( "geoA", fc.roleSuffix ),
		       std::string( "GS1(" ) + fc.family + ") geometry chunk is named tmpl_geoA_" + fc.roleSuffix );

		bool allApplied = true;
		for( const Agent::AgentChunkResult& cr : sr.chunkResults ) if( !cr.applied ) allApplied = false;
		Check( allApplied, std::string( "GS1(" ) + fc.family + ") every generated chunk applied" );

		Check( pJob->GetGeometries() && pJob->GetGeometries()->GetItem( sr.geometryName.c_str() ) != nullptr,
		       std::string( "GS1(" ) + fc.family + ") the live geometry manager resolved the graph" );

		double ex = 0.0, ey = 0.0, ez = 0.0;
		Check( GeometryBBoxExtents( pJob, sr.geometryName, ex, ey, ez ),
		       std::string( "GS1(" ) + fc.family + ") geometry bbox is queryable" );
		Check( ex > 0.0 && ey > 0.0 && ez > 0.0,
		       std::string( "GS1(" ) + fc.family + ") geometry bbox has NONZERO extent on all three axes (" +
		       std::to_string( ex ) + ", " + std::to_string( ey ) + ", " + std::to_string( ez ) + ")" );

		// Bind an object (+ a plain hand-authored material -- this tool
		// never emits one) to the new geometry and render a small
		// non-black check: "derive+render clean" per the family's own
		// binding, exactly MS1's rigor level for the material scaffold.
		std::vector<std::string> objChunks;
		objChunks.push_back( "uniformcolor_painter\n{\nname pnt_" + std::string( fc.family ) + "\ncolor 0.6 0.5 0.4\n}" );
		objChunks.push_back( "lambertian_material\n{\nname mat_" + std::string( fc.family ) +
			"\nreflectance pnt_" + fc.family + "\n}" );
		objChunks.push_back( "standard_object\n{\nname obj_" + std::string( fc.family ) +
			"\ngeometry " + sr.geometryName + "\nmaterial mat_" + fc.family + "\nposition 0 0 0\n}" );
		const std::vector<Agent::AgentChunkResult> objResults = sess->InsertChunks( objChunks );
		bool objAllApplied = true;
		for( const Agent::AgentChunkResult& r : objResults ) if( !r.applied ) objAllApplied = false;
		Check( objAllApplied,
		       std::string( "GS1(" ) + fc.family + ") the follow-up material+object binding the scaffold geometry applied" );

		Agent::AgentRenderParams rp;
		rp.width = 32; rp.height = 32; rp.samples = 4;
		const Agent::AgentRenderResult rr = sess->Render( rp );
		Check( rr.ok, std::string( "GS1(" ) + fc.family + ") the scene renders" );
		Check( rr.meanR + rr.meanG + rr.meanB > 0.0,
		       std::string( "GS1(" ) + fc.family + ") the render is non-black" );

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}
}

//! GS1b: the SPATIAL-EFFECT assertion for displaced_slab -- render the
//! flat base box and the displaced geometry (SAME expansion, SAME
//! material, SAME camera/light, only the bound geometry chunk differs)
//! and require the displaced render's per-pixel luma STDDEV to be
//! markedly higher than the flat render's.  A flat box under a single
//! directional light has a near-uniform top/side shading (its stddev is
//! essentially Monte Carlo noise); a genuinely bumpy surface has real
//! per-pixel shading variation from its varying normals.  This is a
//! render-based check (unlike GS1's/GS1c's direct bbox queries)
//! precisely because "genuinely bumpy" is a SHADING claim, not a bbox
//! claim -- displacement barely moves the silhouette/bbox at these
//! amplitudes, so a bbox check alone would not distinguish "real bump"
//! from "decoy no-op displacement" (the S2.1 decoy-landmine lesson,
//! applied here to geometry).
static void TestGeometryScaffoldDisplacedBumpyVsFlat()
{
	std::printf( "GS1b: insert_geometry_scaffold -- displaced_slab render genuinely differs from its own flat base...\n" );

	auto renderVariant = [&]( const char* roleSuffix, DecodedLuma& outLuma ) -> bool {
		const std::string tmp = TempPath( ( std::string( "agentcrud_gs1b_" ) + roleSuffix + ".RISEscene" ).c_str() );
		Job* pJob = LoadScene( kGeoRenderScene, tmp );
		if( !pJob ) return false;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
		const Agent::AgentSession::AgentGeometryScaffoldResult sr =
			sess->InsertGeometryScaffold( "displaced_slab", "bumpy", 1.2, 0.75, 1.0 );
		bool ok = sr.ok;

		std::vector<std::string> objChunks;
		objChunks.push_back( "uniformcolor_painter\n{\nname pnt_bv\ncolor 0.6 0.55 0.5\n}" );
		objChunks.push_back( "lambertian_material\n{\nname mat_bv\nreflectance pnt_bv\n}" );
		objChunks.push_back( "standard_object\n{\nname obj_bv\ngeometry " + TmplName( "bumpy", roleSuffix ) +
			"\nmaterial mat_bv\n}" );
		const std::vector<Agent::AgentChunkResult> objResults = sess->InsertChunks( objChunks );
		for( const Agent::AgentChunkResult& r : objResults ) if( !r.applied ) ok = false;

		Agent::AgentRenderParams rp;
		rp.width = 48; rp.height = 48; rp.samples = 16;
		const Agent::AgentRenderResult rr = sess->Render( rp );
		ok = ok && rr.ok;
		if( ok ) ok = DecodeRenderLuma( rr.png, outLuma );

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
		return ok;
	};

	DecodedLuma flatLuma, bumpyLuma;
	Check( renderVariant( "base", flatLuma ),  "GS1b flat-base variant (tmpl_bumpy_base) renders and decodes" );
	Check( renderVariant( "disp", bumpyLuma ), "GS1b displaced variant (tmpl_bumpy_disp) renders and decodes" );

	const double flatStdDev  = LumaStdDev( flatLuma );
	const double bumpyStdDev = LumaStdDev( bumpyLuma );
	std::printf( "    GS1b flat luma stddev %.5f, bumpy luma stddev %.5f\n", flatStdDev, bumpyStdDev );
	Check( bumpyStdDev > flatStdDev * 2.0,
	       "GS1b the displaced render's per-pixel luma stddev is markedly HIGHER than the flat base's "
	       "(genuinely varying shading, not just MC noise): flat=" + std::to_string( flatStdDev ) +
	       " bumpy=" + std::to_string( bumpyStdDev ) );
	Check( bumpyStdDev > 0.01,
	       "GS1b the displaced render's stddev also clears an ABSOLUTE floor, not just a relative one "
	       "(bumpy=" + std::to_string( bumpyStdDev ) + ")" );
}

//! GS1c: the SPATIAL-EFFECT assertion for the other three families (and,
//! cheaply, displaced_slab too) -- `size`/`aspect` genuinely flow into
//! the realized geometry's bbox, not just into the chunk text.  Two
//! FRESH expansions under the SAME `name` (so every jittered internal
//! constant is IDENTICAL between them -- only the explicit `aspect`
//! differs) at a LOW and a HIGH aspect; asserts the elongation axis's
//! bbox extent grows by more than an absolute floor derived from each
//! family's own authored jitter ranges (a DIFFERENCE assertion, not a
//! ratio one, so it is insensitive to the constant, aspect-independent
//! term in blended_vessel's/sdf_column's bbox).  That constant term is
//! NOT the flat-bottom `box subtract` part -- SDFGeometry::ComputeBounds
//! (SDFGeometry.cpp ~385-387) SKIPS every subtract-op part entirely
//! ("a carve never extends the solid" -> no-op on the bound), so the cut
//! contributes NOTHING to the bbox union.  The real source is the FIRST
//! `roundcone` part's own local AABB (primLocalAABB, SDFGeometry.cpp
//! ~175-195): a roundcone's `ry0` (its bottom extent) is `-pt.a` --
//! i.e. `-baseR`, from the rounded cap that bulges below the part's own
//! y=0 -- and `baseR` depends only on `name`+`size` (never `aspect`) in
//! both families, so it is IDENTICAL between the low- and high-aspect
//! calls below and cancels out of the delta.
static void TestGeometryScaffoldAspectFlow()
{
	std::printf( "GS1c: insert_geometry_scaffold -- size/aspect genuinely flow into every family's bbox...\n" );

	struct FamilyAxis { const char* family; int axis; };   // axis: 0=X, 1=Y -- which bbox extent `aspect` elongates
	const FamilyAxis cases[] = {
		{ "displaced_slab", 0 },   // footprint width (X) grows with aspect
		{ "sweep_rail",     0 },   // path length (X) grows with aspect
		{ "blended_vessel", 1 },   // total height (Y) grows with aspect
		{ "sdf_column",     1 },   // shaft height (Y) grows with aspect
	};
	const double kLowAspect  = 0.5;
	const double kHighAspect = 3.0;
	const double kSize       = 1.0;
	// A floor well below the SMALLEST possible delta across every
	// family's own authored jitter range at size=1.0 (see each BuildXxx
	// in AgentSession.cpp -- the tightest is displaced_slab's
	// width = size*sqrt(aspect), delta = sqrt(3.0)-sqrt(0.5) ~= 1.02).
	const double kMinDelta = 0.5;

	for( const FamilyAxis& fc : cases ) {
		auto expandAndBBox = [&]( double aspect, double ext[3] ) -> bool {
			const std::string tmp = TempPath( ( std::string( "agentcrud_gs1c_" ) + fc.family + "_" +
				std::to_string( static_cast<int>( aspect * 100 ) ) + ".RISEscene" ).c_str() );
			Job* pJob = LoadScene( kScene, tmp );
			if( !pJob ) return false;
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const Agent::AgentSession::AgentGeometryScaffoldResult sr =
				sess->InsertGeometryScaffold( fc.family, "aspA", kSize, 0.5, aspect );
			bool ok = sr.ok;
			if( ok ) ok = GeometryBBoxExtents( pJob, sr.geometryName, ext[0], ext[1], ext[2] );
			sess.reset();
			pJob->release();
			std::remove( tmp.c_str() );
			return ok;
		};

		double lowExt[3] = { 0.0, 0.0, 0.0 }, highExt[3] = { 0.0, 0.0, 0.0 };
		Check( expandAndBBox( kLowAspect, lowExt ),
		       std::string( "GS1c(" ) + fc.family + ") low-aspect expansion resolves a bbox" );
		Check( expandAndBBox( kHighAspect, highExt ),
		       std::string( "GS1c(" ) + fc.family + ") high-aspect expansion resolves a bbox" );

		const double delta = highExt[fc.axis] - lowExt[fc.axis];
		Check( delta > kMinDelta,
		       std::string( "GS1c(" ) + fc.family + ") the elongation axis's bbox extent grows with `aspect` "
		       "well past the deterministic-jitter floor (low=" + std::to_string( lowExt[fc.axis] ) +
		       " high=" + std::to_string( highExt[fc.axis] ) + " delta=" + std::to_string( delta ) +
		       " > " + std::to_string( kMinDelta ) + ")" );
	}
}

//! GS1d (P2a fix-round): `detail` genuinely flows into the emitted smin
//! blend-radius (`k`) fields for BOTH sdf-based families.  GS1c only
//! pins `aspect`'s flow into bbox extent -- a mutation that hardcodes
//! blended_vessel's smin tightness (drops the `* tightness` factor from
//! k1/k2) passes GS1 AND GS1c untouched (neither observes `k`, and
//! bbox extent is insensitive to a smin blend radius at these part
//! sizes), so this is a DEDICATED text-extraction pin on the `k` token
//! itself, mirroring GS2's persistence-extraction pattern.  Two FRESH
//! documents, SAME name, detail=0.1 vs detail=0.9 (size/aspect held
//! fixed) -- every other jittered constant is IDENTICAL between them,
//! so any difference in the extracted `k` token is attributable to
//! `detail` alone.  sdf_column is structurally identical to
//! blended_vessel here (same `tightness = 1.0 - 0.6*detail` factor on
//! its own k1/k2), so both families get the SAME check, independently.
static void TestGeometryScaffoldSdfDetailFlow()
{
	std::printf( "GS1d: insert_geometry_scaffold -- `detail` genuinely flows into smin blend-radius k (both sdf families)...\n" );

	struct Case { const char* family; const char* roleSuffix; };
	const Case cases[] = {
		{ "blended_vessel", "vessel" },   // part[1] is the base->belly smin (k1)
		{ "sdf_column",     "col" },      // part[1] is the base->shaft smin (k1)
	};

	for( const Case& c : cases ) {
		auto expandK = [&]( double detail ) -> std::string {
			const std::string tmp = TempPath( ( std::string( "agentcrud_gs1d_" ) + c.family + "_" +
				std::to_string( static_cast<int>( detail * 100 ) ) + ".RISEscene" ).c_str() );
			Job* pJob = LoadScene( kScene, tmp );
			if( !pJob ) return std::string();
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const Agent::AgentSession::AgentGeometryScaffoldResult sr =
				sess->InsertGeometryScaffold( c.family, "detflow", 1.0, detail, 1.0 );
			std::string k;
			if( sr.ok ) {
				const std::string doc = sess->ReadDocument();
				const std::string partLine = ExtractNthPartLine( doc, TmplName( "detflow", c.roleSuffix ), 1 );
				k = NthWhitespaceToken( partLine, 2 );
			}
			sess.reset();
			pJob->release();
			std::remove( tmp.c_str() );
			return k;
		};

		const std::string kLow  = expandK( 0.1 );
		const std::string kHigh = expandK( 0.9 );
		Check( !kLow.empty() && !kHigh.empty(),
		       std::string( "GS1d(" ) + c.family + ") extracted a smin `k` token at both detail=0.1 and detail=0.9" );
		Check( kLow != kHigh,
		       std::string( "GS1d(" ) + c.family + ") `detail` genuinely changes the emitted smin `k` (0.1 -> " +
		       kLow + ", 0.9 -> " + kHigh + ") -- pins the mutation class that hardcoding smin tightness would hide" );
	}
}

//! GS2: determinism -- the SAME name, in TWO FRESH documents, produces
//! BYTE-IDENTICAL generated chunk text (no RNG, no clock); a DIFFERENT
//! name visibly differs in its jittered constants (not just the renamed
//! chunk tokens).
static void TestGeometryScaffoldDeterminism()
{
	std::printf( "GS2: insert_geometry_scaffold -- determinism (same name twice byte-identical; different name differs)...\n" );

	auto expandFresh = [&]( const std::string& name ) -> std::string {
		const std::string tmp = TempPath( ( "agentcrud_gs2_" + name + ".RISEscene" ).c_str() );
		Job* pJob = LoadScene( kScene, tmp );
		if( !pJob ) return std::string();
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
		const Agent::AgentSession::AgentGeometryScaffoldResult sr =
			sess->InsertGeometryScaffold( "displaced_slab", name, 1.1, 0.5, 1.0 );
		Check( sr.ok, "GS2 expansion for `" + name + "` is well-formed" );
		const std::string doc = sess->ReadDocument();
		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
		return doc;
	};

	const std::string docA1 = expandFresh( "gscafA" );
	const std::string docA2 = expandFresh( "gscafA" );
	Check( !docA1.empty() && !docA2.empty(), "GS2 both `gscafA` fixtures produced a document" );
	Check( docA1 == docA2,
	       "GS2 SAME name, two FRESH documents -> BYTE-IDENTICAL generated chunk text (deterministic, no RNG/clock)" );

	const std::string docB = expandFresh( "gscafB" );
	Check( !docB.empty(), "GS2 `gscafB` fixture produced a document" );
	Check( docA1 != docB, "GS2 a DIFFERENT name produces a different document (trivially true from the renamed chunks alone)" );

	// The STRONGER claim: the JITTERED NUMERIC CONSTANT differs, not just
	// the renamed chunk tokens -- extract the noise painter's own
	// `persistence` value for each name and require them to differ.
	const std::string persA = ExtractParamAfter( docA1, TmplName( "gscafA", "bump" ), "persistence" );
	const std::string persB = ExtractParamAfter( docB,  TmplName( "gscafB", "bump" ), "persistence" );
	Check( !persA.empty() && !persB.empty(), "GS2 extracted a `persistence` value from both fixtures' noise painter" );
	Check( persA != persB,
	       "GS2 a DIFFERENT name jitters a DIFFERENT `persistence` value (the internal constants really do vary with `name`, not just the labels)" );
}

//! GS3: each of the 5 required params, omitted in turn, is a BLOCKING
//! error naming the missing param -- driven through the REAL wire
//! (AgentRpcDispatcher::HandleLine), mirroring MS3's style.
static void TestGeometryScaffoldMissingParams()
{
	std::printf( "GS3: insert_geometry_scaffold -- each missing required param -> blocking -32602...\n" );
	const std::string tmp = TempPath( "agentcrud_gs3.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "GS3 fixture loads" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const std::string headBefore = sess->ReadDocument();
	Agent::AgentRpcDispatcher disp( std::move( sess ) );

	struct Case { const char* id; const char* paramsJson; const char* missing; };
	const Case cases[] = {
		{ "1", "{\"name\":\"x1\",\"size\":1.0,\"detail\":0.5,\"aspect\":1.0}", "family" },
		{ "2", "{\"family\":\"sdf_column\",\"size\":1.0,\"detail\":0.5,\"aspect\":1.0}", "name" },
		{ "3", "{\"family\":\"sdf_column\",\"name\":\"x3\",\"detail\":0.5,\"aspect\":1.0}", "size" },
		{ "4", "{\"family\":\"sdf_column\",\"name\":\"x4\",\"size\":1.0,\"aspect\":1.0}", "detail" },
		{ "5", "{\"family\":\"sdf_column\",\"name\":\"x5\",\"size\":1.0,\"detail\":0.5}", "aspect" },
	};
	int id = 10;
	for( const Case& c : cases ) {
		const std::string req = std::string( "{\"jsonrpc\":\"2.0\",\"id\":" ) + std::to_string( id++ ) +
			",\"method\":\"insert_geometry_scaffold\",\"params\":" + c.paramsJson + "}";
		const std::string resp = disp.HandleLine( req );
		Check( resp.find( "-32602" ) != std::string::npos,
		       std::string( "GS3(" ) + c.id + ") missing `" + c.missing + "` -> -32602 invalid params" );
		Check( resp.find( c.missing ) != std::string::npos,
		       std::string( "GS3(" ) + c.id + ") the error message NAMES the missing param `" + c.missing + "`" );
	}

	Check( disp.Session() && disp.Session()->ReadDocument() == headBefore,
	       "GS3 none of the 5 missing-param refusals mutated the document" );

	pJob->release();
	std::remove( tmp.c_str() );
}

//! GS4: an unrecognized `family` is refused with a message listing the
//! valid families -- document unchanged.
static void TestGeometryScaffoldBadFamily()
{
	std::printf( "GS4: insert_geometry_scaffold -- unknown family -> error listing valid families...\n" );
	const std::string tmp = TempPath( "agentcrud_gs4.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "GS4 fixture loads" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const std::string headBefore = sess->ReadDocument();

	const Agent::AgentSession::AgentGeometryScaffoldResult sr =
		sess->InsertGeometryScaffold( "twisted_lattice", "x", 1.0, 0.5, 1.0 );
	Check( !sr.ok, "GS4 an unknown family refuses the call (ok==false)" );
	Check( sr.chunkResults.empty(), "GS4 no chunks were generated for an unknown family" );
	const char* families[] = { "displaced_slab", "sweep_rail", "blended_vessel", "sdf_column",
	                           "blended_chain", "volume_bank" };
	for( const char* f : families ) {
		Check( sr.message.find( f ) != std::string::npos,
		       std::string( "GS4 the error message lists valid family `" ) + f + "`" );
	}
	Check( sess->ReadDocument() == headBefore, "GS4 the refusal mutated nothing" );

	pJob->release();
	std::remove( tmp.c_str() );
}

//! GS4b (fix-round-parity P3): `name` past the sane length cap
//! (kScaffoldMaxNameLength == 64, the SAME shared constant
//! insert_material_scaffold uses) is refused, same as any other invalid
//! `name`.
static void TestGeometryScaffoldNameLengthCap()
{
	std::printf( "GS4b: insert_geometry_scaffold -- `name` past the length cap is refused...\n" );
	const std::string tmp = TempPath( "agentcrud_gs4b.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "GS4b fixture loads" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const std::string tooLongName( 65, 'a' );   // one past the 64-char cap
	const Agent::AgentSession::AgentGeometryScaffoldResult sr =
		sess->InsertGeometryScaffold( "sdf_column", tooLongName, 1.0, 0.5, 1.0 );
	Check( !sr.ok, "GS4b a 65-char `name` (one past the cap) is refused" );

	pJob->release();
	std::remove( tmp.c_str() );
}

//! GS5: a name collision -- a family/name expanded once, then the SAME
//! family+name expanded again -- refuses the WHOLE second call cleanly
//! (document unchanged), rather than landing a partial second graph;
//! PLUS a collision against a HAND-AUTHORED chunk sharing the derived
//! name (geometry families don't share role suffixes with each other
//! the way the material families do, so a cross-family collision isn't
//! reachable here -- a hand-authored collision covers the same
//! "collides against anything already in the document" guarantee).
static void TestGeometryScaffoldNameCollision()
{
	std::printf( "GS5: insert_geometry_scaffold -- name collision -> clean refusal, document unchanged...\n" );
	const std::string tmp = TempPath( "agentcrud_gs5.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "GS5 fixture loads" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

	const Agent::AgentSession::AgentGeometryScaffoldResult sr1 =
		sess->InsertGeometryScaffold( "blended_vessel", "gdup1", 1.0, 0.5, 1.0 );
	Check( sr1.ok && !sr1.chunkResults.empty() && sr1.chunkResults.back().applied,
	       "GS5 the FIRST expansion under this name applies cleanly" );

	const std::string headAfterFirst = sess->ReadDocument();
	const RISE::Cst::CstHeadVersion vAfterFirst = sess->HeadVersion();

	// A SECOND expansion, same family AND name -- the generated chunk
	// name collides with the first expansion's.
	const Agent::AgentSession::AgentGeometryScaffoldResult sr2 =
		sess->InsertGeometryScaffold( "blended_vessel", "gdup1", 2.0, 0.9, 1.5 );
	Check( !sr2.ok, "GS5 the SECOND expansion (same family+name) is refused (ok==false)" );
	Check( sr2.chunkResults.empty(), "GS5 the refused expansion generated NO chunk results (refused before InsertChunks ran)" );
	Check( sr2.message.find( "gdup1" ) != std::string::npos, "GS5 the refusal message names the colliding `name`" );

	Check( sess->ReadDocument() == headAfterFirst, "GS5 the document is BYTE-IDENTICAL to before the collision (no partial graph landed)" );
	Check( sess->HeadVersion() == vAfterFirst, "GS5 the head revision did not move" );

	// A collision against a HAND-AUTHORED chunk sharing the derived name.
	const std::vector<Agent::AgentChunkResult> handResults = sess->InsertChunks( {
		"sdf_geometry\n{\nname tmpl_gdup2_col\npart sphere union 0  0 0 0  0 0 0  1 1 1  0.2 0.2 0.2  0.0\n}"
	} );
	Check( handResults.size() == 1 && handResults[0].applied, "GS5 the hand-authored collision fixture itself applied" );
	const std::string headAfterHand = sess->ReadDocument();

	const Agent::AgentSession::AgentGeometryScaffoldResult sr3 =
		sess->InsertGeometryScaffold( "sdf_column", "gdup2", 1.0, 0.5, 1.0 );
	Check( !sr3.ok, "GS5 a collision against a HAND-AUTHORED chunk sharing the derived name is also refused" );
	Check( sess->ReadDocument() == headAfterHand, "GS5 that hand-authored collision ALSO left the document byte-identical" );

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

//! GS6: under External authority with a live controller attached (the
//! same Secure-MCP staging posture insert_chunk/insert_chunks/
//! insert_material_scaffold use), the expansion STAGES proposals rather
//! than committing -- every generated chunk comes back status=="staged",
//! applied==false, and the document is untouched until an Owner resolves
//! them.
static void TestGeometryScaffoldProposalMode()
{
	std::printf( "GS6: insert_geometry_scaffold -- External authority STAGES, does not commit...\n" );
	const std::string tmp = TempPath( "agentcrud_gs6.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "GS6 fixture loads" );
	if( !pJob ) return;

	TestController c( *pJob, /*simulatedRenderMs*/ 0 );
	c.Start();

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob, Agent::AgentAuthority::External );
	sess->AttachController( &c );
	Check( sess->Authority() == Agent::AgentAuthority::External, "GS6 session reports External authority" );

	const std::string headBefore = sess->ReadDocument();
	const Agent::AgentSession::AgentGeometryScaffoldResult sr =
		sess->InsertGeometryScaffold( "sweep_rail", "extG", 1.0, 0.4, 1.0 );

	Check( sr.ok, "GS6 the call itself is well-formed under External authority (ok==true -- generation + collision-precheck succeeded)" );
	Check( !sr.chunkResults.empty(), "GS6 the expansion produced per-chunk results" );
	bool allStaged = true;
	for( const Agent::AgentChunkResult& cr : sr.chunkResults ) {
		if( cr.applied || cr.status != "staged" ) allStaged = false;
	}
	Check( allStaged, "GS6 EVERY generated chunk is staged (applied==false, status==\"staged\"), none committed directly" );
	Check( sess->ReadDocument() == headBefore, "GS6 the document is UNCHANGED -- nothing committed, only proposals queued" );

	c.Stop();
	pJob->release();
	std::remove( tmp.c_str() );
}

//----------------------------------------------------------------------
// GS7-GS20: Arc-75 slice E3 -- insert_geometry_scaffold's two NEW
// families, blended_chain and volume_bank.  Same fixture/helper
// conventions as GS1-GS6 above (TempPath/LoadScene/TmplName/
// ExtractParamAfter/ExtractNthPartLine/GeometryBBoxExtents reused
// verbatim).
//----------------------------------------------------------------------

//! The Nth (0-indexed) `part` line's count for `chunkName` -- keeps
//! incrementing until ExtractNthPartLine comes back empty.  Every part
//! line this generator ever emits has real content, so "empty" only
//! ever means "ran off the end of the list".
static std::size_t CountPartLines( const std::string& doc, const std::string& chunkName )
{
	std::size_t n = 0;
	while( !ExtractNthPartLine( doc, chunkName, n ).empty() ) ++n;
	return n;
}

//! GS7: blended_chain -- basic expansion.  ONE sdf_geometry chunk
//! applies; its bbox spans (at least) the authored path's own extent
//! on every axis (the chain's radius only ADDS to the path's own
//! bounding box, never subtracts); the geometry binds to a plain
//! object+material and that graph applies cleanly.
static void TestGeometryScaffoldBlendedChainBasic()
{
	std::printf( "GS7: insert_geometry_scaffold -- blended_chain basic expansion...\n" );
	const std::string tmp = TempPath( "agentcrud_gs7.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "GS7 fixture loads" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const Agent::AgentSession::AgentGeometryScaffoldResult sr = sess->InsertGeometryScaffold(
		"blended_chain", "chainA", 0.2, 0.5, 0.5, "0 0 0; 0.5 1.0 0.2; 1.0 2.0 0.0", 0.4, std::string() );

	Check( sr.ok, std::string( "GS7 call itself is well-formed: " ) + sr.message );
	Check( sr.chunkResults.size() == 1, "GS7 exactly ONE chunk generated (the sdf_geometry chain)" );
	Check( sr.geometryKind == "sdf_geometry", "GS7 geometry chunk kind is sdf_geometry" );
	Check( sr.geometryName == TmplName( "chainA", "chain" ), "GS7 geometry chunk named tmpl_chainA_chain" );
	Check( sr.materialName.empty() && sr.objectName.empty(),
	       "GS7 blended_chain, like the original four families, emits NO material/object" );

	double ex = 0.0, ey = 0.0, ez = 0.0;
	Check( GeometryBBoxExtents( pJob, sr.geometryName, ex, ey, ez ), "GS7 the chain geometry resolves through the live manager" );
	// Path bbox: x in [0,1.0], y in [0,2.0], z in [0,0.2] -> own extents 1.0/2.0/0.2.
	Check( ex >= 1.0 - 1e-6, "GS7 bbox X extent covers the path's own X extent" );
	Check( ey >= 2.0 - 1e-6, "GS7 bbox Y extent covers the path's own Y extent" );
	Check( ez >= 0.2 - 1e-6, "GS7 bbox Z extent covers the path's own Z extent" );

	const std::vector<Agent::AgentChunkResult> bound = sess->InsertChunks( {
		"uniformcolor_painter\n{\nname pnt_gs7\ncolor 0.6 0.6 0.6\n}",
		"lambertian_material\n{\nname mat_gs7\nreflectance pnt_gs7\n}",
		"standard_object\n{\nname obj_gs7\ngeometry " + sr.geometryName + "\nmaterial mat_gs7\n}",
	} );
	bool allApplied = true;
	for( const auto& cr : bound ) if( !cr.applied ) allApplied = false;
	Check( allApplied, "GS7 binding chunks (painter/material/object) all applied" );

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

//! GS8: `taper` genuinely flows into the realized silhouette -- SAME
//! `name` for both calls (so every other jitter draw is pinned
//! identical), taper=0.9 vs taper=0.0, on a path whose LAST point is
//! the bbox's extremal X corner.  Node 0's radius is UNAFFECTED by
//! taper (taperFactor at u=0 is always 1 regardless of `taper`), but
//! the last node's radius falls from 100% of `size` (taper=0) to ~12%
//! (taper=0.9) -- the X extent (which spans from node0's -radius edge
//! to node-last's +radius edge) must shrink measurably.
static void TestGeometryScaffoldBlendedChainTaperFlow()
{
	std::printf( "GS8: insert_geometry_scaffold -- blended_chain taper genuinely changes the bbox...\n" );

	const std::string points = "0 0 0; 1.0 0.3 0; 2.0 0.0 0";
	const double kSize = 0.2;

	auto expandExtentX = [&]( double taper ) -> double {
		const std::string tmp = TempPath( "agentcrud_gs8.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		if( !pJob ) return -1.0;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
		const auto sr = sess->InsertGeometryScaffold( "blended_chain", "chainT", kSize, 0.5, 1.0, points, taper, std::string() );
		double ex = -1.0, ey = 0.0, ez = 0.0;
		if( sr.ok ) GeometryBBoxExtents( pJob, sr.geometryName, ex, ey, ez );
		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
		return ex;
	};

	const double exLow  = expandExtentX( 0.0 );
	const double exHigh = expandExtentX( 0.9 );
	Check( exLow > 0.0 && exHigh > 0.0, "GS8 both taper variants produced a valid bbox" );
	Check( exLow - exHigh > kSize * 0.3,
	       "GS8 taper=0.9 measurably shrinks the X extent vs taper=0.0 (tip radius genuinely falls off)" );
}

//! GS9 (mutation-style red-proof): `detail` genuinely flows into NODE
//! COUNT -- extracted as the raw `part`-line count of the emitted
//! sdf_geometry chunk, not just a coarse bbox proxy (the GS1d "decoy
//! landmine" discipline: a bbox check alone would not catch a mutation
//! that hardcodes node count away).
static void TestGeometryScaffoldBlendedChainDetailFlow()
{
	std::printf( "GS9: insert_geometry_scaffold -- blended_chain detail genuinely changes node/part count...\n" );

	const std::string points = "0 0 0; 1.0 1.0 0; 2.0 0.0 0";
	auto expandPartCount = [&]( double detail ) -> std::size_t {
		const std::string tmp = TempPath( "agentcrud_gs9.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		if( !pJob ) return 0;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
		const auto sr = sess->InsertGeometryScaffold( "blended_chain", "chainD", 0.2, detail, 1.0, points, 0.3, std::string() );
		std::size_t n = 0;
		if( sr.ok ) n = CountPartLines( sess->ReadDocument(), sr.geometryName );
		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
		return n;
	};

	const std::size_t nLow  = expandPartCount( 0.0 );
	const std::size_t nHigh = expandPartCount( 1.0 );
	Check( nLow > 0 && nHigh > 0, "GS9 both detail variants produced part lines" );
	Check( nHigh > nLow, "GS9 detail=1.0 produces MORE nodes/part-lines than detail=0.0 (node density genuinely flows)" );
}

//! GS10: hostile `points` strings -- wrong arity (1, and 7), a
//! malformed triplet (too few/too many numbers), a non-finite
//! coordinate, an empty string, and non-numeric garbage -- ALL refused
//! with an actionable message, document unchanged.
static void TestGeometryScaffoldBlendedChainHostilePoints()
{
	std::printf( "GS10: insert_geometry_scaffold -- blended_chain hostile `points` strings -> actionable refusals...\n" );
	const std::string tmp = TempPath( "agentcrud_gs10.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "GS10 fixture loads" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const std::string headBefore = sess->ReadDocument();

	struct Case { const char* id; const char* points; };
	const Case cases[] = {
		{ "onePoint",    "0 0 0" },                                                   // arity < 2
		{ "sevenPoints", "0 0 0; 1 0 0; 2 0 0; 3 0 0; 4 0 0; 5 0 0; 6 0 0" },          // arity > 6
		{ "nanCoord",    "0 0 0; nan 1 1" },
		{ "tooFewNums",  "0 0 0; 1 1" },
		{ "tooManyNums", "0 0 0; 1 1 1 1" },
		{ "empty",       "" },
		{ "garbage",     "abc def ghi; 1 2 3" },
	};

	for( const Case& c : cases ) {
		const auto sr = sess->InsertGeometryScaffold( "blended_chain", std::string( "hostile_" ) + c.id,
			0.2, 0.5, 1.0, c.points, 0.3, std::string() );
		Check( !sr.ok, std::string( "GS10(" ) + c.id + ") hostile points string is refused" );
		Check( !sr.message.empty(), std::string( "GS10(" ) + c.id + ") refusal carries a message" );
	}

	Check( sess->ReadDocument() == headBefore, "GS10 none of the hostile-points refusals mutated the document" );

	pJob->release();
	std::remove( tmp.c_str() );
}

//! GS11: determinism -- SAME name, two FRESH documents -> BYTE-
//! IDENTICAL; a DIFFERENT name jitters different node positions/radii
//! (pinned via a raw `part` line, not just the renamed chunk labels).
static void TestGeometryScaffoldBlendedChainDeterminism()
{
	std::printf( "GS11: insert_geometry_scaffold -- blended_chain determinism...\n" );

	const std::string points = "0 0 0; 0.6 1.1 -0.2; 1.3 1.8 0.4";
	auto expandFresh = [&]( const std::string& name ) -> std::string {
		const std::string tmp = TempPath( ( "agentcrud_gs11_" + name + ".RISEscene" ).c_str() );
		Job* pJob = LoadScene( kScene, tmp );
		if( !pJob ) return std::string();
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
		const auto sr = sess->InsertGeometryScaffold( "blended_chain", name, 0.25, 0.5, 1.0, points, 0.4, std::string() );
		Check( sr.ok, "GS11 expansion for `" + name + "` is well-formed" );
		const std::string doc = sess->ReadDocument();
		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
		return doc;
	};

	const std::string docA1 = expandFresh( "chainDetA" );
	const std::string docA2 = expandFresh( "chainDetA" );
	Check( !docA1.empty() && !docA2.empty(), "GS11 both `chainDetA` fixtures produced a document" );
	Check( docA1 == docA2, "GS11 SAME name, two FRESH documents -> BYTE-IDENTICAL chunk text" );

	const std::string docB = expandFresh( "chainDetB" );
	Check( !docB.empty(), "GS11 `chainDetB` fixture produced a document" );
	Check( docA1 != docB, "GS11 a DIFFERENT name produces a different document" );

	const std::string lineA = ExtractNthPartLine( docA1, TmplName( "chainDetA", "chain" ), 1 );
	const std::string lineB = ExtractNthPartLine( docB,  TmplName( "chainDetB", "chain" ), 1 );
	Check( !lineA.empty() && !lineB.empty(), "GS11 extracted part-line 1 from both fixtures" );
	Check( lineA != lineB, "GS11 a DIFFERENT name jitters different node positions/radii (not just labels)" );
}

//! GS12: wire-level required-param shape for blended_chain -- `points`
//! and `taper` are REQUIRED (missing -> -32602 naming the param);
//! `aspect` is genuinely NOT required (a well-formed call omitting it
//! entirely still succeeds).
static void TestGeometryScaffoldBlendedChainMissingParamsWire()
{
	std::printf( "GS12: insert_geometry_scaffold -- blended_chain wire-level required params (points/taper, NOT aspect)...\n" );
	const std::string tmp = TempPath( "agentcrud_gs12.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "GS12 fixture loads" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const std::string headBefore = sess->ReadDocument();
	Agent::AgentRpcDispatcher disp( std::move( sess ) );

	struct Case { const char* id; const char* paramsJson; const char* missing; };
	const Case cases[] = {
		{ "1", "{\"family\":\"blended_chain\",\"name\":\"cw1\",\"size\":0.2,\"detail\":0.5,\"taper\":0.3}", "points" },
		{ "2", "{\"family\":\"blended_chain\",\"name\":\"cw2\",\"size\":0.2,\"detail\":0.5,\"points\":\"0 0 0; 1 1 1\"}", "taper" },
	};
	int id = 120;
	for( const Case& c : cases ) {
		const std::string req = std::string( "{\"jsonrpc\":\"2.0\",\"id\":" ) + std::to_string( id++ ) +
			",\"method\":\"insert_geometry_scaffold\",\"params\":" + c.paramsJson + "}";
		const std::string resp = disp.HandleLine( req );
		Check( resp.find( "-32602" ) != std::string::npos,
		       std::string( "GS12(" ) + c.id + ") missing `" + c.missing + "` -> -32602 invalid params" );
		Check( resp.find( c.missing ) != std::string::npos,
		       std::string( "GS12(" ) + c.id + ") the error message NAMES the missing param `" + c.missing + "`" );
	}
	Check( disp.Session() && disp.Session()->ReadDocument() == headBefore,
	       "GS12 the two missing-param refusals mutated nothing" );

	{
		const std::string req = "{\"jsonrpc\":\"2.0\",\"id\":129,\"method\":\"insert_geometry_scaffold\","
			"\"params\":{\"family\":\"blended_chain\",\"name\":\"cw3\",\"size\":0.2,\"detail\":0.5,"
			"\"points\":\"0 0 0; 1 1 1\",\"taper\":0.3}}";
		const std::string resp = disp.HandleLine( req );
		Check( resp.find( "\"error\"" ) == std::string::npos,
		       "GS12(3) a well-formed blended_chain call WITHOUT `aspect` succeeds (genuinely not required)" );
	}

	pJob->release();
	std::remove( tmp.c_str() );
}

//! GS13: volume_bank -- basic expansion.  SEVEN chunks (container,
//! two tone painters, the domain-warped density painter, the
//! dielectric shell, the medium, the standard_object); the object's
//! own text correctly WIRES geometry/material/interior_medium to the
//! other emitted chunks; the success `message` carries the factual
//! bbox-coupling note; and -- E1 gate interplay -- deriving the
//! resulting document does NOT trip LUMINAIRE_NULL_GEOMETRY (the
//! emitted object is non-emissive with real geometry, nothing like the
//! null-geometry csg_object class that gate targets).
static void TestGeometryScaffoldVolumeBankBasic()
{
	std::printf( "GS13: insert_geometry_scaffold -- volume_bank basic expansion + wiring + E1 non-trip...\n" );
	const std::string tmp = TempPath( "agentcrud_gs13.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "GS13 fixture loads" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const Agent::AgentSession::AgentGeometryScaffoldResult sr = sess->InsertGeometryScaffold(
		"volume_bank", "bankA", 1.2, 0.5, 1.8, std::string(), 0.0, "0.7 0.75 0.9" );

	Check( sr.ok, std::string( "GS13 call itself is well-formed: " ) + sr.message );
	Check( sr.chunkResults.size() == 7,
	       "GS13 exactly SEVEN chunks generated (container, 2 tone painters, density, shell, medium, object)" );
	Check( sr.geometryKind == "ellipsoid_geometry", "GS13 geometry chunk kind is ellipsoid_geometry" );
	Check( sr.materialKind == "dielectric_material", "GS13 material chunk kind is dielectric_material" );
	Check( sr.mediumKind == "painter_heterogeneous_medium", "GS13 medium chunk kind is painter_heterogeneous_medium" );
	Check( sr.objectKind == "standard_object", "GS13 object chunk kind is standard_object" );
	Check( !sr.message.empty(), "GS13 an ok==true volume_bank result carries a non-empty `message`" );
	Check( sr.message.find( "bbox" ) != std::string::npos,
	       "GS13 the message actually mentions the bbox coupling caveat" );

	bool allApplied = true;
	for( const auto& cr : sr.chunkResults ) if( !cr.applied ) allApplied = false;
	Check( allApplied, "GS13 every generated chunk applied" );

	const std::string doc = sess->ReadDocument();
	Check( ExtractParamAfter( doc, sr.objectName, "geometry" ) == sr.geometryName,
	       "GS13 the emitted object's `geometry` references the emitted container" );
	Check( ExtractParamAfter( doc, sr.objectName, "material" ) == sr.materialName,
	       "GS13 the emitted object's `material` references the emitted dielectric shell" );
	Check( ExtractParamAfter( doc, sr.objectName, "interior_medium" ) == sr.mediumName,
	       "GS13 the emitted object's `interior_medium` references the emitted medium" );

	const std::vector<Agent::AgentDiagnostic> diags = Agent::AgentSession::ValidateText( doc );
	bool sawNullGeomEmitter = false;
	for( const Agent::AgentDiagnostic& d : diags ) if( d.code == "LUMINAIRE_NULL_GEOMETRY" ) sawNullGeomEmitter = true;
	Check( !sawNullGeomEmitter, "GS13 volume_bank's non-emissive, real-geometry object does NOT trip the E1 emissive-CSG gate" );

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

//! GS14: `aspect` genuinely elongates the container along local X.
static void TestGeometryScaffoldVolumeBankAspectFlow()
{
	std::printf( "GS14: insert_geometry_scaffold -- volume_bank aspect elongates the container along X...\n" );

	auto expandExtentX = [&]( double aspect ) -> double {
		const std::string tmp = TempPath( "agentcrud_gs14.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		if( !pJob ) return -1.0;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
		const auto sr = sess->InsertGeometryScaffold( "volume_bank", "bankAsp", 1.0, 0.5, aspect, std::string(), 0.0, "0.6 0.6 0.6" );
		double ex = -1.0, ey = 0.0, ez = 0.0;
		if( sr.ok ) GeometryBBoxExtents( pJob, sr.geometryName, ex, ey, ez );
		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
		return ex;
	};

	const double exLow  = expandExtentX( 1.0 );
	const double exHigh = expandExtentX( 3.0 );
	Check( exLow > 0.0 && exHigh > 0.0, "GS14 both aspect variants produced a valid bbox" );
	Check( exHigh > exLow * 1.8, "GS14 aspect=3.0 measurably elongates the container's X extent vs aspect=1.0" );
}

//! GS15 (mutation-style red-proof): `tone` genuinely flows into the
//! MEDIUM'S OWN `scattering` value -- a different tone -> a different
//! raw `scattering` token, pinned via text extraction (not a render
//! proxy, which Monte Carlo noise would make vacuous).
static void TestGeometryScaffoldVolumeBankToneFlow()
{
	std::printf( "GS15: insert_geometry_scaffold -- volume_bank tone genuinely changes the medium's scattering...\n" );

	auto expandScattering = [&]( const std::string& tone ) -> std::string {
		const std::string tmp = TempPath( "agentcrud_gs15.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		if( !pJob ) return std::string();
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
		const auto sr = sess->InsertGeometryScaffold( "volume_bank", "bankTone", 1.0, 0.5, 1.5, std::string(), 0.0, tone );
		std::string scattering;
		if( sr.ok ) scattering = ExtractParamAfter( sess->ReadDocument(), sr.mediumName, "scattering" );
		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
		return scattering;
	};

	const std::string scatWarm = expandScattering( "0.95 0.15 0.05" );
	const std::string scatCool = expandScattering( "0.05 0.15 0.95" );
	Check( !scatWarm.empty() && !scatCool.empty(), "GS15 both tone variants produced a `scattering` value" );
	Check( scatWarm != scatCool, "GS15 a DIFFERENT `tone` produces a DIFFERENT medium `scattering` value" );
}

//! GS16 (mutation-style red-proof; P1c fix round): `detail` genuinely
//! flows into the density painter's `warp_amplitude` -- pinned via text
//! extraction, but ONLY after confirming the MEDIUM chunk's own
//! `density_painter` reference actually points at that chunk.  The
//! ORIGINAL version of this test extracted `warp_amplitude` straight
//! off `tmpl_bankDet_density` by NAME, which is silently vacuous
//! against a mutation that re-points the medium's `density_painter` at
//! some OTHER (e.g. constant) painter and leaves the real density
//! chunk orphaned in the document -- the orphan would still exist,
//! still be named correctly, and still show a `detail`-varying
//! `warp_amplitude`, even though the RENDERED medium no longer reads
//! it at all.  Checking the wiring FIRST closes that gap.
static void TestGeometryScaffoldVolumeBankDetailFlow()
{
	std::printf( "GS16: insert_geometry_scaffold -- volume_bank detail genuinely changes the WIRED density painter's warp_amplitude...\n" );

	auto expandWarpAmp = [&]( double detail ) -> std::string {
		const std::string tmp = TempPath( "agentcrud_gs16.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		if( !pJob ) return std::string();
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
		const auto sr = sess->InsertGeometryScaffold( "volume_bank", "bankDet", 1.0, detail, 1.5, std::string(), 0.0, "0.6 0.6 0.6" );
		std::string warpAmp;
		if( sr.ok ) {
			const std::string doc = sess->ReadDocument();
			// THE WIRING CHECK: the medium's `density_painter` param must
			// actually reference the density chunk this generator emitted
			// -- not just that a correctly-named chunk happens to exist
			// somewhere in the document.
			const std::string wired = ExtractParamAfter( doc, sr.mediumName, "density_painter" );
			Check( wired == TmplName( "bankDet", "density" ),
			       "GS16 the medium's `density_painter` is WIRED to the emitted density painter (not an orphan)" );
			if( wired == TmplName( "bankDet", "density" ) ) {
				warpAmp = ExtractParamAfter( doc, wired, "warp_amplitude" );
			}
		}
		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
		return warpAmp;
	};

	const std::string ampLow  = expandWarpAmp( 0.0 );
	const std::string ampHigh = expandWarpAmp( 1.0 );
	Check( !ampLow.empty() && !ampHigh.empty(), "GS16 both detail variants produced a `warp_amplitude` value" );
	Check( ampLow != ampHigh, "GS16 a DIFFERENT `detail` produces a DIFFERENT density-painter `warp_amplitude`" );
}

//! GS17: hostile `tone` strings -- empty, wrong arity (2, 4), out of
//! [0,1] range (both directions), non-finite, and non-numeric garbage
//! -- ALL refused with an actionable message, document unchanged.
static void TestGeometryScaffoldVolumeBankHostileTone()
{
	std::printf( "GS17: insert_geometry_scaffold -- volume_bank hostile `tone` strings -> actionable refusals...\n" );
	const std::string tmp = TempPath( "agentcrud_gs17.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "GS17 fixture loads" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const std::string headBefore = sess->ReadDocument();

	struct Case { const char* id; const char* tone; };
	const Case cases[] = {
		{ "empty",     "" },
		{ "twoNums",   "0.5 0.5" },
		{ "fourNums",  "0.5 0.5 0.5 0.5" },
		{ "outOfLow",  "-0.1 0.5 0.5" },
		{ "outOfHigh", "0.5 1.5 0.5" },
		{ "nan",       "nan 0.5 0.5" },
		{ "garbage",   "red green blue" },
	};

	for( const Case& c : cases ) {
		const auto sr = sess->InsertGeometryScaffold( "volume_bank", std::string( "hbank_" ) + c.id,
			1.0, 0.5, 1.5, std::string(), 0.0, c.tone );
		Check( !sr.ok, std::string( "GS17(" ) + c.id + ") hostile tone string is refused" );
		Check( !sr.message.empty(), std::string( "GS17(" ) + c.id + ") refusal carries a message" );
	}

	Check( sess->ReadDocument() == headBefore, "GS17 none of the hostile-tone refusals mutated the document" );

	pJob->release();
	std::remove( tmp.c_str() );
}

//! GS18: wire-level required-param shape for volume_bank -- `aspect`
//! and `tone` are REQUIRED (missing -> -32602 naming the param);
//! `points`/`taper` are genuinely NOT required.
static void TestGeometryScaffoldVolumeBankMissingParamsWire()
{
	std::printf( "GS18: insert_geometry_scaffold -- volume_bank wire-level required params (aspect/tone, NOT points/taper)...\n" );
	const std::string tmp = TempPath( "agentcrud_gs18.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "GS18 fixture loads" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const std::string headBefore = sess->ReadDocument();
	Agent::AgentRpcDispatcher disp( std::move( sess ) );

	struct Case { const char* id; const char* paramsJson; const char* missing; };
	const Case cases[] = {
		{ "1", "{\"family\":\"volume_bank\",\"name\":\"bw1\",\"size\":1.0,\"detail\":0.5,\"tone\":\"0.5 0.5 0.5\"}", "aspect" },
		{ "2", "{\"family\":\"volume_bank\",\"name\":\"bw2\",\"size\":1.0,\"detail\":0.5,\"aspect\":1.5}", "tone" },
	};
	int id = 130;
	for( const Case& c : cases ) {
		const std::string req = std::string( "{\"jsonrpc\":\"2.0\",\"id\":" ) + std::to_string( id++ ) +
			",\"method\":\"insert_geometry_scaffold\",\"params\":" + c.paramsJson + "}";
		const std::string resp = disp.HandleLine( req );
		Check( resp.find( "-32602" ) != std::string::npos,
		       std::string( "GS18(" ) + c.id + ") missing `" + c.missing + "` -> -32602 invalid params" );
		Check( resp.find( c.missing ) != std::string::npos,
		       std::string( "GS18(" ) + c.id + ") the error message NAMES the missing param `" + c.missing + "`" );
	}
	Check( disp.Session() && disp.Session()->ReadDocument() == headBefore,
	       "GS18 the two missing-param refusals mutated nothing" );

	{
		const std::string req = "{\"jsonrpc\":\"2.0\",\"id\":139,\"method\":\"insert_geometry_scaffold\","
			"\"params\":{\"family\":\"volume_bank\",\"name\":\"bw3\",\"size\":1.0,\"detail\":0.5,"
			"\"aspect\":1.5,\"tone\":\"0.5 0.6 0.7\"}}";
		const std::string resp = disp.HandleLine( req );
		Check( resp.find( "\"error\"" ) == std::string::npos,
		       "GS18(3) a well-formed volume_bank call WITHOUT `points`/`taper` succeeds (genuinely not required)" );
	}

	pJob->release();
	std::remove( tmp.c_str() );
}

//! GS19: volume_bank determinism -- SAME name, two FRESH documents ->
//! BYTE-IDENTICAL; a DIFFERENT name jitters a different density-
//! painter `persistence` (not just the renamed chunk labels).
static void TestGeometryScaffoldVolumeBankDeterminism()
{
	std::printf( "GS19: insert_geometry_scaffold -- volume_bank determinism...\n" );

	auto expandFresh = [&]( const std::string& name ) -> std::string {
		const std::string tmp = TempPath( ( "agentcrud_gs19_" + name + ".RISEscene" ).c_str() );
		Job* pJob = LoadScene( kScene, tmp );
		if( !pJob ) return std::string();
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
		const auto sr = sess->InsertGeometryScaffold( "volume_bank", name, 1.1, 0.5, 1.8, std::string(), 0.0, "0.6 0.7 0.8" );
		Check( sr.ok, "GS19 expansion for `" + name + "` is well-formed" );
		const std::string doc = sess->ReadDocument();
		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
		return doc;
	};

	const std::string docA1 = expandFresh( "bankDetmA" );
	const std::string docA2 = expandFresh( "bankDetmA" );
	Check( !docA1.empty() && !docA2.empty(), "GS19 both `bankDetmA` fixtures produced a document" );
	Check( docA1 == docA2, "GS19 SAME name, two FRESH documents -> BYTE-IDENTICAL chunk text" );

	const std::string docB = expandFresh( "bankDetmB" );
	Check( !docB.empty(), "GS19 `bankDetmB` fixture produced a document" );
	Check( docA1 != docB, "GS19 a DIFFERENT name produces a different document" );

	const std::string persA = ExtractParamAfter( docA1, TmplName( "bankDetmA", "density" ), "persistence" );
	const std::string persB = ExtractParamAfter( docB,  TmplName( "bankDetmB", "density" ), "persistence" );
	Check( !persA.empty() && !persB.empty(), "GS19 extracted a `persistence` value from both fixtures' density painter" );
	Check( persA != persB, "GS19 a DIFFERENT name jitters a DIFFERENT `persistence` value" );
}

//! GS20: the ORIGINAL four families genuinely IGNORE the E3-added
//! `points`/`taper`/`tone` params -- a call passing garbage for all
//! three produces BYTE-IDENTICAL output to omitting them entirely
//! (same `name`, so every jittered constant is pinned identical; the
//! ONLY variable is whether points/taper/tone carry garbage).  The
//! "points ignored" red-proof the slice brief calls for.
static void TestGeometryScaffoldOriginalFamiliesIgnoreNewParams()
{
	std::printf( "GS20: insert_geometry_scaffold -- original families genuinely IGNORE points/taper/tone...\n" );

	auto expand = [&]( const std::string& tmpSuffix, const std::string& points, double taper, const std::string& tone ) -> std::string {
		const std::string tmp = TempPath( ( "agentcrud_gs20_" + tmpSuffix + ".RISEscene" ).c_str() );
		Job* pJob = LoadScene( kScene, tmp );
		if( !pJob ) return std::string();
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
		const auto sr = sess->InsertGeometryScaffold( "sdf_column", "ignA", 1.0, 0.5, 1.2, points, taper, tone );
		Check( sr.ok, std::string( "GS20 expansion (" ) + tmpSuffix + ") is well-formed: " + sr.message );
		const std::string doc = sess->ReadDocument();
		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
		return doc;
	};

	const std::string docPlain   = expand( "plain",   std::string(), 0.0, std::string() );
	const std::string docGarbage = expand( "garbage", "this is not a points string!!", 0.77, "not a tone either" );

	Check( !docPlain.empty() && !docGarbage.empty(), "GS20 both `ignA` variants produced a document" );
	Check( docPlain == docGarbage,
	       "GS20 sdf_column's output is BYTE-IDENTICAL whether points/taper/tone are omitted or garbage -- genuinely ignored" );
}

//----------------------------------------------------------------------
// GS21-GS25: E3 FIX ROUND (P1a-P1d, P2) -- two fresh reviewers found the
// original blended_chain smin floor mathematically insufficient (a
// spacing-only guess, not derived from the actual bridging condition of
// RISE's polynomial smin), plus three test-coverage gaps (endpoint
// exactness, volume_bank wiring, volume_bank bbox coupling) that
// existing mutation-style checks did not actually kill.  See
// BuildBlendedChain's own comment (AgentSession.cpp) for the sminP
// bridging derivation these tests pin.
//----------------------------------------------------------------------

//! One parsed chain-part line's geometrically relevant fields (position,
//! smin `k`, radius) -- pulled via NthWhitespaceToken off the SAME
//! ExtractNthPartLine text ExtractNthPartLine already returns (the
//! `<prim> <op> <k>  <px py pz>  ...  <a b c>  <round>` 16-token grammar
//! -- k is token 2, px/py/pz are tokens 3/4/5, a (radius, for a sphere
//! part) is token 12).
struct ChainPartNode
{
	double px = 0.0, py = 0.0, pz = 0.0, k = 0.0, radius = 0.0;
};

static bool ParseChainPartLine( const std::string& line, ChainPartNode& out )
{
	if( line.empty() ) return false;
	const std::string kStr  = NthWhitespaceToken( line, 2 );
	const std::string pxStr = NthWhitespaceToken( line, 3 );
	const std::string pyStr = NthWhitespaceToken( line, 4 );
	const std::string pzStr = NthWhitespaceToken( line, 5 );
	const std::string aStr  = NthWhitespaceToken( line, 12 );
	if( kStr.empty() || pxStr.empty() || pyStr.empty() || pzStr.empty() || aStr.empty() ) return false;
	out.k      = std::atof( kStr.c_str() );
	out.px     = std::atof( pxStr.c_str() );
	out.py     = std::atof( pyStr.c_str() );
	out.pz     = std::atof( pzStr.c_str() );
	out.radius = std::atof( aStr.c_str() );
	return true;
}

//! Every part-line node of `chunkName`, IN ORDER -- keeps incrementing
//! until ExtractNthPartLine (or the parse) comes back empty, same
//! convention as CountPartLines above.
static std::vector<ChainPartNode> ParseAllChainNodes( const std::string& doc, const std::string& chunkName )
{
	std::vector<ChainPartNode> out;
	for( std::size_t i = 0; ; ++i ) {
		const std::string line = ExtractNthPartLine( doc, chunkName, i );
		if( line.empty() ) break;
		ChainPartNode n;
		if( !ParseChainPartLine( line, n ) ) break;
		out.push_back( n );
	}
	return out;
}

//! GS21 (P1a fix-round text-level invariant -- "kills mutation a", the
//! raw-k-with-no-bridging-floor mutation): parses EVERY joint of an
//! emitted blended_chain and asserts the EXACT polynomial-smin bridging
//! condition holds -- k_j >= 2*max(0, spacing_j - r_{j-1} - r_j) (see
//! BuildBlendedChain's own comment for the sminP derivation).  This
//! checks the TIGHT mathematical necessity, NOT the generator's own
//! 1.25x-padded floor -- so it cannot be fooled by an implementation
//! that merely echoes its own formula back at itself; a genuinely
//! insufficient k (even one that satisfies some OTHER, wrong formula)
//! fails this check.  Run across: reviewer B's adversarial config (2
//! points 8 units apart, size 0.3, taper 1.0 -- full taper, the worst
//! case for radius/spacing mismatch -- detail 0.0 -- minimum detail-
//! driven node count, relying entirely on the geometric density uplift
//! + the per-joint floor), the skill doc's shipped worked example
//! (modeling-workflow-and-geometry.md's `branch1`), and 3 additional
//! jittered name variants on a shared moderate config.
static void TestGeometryScaffoldBlendedChainContinuityInvariant()
{
	std::printf( "GS21: insert_geometry_scaffold -- blended_chain EXACT smin-bridging invariant holds per joint...\n" );

	auto checkInvariant = [&]( const std::string& label, const std::string& name,
	                           double size, double detail, const std::string& points, double taper ) {
		const std::string tmp = TempPath( ( "agentcrud_gs21_" + name + ".RISEscene" ).c_str() );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, label + " fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
		const auto sr = sess->InsertGeometryScaffold( "blended_chain", name, size, detail, 1.0, points, taper, std::string() );
		Check( sr.ok, label + " expansion is well-formed: " + sr.message );
		if( sr.ok ) {
			const std::vector<ChainPartNode> nodes = ParseAllChainNodes( sess->ReadDocument(), sr.geometryName );
			Check( nodes.size() >= 2, label + " parsed at least 2 chain nodes" );
			bool allBridge = true;
			double worstMargin = 1e30;
			for( std::size_t i = 1; i < nodes.size(); ++i ) {
				const double dx = nodes[i].px - nodes[i-1].px;
				const double dy = nodes[i].py - nodes[i-1].py;
				const double dz = nodes[i].pz - nodes[i-1].pz;
				const double spacing = std::sqrt( dx*dx + dy*dy + dz*dz );
				const double gap = std::max( 0.0, spacing - nodes[i-1].radius - nodes[i].radius );
				const double required = 2.0 * gap;
				if( nodes[i].k < required - 1e-9 ) allBridge = false;
				worstMargin = std::min( worstMargin, nodes[i].k - required );
			}
			Check( allBridge, label + " EVERY joint satisfies k >= 2*surface_gap (the exact sminP bridging condition)" );
			std::printf( "    %s: %zu nodes, worst (k - required) margin = %.6f\n",
			             label.c_str(), nodes.size(), worstMargin );
		}
		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	};

	checkInvariant( "GS21(adversarial)", "gs21adv", 0.3, 0.0, "0 0 0; 8 0 0", 1.0 );
	checkInvariant( "GS21(skilldoc)", "branch1", 0.22, 0.6,
	                "0 0 0; 0.4 1.1 -0.2; 0.9 1.6 0.3; 1.5 1.4 0.9", 0.65 );
	checkInvariant( "GS21(jitterA)", "gs21jitA", 0.25, 0.4, "0 0 0; 1 1 0.5; 2 0.5 1", 0.5 );
	checkInvariant( "GS21(jitterB)", "gs21jitB", 0.25, 0.4, "0 0 0; 1 1 0.5; 2 0.5 1", 0.5 );
	checkInvariant( "GS21(jitterC)", "gs21jitC", 0.25, 0.4, "0 0 0; 1 1 0.5; 2 0.5 1", 0.5 );
}

//! GS22 (P1b fix-round -- "kills mutation b", the u*m extrapolation
//! bug class): parses the FIRST and LAST emitted chain nodes and
//! asserts they equal the authored first/last `points` triplet within
//! 1e-6 -- the documented attachment-mechanism guarantee, previously
//! only checked informally (GS7's bbox-covers-the-path-extent
//! assertion, which a u*m-style off-by-one could still pass).
static void TestGeometryScaffoldBlendedChainEndpointExactness()
{
	std::printf( "GS22: insert_geometry_scaffold -- blended_chain first/last node lands EXACTLY on the authored endpoints...\n" );
	const std::string tmp = TempPath( "agentcrud_gs22.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "GS22 fixture loads" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const std::string points = "0.3 -0.5 1.2; 1.1 0.7 -0.3; 2.4 1.9 0.6";
	const auto sr = sess->InsertGeometryScaffold( "blended_chain", "gs22ep", 0.2, 0.5, 1.0, points, 0.4, std::string() );
	Check( sr.ok, std::string( "GS22 expansion is well-formed: " ) + sr.message );
	if( sr.ok ) {
		const std::vector<ChainPartNode> nodes = ParseAllChainNodes( sess->ReadDocument(), sr.geometryName );
		Check( nodes.size() >= 2, "GS22 parsed at least 2 chain nodes" );
		if( nodes.size() >= 2 ) {
			const ChainPartNode& first = nodes.front();
			const ChainPartNode& last  = nodes.back();
			Check( std::fabs( first.px - 0.3 ) < 1e-6 && std::fabs( first.py - ( -0.5 ) ) < 1e-6 &&
			       std::fabs( first.pz - 1.2 ) < 1e-6,
			       "GS22 the FIRST node lands EXACTLY on the first authored point (within 1e-6)" );
			Check( std::fabs( last.px - 2.4 ) < 1e-6 && std::fabs( last.py - 1.9 ) < 1e-6 &&
			       std::fabs( last.pz - 0.6 ) < 1e-6,
			       "GS22 the LAST node lands EXACTLY on the last authored point (within 1e-6)" );
		}
	}

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

//! GS23 (P1d fix-round -- "kills mutation e", a wrong bbox-margin
//! mutation): parses the medium's `bbox_min`/`bbox_max` AND the
//! container's own `radii` from the emitted volume_bank document and
//! asserts the bbox contains the container's realized extent with
//! (approximately) the documented 8% margin -- BOTH directions: the
//! bbox must not be too small (the density field would clip the
//! container's own surface) and must not be "wildly larger" than the
//! documented margin either (a stale/mismatched constant silently
//! drifting).  The emitted object sits at the origin with an identity
//! transform (BuildVolumeBank's own documented design), so the
//! container's WORLD-space extent is exactly +-radii.
static void TestGeometryScaffoldVolumeBankBboxContainerCoupling()
{
	std::printf( "GS23: insert_geometry_scaffold -- volume_bank bbox_min/max match the container's radii within the documented 8%% margin...\n" );
	const std::string tmp = TempPath( "agentcrud_gs23.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "GS23 fixture loads" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const auto sr = sess->InsertGeometryScaffold( "volume_bank", "gs23bbox", 1.3, 0.5, 2.1, std::string(), 0.0, "0.5 0.6 0.7" );
	Check( sr.ok, std::string( "GS23 expansion is well-formed: " ) + sr.message );
	if( sr.ok ) {
		const std::string doc = sess->ReadDocument();
		const std::string radiiStr   = ExtractParamAfter( doc, sr.geometryName, "radii" );
		const std::string bboxMinStr = ExtractParamAfter( doc, sr.mediumName, "bbox_min" );
		const std::string bboxMaxStr = ExtractParamAfter( doc, sr.mediumName, "bbox_max" );
		Check( !radiiStr.empty() && !bboxMinStr.empty() && !bboxMaxStr.empty(),
		       "GS23 extracted radii/bbox_min/bbox_max text" );

		double rx=0.0, ry=0.0, rz=0.0, bMinX=0.0, bMinY=0.0, bMinZ=0.0, bMaxX=0.0, bMaxY=0.0, bMaxZ=0.0;
		std::sscanf( radiiStr.c_str(), "%lf %lf %lf", &rx, &ry, &rz );
		std::sscanf( bboxMinStr.c_str(), "%lf %lf %lf", &bMinX, &bMinY, &bMinZ );
		std::sscanf( bboxMaxStr.c_str(), "%lf %lf %lf", &bMaxX, &bMaxY, &bMaxZ );

		const double kMargin = 1.08;
		const double kTol = 0.01;   // 1% relative tolerance around the documented margin
		auto checkAxis = [&]( const char* axis, double r, double bMin, double bMax ) {
			Check( bMax >= r, std::string( "GS23 bbox_max." ) + axis + " contains the container's radius (not too small)" );
			Check( bMin <= -r, std::string( "GS23 bbox_min." ) + axis + " contains the container's radius (not too small)" );
			Check( std::fabs( bMax - r * kMargin ) < r * kTol,
			       std::string( "GS23 bbox_max." ) + axis + " matches radius*1.08 within 1% (not wildly larger than documented)" );
			Check( std::fabs( bMin - ( -r * kMargin ) ) < r * kTol,
			       std::string( "GS23 bbox_min." ) + axis + " matches -radius*1.08 within 1% (not wildly larger than documented)" );
		};
		checkAxis( "x", rx, bMinX, bMaxX );
		checkAxis( "y", ry, bMinY, bMaxY );
		checkAxis( "z", rz, bMinZ, bMaxZ );
	}

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

//! GS24 (P2 fix-round): a `points` coordinate past the 1e6 sane
//! magnitude bound is refused with an actionable message naming the
//! offending triplet -- document unchanged.
static void TestGeometryScaffoldBlendedChainPointsMagnitudeCap()
{
	std::printf( "GS24: insert_geometry_scaffold -- blended_chain `points` magnitude cap...\n" );
	const std::string tmp = TempPath( "agentcrud_gs24.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "GS24 fixture loads" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const std::string headBefore = sess->ReadDocument();

	const auto sr = sess->InsertGeometryScaffold( "blended_chain", "gs24big", 0.2, 0.5, 1.0,
		"0 0 0; 1e9 0 0", 0.3, std::string() );
	Check( !sr.ok, "GS24 a points coordinate past the 1e6 magnitude cap is refused" );
	Check( sr.message.find( "1e9" ) != std::string::npos,
	       "GS24 the refusal names the offending triplet" );
	Check( sess->ReadDocument() == headBefore, "GS24 the refusal mutated nothing" );

	pJob->release();
	std::remove( tmp.c_str() );
}

//! GS25 (P1a re-verification -- render-based, INDEPENDENT of the
//! algebraic invariant in GS21, which shares the SAME bridging formula
//! with the generator itself and so cannot catch a bug in that shared
//! formula): renders reviewer B's adversarial config (the exact config
//! that fractured into 7 blobs against the pre-fix floor) front-on and
//! confirms a CONTINUOUS silhouette -- along the row through the
//! chain's centerline, every pixel between the first and last bright
//! (object) pixel is ALSO bright; a fractured chain shows dark
//! "interior gap" pixels between separate blobs (the exact methodology
//! the reviewing round itself used: "278/640 interior gap pixels" on
//! the pre-fix floor-removed variant).
static const char* const kChainRenderScene =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 8\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 240\n\theight 240\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 14\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n"
	"directional_light\n{\n\tname key\n\tpower 3.0\n\tcolor 1 1 1\n\tdirection 0.2 0.3 1.0\n}\n";

static void TestGeometryScaffoldBlendedChainAdversarialContinuityRender()
{
	std::printf( "GS25: insert_geometry_scaffold -- blended_chain adversarial config renders a CONTINUOUS silhouette...\n" );
	const std::string tmp = TempPath( "agentcrud_gs25.RISEscene" );
	Job* pJob = LoadScene( kChainRenderScene, tmp );
	Check( pJob != nullptr, "GS25 fixture loads" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const auto sr = sess->InsertGeometryScaffold( "blended_chain", "gs25adv", 0.3, 0.0, 1.0, "-4 0 0; 4 0 0", 1.0, std::string() );
	Check( sr.ok, std::string( "GS25 expansion is well-formed: " ) + sr.message );
	if( !sr.ok ) { pJob->release(); std::remove( tmp.c_str() ); return; }

	const std::vector<Agent::AgentChunkResult> bound = sess->InsertChunks( {
		"uniformcolor_painter\n{\nname pnt_gs25\ncolor 0.8 0.8 0.8\n}",
		"lambertian_material\n{\nname mat_gs25\nreflectance pnt_gs25\n}",
		"standard_object\n{\nname obj_gs25\ngeometry " + sr.geometryName + "\nmaterial mat_gs25\n}",
	} );
	bool allApplied = true;
	for( const auto& cr : bound ) if( !cr.applied ) allApplied = false;
	Check( allApplied, "GS25 binding chunks (painter/material/object) all applied" );

	Agent::AgentRenderParams rp;
	rp.width = 240; rp.height = 240; rp.samples = 8;
	const Agent::AgentRenderResult rr = sess->Render( rp );
	Check( rr.ok, "GS25 the adversarial config renders" );

	DecodedLuma luma;
	Check( rr.ok && DecodeRenderLuma( rr.png, luma ), "GS25 the render decodes" );
	if( rr.ok && luma.w > 0 && luma.h > 0 ) {
		const unsigned int rowY = luma.h / 2;
		const double kThresh = 0.02;
		int firstBright = -1, lastBright = -1;
		for( unsigned int x = 0; x < luma.w; ++x ) {
			if( luma.luma[ (std::size_t)rowY * luma.w + x ] > kThresh ) {
				if( firstBright < 0 ) firstBright = static_cast<int>( x );
				lastBright = static_cast<int>( x );
			}
		}
		Check( firstBright >= 0 && lastBright > firstBright,
		       "GS25 the centerline row shows a bright object span" );
		int gapPixels = 0;
		if( firstBright >= 0 && lastBright > firstBright ) {
			for( int x = firstBright + 1; x < lastBright; ++x ) {
				if( luma.luma[ (std::size_t)rowY * luma.w + x ] <= kThresh ) ++gapPixels;
			}
		}
		std::printf( "    GS25 centerline span [%d,%d] of %u px, interior gap pixels = %d\n",
		             firstBright, lastBright, luma.w, gapPixels );
		Check( gapPixels == 0,
		       "GS25 ZERO interior gap pixels between the object's silhouette span (continuous, not fractured)" );
	}

	sess.reset();
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
	TestNonSamplingEmitterGate();
	TestActionableRemoveDiagnostics();
	TestInsertChunksBatchAllApply();
	TestInsertChunksBestEffort();
	TestInsertChunksValidation();
	TestInsertChunksWireShape();
	TestProposePatchesBatch();
	TestProposePatchesBestEffort();
	TestProposePatchesValidation();
	TestProposePatchesConflictIsBatchFatal();
	TestMaterialScaffoldFamilies();
	TestMaterialScaffoldDeterminism();
	TestMaterialScaffoldMissingParams();
	TestMaterialScaffoldBadFamily();
	TestMaterialScaffoldNameLengthCap();
	TestMaterialScaffoldNameCollision();
	TestMaterialScaffoldProposalMode();
	TestGeometryScaffoldFamilies();
	TestGeometryScaffoldDisplacedBumpyVsFlat();
	TestGeometryScaffoldAspectFlow();
	TestGeometryScaffoldSdfDetailFlow();
	TestGeometryScaffoldDeterminism();
	TestGeometryScaffoldMissingParams();
	TestGeometryScaffoldBadFamily();
	TestGeometryScaffoldNameLengthCap();
	TestGeometryScaffoldNameCollision();
	TestGeometryScaffoldProposalMode();
	TestGeometryScaffoldBlendedChainBasic();
	TestGeometryScaffoldBlendedChainTaperFlow();
	TestGeometryScaffoldBlendedChainDetailFlow();
	TestGeometryScaffoldBlendedChainHostilePoints();
	TestGeometryScaffoldBlendedChainDeterminism();
	TestGeometryScaffoldBlendedChainMissingParamsWire();
	TestGeometryScaffoldVolumeBankBasic();
	TestGeometryScaffoldVolumeBankAspectFlow();
	TestGeometryScaffoldVolumeBankToneFlow();
	TestGeometryScaffoldVolumeBankDetailFlow();
	TestGeometryScaffoldVolumeBankHostileTone();
	TestGeometryScaffoldVolumeBankMissingParamsWire();
	TestGeometryScaffoldVolumeBankDeterminism();
	TestGeometryScaffoldOriginalFamiliesIgnoreNewParams();
	TestGeometryScaffoldBlendedChainContinuityInvariant();
	TestGeometryScaffoldBlendedChainEndpointExactness();
	TestGeometryScaffoldVolumeBankBboxContainerCoupling();
	TestGeometryScaffoldBlendedChainPointsMagnitudeCap();
	TestGeometryScaffoldBlendedChainAdversarialContinuityRender();

	std::printf( "AgentChunkCrudTest: %d passed, %d failed\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
