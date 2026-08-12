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
#include "../src/Library/Parsers/ChunkParserRegistry.h"   // R1c: enumerate every registered rasterizer kind for the classification-coverage assertion
#include "../src/Library/SceneEditor/ChunkDescriptorRegistry.h"   // A81g: pin that `ambient_light` is still the registry keyword the ban names
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
//
// R1c (2026-08-09): the inserted kind is `vcm_pel_rasterizer`, not the
// `bdpt_pel_rasterizer` this test used before the agent rasterizer
// allowlist landed.  BDPT is now refused for agent selection (see
// TestRasterizerAllowlistGate); VCM is on the allowlist and is equally
// different-keyword from the fixture's authored pathtracing rasterizer,
// so the ACTIVATION property under test is unchanged.
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
		"vcm_pel_rasterizer\n{\n\tsamples 4\n}" );
	Check( r.applied, "inserting a different-keyword rasterizer applies" );
	Check( pJob->GetActiveRasterizerName() == "vcm_pel_rasterizer",
	       "the inserted rasterizer is ACTIVE live (activation restore skipped)" );

	// ... and post-insert the render result OBSERVES the switch -- the agent
	// no longer has to take activation on faith.
	{
		Agent::AgentRenderResult rrPost = sess->Render();
		Check( rrPost.ok, "the post-insert render succeeds" );
		Check( rrPost.integrator == "vcm_pel_rasterizer",
		       "the post-insert render reports integrator=vcm_pel_rasterizer (the field reflects the insert)" );
	}

	// Live == reload: derive the serialized bytes into a FRESH Job and
	// compare active-rasterizer names (last-wins on load).
	{
		const std::string bytes = sess->ReadDocument();
		const std::string tmp2 = TempPath( "agentcrud_g3_reload.RISEscene" );
		Job* fresh = LoadScene( bytes.c_str(), tmp2 );
		Check( fresh != nullptr, "the post-insert head reloads" );
		if( fresh ) {
			Check( fresh->GetActiveRasterizerName() == "vcm_pel_rasterizer",
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
// R1c (2026-08-09): the AGENT RASTERIZER ALLOWLIST gate.
//
// USER DIRECTIVE: a scene-editing agent must never select the MLT
// rasterizer ("its a specialized rasterizer"), narrowed by the user to
// "agents may use PT and VCM only".  ALLOWED: pathtracing_pel /
// pathtracing_spectral / vcm_pel / vcm_spectral.  BLOCKED: bdpt_pel /
// bdpt_spectral / mlt / mlt_spectral / auto / auto_spectral.
// DELIBERATELY UNGATED: pixelpel_rasterizer /
// pixelintegratingspectral_rasterizer -- they are not integrator
// choices, and pixelpel is required for alpha-mask scenes.
//
// Sub-tests, one per enforcement path this gate had to close:
//   (a) insert_chunk of EACH blocked kind -> refused, document
//       byte-identical, head revision unmoved, message names the kind
//       AND the whole allowed set AND the no-override alternative;
//   (b) insert_chunk of EACH allowed kind -> applies;
//   (c) the two utility rasterizers -> NOT blocked;
//   (d) STATE-VS-DELTA on a scene that ALREADY carries mlt_rasterizer:
//       an unrelated edit applies, a patch to the MLT chunk's OWN params
//       applies, the scene RENDERS, and a SECOND blocked rasterizer is
//       still refused;
//   (e) insert_chunks with one blocked element -> the WHOLE batch is
//       refused atomically (nothing inserted, head unmoved) -- R1a's
//       all-or-nothing shape, deliberately NOT insert_chunks' usual
//       best-effort, because a policy refusal is not an authoring
//       failure;
//   (f) propose_patch VALUE-SPLICE injection: a value carrying a closing
//       brace + a whole mlt_rasterizer chunk cannot smuggle one in --
//       the gate compares DOCUMENTS, not patch triples;
//   (g) the auto_* `integrator` pin: bdpt refused, pt/vcm/auto fine,
//       every other param on the same chunk still editable, and (round-3
//       FIX A) bdpt is refused EVEN WHEN the head already reads bdpt --
//       the gate is a STATELESS allowlist, not a delta against the
//       chunk's current value;
//   (h) the PROPOSAL path: a blocked insert staged under External
//       authority is re-gated at RESOLVE time (the E1 lesson -- a
//       proposal staged before the gate must not slip through on
//       approval);
//   (i) CLASSIFICATION COVERAGE: every rasterizer keyword the PARSER
//       registers is explicitly classified by this policy.  A newly
//       added rasterizer kind fails here -- and blocks at runtime
//       meanwhile, because the classifier is an ALLOWLIST.
//----------------------------------------------------------------------
static const char* const kRasterAllowlistScene =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 2\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 8\n\theight 8\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 6\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 50.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname albedo\n\tcolor 0.8 0.8 0.8\n}\n\n"
	"lambertian_material\n{\n\tname matte\n\treflectance albedo\n}\n\n"
	"sphere_geometry\n{\n\tname sph\n\tradius 1.0\n}\n\n"
	"standard_object\n{\n\tname ball\n\tgeometry sph\n\tmaterial matte\n}\n\n"
	"omni_light\n{\n\tname lamp\n\tpower 20\n\tcolor 1 1 1\n\tposition 0 3 3\n}\n";

//! A scene whose ACTIVE rasterizer is a BLOCKED one, authored by the
//! USER (a file load, never an agent edit).  The state-vs-delta arm's
//! whole point: this scene must stay fully usable.
static const char* const kUserAuthoredMLTScene =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"mlt_rasterizer\n{\n\tbootstrap_samples 16\n\tchains 1\n\tmutations_per_pixel 1\n"
	"\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 8\n\theight 8\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 6\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 50.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname albedo\n\tcolor 0.8 0.8 0.8\n}\n\n"
	"lambertian_material\n{\n\tname matte\n\treflectance albedo\n}\n\n"
	"sphere_geometry\n{\n\tname sph\n\tradius 1.0\n}\n\n"
	"standard_object\n{\n\tname ball\n\tgeometry sph\n\tmaterial matte\n}\n\n"
	"omni_light\n{\n\tname lamp\n\tpower 20\n\tcolor 1 1 1\n\tposition 0 3 3\n}\n";

//! The six kinds this policy blocks, each with a minimal, VALID body so
//! a refusal can never be confused with a parse/derive failure.
struct BlockedRasterizerCase { const char* kind; const char* chunkText; };
static const BlockedRasterizerCase kBlockedRasterizerCases[] = {
	{ "bdpt_pel_rasterizer",
	  "bdpt_pel_rasterizer\n{\n\tsamples 2\n\tpixel_filter box\n\toidn_denoise false\n}" },
	{ "bdpt_spectral_rasterizer",
	  "bdpt_spectral_rasterizer\n{\n\tsamples 2\n\tpixel_filter box\n\toidn_denoise false\n}" },
	{ "mlt_rasterizer",
	  "mlt_rasterizer\n{\n\tbootstrap_samples 16\n\tchains 1\n\tmutations_per_pixel 1\n\tpixel_filter box\n\toidn_denoise false\n}" },
	{ "mlt_spectral_rasterizer",
	  "mlt_spectral_rasterizer\n{\n\tbootstrap_samples 16\n\tchains 1\n\tmutations_per_pixel 1\n\tpixel_filter box\n\toidn_denoise false\n}" },
	{ "auto_rasterizer",
	  "auto_rasterizer\n{\n\tsamples 2\n\tintegrator pt\n\tpixel_filter box\n\toidn_denoise false\n}" },
	{ "auto_spectral_rasterizer",
	  "auto_spectral_rasterizer\n{\n\tsamples 2\n\tintegrator pt\n\tpixel_filter box\n\toidn_denoise false\n}" },
};
static const std::size_t kBlockedRasterizerCaseCount =
	sizeof( kBlockedRasterizerCases ) / sizeof( kBlockedRasterizerCases[0] );

//! Every clause the refusal message owes the model: the rejected kind,
//! the FULL allowed set spelled out, and the human alternative.
static void CheckRasterizerRefusalMessage( const std::string& msg, const std::string& kind,
                                           const char* tag )
{
	Check( msg.find( kind ) != std::string::npos,
	       std::string( tag ) + " message NAMES the rejected kind (" + kind + ")" );
	Check( msg.find( "pathtracing_pel_rasterizer" ) != std::string::npos &&
	       msg.find( "pathtracing_spectral_rasterizer" ) != std::string::npos &&
	       msg.find( "vcm_pel_rasterizer" ) != std::string::npos &&
	       msg.find( "vcm_spectral_rasterizer" ) != std::string::npos,
	       std::string( tag ) + " message names the FULL allowed set explicitly" );
	Check( msg.find( "no override" ) != std::string::npos ||
	       msg.find( "NO override" ) != std::string::npos,
	       std::string( tag ) + " message states there is NO escape parameter" );
	Check( msg.find( "USER" ) != std::string::npos || msg.find( "user" ) != std::string::npos,
	       std::string( tag ) + " message states the alternative (the user selects it themselves)" );
}

static void TestRasterizerAllowlistGate()
{
	std::printf( "R1c: agent rasterizer allowlist (PT + VCM only; MLT/BDPT/auto refused)...\n" );

	// ---- (a) EVERY blocked kind is refused, atomically ------------------
	for( std::size_t i = 0; i < kBlockedRasterizerCaseCount; ++i )
	{
		const BlockedRasterizerCase& bc = kBlockedRasterizerCases[i];
		const std::string tmp = TempPath( ( std::string( "agentcrud_r1c_a_" ) + bc.kind + ".RISEscene" ).c_str() );
		Job* pJob = LoadScene( kRasterAllowlistScene, tmp );
		Check( pJob != nullptr, std::string( "R1c(a) fixture loads for " ) + bc.kind );
		if( !pJob ) continue;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

		const std::string headBefore = sess->ReadDocument();
		const RISE::Cst::CstHeadVersion vBefore = sess->HeadVersion();

		Agent::AgentChunkResult r = sess->InsertChunk( bc.chunkText );
		Check( !r.applied && r.status == "rejected",
		       std::string( "R1c(a) inserting " ) + bc.kind + " is REFUSED" );
		Check( !r.retriable,
		       std::string( "R1c(a) the refusal is PERMANENT (retriable=false) for " ) + bc.kind );
		Check( r.kind == bc.kind,
		       std::string( "R1c(a) the refusal still echoes the chunk kind for " ) + bc.kind );
		Check( sess->ReadDocument() == headBefore,
		       std::string( "R1c(a) the document is byte-identical after refusing " ) + bc.kind );
		Check( sess->HeadVersion() == vBefore,
		       std::string( "R1c(a) the head version did not move after refusing " ) + bc.kind );
		Check( pJob->GetActiveRasterizerName() == "pathtracing_pel_rasterizer",
		       std::string( "R1c(a) the ACTIVE rasterizer is untouched after refusing " ) + bc.kind );
		CheckRasterizerRefusalMessage( r.message, bc.kind, "R1c(a)" );
		if( i == 0 ) std::printf( "  R1c(a) message: %s\n", r.message.c_str() );

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}

	// ---- (b) EVERY allowed kind applies ---------------------------------
	// The fixture already declares pathtracing_pel_rasterizer, so a second
	// copy of THAT one is a duplicate-singleton case rather than a policy
	// case; every allowed kind gets its own fresh fixture regardless, and
	// the assertion is only that the ALLOWLIST does not refuse it.
	{
		static const char* const kAllowedInserts[] = {
			"pathtracing_spectral_rasterizer\n{\n\tsamples 2\n\tpixel_filter box\n\toidn_denoise false\n}",
			"vcm_pel_rasterizer\n{\n\tsamples 2\n\tpixel_filter box\n\toidn_denoise false\n}",
			"vcm_spectral_rasterizer\n{\n\tsamples 2\n\tpixel_filter box\n\toidn_denoise false\n}",
		};
		static const char* const kAllowedNames[] = {
			"pathtracing_spectral_rasterizer", "vcm_pel_rasterizer", "vcm_spectral_rasterizer",
		};
		for( std::size_t i = 0; i < 3; ++i )
		{
			const std::string tmp = TempPath( ( std::string( "agentcrud_r1c_b_" ) + kAllowedNames[i] + ".RISEscene" ).c_str() );
			Job* pJob = LoadScene( kRasterAllowlistScene, tmp );
			Check( pJob != nullptr, std::string( "R1c(b) fixture loads for " ) + kAllowedNames[i] );
			if( !pJob ) continue;
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			Agent::AgentChunkResult r = sess->InsertChunk( kAllowedInserts[i] );
			Check( r.applied && r.status == "applied",
			       std::string( "R1c(b) inserting the ALLOWED " ) + kAllowedNames[i] + " succeeds" );
			Check( pJob->GetActiveRasterizerName() == kAllowedNames[i],
			       std::string( "R1c(b) the allowed insert really becomes ACTIVE: " ) + kAllowedNames[i] );
			sess.reset();
			pJob->release();
			std::remove( tmp.c_str() );
		}
	}

	// ---- (c) the DELIBERATELY UNGATED utility rasterizers ---------------
	// pixelpel_rasterizer is REQUIRED for alpha-mask scenes
	// (docs/SCENE_CONVENTIONS.md) and neither of these is an integrator
	// choice, so the directive does not reach them.
	{
		static const char* const kUtilityInserts[] = {
			"pixelpel_rasterizer\n{\n\tsamples 1\n\tpixel_filter box\n\toidn_denoise false\n}",
			"pixelintegratingspectral_rasterizer\n{\n\tsamples 1\n\tpixel_filter box\n\toidn_denoise false\n}",
		};
		static const char* const kUtilityNames[] = {
			"pixelpel_rasterizer", "pixelintegratingspectral_rasterizer",
		};
		for( std::size_t i = 0; i < 2; ++i )
		{
			const std::string tmp = TempPath( ( std::string( "agentcrud_r1c_c_" ) + kUtilityNames[i] + ".RISEscene" ).c_str() );
			Job* pJob = LoadScene( kRasterAllowlistScene, tmp );
			Check( pJob != nullptr, std::string( "R1c(c) fixture loads for " ) + kUtilityNames[i] );
			if( !pJob ) continue;
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			Agent::AgentChunkResult r = sess->InsertChunk( kUtilityInserts[i] );
			Check( r.applied && r.status == "applied",
			       std::string( "R1c(c) the UNGATED utility rasterizer " ) + kUtilityNames[i] + " is NOT blocked" );
			sess.reset();
			pJob->release();
			std::remove( tmp.c_str() );
		}
	}

	// ---- (d) STATE-VS-DELTA on a USER-authored MLT scene ----------------
	{
		const std::string tmp = TempPath( "agentcrud_r1c_d.RISEscene" );
		Job* pJob = LoadScene( kUserAuthoredMLTScene, tmp );
		Check( pJob != nullptr, "R1c(d) a USER-authored mlt_rasterizer scene loads" );
		if( pJob )
		{
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			Check( pJob->GetActiveRasterizerName() == "mlt_rasterizer",
			       "R1c(d) RED-PROVE: MLT really is the ACTIVE rasterizer on this head" );

			// (d1) an edit to an UNRELATED chunk applies.
			{
				Agent::AgentSetPatch p;
				p.target = "sph";
				p.kind   = "sphere_geometry";
				p.param  = "radius";
				p.value  = "1.4";
				Agent::AgentPatchResult r = sess->ProposePatch( p );
				Check( r.applied && r.status == "applied",
				       "R1c(d1) an unrelated edit on a pre-existing MLT scene APPLIES (state, not delta)" );
			}

			// (d2) an edit to the MLT chunk's OWN params applies -- the
			// kind-addressed singleton form (a rasterizer has no `name`).
			{
				Agent::AgentSetPatch p;
				p.target = "";
				p.kind   = "mlt_rasterizer";
				p.param  = "mutations_per_pixel";
				p.value  = "2";
				Agent::AgentPatchResult r = sess->ProposePatch( p );
				Check( r.applied && r.status == "applied",
				       "R1c(d2) patching the BLOCKED rasterizer's OWN params APPLIES (it is already here)" );
				Check( sess->ReadDocument().find( "mutations_per_pixel 2" ) != std::string::npos,
				       "R1c(d2) the param edit really landed in the document" );
			}

			// (d3) the scene still RENDERS -- the gate never touches the
			// render path, only document mutation.
			{
				Agent::AgentRenderResult rr = sess->Render();
				Check( rr.ok, "R1c(d3) a pre-existing MLT scene still RENDERS through the agent surface" );
				Check( rr.integrator == "mlt_rasterizer",
				       "R1c(d3) and it renders through MLT, honestly reported" );
			}

			// (d4) but a SECOND blocked rasterizer is still refused -- an
			// insert is purely additive, so it is always a NEW selection.
			{
				const std::string headBefore = sess->ReadDocument();
				Agent::AgentChunkResult r = sess->InsertChunk( kBlockedRasterizerCases[0].chunkText );
				Check( !r.applied && r.status == "rejected",
				       "R1c(d4) inserting a SECOND blocked rasterizer is STILL refused" );
				Check( sess->ReadDocument() == headBefore,
				       "R1c(d4) and the document is byte-identical" );
			}

			// (d5) PIN THE REASONING behind leaving the REMOVE verbs ungated:
			// a rasterizer chunk declares no `name` param, and the resolver's
			// unique-in-kind positional fallback fires only for `camera` and
			// for descriptor-`unnamedRepeatable` kinds (timeline/keyframe) --
			// neither of which a rasterizer is.  So neither remove verb can
			// even RESOLVE a rasterizer, and neither can therefore re-activate
			// a shadowed one.  Asserted empirically rather than reasoned,
			// because the gate's coverage argument depends on it.
			{
				const std::string headBefore = sess->ReadDocument();
				Agent::AgentChunkResult r1 = sess->RemoveChunk( "mlt_rasterizer" );
				Check( !r1.applied, "R1c(d5) remove_chunk cannot resolve a rasterizer by keyword" );
				Agent::AgentChunkResult r2 = sess->RemoveChunk( "mlt_rasterizer", "mlt_rasterizer" );
				Check( !r2.applied, "R1c(d5) ... nor with an explicit `kind`" );
				Agent::AgentChunkResult r3 = sess->RemoveChunk( "", "mlt_rasterizer" );
				Check( !r3.applied, "R1c(d5) ... nor via an empty-name kind address" );
				std::vector<std::string> batch;
				batch.push_back( "mlt_rasterizer" );
				Agent::AgentSession::AgentRemoveBatchResult rb = sess->RemoveChunks( batch );
				Check( !rb.applied, "R1c(d5) ... nor through the batch remove verb" );
				Check( sess->ReadDocument() == headBefore,
				       "R1c(d5) and every one of those attempts left the head byte-identical" );
				Check( pJob->GetActiveRasterizerName() == "mlt_rasterizer",
				       "R1c(d5) the active rasterizer is still the user's MLT" );
			}

			sess.reset();
			pJob->release();
		}
		std::remove( tmp.c_str() );
	}

	// ---- (e) insert_chunks: ONE blocked element refuses the WHOLE batch --
	{
		const std::string tmp = TempPath( "agentcrud_r1c_e.RISEscene" );
		Job* pJob = LoadScene( kRasterAllowlistScene, tmp );
		Check( pJob != nullptr, "R1c(e) fixture loads" );
		if( pJob )
		{
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const std::string headBefore = sess->ReadDocument();
			const RISE::Cst::CstHeadVersion vBefore = sess->HeadVersion();

			std::vector<std::string> batch;
			batch.push_back( "uniformcolor_painter\n{\n\tname pnt_r1c\n\tcolor 0.2 0.4 0.6\n}" );
			batch.push_back( "mlt_rasterizer\n{\n\tbootstrap_samples 16\n\tchains 1\n"
			                 "\tmutations_per_pixel 1\n\tpixel_filter box\n\toidn_denoise false\n}" );
			batch.push_back( "sphere_geometry\n{\n\tname sph_r1c\n\tradius 0.3\n}" );
			const std::vector<Agent::AgentChunkResult> rs = sess->InsertChunks( batch );

			Check( rs.size() == batch.size(),
			       "R1c(e) the batch still returns one result per input element" );
			bool allRejected = true;
			for( const Agent::AgentChunkResult& e : rs ) if( e.applied || e.status != "rejected" ) allRejected = false;
			Check( allRejected, "R1c(e) EVERY element reports the same refusal (all-or-nothing)" );
			Check( sess->ReadDocument() == headBefore,
			       "R1c(e) NOTHING was inserted -- the document is byte-identical" );
			Check( sess->HeadVersion() == vBefore,
			       "R1c(e) the head version did not move" );
			Check( !rs.empty() && rs[0].message.find( "NOTHING was inserted" ) != std::string::npos,
			       "R1c(e) the message states the all-or-nothing contract explicitly" );
			Check( !rs.empty() && rs[0].message.find( "chunks[1]" ) != std::string::npos,
			       "R1c(e) the message names the OFFENDING INDEX so the fix is one edit" );
			if( !rs.empty() ) {
				CheckRasterizerRefusalMessage( rs[0].message, "mlt_rasterizer", "R1c(e)" );
				std::printf( "  R1c(e) message: %s\n", rs[0].message.c_str() );
			}
			// RED-PROVE the fixture: the SAME batch WITHOUT the blocked
			// element lands every element, so (e)'s refusal is about the
			// policy and not about the batch being malformed.
			{
				std::vector<std::string> ok;
				ok.push_back( batch[0] );
				ok.push_back( batch[2] );
				const std::vector<Agent::AgentChunkResult> rs2 = sess->InsertChunks( ok );
				bool allApplied = rs2.size() == 2;
				for( const Agent::AgentChunkResult& e : rs2 ) if( !e.applied ) allApplied = false;
				Check( allApplied, "R1c(e) RED-PROVE: the same batch MINUS the blocked element applies fully" );
			}

			sess.reset();
			pJob->release();
		}
		std::remove( tmp.c_str() );
	}

	// ---- (f) VALUE-SPLICE injection through propose_patch ----------------
	// A param value is spliced into the document as TEXT.  The gate
	// compares DOCUMENTS rather than patch triples precisely so a value
	// carrying a closing brace plus a whole chunk cannot smuggle a blocked
	// rasterizer past a keyword-only check.
	{
		const std::string tmp = TempPath( "agentcrud_r1c_f.RISEscene" );
		Job* pJob = LoadScene( kRasterAllowlistScene, tmp );
		Check( pJob != nullptr, "R1c(f) fixture loads" );
		if( pJob )
		{
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const std::string headBefore = sess->ReadDocument();

			Agent::AgentSetPatch p;
			p.target = "sph";
			p.kind   = "sphere_geometry";
			p.param  = "radius";
			p.value  = "1.0\n}\n\nmlt_rasterizer\n{\n\tbootstrap_samples 16\n\tchains 1\n"
			           "\tmutations_per_pixel 1\n\tpixel_filter box\n\toidn_denoise false\n";
			Agent::AgentPatchResult r = sess->ProposePatch( p );
			Check( !r.applied,
			       "R1c(f) a value-splice that would introduce mlt_rasterizer does NOT apply" );
			Check( sess->ReadDocument() == headBefore,
			       "R1c(f) the document is byte-identical after the injection attempt" );
			Check( pJob->GetActiveRasterizerName() == "pathtracing_pel_rasterizer",
			       "R1c(f) the ACTIVE rasterizer is untouched" );
			std::printf( "  R1c(f) message: %s\n", r.message.c_str() );

			// The propose_patch above is refused by the DERIVE layer too (a
			// Double slot rejects the non-numeric token), so it alone does not
			// prove the allowlist arm fired.  Drive the gate function DIRECTLY
			// on the same head to red-prove arm (b): the candidate-document
			// comparison sees the spliced chunk regardless of whether the
			// derive layer would also have caught it.
			{
				const std::string clause = RISE::Agent::CheckRasterizerAllowlistGateForPatch(
					headBefore, "sph", "sphere_geometry", "radius", p.value );
				Check( !clause.empty(),
				       "R1c(f) RED-PROVE: the allowlist gate ITSELF refuses the value-splice "
				       "(it compares candidate DOCUMENTS, not patch triples)" );
				CheckRasterizerRefusalMessage( clause, "mlt_rasterizer", "R1c(f)" );

				// ... and a splice carrying an ALLOWED rasterizer is NOT
				// refused by this gate (whatever the derive layer then says).
				const std::string okClause = RISE::Agent::CheckRasterizerAllowlistGateForPatch(
					headBefore, "sph", "sphere_geometry", "radius",
					"1.0\n}\n\nvcm_pel_rasterizer\n{\n\tsamples 2\n" );
				Check( okClause.empty(),
				       "R1c(f) RED-PROVE (negation): the SAME splice with an ALLOWED rasterizer "
				       "is not refused by the allowlist gate" );

				// A splice into a chunk that is NOT touched at all leaves the
				// document's rasterizer multiset alone -- no false positive.
				const std::string noneClause = RISE::Agent::CheckRasterizerAllowlistGateForPatch(
					headBefore, "sph", "sphere_geometry", "radius", "2.5" );
				Check( noneClause.empty(), "R1c(f) an ordinary param edit is never refused" );
			}

			sess.reset();
			pJob->release();
		}
		std::remove( tmp.c_str() );
	}

	// ---- (g) the auto_* dispatcher's `integrator` pin --------------------
	{
		std::string autoScene = kRasterAllowlistScene;
		// Replace the authored PT rasterizer with a USER-authored auto one
		// (file-loaded, never agent-inserted -- state-vs-delta again).
		const std::string ptChunk =
			"pathtracing_pel_rasterizer\n{\n\tsamples 2\n\tpixel_filter box\n\toidn_denoise false\n}";
		const std::string autoChunk =
			"auto_rasterizer\n{\n\tsamples 2\n\tintegrator pt\n\tpixel_filter box\n\toidn_denoise false\n}";
		const std::size_t at = autoScene.find( ptChunk );
		Check( at != std::string::npos, "R1c(g) fixture surgery finds the authored PT chunk" );
		if( at != std::string::npos ) autoScene.replace( at, ptChunk.size(), autoChunk );

		const std::string tmp = TempPath( "agentcrud_r1c_g.RISEscene" );
		Job* pJob = LoadScene( autoScene.c_str(), tmp );
		Check( pJob != nullptr, "R1c(g) a USER-authored auto_rasterizer scene loads" );
		if( pJob )
		{
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

			// (g1) pinning bdpt is REFUSED -- that IS selecting BDPT.
			{
				const std::string headBefore = sess->ReadDocument();
				Agent::AgentSetPatch p;
				p.target = "";
				p.kind   = "auto_rasterizer";
				p.param  = "integrator";
				p.value  = "bdpt";
				Agent::AgentPatchResult r = sess->ProposePatch( p );
				Check( !r.applied && r.status == "rejected",
				       "R1c(g1) pinning auto_rasterizer's integrator to bdpt is REFUSED" );
				Check( r.message.find( "bdpt" ) != std::string::npos &&
				       r.message.find( "auto_rasterizer" ) != std::string::npos,
				       "R1c(g1) the message names both the pin value and the chunk" );
				Check( r.message.find( "no override" ) != std::string::npos ||
				       r.message.find( "NO override" ) != std::string::npos,
				       "R1c(g1) the message states there is NO escape parameter" );
				Check( sess->ReadDocument() == headBefore,
				       "R1c(g1) the document is byte-identical" );
				std::printf( "  R1c(g1) message: %s\n", r.message.c_str() );
			}

			// (g1b) round-3 FIX A RED-PROVE: pinning bdpt is refused EVEN
			// WHEN the head ALREADY reads bdpt.  A separate fixture/session
			// (this one's head is still pinned to `pt`) whose
			// auto_rasterizer is USER-authored with `integrator bdpt`
			// already in place -- the prior DELTA form treated re-writing
			// the SAME value as an inert no-op and let it through; the
			// STATELESS allowlist form refuses unconditionally, closing the
			// TOCTOU hole where a co-editor could move the pin off bdpt
			// between gate-check and commit.
			{
				std::string bdptAutoScene = kRasterAllowlistScene;
				const std::string ptChunk2 =
					"pathtracing_pel_rasterizer\n{\n\tsamples 2\n\tpixel_filter box\n\toidn_denoise false\n}";
				const std::string bdptAutoChunk =
					"auto_rasterizer\n{\n\tsamples 2\n\tintegrator bdpt\n\tpixel_filter box\n\toidn_denoise false\n}";
				const std::size_t at2 = bdptAutoScene.find( ptChunk2 );
				Check( at2 != std::string::npos, "R1c(g1b) fixture surgery finds the authored PT chunk" );
				if( at2 != std::string::npos ) bdptAutoScene.replace( at2, ptChunk2.size(), bdptAutoChunk );

				const std::string tmp2 = TempPath( "agentcrud_r1c_g1b.RISEscene" );
				Job* pJob2 = LoadScene( bdptAutoScene.c_str(), tmp2 );
				Check( pJob2 != nullptr, "R1c(g1b) a USER-authored auto_rasterizer scene ALREADY pinned to bdpt loads" );
				if( pJob2 )
				{
					std::unique_ptr<Agent::AgentSession> sess2 = Agent::AgentSession::WrapJob( pJob2 );
					const std::string headBefore2 = sess2->ReadDocument();

					Agent::AgentSetPatch p2;
					p2.target = "";
					p2.kind   = "auto_rasterizer";
					p2.param  = "integrator";
					p2.value  = "bdpt";
					Agent::AgentPatchResult r2 = sess2->ProposePatch( p2 );
					Check( !r2.applied && r2.status == "rejected",
					       "R1c(g1b) re-pinning bdpt on a chunk ALREADY pinned to bdpt is STILL REFUSED" );
					Check( r2.message.find( "bdpt" ) != std::string::npos,
					       "R1c(g1b) the message still names the rejected pin" );
					Check( sess2->ReadDocument() == headBefore2,
					       "R1c(g1b) the document is byte-identical" );

					// The state-vs-delta carve-out still holds for every OTHER
					// param on this same already-bdpt-pinned chunk.
					Agent::AgentSetPatch p3;
					p3.target = "";
					p3.kind   = "auto_rasterizer";
					p3.param  = "samples";
					p3.value  = "5";
					Agent::AgentPatchResult r3 = sess2->ProposePatch( p3 );
					Check( r3.applied && r3.status == "applied",
					       "R1c(g1b) an UNRELATED param on the already-bdpt-pinned chunk still APPLIES" );

					sess2.reset();
					pJob2->release();
				}
				std::remove( tmp2.c_str() );
			}

			// (g2) pinning vcm / pt / auto is fine.
			{
				static const char* const kOkPins[] = { "vcm", "pt", "auto" };
				for( std::size_t i = 0; i < 3; ++i ) {
					Agent::AgentSetPatch p;
					p.target = "";
					p.kind   = "auto_rasterizer";
					p.param  = "integrator";
					p.value  = kOkPins[i];
					Agent::AgentPatchResult r = sess->ProposePatch( p );
					Check( r.applied && r.status == "applied",
					       std::string( "R1c(g2) pinning integrator=" ) + kOkPins[i] + " APPLIES" );
				}
			}

			// (g3) every OTHER param on the same blocked chunk is editable.
			{
				Agent::AgentSetPatch p;
				p.target = "";
				p.kind   = "auto_rasterizer";
				p.param  = "samples";
				p.value  = "3";
				Agent::AgentPatchResult r = sess->ProposePatch( p );
				Check( r.applied && r.status == "applied",
				       "R1c(g3) an UNRELATED param on the blocked auto_rasterizer still APPLIES" );
			}

			sess.reset();
			pJob->release();
		}
		std::remove( tmp.c_str() );
	}

	// ---- (h) the PROPOSAL path is re-gated at RESOLVE time ---------------
	// The E1 lesson (P1-3): a proposal that reaches the queue must be
	// re-checked on approval.  Here the stage-time gate already refuses,
	// so this drives the RESOLVE-time arm directly through the controller
	// -- proving that even a proposal that somehow reached `pending`
	// (staged before the gate shipped, or staged while innocent) cannot
	// land a blocked rasterizer on approval.
	{
		const std::string tmp = TempPath( "agentcrud_r1c_h.RISEscene" );
		Job* pJob = LoadScene( kRasterAllowlistScene, tmp );
		Check( pJob != nullptr, "R1c(h) fixture loads" );
		if( pJob )
		{
			TestController c( *pJob, /*simulatedRenderMs*/ 0 );
			c.Start();
			std::unique_ptr<Agent::AgentSession> owner =
				Agent::AgentSession::WrapJob( pJob, Agent::AgentAuthority::Owner );
			owner->AttachController( &c );

			// (h1) stage-time: an External session's blocked insert never
			// even reaches the queue.
			{
				std::unique_ptr<Agent::AgentSession> ext =
					Agent::AgentSession::WrapJob( pJob, Agent::AgentAuthority::External );
				ext->AttachController( &c );
				Agent::AgentChunkResult r = ext->InsertChunk( kBlockedRasterizerCases[2].chunkText );
				Check( !r.applied && r.status == "rejected",
				       "R1c(h1) an External-authority blocked insert is refused BEFORE staging" );
				Check( owner->ListProposals().empty(),
				       "R1c(h1) and nothing was enqueued" );
			}

			// (h2) resolve-time: stage a proposal the controller WILL be
			// asked to apply, bypassing the session gate by staging
			// directly on the controller (the shape a pre-gate proposal
			// has), then approve it -- the re-gate must refuse.
			{
				SceneEditController::AgentProposal p;
				p.kind      = SceneEditController::AgentProposalKind::InsertChunk;
				p.chunkText = String( kBlockedRasterizerCases[2].chunkText );
				p.hasExplicitBaseVersion = false;
				RISE::Cst::CstHeadVersion stagedHead{};
				const std::uint64_t id = c.StageProposal( p, &stagedHead );
				Check( id != 0, "R1c(h2) the pre-gate-shaped proposal reaches the queue" );

				const std::string headBefore = owner->ReadDocument();
				Agent::AgentSession::AgentResolveResult rr = owner->ResolveProposal( id, /*approve=*/true );
				Check( rr.ok, "R1c(h2) resolve runs (the id is found)" );
				Check( rr.status == "rejected",
				       "R1c(h2) approving a staged BLOCKED rasterizer is REFUSED at resolve time" );
				Check( rr.message.find( "resolve refused" ) != std::string::npos,
				       "R1c(h2) message carries the resolve-refusal marker" );
				CheckRasterizerRefusalMessage( rr.message, "mlt_rasterizer", "R1c(h2)" );
				Check( owner->ReadDocument() == headBefore,
				       "R1c(h2) the live document never received the staged rasterizer" );
				Check( pJob->GetActiveRasterizerName() == "pathtracing_pel_rasterizer",
				       "R1c(h2) the ACTIVE rasterizer is untouched" );
				std::printf( "  R1c(h2) message: %s\n", rr.message.c_str() );
			}

			owner.reset();
			c.Stop();
			pJob->release();
		}
		std::remove( tmp.c_str() );
	}

	// ---- (i) CLASSIFICATION COVERAGE -------------------------------------
	// Walk the PARSER's own registry.  Every chunk whose descriptor is
	// ChunkCategory::Rasterizer AND whose keyword ends in `_rasterizer`
	// (the same pair the gate itself uses -- `light_rr_threshold` is
	// category Rasterizer but is not a rasterizer) must be a kind this
	// policy has explicitly considered.  A rasterizer added to the parser
	// tomorrow fails HERE and blocks at runtime meanwhile.
	{
		unsigned int seen = 0, allowed = 0, ungated = 0, blocked = 0;
		for( const RISE::ChunkParserEntry& e : RISE::CreateAllChunkParsers() )
		{
			const std::string& kw = e.keyword;
			const RISE::Agent::AgentRasterizerPolicy cls = RISE::Agent::ClassifyAgentRasterizerKind( kw );
			if( cls == RISE::Agent::AgentRasterizerPolicy::NotARasterizer ) continue;
			++seen;
			if( cls == RISE::Agent::AgentRasterizerPolicy::Allowed )        ++allowed;
			else if( cls == RISE::Agent::AgentRasterizerPolicy::UngatedUtility ) ++ungated;
			else                                                            ++blocked;
			Check( RISE::Agent::AgentRasterizerKindIsExplicitlyClassified( kw ),
			       "R1c(i) rasterizer kind `" + kw + "` is EXPLICITLY classified by the R1c policy "
			       "(a new rasterizer defaults to BLOCKED -- classify it deliberately)" );
		}
		Check( seen == 12,
		       "R1c(i) the parser registers exactly 12 rasterizer kinds (update this count AND the "
		       "policy sets together when that changes)" );
		Check( allowed == 4, "R1c(i) exactly FOUR kinds are agent-selectable (PT + VCM, pel + spectral)" );
		Check( ungated == 2, "R1c(i) exactly TWO utility rasterizers are deliberately ungated" );
		Check( blocked == 6, "R1c(i) exactly SIX kinds are blocked (BDPT, MLT, auto -- pel + spectral)" );
		// `light_rr_threshold` is ChunkCategory::Rasterizer but is NOT a
		// rasterizer -- the suffix half of the predicate is load-bearing.
		Check( RISE::Agent::ClassifyAgentRasterizerKind( "light_rr_threshold" ) ==
		       RISE::Agent::AgentRasterizerPolicy::NotARasterizer,
		       "R1c(i) light_rr_threshold is category Rasterizer but is correctly NOT a rasterizer kind" );
		Check( RISE::Agent::ClassifyAgentRasterizerKind( "omni_light" ) ==
		       RISE::Agent::AgentRasterizerPolicy::NotARasterizer,
		       "R1c(i) a non-rasterizer chunk classifies as NotARasterizer" );
	}

	// ---- (i2) round-3 FIX A: INTEGRATOR-PIN CLASSIFICATION COVERAGE -----
	// Walk auto_rasterizer's AND auto_spectral_rasterizer's OWN descriptor
	// (not a hand-typed literal) for the `integrator` param's enumValues.
	// Every value the descriptor accepts must be a value this policy has
	// explicitly classified -- a value the descriptor gains tomorrow still
	// defaults to REFUSED at runtime (allowlist semantics), but fails HERE
	// so the omission is a decision someone makes deliberately, mirroring
	// (i) above for rasterizer KINDS.
	{
		static const char* const kAutoKeywords[] = { "auto_rasterizer", "auto_spectral_rasterizer" };
		unsigned int paramsChecked = 0;
		for( const char* autoKw : kAutoKeywords )
		{
			bool foundChunk = false;
			for( const RISE::ChunkParserEntry& e : RISE::CreateAllChunkParsers() )
			{
				if( e.keyword != autoKw ) continue;
				foundChunk = true;
				const RISE::ChunkDescriptor& d = e.parser->Describe();
				bool foundParam = false;
				for( const RISE::ParameterDescriptor& p : d.parameters )
				{
					if( p.name != "integrator" ) continue;
					foundParam = true;
					++paramsChecked;
					for( const std::string& ev : p.enumValues )
					{
						Check( RISE::Agent::AgentIntegratorPinIsExplicitlyClassified( ev ),
						       "R1c(i2) `" + std::string( autoKw ) + "`'s integrator enum value `" + ev +
						       "` is EXPLICITLY classified by the R1c pin policy "
						       "(a new accepted value defaults to REFUSED -- classify it deliberately)" );
					}
				}
				Check( foundParam, std::string( "R1c(i2) " ) + autoKw + " declares an `integrator` param" );
				break;
			}
			Check( foundChunk, std::string( "R1c(i2) the parser registers " ) + autoKw );
		}
		Check( paramsChecked == 2, "R1c(i2) both auto_* chunks' integrator params were checked" );
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
// RC1: remove_chunks -- the ATOMIC BATCH form of remove_chunk (headless,
// direct AgentSession::RemoveChunks API).  This is the verb's core
// contract, so every clause of it is red-provable here:
//   (a) N targets removed in ONE call with EXACTLY ONE head-version bump
//       (the measured motivation: 20 remove_chunk calls -> 1);
//   (b) INTRA-BATCH references resolve in BOTH list orders (producer
//       first AND consumer first) -- there is no ordering requirement;
//   (c) an OUTSIDE-batch referrer refuses the WHOLE batch: NOTHING
//       removed, head byte-identical, and the per-target issue names the
//       outside referrer while the intra-batch-only target gets NO issue;
//   (d) an unknown target mid-batch refuses atomically with a per-target
//       "unknown_target" issue naming the offender;
//   (e) duplicates are DEDUPED (removed once) and reported in `note`,
//       never refused;
//   (f) a single-element batch is semantically identical to the singular
//       verb -- same outcome, byte-identical resulting document.
//----------------------------------------------------------------------
static void TestRemoveChunksBatch()
{
	std::printf( "RC1: remove_chunks -- the ATOMIC batch remove (headless)...\n" );

	// The emissive subassembly of kScene is a self-contained 4-chunk graph:
	//   pnt_emit -> mat_emit -> obj_emit  and  quad_emit -> obj_emit
	// Nothing OUTSIDE it references any of the four, so it is exactly the
	// "tear down a subassembly" shape the verb exists for.
	const char* const kEmissiveSubassembly[] = { "pnt_emit", "mat_emit", "quad_emit", "obj_emit" };

	// (a) N targets, ONE head bump.
	{
		const std::string tmp = TempPath( "agentcrud_rc1a.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "RC1(a) fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

		const RISE::Cst::CstHeadVersion before = sess->HeadVersion();
		std::vector<std::string> targets( kEmissiveSubassembly, kEmissiveSubassembly + 4 );
		const Agent::AgentSession::AgentRemoveBatchResult r = sess->RemoveChunks( targets );

		Check( r.applied && r.status == "applied", "RC1(a) the 4-chunk batch APPLIES" );
		Check( r.rawCode == 2, "RC1(a) rawCode is 2 (a D2 full re-derive), never 1" );
		Check( r.note.empty(), "RC1(a) no dedupe note when nothing was listed twice" );
		Check( r.targetResults.size() == 4, "RC1(a) one result per target" );
		// THE headline property: FOUR chunks left the document for the price of ONE
		// revision bump.  Four separate remove_chunk calls would bump it four times.
		Check( r.headVersion.revision == before.revision + 1,
		       "RC1(a) EXACTLY ONE head-version bump for the whole 4-chunk batch" );
		Check( r.headVersion.uuid == before.uuid, "RC1(a) same document uuid (not a reload)" );

		const std::string doc = sess->ReadDocument();
		for( const char* n : kEmissiveSubassembly )
			Check( doc.find( std::string( "name " ) + n ) == std::string::npos,
			       std::string( "RC1(a) '" ) + n + "' is GONE from the document" );
		Check( doc.find( "name mat_diffuse" ) != std::string::npos,
		       "RC1(a) the untargeted diffuse subassembly is untouched" );
		// Every per-target entry mirrors the batch verdict and echoes its resolved kind.
		bool everyEntryApplied = true, everyEntryHasKind = true;
		for( const Agent::AgentChunkResult& tr : r.targetResults ) {
			if( !tr.applied || tr.status != "applied" ) everyEntryApplied = false;
			if( tr.kind.empty() ) everyEntryHasKind = false;
		}
		Check( everyEntryApplied, "RC1(a) every per-target entry mirrors the batch verdict" );
		Check( everyEntryHasKind, "RC1(a) every per-target entry echoes its resolved chunk keyword" );

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}

	// (b) INTRA-BATCH references, BOTH orders.  pnt_emit is referenced ONLY by
	// mat_emit, which is itself in the batch -- so it must come out regardless of
	// which one the caller listed first.  This is the property N sequential
	// remove_chunk calls cannot offer at all (producer-first would be refused).
	for( int order = 0; order < 2; ++order )
	{
		const bool producerFirst = ( order == 0 );
		const char* label = producerFirst ? "RC1(b1) producer-first" : "RC1(b2) consumer-first";
		const std::string tmp = TempPath( producerFirst ? "agentcrud_rc1b1.RISEscene"
		                                                : "agentcrud_rc1b2.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, std::string( label ) + " fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

		std::vector<std::string> targets;
		if( producerFirst ) { targets.push_back( "pnt_emit" ); targets.push_back( "mat_emit" ); }
		else                { targets.push_back( "mat_emit" ); targets.push_back( "pnt_emit" ); }
		// obj_emit references mat_emit from outside the pair, so it has to join the
		// batch too -- include it LAST in both orders so the ordering variable under
		// test is strictly the producer/consumer pair above.
		targets.push_back( "obj_emit" );

		const RISE::Cst::CstHeadVersion before = sess->HeadVersion();
		const Agent::AgentSession::AgentRemoveBatchResult r = sess->RemoveChunks( targets );
		Check( r.applied, std::string( label ) + ": the batch APPLIES (list order is irrelevant)" );
		Check( r.headVersion.revision == before.revision + 1,
		       std::string( label ) + ": still exactly ONE head bump" );
		const std::string doc = sess->ReadDocument();
		Check( doc.find( "name pnt_emit" ) == std::string::npos &&
		       doc.find( "name mat_emit" ) == std::string::npos &&
		       doc.find( "name obj_emit" ) == std::string::npos,
		       std::string( label ) + ": all three chunks are gone" );

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}

	// (c) An OUTSIDE-batch referrer refuses the WHOLE batch.  mat_diffuse is bound
	// by obj_sph, which is NOT in the batch; pnt_albedo's only referrer IS in the
	// batch.  So the batch must be refused, NOTHING removed, and only mat_diffuse
	// carries a still_referenced issue.
	{
		const std::string tmp = TempPath( "agentcrud_rc1c.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "RC1(c) fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

		const RISE::Cst::CstHeadVersion before = sess->HeadVersion();
		const std::string docBefore = sess->ReadDocument();
		std::vector<std::string> targets;
		targets.push_back( "pnt_albedo" );
		targets.push_back( "mat_diffuse" );
		const Agent::AgentSession::AgentRemoveBatchResult r = sess->RemoveChunks( targets );

		Check( !r.applied && r.status == "rejected", "RC1(c) the batch is REFUSED" );
		// ALL-OR-NOTHING, red-proven on BOTH observables.
		Check( r.headVersion.revision == before.revision,
		       "RC1(c) head-version UNCHANGED -- nothing was removed" );
		Check( sess->ReadDocument() == docBefore,
		       "RC1(c) the document is BYTE-IDENTICAL -- pnt_albedo, whose only referrer WAS in the "
		       "batch, was not partially removed either" );
		Check( r.message.find( "NOTHING was removed" ) != std::string::npos,
		       "RC1(c) the message STATES the all-or-nothing outcome (never left to inference)" );

		Check( r.targetResults.size() == 2, "RC1(c) two per-target entries" );
		if( r.targetResults.size() == 2 ) {
			// pnt_albedo: its only referrer (mat_diffuse) is IN the batch -> NOT blocking.
			Check( r.targetResults[0].name == "pnt_albedo", "RC1(c) entry 0 is pnt_albedo (first-occurrence order)" );
			Check( r.targetResults[0].issues.empty(),
			       "RC1(c) pnt_albedo carries NO issue -- its only referrer is INSIDE the batch, which "
			       "does not block a batch removal" );
			// mat_diffuse: obj_sph is OUTSIDE the batch -> blocking, and named.
			Check( r.targetResults[1].name == "mat_diffuse", "RC1(c) entry 1 is mat_diffuse" );
			Check( r.targetResults[1].issues.size() == 1, "RC1(c) mat_diffuse carries exactly ONE issue" );
			if( r.targetResults[1].issues.size() == 1 ) {
				const Agent::AgentChunkIssue& u = r.targetResults[1].issues[0];
				Check( u.reason == "still_referenced", "RC1(c) reason is \"still_referenced\"" );
				Check( u.value == "mat_diffuse", "RC1(c) issue value is the target's own name" );
				bool sawIt = false;
				for( const std::string& sug : u.suggestions ) if( sug == "obj_sph" ) sawIt = true;
				Check( sawIt, "RC1(c) suggestions NAME the OUTSIDE-batch referrer 'obj_sph'" );
			}
		}
		Check( r.message.find( "OUTSIDE this batch" ) != std::string::npos &&
		       r.message.find( "obj_sph" ) != std::string::npos,
		       "RC1(c) the ACTIONABLE clause names the outside referrer and says it is outside" );
		std::printf( "  RC1(c) message: %s\n", r.message.c_str() );

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}

	// (d) An UNKNOWN target in the MIDDLE of an otherwise-valid batch.
	{
		const std::string tmp = TempPath( "agentcrud_rc1d.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "RC1(d) fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

		const RISE::Cst::CstHeadVersion before = sess->HeadVersion();
		const std::string docBefore = sess->ReadDocument();
		// obj_emit has no referrers at all, and quad_emit's ONLY referrer (obj_emit) is
		// itself in the batch -- so both valid targets are genuinely unblocked, and the
		// ONLY thing wrong with this batch is the typo in the middle.
		std::vector<std::string> targets;
		targets.push_back( "obj_emit" );
		targets.push_back( "pnt_emitt" );          // typo -- resolves to nothing
		targets.push_back( "quad_emit" );
		const Agent::AgentSession::AgentRemoveBatchResult r = sess->RemoveChunks( targets );

		Check( !r.applied && r.status == "rejected", "RC1(d) the batch is REFUSED" );
		Check( r.headVersion.revision == before.revision && sess->ReadDocument() == docBefore,
		       "RC1(d) ATOMIC: obj_emit (which resolved fine, and comes BEFORE the offender) was NOT "
		       "removed -- the document is byte-identical" );
		Check( r.message.find( "pnt_emitt" ) != std::string::npos,
		       "RC1(d) the message NAMES the offending target" );
		Check( r.targetResults.size() == 3, "RC1(d) three per-target entries" );
		if( r.targetResults.size() == 3 ) {
			Check( r.targetResults[0].issues.empty() && r.targetResults[2].issues.empty(),
			       "RC1(d) the two VALID targets carry no issue" );
			Check( r.targetResults[1].issues.size() == 1, "RC1(d) the offender carries exactly ONE issue" );
			if( r.targetResults[1].issues.size() == 1 ) {
				const Agent::AgentChunkIssue& u = r.targetResults[1].issues[0];
				Check( u.reason == "unknown_target", "RC1(d) reason is \"unknown_target\"" );
				Check( u.value == "pnt_emitt", "RC1(d) issue value is the unresolvable name" );
				bool sawNearMiss = false;
				for( const std::string& sug : u.suggestions ) if( sug == "pnt_emit" ) sawNearMiss = true;
				Check( sawNearMiss, "RC1(d) suggestions rank the near-miss 'pnt_emit' (the name actually meant)" );
			}
		}
		std::printf( "  RC1(d) message: %s\n", r.message.c_str() );

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}

	// (e) DUPLICATES are deduped, not refused.
	{
		const std::string tmp = TempPath( "agentcrud_rc1e.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "RC1(e) fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

		const RISE::Cst::CstHeadVersion before = sess->HeadVersion();
		std::vector<std::string> targets;
		targets.push_back( "pnt_emit" );
		targets.push_back( "mat_emit" );
		targets.push_back( "pnt_emit" );           // listed twice
		targets.push_back( "quad_emit" );
		targets.push_back( "obj_emit" );
		targets.push_back( "pnt_emit" );           // and a third time
		const Agent::AgentSession::AgentRemoveBatchResult r = sess->RemoveChunks( targets );

		Check( r.applied, "RC1(e) duplicates do NOT refuse the batch" );
		Check( r.targetResults.size() == 4, "RC1(e) 6 inputs collapse to 4 UNIQUE per-target entries" );
		Check( r.headVersion.revision == before.revision + 1, "RC1(e) still exactly ONE head bump" );
		Check( r.note.find( "deduped" ) != std::string::npos &&
		       r.note.find( "'pnt_emit' listed 3x" ) != std::string::npos,
		       "RC1(e) the dedupe is reported HONESTLY in `note`, naming the repeated target and its count" );
		std::printf( "  RC1(e) note: %s\n", r.note.c_str() );

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}

	// (f) A SINGLE-element batch is semantically identical to the singular verb.
	// Run both on two independent fixtures loaded from the SAME text and compare
	// the resulting documents byte-for-byte.
	{
		const std::string tmpBatch = TempPath( "agentcrud_rc1f_batch.RISEscene" );
		const std::string tmpSing  = TempPath( "agentcrud_rc1f_sing.RISEscene" );
		Job* jBatch = LoadScene( kScene, tmpBatch );
		Job* jSing  = LoadScene( kScene, tmpSing );
		Check( jBatch != nullptr && jSing != nullptr, "RC1(f) both fixtures load" );
		if( !jBatch || !jSing ) return;
		std::unique_ptr<Agent::AgentSession> sBatch = Agent::AgentSession::WrapJob( jBatch );
		std::unique_ptr<Agent::AgentSession> sSing  = Agent::AgentSession::WrapJob( jSing );

		std::vector<std::string> one( 1, std::string( "obj_emit" ) );
		const Agent::AgentSession::AgentRemoveBatchResult rb = sBatch->RemoveChunks( one );
		const Agent::AgentChunkResult                     rs = sSing->RemoveChunk( "obj_emit" );

		Check( rb.applied == rs.applied && rb.status == rs.status && rb.rawCode == rs.rawCode,
		       "RC1(f) a 1-element batch reports the SAME verdict as the singular verb" );
		Check( rb.targetResults.size() == 1 && rb.targetResults[0].kind == rs.kind,
		       "RC1(f) the single per-target entry echoes the SAME resolved kind" );
		Check( sBatch->ReadDocument() == sSing->ReadDocument(),
		       "RC1(f) the resulting documents are BYTE-IDENTICAL -- the batch path is not a different erase" );

		sBatch.reset(); sSing.reset();
		jBatch->release(); jSing->release();
		std::remove( tmpBatch.c_str() ); std::remove( tmpSing.c_str() );
	}

	// (g) Request-shape refusals: an empty list, and an empty element.  Neither
	// touches the document.
	{
		const std::string tmp = TempPath( "agentcrud_rc1g.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "RC1(g) fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
		const std::string docBefore = sess->ReadDocument();

		const Agent::AgentSession::AgentRemoveBatchResult rEmpty =
			sess->RemoveChunks( std::vector<std::string>() );
		Check( !rEmpty.applied && rEmpty.status == "rejected" && rEmpty.targetResults.empty(),
		       "RC1(g) an EMPTY targets list is refused with no per-target entries" );

		std::vector<std::string> withBlank;
		withBlank.push_back( "pnt_emit" );
		withBlank.push_back( "" );
		const Agent::AgentSession::AgentRemoveBatchResult rBlank = sess->RemoveChunks( withBlank );
		Check( !rBlank.applied && rBlank.status == "rejected",
		       "RC1(g) an EMPTY element refuses the batch (it addresses no chunk)" );
		Check( rBlank.message.find( "targets[1]" ) != std::string::npos,
		       "RC1(g) the refusal names the offending INDEX" );
		Check( sess->ReadDocument() == docBefore,
		       "RC1(g) neither refusal touched the document" );

		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}
}

//----------------------------------------------------------------------
// RC2: remove_chunks over the WIRE -- the JSON-RPC envelope shape
// (applied is a BOOL, removed/total counts, the conditional `note` key,
// per-target `results` with `issues`), plus the param-validation errors.
//----------------------------------------------------------------------
static void TestRemoveChunksWireShape()
{
	std::printf( "RC2: remove_chunks wire shape through the LIVE dispatcher...\n" );

	const std::string tmp = TempPath( "agentcrud_rc2.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "RC2 fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	Agent::AgentRpcDispatcher rpc( std::move( sess ) );

	// Param validation: a singular 'target' is the likeliest slip, and the error
	// must say so in the SAME round-trip.
	{
		const std::string resp = rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"remove_chunks\",\"params\":{\"target\":\"pnt_emit\"}}" );
		Check( resp.find( "\"error\"" ) != std::string::npos, "RC2 a singular 'target' is a param error" );
		Check( resp.find( "rename to 'targets'" ) != std::string::npos,
		       "RC2 the error NAMES the wrong key the caller actually sent" );
	}
	{
		const std::string resp = rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"remove_chunks\",\"params\":{\"targets\":[]}}" );
		Check( resp.find( "non-empty array" ) != std::string::npos, "RC2 an empty array is a param error" );
	}
	{
		const std::string resp = rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"remove_chunks\",\"params\":{\"targets\":[\"a\",7]}}" );
		Check( resp.find( "'targets[1]' must be a string" ) != std::string::npos,
		       "RC2 a non-string element is a param error naming its index" );
	}

	// The success envelope.
	{
		const std::string resp = rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"remove_chunks\",\"params\":{\"targets\":"
			"[\"pnt_emit\",\"mat_emit\",\"pnt_emit\",\"quad_emit\",\"obj_emit\"]}}" );
		Agent::JsonValue result;
		Check( JsonResultObj( resp, result ), "RC2 remove_chunks returns a JSON-RPC result object" );
		Check( result.get( "applied" ).isBool() && result.get( "applied" ).asBool(),
		       "RC2 `applied` is a BOOL and is true (all-or-nothing, NOT a count like insert_chunks')" );
		Check( result.get( "removed" ).asNumber( -1 ) == 4.0, "RC2 `removed` is 4 (the deduped count)" );
		Check( result.get( "total" ).asNumber( -1 ) == 4.0,   "RC2 `total` is 4" );
		Check( result.has( "note" ) && result.get( "note" ).asString().find( "deduped" ) != std::string::npos,
		       "RC2 the dedupe rides in the CONDITIONAL `note` key" );
		Check( result.get( "results" ).isArray() && result.get( "results" ).size() == 4,
		       "RC2 `results` has one entry per UNIQUE target" );
		Check( result.get( "results" ).at( 0 ).get( "name" ).asString() == "pnt_emit",
		       "RC2 results are in first-occurrence order" );
	}

	// A refusal envelope: `note` is OMITTED entirely when there is nothing to say,
	// and the per-target `issues` localize the cause.
	{
		const std::string resp = rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"remove_chunks\",\"params\":{\"targets\":"
			"[\"pnt_albedo\",\"mat_diffuse\"]}}" );
		Agent::JsonValue result;
		Check( JsonResultObj( resp, result ), "RC2 the refusal is a result object (not a JSON-RPC error)" );
		Check( !result.get( "applied" ).asBool() && result.get( "status" ).asString() == "rejected",
		       "RC2 refusal: applied false, status rejected" );
		Check( result.get( "removed" ).asNumber( -1 ) == 0.0,
		       "RC2 refusal: `removed` is 0 -- the all-or-nothing outcome stated numerically" );
		Check( !result.has( "note" ), "RC2 the `note` key is OMITTED when there is nothing to remark on" );
		const Agent::JsonValue& entries = result.get( "results" );
		Check( entries.isArray() && entries.size() == 2, "RC2 refusal still carries both per-target entries" );
		if( entries.isArray() && entries.size() == 2 ) {
			Check( !entries.at( 0 ).has( "issues" ),
			       "RC2 the intra-batch-only target has NO `issues` key (omitted when empty)" );
			Check( entries.at( 1 ).get( "issues" ).isArray() &&
			       entries.at( 1 ).get( "issues" ).at( 0 ).get( "reason" ).asString() == "still_referenced",
			       "RC2 the blocked target's `issues` carry reason still_referenced" );
		}
	}

	std::remove( tmp.c_str() );
}


//----------------------------------------------------------------------
// RC3: remove_chunks on the LIVE (controller-attached) path -- the two
// things the headless RC1 cannot reach:
//   (a) an OWNER batch remove commits through
//       SceneEditController::ApplyAgentRemoveChunks with ONE head bump;
//   (b) an EXTERNAL batch remove stages as ONE bundled proposal (never N
//       -- an Owner must approve or reject exactly the atomic edit that
//       was proposed), whose approval then applies it atomically.
//----------------------------------------------------------------------
static void TestRemoveChunksLiveAndProposal()
{
	std::printf( "RC3: remove_chunks LIVE commit + ONE bundled proposal...\n" );

	// (a) OWNER, live controller attached.
	{
		const std::string tmp = TempPath( "agentcrud_rc3a.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "RC3(a) fixture loads" );
		if( !pJob ) return;

		TestController c( *pJob, /*simulatedRenderMs*/ 0 );
		c.Start();
		std::unique_ptr<Agent::AgentSession> owner = Agent::AgentSession::WrapJob( pJob, Agent::AgentAuthority::Owner );
		owner->AttachController( &c );

		const RISE::Cst::CstHeadVersion before = pJob->GetCstHeadVersion();
		std::vector<std::string> targets;
		targets.push_back( "pnt_emit" );
		targets.push_back( "mat_emit" );
		targets.push_back( "quad_emit" );
		targets.push_back( "obj_emit" );
		const Agent::AgentSession::AgentRemoveBatchResult r = owner->RemoveChunks( targets );

		Check( r.applied && r.status == "applied", "RC3(a) the LIVE batch APPLIES" );
		Check( r.headVersion.revision == before.revision + 1,
		       "RC3(a) EXACTLY ONE head bump on the LIVE path too" );
		Check( pJob->GetMaterials() && pJob->GetMaterials()->GetItem( "mat_emit" ) == nullptr,
		       "RC3(a) the LIVE derived scene really lost the emissive material" );
		Check( c.HasUnsavedChanges(), "RC3(a) the batch marks the editor dirty" );

		// A LIVE refusal is equally atomic.
		const std::string docBefore = owner->ReadDocument();
		const RISE::Cst::CstHeadVersion beforeRefusal = pJob->GetCstHeadVersion();
		std::vector<std::string> blocked;
		blocked.push_back( "pnt_albedo" );
		blocked.push_back( "mat_diffuse" );
		const Agent::AgentSession::AgentRemoveBatchResult rr = owner->RemoveChunks( blocked );
		Check( !rr.applied && rr.status == "rejected", "RC3(a) the outside-referenced LIVE batch is REFUSED" );
		Check( pJob->GetCstHeadVersion().revision == beforeRefusal.revision &&
		       owner->ReadDocument() == docBefore,
		       "RC3(a) the LIVE refusal removed NOTHING -- document byte-identical" );
		Check( rr.targetResults.size() == 2 && rr.targetResults[1].issues.size() == 1 &&
		       rr.targetResults[1].issues[0].reason == "still_referenced",
		       "RC3(a) the LIVE refusal carries the same per-target still_referenced localization" );

		owner.reset();
		c.Stop();
		pJob->release();
		std::remove( tmp.c_str() );
	}

	// (b) EXTERNAL authority -> ONE bundled proposal -> approve -> atomic apply.
	{
		const std::string tmp = TempPath( "agentcrud_rc3b.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "RC3(b) fixture loads" );
		if( !pJob ) return;

		TestController c( *pJob, /*simulatedRenderMs*/ 0 );
		c.Start();
		std::unique_ptr<Agent::AgentSession> owner = Agent::AgentSession::WrapJob( pJob, Agent::AgentAuthority::Owner );
		std::unique_ptr<Agent::AgentSession> ext   = Agent::AgentSession::WrapJob( pJob, Agent::AgentAuthority::External );
		owner->AttachController( &c );
		ext->AttachController( &c );

		const std::string docBefore = owner->ReadDocument();
		std::vector<std::string> targets;
		targets.push_back( "pnt_emit" );
		targets.push_back( "mat_emit" );
		targets.push_back( "quad_emit" );
		targets.push_back( "obj_emit" );
		const Agent::AgentSession::AgentRemoveBatchResult staged = ext->RemoveChunks( targets );

		Check( !staged.applied && staged.status == "staged", "RC3(b) the External batch STAGES" );
		Check( owner->ReadDocument() == docBefore, "RC3(b) staging is INERT -- document untouched" );
		Check( staged.message.find( "ONE proposal for all 4 targets" ) != std::string::npos,
		       "RC3(b) the staged message states the batch is ONE proposal" );

		// THE bundling claim: ONE queue entry, not four.
		std::vector<Agent::AgentSession::AgentProposalEntry> pending;
		for( const auto& p : owner->ListProposals() ) if( p.status == "pending" ) pending.push_back( p );
		Check( pending.size() == 1,
		       "RC3(b) EXACTLY ONE proposal is queued for the whole 4-target batch (never four -- an Owner "
		       "must not be able to partially approve an all-or-nothing edit)" );
		if( pending.size() != 1 ) { owner.reset(); ext.reset(); c.Stop(); pJob->release(); std::remove( tmp.c_str() ); return; }
		Check( pending[0].kind == "remove_chunks", "RC3(b) the entry's kind is \"remove_chunks\"" );
		// The card needs the names: they ride '\n'-packed in `target`.
		for( const std::string& n : targets )
			Check( pending[0].target.find( n ) != std::string::npos,
			       std::string( "RC3(b) the queued entry names '" ) + n + "'" );

		const Agent::AgentSession::AgentResolveResult rr = owner->ResolveProposal( pending[0].id, /*approve=*/true );
		Check( rr.ok && rr.status == "applied", "RC3(b) approving the bundled proposal APPLIES it" );
		const std::string docAfter = owner->ReadDocument();
		for( const std::string& n : targets )
			Check( docAfter.find( "name " + n ) == std::string::npos,
			       std::string( "RC3(b) '" ) + n + "' is gone after the single approval" );
		Check( docAfter.find( "name mat_diffuse" ) != std::string::npos,
		       "RC3(b) the untargeted subassembly survived the approval" );

		owner.reset();
		ext.reset();
		c.Stop();
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

//----------------------------------------------------------------------
// RG1-RG12: R2 (2026-08-10, replace_geometry_scaffold) -- one-call form
// revision.  Same fixture/helper conventions as GS1-GS25 above
// (TempPath/LoadScene/TmplName/ExtractParamAfter reused verbatim).
//----------------------------------------------------------------------

//! The whole `standard_object { ... }` chunk text for `name`, from its
//! keyword line through its closing brace -- the byte-exact unit RG1
//! compares before/after to prove that ONLY the `geometry` param moved.
static std::string ExtractObjectChunkText( const std::string& doc, const std::string& name )
{
	const std::string marker = "name " + name + "\n";
	const std::size_t namePos = doc.find( marker );
	if( namePos == std::string::npos ) return std::string();
	const std::size_t open = doc.rfind( "standard_object", namePos );
	if( open == std::string::npos ) return std::string();
	const std::size_t close = doc.find( "\n}", namePos );
	if( close == std::string::npos ) return std::string();
	return doc.substr( open, ( close + 2 ) - open );
}

//! The value of `param` inside the `standard_object` named `name` --
//! tab-tolerant, unlike ExtractParamAfter above (the fixture scene's
//! chunks are TAB-indented; the scaffold generator's are not, which is
//! why the older helper never had to handle it).
static std::string ExtractObjectParam( const std::string& doc, const std::string& name, const std::string& param )
{
	const std::string chunk = ExtractObjectChunkText( doc, name );
	if( chunk.empty() ) return std::string();
	const std::size_t at = chunk.find( "\n" );
	std::size_t pos = ( at == std::string::npos ) ? 0 : at;
	while( pos < chunk.size() ) {
		const std::size_t eol = chunk.find( '\n', pos + 1 );
		std::string line = chunk.substr( pos + 1, ( eol == std::string::npos ? chunk.size() : eol ) - pos - 1 );
		std::size_t b = line.find_first_not_of( " \t" );
		if( b != std::string::npos ) {
			line = line.substr( b );
			if( line.compare( 0, param.size(), param ) == 0 && line.size() > param.size() &&
			    ( line[param.size()] == ' ' || line[param.size()] == '\t' ) ) {
				std::string v = line.substr( param.size() );
				const std::size_t vb = v.find_first_not_of( " \t" );
				if( vb == std::string::npos ) return std::string();
				v = v.substr( vb );
				while( !v.empty() && ( v.back() == '\r' || v.back() == ' ' || v.back() == '\t' ) ) v.pop_back();
				return v;
			}
		}
		if( eol == std::string::npos ) break;
		pos = eol;
	}
	return std::string();
}

//! RG1: the happy path, per REPLACEABLE family (all five -- volume_bank
//! is refused, RG5).  The object's `geometry` rebinds to the generated
//! chunk, every generated chunk is in the document, the head bumps
//! EXACTLY ONCE for the whole composite, and every OTHER param on the
//! object (position/orientation/scale/material) is byte-identical.
static void TestReplaceGeometryScaffoldFamilies()
{
	std::printf( "RG1: replace_geometry_scaffold -- all 5 replaceable families rebind + one head bump + transform preserved...\n" );

	struct RCase { const char* family; const char* geometryKind; const char* roleSuffix; };
	const RCase cases[] = {
		{ "displaced_slab", "displaced_geometry", "disp"   },
		{ "sweep_rail",     "sweep_geometry",     "rail"   },
		{ "blended_vessel", "sdf_geometry",       "vessel" },
		{ "sdf_column",     "sdf_geometry",       "col"    },
		{ "blended_chain",  "sdf_geometry",       "chain"  },
	};

	for( const RCase& rc : cases ) {
		const std::string tmp = TempPath( ( std::string( "agentcrud_rg1_" ) + rc.family + ".RISEscene" ).c_str() );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, std::string( "RG1(" ) + rc.family + ") fixture loads" );
		if( !pJob ) continue;

		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

		// Give obj_sph a transform + orientation so "preserved" is a real
		// claim about real params, not vacuously true on an empty object.
		{
			Agent::AgentSetPatch p1;
			p1.target = "obj_sph"; p1.kind = "standard_object";
			p1.param = "position"; p1.value = "0.25 -0.5 0.75";
			Check( sess->ProposePatch( p1 ).applied, std::string( "RG1(" ) + rc.family + ") fixture position patch applied" );
			Agent::AgentSetPatch p2;
			p2.target = "obj_sph"; p2.kind = "standard_object";
			p2.param = "orientation"; p2.value = "10 20 30";
			Check( sess->ProposePatch( p2 ).applied, std::string( "RG1(" ) + rc.family + ") fixture orientation patch applied" );
		}

		const std::string docBefore = sess->ReadDocument();
		const std::string objBefore = ExtractObjectChunkText( docBefore, "obj_sph" );
		Check( !objBefore.empty(), std::string( "RG1(" ) + rc.family + ") the object chunk is extractable before the call" );
		const RISE::Cst::CstHeadVersion v0 = sess->HeadVersion();

		const bool isChain = ( std::string( rc.family ) == "blended_chain" );
		const Agent::AgentSession::AgentGeometryScaffoldResult sr = sess->ReplaceGeometryScaffold(
			"obj_sph", rc.family, "formA", isChain ? 0.25 : 1.2, 0.6, 1.3,
			isChain ? std::string( "0 0 0; 0.4 0.9 0.1; 0.9 1.4 -0.2" ) : std::string(),
			isChain ? 0.6 : 0.0, std::string(), &v0 );

		Check( sr.ok, std::string( "RG1(" ) + rc.family + ") call succeeded: " + sr.message );
		if( !sr.ok ) { pJob->release(); std::remove( tmp.c_str() ); continue; }

		Check( sr.geometryKind == rc.geometryKind,
		       std::string( "RG1(" ) + rc.family + ") geometry kind matches the family design" );
		Check( sr.geometryName == TmplName( "formA", rc.roleSuffix ),
		       std::string( "RG1(" ) + rc.family + ") geometry chunk is named tmpl_formA_" + rc.roleSuffix );
		Check( sr.replacedObject == "obj_sph", std::string( "RG1(" ) + rc.family + ") result echoes the rebound object" );
		Check( sr.previousGeometryName == "sph",
		       std::string( "RG1(" ) + rc.family + ") result names the PREVIOUS geometry" );

		// ONE head bump for the WHOLE composite -- the headline property.
		const RISE::Cst::CstHeadVersion v1 = sess->HeadVersion();
		Check( v1.revision == v0.revision + 1,
		       std::string( "RG1(" ) + rc.family + ") EXACTLY ONE head-version bump for the whole composite (was " +
		       std::to_string( (unsigned long long)v0.revision ) + ", now " +
		       std::to_string( (unsigned long long)v1.revision ) + ")" );

		const std::string docAfter = sess->ReadDocument();
		Check( ExtractObjectParam( docAfter, "obj_sph", "geometry" ) == sr.geometryName,
		       std::string( "RG1(" ) + rc.family + ") the object's geometry slot now names the NEW chunk" );
		for( const Agent::AgentChunkResult& cr : sr.chunkResults ) {
			Check( cr.applied, std::string( "RG1(" ) + rc.family + ") generated chunk `" + cr.name + "` reports applied" );
			Check( docAfter.find( "name " + cr.name + "\n" ) != std::string::npos,
			       std::string( "RG1(" ) + rc.family + ") generated chunk `" + cr.name + "` is in the document" );
		}
		Check( pJob->GetGeometries() && pJob->GetGeometries()->GetItem( sr.geometryName.c_str() ) != nullptr,
		       std::string( "RG1(" ) + rc.family + ") the live geometry manager resolved the new graph" );

		// TRANSFORM PRESERVED: the object chunk differs from before ONLY in
		// its `geometry` value.  Rebuild the before-text with the geometry
		// value swapped and require BYTE equality with the after-text.
		const std::string objAfter = ExtractObjectChunkText( docAfter, "obj_sph" );
		std::string objExpected = objBefore;
		{
			const std::string from = "geometry sph";
			const std::size_t at = objExpected.find( from );
			Check( at != std::string::npos, std::string( "RG1(" ) + rc.family + ") the before-text carries `geometry sph`" );
			if( at != std::string::npos )
				objExpected = objExpected.substr( 0, at ) + "geometry " + sr.geometryName +
				              objExpected.substr( at + from.size() );
		}
		Check( objAfter == objExpected,
		       std::string( "RG1(" ) + rc.family + ") the object chunk is BYTE-IDENTICAL apart from the geometry value "
		       "(position/orientation/material/every other param preserved)" );

		// The old geometry was referenced ONLY by obj_sph, so it goes.
		Check( sr.previousGeometryRemoved,
		       std::string( "RG1(" ) + rc.family + ") the now-unreferenced previous geometry was removed" );
		Check( docAfter.find( "name sph\n" ) == std::string::npos,
		       std::string( "RG1(" ) + rc.family + ") `sph` is gone from the document" );

		pJob->release();
		std::remove( tmp.c_str() );
	}
}

//! RG2: orphan policy -- the previous geometry is RETAINED (and its
//! referrers named) when a SECOND object still references it.
static void TestReplaceGeometryScaffoldRetainsSharedGeometry()
{
	std::printf( "RG2: replace_geometry_scaffold -- shared previous geometry is RETAINED and its referrers reported...\n" );
	const std::string tmp = TempPath( "agentcrud_rg2.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "RG2 fixture loads" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const Agent::AgentChunkResult ins = sess->InsertChunk(
		"standard_object\n{\nname obj_sph2\ngeometry sph\nmaterial mat_diffuse\nposition 2 0 0\n}" );
	Check( ins.applied, "RG2 a SECOND object referencing `sph` applied" );

	const Agent::AgentSession::AgentGeometryScaffoldResult sr =
		sess->ReplaceGeometryScaffold( "obj_sph", "sweep_rail", "shared1", 1.0, 0.4, 1.0 );
	Check( sr.ok, std::string( "RG2 the replace succeeded: " ) + sr.message );
	Check( !sr.previousGeometryRemoved, "RG2 the previous geometry was NOT removed (still referenced)" );
	bool namesOther = false;
	for( const std::string& r : sr.previousGeometryReferrers ) if( r == "obj_sph2" ) namesOther = true;
	Check( namesOther, "RG2 the result NAMES the remaining referrer (`obj_sph2`)" );
	Check( sr.message.find( "obj_sph2" ) != std::string::npos, "RG2 the message names the remaining referrer too" );

	const std::string docAfter = sess->ReadDocument();
	Check( docAfter.find( "name sph\n" ) != std::string::npos, "RG2 `sph` is still in the document" );
	Check( ExtractObjectParam( docAfter, "obj_sph2", "geometry" ) == "sph",
	       "RG2 the OTHER object still points at `sph`" );
	Check( ExtractObjectParam( docAfter, "obj_sph", "geometry" ) == sr.geometryName,
	       "RG2 the TARGET object points at the new geometry" );

	pJob->release();
	std::remove( tmp.c_str() );
}

//! RG3: deeper orphans are REPORTED, never chased.  Replace a
//! displaced_slab-produced geometry: its base box + its noise Function2D
//! become unreferenced when the displaced_geometry goes, and the verb
//! must NAME them rather than delete them.
static void TestReplaceGeometryScaffoldReportsDeeperOrphans()
{
	std::printf( "RG3: replace_geometry_scaffold -- deeper orphans REPORTED, not removed...\n" );
	const std::string tmp = TempPath( "agentcrud_rg3.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "RG3 fixture loads" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

	// First give obj_sph a displaced_slab form (3 chunks: base box, noise
	// Function2D, displaced_geometry) -- then replace THAT.
	const Agent::AgentSession::AgentGeometryScaffoldResult first =
		sess->ReplaceGeometryScaffold( "obj_sph", "displaced_slab", "slabA", 1.2, 0.5, 1.0 );
	Check( first.ok, std::string( "RG3 the first (displaced_slab) replace succeeded: " ) + first.message );
	Check( first.chunkResults.size() == 3, "RG3 displaced_slab generated its 3-chunk graph" );

	const std::string docMid = sess->ReadDocument();
	const RISE::Cst::CstHeadVersion vMid = sess->HeadVersion();

	const Agent::AgentSession::AgentGeometryScaffoldResult second =
		sess->ReplaceGeometryScaffold( "obj_sph", "sweep_rail", "railA", 1.0, 0.4, 1.0 );
	Check( second.ok, std::string( "RG3 the second (sweep_rail) replace succeeded: " ) + second.message );
	Check( second.previousGeometryRemoved, "RG3 the displaced_geometry chunk itself WAS removed" );
	Check( second.previousGeometryKind == "displaced_geometry", "RG3 the previous geometry's kind is reported" );

	const std::string docAfter = sess->ReadDocument();
	Check( docAfter.find( "name " + first.geometryName + "\n" ) == std::string::npos,
	       "RG3 the old displaced_geometry is gone" );

	// Its two feeders are now unreferenced -- REPORTED, still present.
	Check( !second.reportedOrphans.empty(), "RG3 at least one deeper orphan was REPORTED" );
	bool sawBase = false, sawNoise = false;
	for( const std::string& o : second.reportedOrphans ) {
		if( o.find( TmplName( "slabA", "base" ) ) != std::string::npos ) sawBase = true;
		if( o.find( TmplName( "slabA", "noise" ) ) != std::string::npos ) sawNoise = true;
		// Reported in "keyword/name" form so a model can hand them to remove_chunks.
		Check( o.find( '/' ) != std::string::npos, "RG3 each reported orphan carries its keyword/name form" );
		const std::string bare = o.substr( o.find( '/' ) + 1 );
		Check( docAfter.find( "name " + bare + "\n" ) != std::string::npos,
		       "RG3 reported orphan `" + bare + "` is STILL IN the document (reported, not removed)" );
	}
	Check( sawBase || sawNoise, "RG3 the displaced_slab feeders are among the reported orphans" );
	Check( second.message.find( "remove_chunks" ) != std::string::npos,
	       "RG3 the message points the model at remove_chunks for the leftovers" );

	// ONE head bump for the second composite too.
	Check( sess->HeadVersion().revision == vMid.revision + 1, "RG3 the second composite bumped the head exactly once" );
	Check( docMid != docAfter, "RG3 the document actually changed" );

	pJob->release();
	std::remove( tmp.c_str() );
}

//! RG4: volume_bank is refused with the actionable message, document
//! byte-identical and head unbumped.
static void TestReplaceGeometryScaffoldVolumeBankRefused()
{
	std::printf( "RG4: replace_geometry_scaffold -- volume_bank refused (it emits its own object)...\n" );
	const std::string tmp = TempPath( "agentcrud_rg4.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "RG4 fixture loads" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const std::string docBefore = sess->ReadDocument();
	const RISE::Cst::CstHeadVersion v0 = sess->HeadVersion();

	// NOTE the deliberately ABSENT `tone`: volume_bank's own required param.
	// The family refusal must WIN over any per-family param complaint --
	// telling a caller to supply `tone` for a family they cannot use here
	// would send them one full turn in the wrong direction.
	const Agent::AgentSession::AgentGeometryScaffoldResult sr = sess->ReplaceGeometryScaffold(
		"obj_sph", "volume_bank", "bankA", 1.4, 0.6, 2.0, std::string(), 0.0, std::string() );
	Check( !sr.ok, "RG4 volume_bank is REFUSED" );
	Check( sr.message.find( "tone" ) == std::string::npos,
	       "RG4 the refusal is about the FAMILY, not about volume_bank's own missing `tone` param" );
	Check( sr.message.find( "volume_bank" ) != std::string::npos, "RG4 the refusal names the family" );
	Check( sr.message.find( "insert_geometry_scaffold" ) != std::string::npos,
	       "RG4 the refusal points at insert_geometry_scaffold (the route that works)" );
	Check( sr.message.find( "document unchanged" ) != std::string::npos, "RG4 the refusal says the document is unchanged" );
	Check( sess->ReadDocument() == docBefore, "RG4 the document IS byte-identical" );
	Check( sess->HeadVersion().revision == v0.revision, "RG4 the head version is unbumped" );

	pJob->release();
	std::remove( tmp.c_str() );
}

//! RG5: passing a GEOMETRY chunk name where an OBJECT name belongs --
//! the likely model mistake -- is diagnosed SPECIFICALLY, naming the
//! objects that consume that geometry.
static void TestReplaceGeometryScaffoldGeometryTargetDiagnosis()
{
	std::printf( "RG5: replace_geometry_scaffold -- a geometry-chunk target names the consuming objects...\n" );
	const std::string tmp = TempPath( "agentcrud_rg5.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "RG5 fixture loads" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const std::string docBefore = sess->ReadDocument();
	const RISE::Cst::CstHeadVersion v0 = sess->HeadVersion();

	const Agent::AgentSession::AgentGeometryScaffoldResult sr =
		sess->ReplaceGeometryScaffold( "sph", "sweep_rail", "geoTgt", 1.0, 0.4, 1.0 );
	Check( !sr.ok, "RG5 a geometry-chunk target is REFUSED" );
	Check( sr.message.find( "sphere_geometry" ) != std::string::npos,
	       "RG5 the refusal says WHAT the target actually is" );
	Check( sr.message.find( "obj_sph" ) != std::string::npos,
	       "RG5 the refusal NAMES the standard_object that consumes it (the argument the caller meant)" );
	Check( sess->ReadDocument() == docBefore, "RG5 the document IS byte-identical" );
	Check( sess->HeadVersion().revision == v0.revision, "RG5 the head version is unbumped" );

	// A non-geometry, non-object target gets the plain kind refusal.
	const Agent::AgentSession::AgentGeometryScaffoldResult sr2 =
		sess->ReplaceGeometryScaffold( "mat_diffuse", "sweep_rail", "matTgt", 1.0, 0.4, 1.0 );
	Check( !sr2.ok, "RG5 a material target is REFUSED" );
	Check( sr2.message.find( "lambertian_material" ) != std::string::npos &&
	       sr2.message.find( "standard_object" ) != std::string::npos,
	       "RG5 the material refusal names the actual kind AND the required one" );
	Check( sess->ReadDocument() == docBefore, "RG5 the document is STILL byte-identical after the second refusal" );

	pJob->release();
	std::remove( tmp.c_str() );
}

//! RG6: unknown and AMBIGUOUS targets both refuse with the document
//! unmutated and the head unbumped.
static void TestReplaceGeometryScaffoldUnknownAndAmbiguousTarget()
{
	std::printf( "RG6: replace_geometry_scaffold -- unknown / ambiguous target refusals...\n" );
	const std::string tmp = TempPath( "agentcrud_rg6.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "RG6 fixture loads" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const std::string docBefore = sess->ReadDocument();
	const RISE::Cst::CstHeadVersion v0 = sess->HeadVersion();

	const Agent::AgentSession::AgentGeometryScaffoldResult unknown =
		sess->ReplaceGeometryScaffold( "obj_sphere", "sweep_rail", "unk1", 1.0, 0.4, 1.0 );
	Check( !unknown.ok, "RG6 an unknown target is REFUSED" );
	Check( unknown.message.find( "obj_sph" ) != std::string::npos,
	       "RG6 the unknown-target refusal offers the near-miss `obj_sph`" );
	Check( sess->ReadDocument() == docBefore, "RG6 the document is byte-identical after the unknown-target refusal" );
	Check( sess->HeadVersion().revision == v0.revision, "RG6 the head is unbumped after the unknown-target refusal" );

	// AMBIGUOUS: a painter and a geometry sharing one bare name.
	{
		const Agent::AgentChunkResult a =
			sess->InsertChunk( "uniformcolor_painter\n{\nname twin\ncolor 0.2 0.3 0.4\n}" );
		Check( a.applied, "RG6 the first `twin` chunk applied" );
		const Agent::AgentChunkResult b = sess->InsertChunk( "sphere_geometry\n{\nname twin\nradius 0.3\n}" );
		Check( b.applied, "RG6 the second `twin` chunk (different kind, same name) applied" );
	}
	const std::string docTwin = sess->ReadDocument();
	const RISE::Cst::CstHeadVersion vTwin = sess->HeadVersion();
	const Agent::AgentSession::AgentGeometryScaffoldResult amb =
		sess->ReplaceGeometryScaffold( "twin", "sweep_rail", "amb1", 1.0, 0.4, 1.0 );
	Check( !amb.ok, "RG6 an ambiguous target is REFUSED" );
	Check( amb.message.find( "ambiguous" ) != std::string::npos, "RG6 the refusal says `ambiguous`" );
	Check( sess->ReadDocument() == docTwin, "RG6 the document is byte-identical after the ambiguous refusal" );
	Check( sess->HeadVersion().revision == vTwin.revision, "RG6 the head is unbumped after the ambiguous refusal" );

	pJob->release();
	std::remove( tmp.c_str() );
}

//! RG7: ATOMIC FAILURE -- a param refusal, a name collision and a stale
//! baseHeadVersion each leave the document byte-identical and the head
//! unbumped.  Nothing partial ever lands.
static void TestReplaceGeometryScaffoldAtomicRefusals()
{
	std::printf( "RG7: replace_geometry_scaffold -- every refusal leaves document + head untouched...\n" );
	const std::string tmp = TempPath( "agentcrud_rg7.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "RG7 fixture loads" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const std::string docBefore = sess->ReadDocument();
	const RISE::Cst::CstHeadVersion v0 = sess->HeadVersion();

	struct Bad { const char* what; const char* family; double size; double detail; double aspect; };
	const Bad bads[] = {
		{ "size <= 0",      "sweep_rail", 0.0, 0.5, 1.0 },
		{ "detail > 1",     "sweep_rail", 1.0, 1.5, 1.0 },
		{ "aspect <= 0",    "sweep_rail", 1.0, 0.5, 0.0 },
		{ "unknown family", "no_such",    1.0, 0.5, 1.0 },
	};
	for( const Bad& b : bads ) {
		const Agent::AgentSession::AgentGeometryScaffoldResult sr =
			sess->ReplaceGeometryScaffold( "obj_sph", b.family, "badA", b.size, b.detail, b.aspect );
		Check( !sr.ok, std::string( "RG7 (" ) + b.what + ") is refused" );
		Check( sr.message.find( "replace_geometry_scaffold refused" ) != std::string::npos,
		       std::string( "RG7 (" ) + b.what + ") refusal is labelled with THIS verb" );
		Check( sess->ReadDocument() == docBefore, std::string( "RG7 (" ) + b.what + ") document byte-identical" );
		Check( sess->HeadVersion().revision == v0.revision, std::string( "RG7 (" ) + b.what + ") head unbumped" );
	}

	// NAME COLLISION: land one expansion, then re-use its name.
	const Agent::AgentSession::AgentGeometryScaffoldResult ok1 =
		sess->ReplaceGeometryScaffold( "obj_sph", "sweep_rail", "collideA", 1.0, 0.4, 1.0 );
	Check( ok1.ok, std::string( "RG7 the first expansion landed: " ) + ok1.message );
	const std::string docMid = sess->ReadDocument();
	const RISE::Cst::CstHeadVersion vMid = sess->HeadVersion();
	const Agent::AgentSession::AgentGeometryScaffoldResult dup =
		sess->ReplaceGeometryScaffold( "obj_sph", "sweep_rail", "collideA", 1.0, 0.4, 1.0 );
	Check( !dup.ok, "RG7 a colliding name is refused" );
	Check( dup.message.find( "already exists" ) != std::string::npos, "RG7 the collision refusal says so" );
	Check( sess->ReadDocument() == docMid, "RG7 document byte-identical after the collision refusal" );
	Check( sess->HeadVersion().revision == vMid.revision, "RG7 head unbumped after the collision refusal" );

	// STALE baseHeadVersion -> CONFLICT, nothing touched.  R2 fix-round (P1): a conflict reaches a
	// commit-stage disposition -- `ok` is true (the request itself was well-formed) and `status`
	// carries the actual outcome, the SAME distinction ProposePatch's identical stale-base check
	// draws; it is not folded into the same `ok==false` bucket as the pre-flight refusals above.
	const Agent::AgentSession::AgentGeometryScaffoldResult stale =
		sess->ReplaceGeometryScaffold( "obj_sph", "sweep_rail", "staleA", 1.0, 0.4, 1.0,
		                               std::string(), 0.0, std::string(), &v0 );
	Check( stale.ok, "RG7 MONEY RED-PROVE: a stale baseHeadVersion conflict reports ok=true" );
	Check( stale.status == "conflict", "RG7 MONEY RED-PROVE: status is \"conflict\"" );
	Check( !stale.retriable, "RG7 a conflict does not set retriable" );
	Check( stale.headVersion == vMid, "RG7 the reported headVersion is the CURRENT head" );
	Check( stale.message.find( "baseHeadVersion" ) != std::string::npos, "RG7 the conflict refusal says baseHeadVersion" );
	Check( sess->ReadDocument() == docMid, "RG7 document byte-identical after the conflict refusal" );
	Check( sess->HeadVersion().revision == vMid.revision, "RG7 head unbumped after the conflict refusal" );

	pJob->release();
	std::remove( tmp.c_str() );
}

//! RG8: the E1 non-sampling-emitter gate and the R1c rasterizer
//! allowlist gate are evaluated against the CANDIDATE document on this
//! path -- and, being DELTA gates, they must not FREEZE a scene that
//! already carries the construct they police.  Both conditions are
//! asserted here: a scene with a pre-existing unacknowledged emissive
//! csg_object, and a scene with a pre-existing BLOCKED rasterizer, are
//! both still form-revisable, and the gates leave their subjects alone.
static void TestReplaceGeometryScaffoldGatesAreDelta()
{
	std::printf( "RG8: replace_geometry_scaffold -- E1 + R1c gates run on the candidate and are DELTA, not state...\n" );

	// (a) A scene whose ACTIVE rasterizer is a BLOCKED kind (bdpt).  The
	// candidate's rasterizer multiset is unchanged by a geometry swap, so
	// the R1c arm must return "" and the replace must land.
	{
		std::string sceneText( kScene );
		sceneText += "\nbdpt_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n}\n";
		const std::string tmp = TempPath( "agentcrud_rg8a.RISEscene" );
		Job* pJob = LoadScene( sceneText.c_str(), tmp );
		Check( pJob != nullptr, "RG8(a) blocked-rasterizer fixture loads" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const Agent::AgentSession::AgentGeometryScaffoldResult sr =
				sess->ReplaceGeometryScaffold( "obj_sph", "sweep_rail", "r1cA", 1.0, 0.4, 1.0 );
			Check( sr.ok, std::string( "RG8(a) a scene that ALREADY declares a blocked rasterizer is still "
			                           "form-revisable (the R1c gate is a DELTA): " ) + sr.message );
			const std::string docAfter = sess->ReadDocument();
			Check( docAfter.find( "bdpt_pel_rasterizer" ) != std::string::npos,
			       "RG8(a) the pre-existing blocked rasterizer is untouched by the composite" );
			// RED-PROVE the gate is actually wired: the SAME candidate arm
			// refuses when a blocked rasterizer really IS newly introduced.
			const Agent::AgentChunkResult bad =
				sess->InsertChunk( "mlt_rasterizer\n{\nsamples 4\n}" );
			Check( !bad.applied && bad.message.find( "SPECIALIZED rasterizer" ) != std::string::npos,
			       "RG8(a) the shared R1c policy still BLOCKS a newly introduced mlt_rasterizer" );
			pJob->release();
			std::remove( tmp.c_str() );
		}
	}

	// (b) A scene carrying a PRE-EXISTING unacknowledged emissive
	// csg_object (exactly the construct E1 refuses to CREATE).  A geometry
	// swap on an unrelated object neither creates nor worsens it, so the
	// E1 arm must return "" and the replace must land -- the "Warning nags,
	// edits proceed" posture arm C of the patch gate documents.
	{
		std::string sceneText( kScene );
		sceneText +=
			"\nsphere_geometry\n{\n\tname csg_a_geo\n\tradius 0.3\n}\n"
			"\nstandard_object\n{\n\tname csg_a\n\tgeometry csg_a_geo\n\tmaterial mat_diffuse\n}\n"
			"\nstandard_object\n{\n\tname csg_b\n\tgeometry csg_a_geo\n\tmaterial mat_diffuse\n\tposition 0.2 0 0\n}\n"
			"\ncsg_object\n{\n\tname csg_emit\n\tobja csg_a\n\tobjb csg_b\n\toperation union\n\tmaterial mat_emit\n}\n";
		const std::string tmp = TempPath( "agentcrud_rg8b.RISEscene" );
		Job* pJob = LoadScene( sceneText.c_str(), tmp );
		// The fixture is only useful if it loads; a csg param-name change
		// upstream would make this vacuous, so say so rather than pass quietly.
		Check( pJob != nullptr, "RG8(b) pre-existing-unacknowledged-emitter fixture loads" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const std::string docBefore = sess->ReadDocument();
			Check( docBefore.find( "csg_emit" ) != std::string::npos, "RG8(b) the csg_object is in the head" );
			// RED-PROVE the fixture is NOT vacuous: E1 must actually see this
			// construct.  Arm A (re-pointing the csg's own `material`) is
			// STATE-based, so re-pointing it at the same emissive material is
			// refused precisely because the candidate comes back unacknowledged.
			{
				Agent::AgentSetPatch p;
				p.target = "csg_emit"; p.kind = "csg_object";
				p.param  = "material"; p.value = "mat_emit";
				const Agent::AgentPatchResult pr = sess->ProposePatch( p );
				Check( !pr.applied && pr.message.find( "allow_non_sampling_emitter" ) != std::string::npos,
				       "RG8(b) RED-PROOF: E1 DOES flag this fixture's csg_object (so the delta assertion below is not vacuous)" );
			}
			const Agent::AgentSession::AgentGeometryScaffoldResult sr =
				sess->ReplaceGeometryScaffold( "obj_sph", "sweep_rail", "e1A", 1.0, 0.4, 1.0 );
			Check( sr.ok, std::string( "RG8(b) an unrelated form revision is NOT frozen by a PRE-EXISTING "
			                           "unacknowledged emissive csg (the E1 gate is a DELTA): " ) + sr.message );
			Check( sess->ReadDocument().find( "csg_emit" ) != std::string::npos,
			       "RG8(b) the csg_object is untouched by the composite" );
			pJob->release();
			std::remove( tmp.c_str() );
		}
	}
}

//! RG9: External authority does NOT stage this verb -- it is refused
//! with the two-call route, document byte-identical.
static void TestReplaceGeometryScaffoldExternalAuthority()
{
	std::printf( "RG9: replace_geometry_scaffold -- External authority is REFUSED (no staged form), document unchanged...\n" );
	const std::string tmp = TempPath( "agentcrud_rg9.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "RG9 fixture loads" );
	if( !pJob ) return;

	TestController c( *pJob, /*simulatedRenderMs*/ 0 );
	c.Start();

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob, Agent::AgentAuthority::External );
	sess->AttachController( &c );
	const std::string docBefore = sess->ReadDocument();

	const Agent::AgentSession::AgentGeometryScaffoldResult sr =
		sess->ReplaceGeometryScaffold( "obj_sph", "sweep_rail", "extR", 1.0, 0.4, 1.0 );
	Check( !sr.ok, "RG9 the call is REFUSED under External authority" );
	Check( sr.message.find( "insert_geometry_scaffold" ) != std::string::npos &&
	       sr.message.find( "propose_patch" ) != std::string::npos,
	       "RG9 the refusal names the two-call staged route that DOES work" );
	Check( sess->ReadDocument() == docBefore, "RG9 the document is byte-identical (nothing staged, nothing committed)" );

	c.Stop();
	pJob->release();
	std::remove( tmp.c_str() );
}

//! RG10: the JSON-RPC wire shape -- params, the result envelope, and the
//! specific errors a model has to be able to act on.
static void TestReplaceGeometryScaffoldWireShape()
{
	std::printf( "RG10: replace_geometry_scaffold -- JSON-RPC wire shape...\n" );
	const std::string tmp = TempPath( "agentcrud_rg10.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "RG10 fixture loads" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	Agent::AgentRpcDispatcher disp( std::move( sess ) );

	// Missing `target` -> invalid params, naming it.
	{
		const std::string resp = disp.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"replace_geometry_scaffold\",\"params\":"
			"{\"family\":\"sweep_rail\",\"name\":\"w1\",\"size\":1.0,\"detail\":0.4,\"aspect\":1.0}}" );
		Check( resp.find( "\"error\"" ) != std::string::npos, "RG10 a missing `target` is an error" );
		Check( resp.find( "'target'" ) != std::string::npos, "RG10 the error names `target`" );
	}
	// Happy path -> the documented result envelope.
	{
		const std::string resp = disp.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"replace_geometry_scaffold\",\"params\":"
			"{\"target\":\"obj_sph\",\"family\":\"sweep_rail\",\"name\":\"wireA\",\"size\":1.0,"
			"\"detail\":0.4,\"aspect\":1.0}}" );
		Check( resp.find( "\"error\"" ) == std::string::npos, std::string( "RG10 the happy path succeeds: " ) + resp );
		Check( resp.find( "\"target\":\"obj_sph\"" ) != std::string::npos, "RG10 the result echoes `target`" );
		Check( resp.find( "\"geometry\"" ) != std::string::npos, "RG10 the result carries `geometry`" );
		Check( resp.find( "\"previousGeometry\"" ) != std::string::npos, "RG10 the result carries `previousGeometry`" );
		Check( resp.find( "\"removed\":true" ) != std::string::npos,
		       "RG10 previousGeometry.removed is true when the old chunk went" );
		Check( resp.find( "\"results\"" ) != std::string::npos, "RG10 the result carries the per-chunk `results` array" );
	}
	// volume_bank at the wire -> refused with the actionable message.
	{
		const std::string resp = disp.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"replace_geometry_scaffold\",\"params\":"
			"{\"target\":\"obj_sph\",\"family\":\"volume_bank\",\"name\":\"wireB\",\"size\":1.0,"
			"\"detail\":0.4,\"aspect\":2.0,\"tone\":\"0.5 0.5 0.5\"}}" );
		Check( resp.find( "\"error\"" ) != std::string::npos, "RG10 volume_bank is an error at the wire too" );
		Check( resp.find( "insert_geometry_scaffold" ) != std::string::npos,
		       "RG10 the wire refusal still points at insert_geometry_scaffold" );
	}
	// An unknown family -> the family list, NOT a per-family param error.
	{
		const std::string resp = disp.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"replace_geometry_scaffold\",\"params\":"
			"{\"target\":\"obj_sph\",\"family\":\"nope\",\"name\":\"wireC\",\"size\":1.0,\"detail\":0.4}}" );
		Check( resp.find( "unknown family" ) != std::string::npos, "RG10 an unknown family says so" );
	}

	pJob->release();
	std::remove( tmp.c_str() );
}

//----------------------------------------------------------------------
// G2 (2026-08-10, refuse-until-filed cap 2026-08-10 supervisor overrule):
// the PART-PLAN GATE.
//
// See AgentSession.h's block above FileBuildPlan for the mechanism and the
// anti-Goodhart properties.  What these tests pin, in order:
//   G2a  the gate REFUSES on the 1st, 2nd and 3rd geometry-creating
//        insert_chunk on a session that has not filed a plan -- document
//        COMPLETELY untouched every time, refusal names the tool, the
//        enum, that any value is accepted, and the ACCURATE remaining-
//        refusal count -- then GIVES UP on the 4th: that call applies and
//        its own result carries the give-up fact.  A gate clearable by a
//        bare retry without filing would yield zero plans to measure,
//        which is why the original once-per-session design was overruled;
//        the cap exists so a session that genuinely cannot form
//        file_build_plan is not stranded forever
//   G2b  a plan filed FIRST means no interception at all, ever
//   G2c  every triggering verb fires it, each on its own fresh session
//   G2d  non-geometry chunks (painter/material/light) do NOT trigger it
//   G2e  all-`primitive` is a complete, accepted plan (anti-Goodhart)
//   G2f  declare `sweep`, insert a box_geometry -> APPLIES (non-binding;
//        the measurement-critical behaviour -- enforcing would hide
//        compliance-without-competence, which is the finding)
//   G2g  the launch-time disable switch turns it off completely
//   G2h  wire shape: a clean -32602 naming the enum, and the filed-plan
//        result payload
//   G2i  an insert_chunks batch is refused ATOMICALLY -- every element, and
//        the document byte-identical
//----------------------------------------------------------------------

//! Build a session with the build-plan gate ARMED.  main() turns the process
//! default OFF for this whole binary (see its comment), so the gate's own
//! tests turn it back on for exactly the width of the construction call --
//! the session SNAPSHOTS it, so restoring immediately after keeps every
//! other test's opt-out in force and leaves no global set behind.
static std::unique_ptr<Agent::AgentSession> WrapJobGateArmed( Job* pJob )
{
	Agent::AgentSession::SetBuildPlanGateDefaultEnabled( true );
	std::unique_ptr<Agent::AgentSession> s = Agent::AgentSession::WrapJob( pJob );
	Agent::AgentSession::SetBuildPlanGateDefaultEnabled( false );
	return s;
}

//! The two-build plan every "a plan is filed" test uses.  Deliberately NOT
//! all-primitive (G2e covers that separately) and deliberately declaring a
//! construction the test then does not use (G2f).
//! G3a (2026-08-10): every entry now carries the REQUIRED `outline`.  These
//! are real, distinct polygons (not one shared placeholder) so that a filing
//! through this helper exercises the rasterizer on more than one shape, and
//! one entry leaves `view` empty to keep the default path covered wherever
//! this helper is used.
static std::vector<Agent::AgentSession::AgentBuildPlanEntry> SamplePlan()
{
	std::vector<Agent::AgentSession::AgentBuildPlanEntry> p;
	Agent::AgentSession::AgentBuildPlanEntry a;
	a.element = "body";  a.construction = "sweep";
	a.pieces.push_back( "piece" );
	a.outline = "0 0; 2 0.5; 2.4 2; 1 3; -0.4 1.8";
	p.push_back( a );
	Agent::AgentSession::AgentBuildPlanEntry b;
	b.element = "base";  b.construction = "csg";    b.note = "two boxes";
	b.pieces.push_back( "piece" );
	b.outline = "0 0; 3 0; 3 1; 0 1";
	b.view = "side";
	p.push_back( b );
	return p;
}

static const char* const kG2GeometryChunk =
	"box_geometry\n{\n\tname g2_box\n\twidth 1.0\n\theight 1.0\n\tdepth 1.0\n}";

//! G2a (2026-08-10 refuse-until-filed cap, supervisor overrule of the
//! original once-per-session design): the gate REFUSES the 1st, 2nd and 3rd
//! geometry-creating insert_chunk on a session that has not filed a plan --
//! document byte-identical and refusal accurate on every one of them -- then
//! GIVES UP on the 4th, which applies and carries the give-up fact in its own
//! result.  What would go red for each assertion is called out inline.
static void TestBuildPlanGateRefusesUntilFiledCapped()
{
	std::printf( "G2a: the build-plan gate refuses up to 3 times, then gives up on the 4th...\n" );
	const std::string tmp = TempPath( "agentcrud_g2a.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "G2a fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );

	Check( sess->BuildPlanGateEnabled(),   "G2a the session is constructed with the gate ARMED" );
	Check( !sess->BuildPlanFiled(),        "G2a no plan is filed yet" );
	Check( !sess->BuildPlanGateHasFired(), "G2a the gate has not fired yet" );
	Check( sess->BuildPlanGateRefusalCount() == 0, "G2a the refusal counter starts at 0" );
	Check( !sess->BuildPlanGateGaveUp(),    "G2a the gate has not given up yet" );

	const std::string docBefore = sess->ReadDocument();
	const RISE::Cst::CstHeadVersion vBefore = sess->HeadVersion();

	// The 1st, 2nd and 3rd calls: all REFUSED, document/head byte-identical
	// every time, remaining-refusal count accurate on each.  Would go red if
	// the gate disarmed after the first refusal (the old once-per-session
	// behaviour), if any refusal mutated the document, or if the
	// remaining-attempts wording drifted from what CheckBuildPlanGate_
	// actually does.
	static const char* const kExpectRemaining[3] =
	{
		"2 more calls will be refused before this gate stops intercepting",
		"1 more call will be refused before this gate stops intercepting",
		"0 more calls will be refused before this gate stops intercepting",
	};
	for( int attempt = 1; attempt <= 3; ++attempt )
	{
		const std::string p = "G2a attempt " + std::to_string( attempt ) + ": ";
		const Agent::AgentChunkResult r = sess->InsertChunk( kG2GeometryChunk );
		Check( !r.applied,             p + "geometry insert is REFUSED" );
		Check( r.status == "rejected", p + "refusal reports status `rejected`" );
		Check( !r.retriable,
		       p + "`retriable` is FALSE -- that wire flag is the GUI clients' silent "
		       "client-side auto-retry signal (ChatViewModel.swift / ChatPanel.cpp re-dispatch up to "
		       "5 times without showing the model anything), which under the capped semantics would "
		       "burn ALL 3 refusals before the model ever saw one" );
		Check( r.message.find( "file_build_plan" ) != std::string::npos,
		       p + "the refusal NAMES the tool to call" );
		Check( r.message.find( "primitive, csg, sweep, chain, displaced, mesh" ) != std::string::npos,
		       p + "the refusal lists the CLOSED construction enum" );
		Check( r.message.find( "`primitive` for every element is a complete plan" ) != std::string::npos,
		       p + "the refusal states that ANY value is accepted (anti-Goodhart)" );
		// S1 (2026-08-11): the refusal describes schema v3, so it must name
		// `pieces` -- a model that follows this sentence and omits them would
		// get a -32602 for a field the refusal never mentioned.
		Check( r.message.find( "`pieces`" ) != std::string::npos &&
		       r.message.find( "at least one name" ) != std::string::npos,
		       p + "the refusal names the REQUIRED piece list and its minimum" );
		Check( r.message.find( "does not constrain" ) != std::string::npos,
		       p + "the refusal states that the plan is NON-BINDING" );
		Check( r.message.find( kExpectRemaining[attempt - 1] ) != std::string::npos,
		       p + "MONEY ASSERTION: the refusal states the ACCURATE remaining-refusal "
		       "count -- a false count in a model-facing payload is a P1 in this repo" );
		Check( r.kind == "box_geometry" && r.name == "g2_box",
		       p + "the refusal still stamps the IDENTITY echo (kind/name)" );
		Check( sess->ReadDocument() == docBefore,
		       p + "MONEY ASSERTION: the document is COMPLETELY untouched by the refusal" );
		Check( sess->HeadVersion() == vBefore, p + "the head version did not move" );
		Check( sess->BuildPlanGateHasFired(),   p + "the gate is marked as having fired" );
		Check( sess->BuildPlanGateRefusalCount() == attempt,
		       p + "the refusal counter incremented exactly once per refusal" );
		Check( !sess->BuildPlanGateGaveUp(), p + "the gate has not given up yet" );
		Check( !sess->BuildPlanFiled(),      p + "a refusal does NOT count as a filed plan" );
	}

	// The 4th call: the gate GIVES UP.  Would go red if the 4th call were
	// still refused (the cap not honoured), if it silently applied with no
	// trace (the give-up not surfaced in the payload), or if the gate kept
	// intercepting afterward (not PERMANENTLY disarmed).
	const Agent::AgentChunkResult r4 = sess->InsertChunk( kG2GeometryChunk );
	Check( r4.applied,
	       "G2a MONEY ASSERTION: the 4th call SUCCEEDS -- the gate gives up rather than refuse a "
	       "4th time" );
	Check( sess->ReadDocument() != docBefore, "G2a the 4th call really did land the chunk" );
	Check( sess->BuildPlanGateGaveUp(),  "G2a the gate is now marked as having given up" );
	Check( sess->BuildPlanGateRefusalCount() == 3,
	       "G2a the refusal counter stays at 3 -- the give-up is not itself a 4th refusal" );
	Check( r4.message.find( "build-plan gate" ) != std::string::npos &&
	       r4.message.find( "3 refusals" ) != std::string::npos &&
	       r4.message.find( "disarmed for this session" ) != std::string::npos,
	       "G2a MONEY ASSERTION: the successful 4th call's OWN result carries the factual give-up "
	       "notice -- greppable in the payload a trajectory census reads, not only in a log line" );

	// A later geometry call, through a DIFFERENT verb, is not intercepted
	// either -- the give-up is permanent for the rest of the session, not
	// scoped to insert_chunk.
	const Agent::AgentSession::AgentGeometryScaffoldResult gs =
		sess->InsertGeometryScaffold( "sdf_column", "g2a", 1.0, 0.5, 1.0 );
	Check( gs.ok, "G2a a LATER geometry-scaffold call in the same session is not intercepted" );
	Check( gs.message.find( "build-plan gate" ) == std::string::npos,
	       "G2a the give-up notice is reported EXACTLY ONCE -- on the call that tripped it, not on "
	       "every later geometry call" );

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

static void TestBuildPlanFiledFirstNeverIntercepts()
{
	std::printf( "G2b: a plan filed FIRST means no interception at all...\n" );
	const std::string tmp = TempPath( "agentcrud_g2b.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "G2b fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );

	const std::string docBefore = sess->ReadDocument();
	const Agent::AgentSession::AgentBuildPlanResult pr = sess->FileBuildPlan( SamplePlan() );
	Check( pr.ok,                       "G2b the plan is accepted" );
	Check( !pr.replacedPreviousPlan,    "G2b the first filing replaced nothing" );
	Check( pr.elements.size() == 2,        "G2b the result echoes both parts" );
	Check( pr.elements[0].element == "body" && pr.elements[0].construction == "sweep",
	       "G2b the echo is FACTUAL: part name and declared construction, in order" );
	Check( pr.elements[1].note == "two boxes", "G2b the optional per-part note round-trips" );
	Check( pr.message.find( "body: sweep" ) != std::string::npos &&
	       pr.message.find( "base: csg" ) != std::string::npos,
	       "G2b the message echoes each part and its declared construction" );
	Check( sess->ReadDocument() == docBefore,
	       "G2b filing a plan does NOT touch the document" );
	Check( sess->BuildPlanFiled(), "G2b the session now reports a filed plan" );

	Check( sess->InsertChunk( kG2GeometryChunk ).applied,
	       "G2b MONEY ASSERTION: with a plan on file the FIRST geometry insert applies -- no "
	       "interception at all" );
	Check( !sess->BuildPlanGateHasFired(), "G2b the gate never fired" );

	// Re-filing REPLACES and is never refused (an over-refusal on a call that
	// costs the document nothing is the E1 review's P1).
	std::vector<Agent::AgentSession::AgentBuildPlanEntry> p2;
	Agent::AgentSession::AgentBuildPlanEntry e; e.element = "everything"; e.construction = "mesh";
	e.pieces.push_back( "piece" );      // S1: `pieces` is required, >= 1
	e.outline = "0 0; 1 0; 1 1; 0 1";   // G3a: `outline` is required
	p2.push_back( e );
	const Agent::AgentSession::AgentBuildPlanResult pr2 = sess->FileBuildPlan( p2 );
	Check( pr2.ok && pr2.replacedPreviousPlan, "G2b re-filing is accepted and reports the replacement" );
	Check( sess->BuildPlan().size() == 1 && sess->BuildPlan()[0].element == "everything",
	       "G2b the re-filed plan REPLACED the previous one" );

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

//! G2a3 (2026-08-10 refuse-until-filed cap): filing at ANY point -- after 0
//! (covered by G2b above), 1 or 2 refusals -- clears the gate IMMEDIATELY
//! and PERMANENTLY, exactly like filing before ever being refused.  Would go
//! red if a refusal count > 0 changed FileBuildPlan's disarming behaviour, or
//! if the gate kept intercepting after a plan filed mid-refusal-sequence.
static void TestBuildPlanFiledMidRefusalSequenceClearsGate()
{
	std::printf( "G2a3: filing after 1 or 2 refusals clears the gate immediately and permanently...\n" );

	// After exactly ONE refusal.
	{
		const std::string tmp = TempPath( "agentcrud_g2a3_1.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "G2a3/1 fixture loads" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
			Check( !sess->InsertChunk( kG2GeometryChunk ).applied, "G2a3/1 the 1st call is refused" );
			Check( sess->BuildPlanGateRefusalCount() == 1, "G2a3/1 refusal count is 1" );
			Check( sess->FileBuildPlan( SamplePlan() ).ok, "G2a3/1 filing after 1 refusal is accepted" );
			Check( !sess->BuildPlanGateGaveUp(), "G2a3/1 filing is NOT the give-up path" );
			const Agent::AgentChunkResult r = sess->InsertChunk( kG2GeometryChunk );
			Check( r.applied,
			       "G2a3/1 MONEY ASSERTION: the very next call APPLIES -- filing mid-sequence disarms "
			       "the gate immediately, it does not need to reach the cap first" );
			Check( r.message.find( "build-plan gate" ) == std::string::npos,
			       "G2a3/1 no give-up notice: the gate cleared by FILING, not by exhausting the cap" );
			sess.reset(); pJob->release(); std::remove( tmp.c_str() );
		}
	}

	// After exactly TWO refusals.
	{
		const std::string tmp = TempPath( "agentcrud_g2a3_2.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "G2a3/2 fixture loads" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
			Check( !sess->InsertChunk( kG2GeometryChunk ).applied, "G2a3/2 the 1st call is refused" );
			Check( !sess->InsertChunk( kG2GeometryChunk ).applied, "G2a3/2 the 2nd call is refused" );
			Check( sess->BuildPlanGateRefusalCount() == 2, "G2a3/2 refusal count is 2" );
			Check( sess->FileBuildPlan( SamplePlan() ).ok, "G2a3/2 filing after 2 refusals is accepted" );
			Check( !sess->BuildPlanGateGaveUp(), "G2a3/2 filing is NOT the give-up path" );
			const Agent::AgentChunkResult r = sess->InsertChunk( kG2GeometryChunk );
			Check( r.applied,
			       "G2a3/2 MONEY ASSERTION: the very next call APPLIES -- filing mid-sequence disarms "
			       "the gate PERMANENTLY, one refusal short of the cap" );
			Check( r.message.find( "build-plan gate" ) == std::string::npos,
			       "G2a3/2 no give-up notice: the gate cleared by FILING, not by exhausting the cap" );
			sess.reset(); pJob->release(); std::remove( tmp.c_str() );
		}
	}
}

static void TestBuildPlanGateEveryTriggeringVerb()
{
	std::printf( "G2c: every triggering verb fires the gate, each on its own fresh session...\n" );

	// insert_chunk is covered by G2a.  Each verb below gets a FRESH session so
	// one covering for another cannot hide a miss -- the red-prove shape
	// AgentChatLoopTest's mutating-verb sweep uses.

	// insert_chunks (batch) -- ATOMIC refusal: every element, document
	// byte-identical.  This also proves the gate costs a batching model ONE
	// call, not N.
	{
		const std::string tmp = TempPath( "agentcrud_g2c1.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "G2c/insert_chunks fixture loads" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
			const std::string docBefore = sess->ReadDocument();
			const RISE::Cst::CstHeadVersion vBefore = sess->HeadVersion();
			std::vector<std::string> batch;
			batch.push_back( "uniformcolor_painter\n{\n\tname g2c_p\n\tcolor 0.2 0.3 0.4\n}" );
			batch.push_back( kG2GeometryChunk );
			batch.push_back( "uniformcolor_painter\n{\n\tname g2c_q\n\tcolor 0.4 0.3 0.2\n}" );
			const std::vector<Agent::AgentChunkResult> rs = sess->InsertChunks( batch );
			Check( rs.size() == 3, "G2c/insert_chunks returns one result per element" );
			bool allRefused = !rs.empty();
			for( const Agent::AgentChunkResult& e : rs )
				allRefused = allRefused && !e.applied && e.status == "rejected" &&
				             e.message.find( "file_build_plan" ) != std::string::npos;
			Check( allRefused,
			       "G2c/insert_chunks MONEY ASSERTION: ONE geometry element refuses the WHOLE batch, "
			       "every element carrying the same verdict (a policy refusal is not an authoring "
			       "failure -- half a landed batch is a scene the model never asked for)" );
			Check( sess->ReadDocument() == docBefore, "G2c/insert_chunks the document is byte-identical" );
			Check( sess->HeadVersion() == vBefore,    "G2c/insert_chunks the head version did not move" );
			sess.reset(); pJob->release(); std::remove( tmp.c_str() );
		}
	}

	// insert_geometry_scaffold
	{
		const std::string tmp = TempPath( "agentcrud_g2c2.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "G2c/insert_geometry_scaffold fixture loads" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
			const std::string docBefore = sess->ReadDocument();
			const Agent::AgentSession::AgentGeometryScaffoldResult gs =
				sess->InsertGeometryScaffold( "sdf_column", "g2c2", 1.0, 0.5, 1.0 );
			Check( !gs.ok, "G2c/insert_geometry_scaffold is REFUSED" );
			Check( gs.message.find( "file_build_plan" ) != std::string::npos,
			       "G2c/insert_geometry_scaffold the refusal names the tool" );
			Check( gs.chunkResults.empty(),
			       "G2c/insert_geometry_scaffold nothing was submitted through InsertChunks" );
			Check( sess->ReadDocument() == docBefore, "G2c/insert_geometry_scaffold document untouched" );
			// The retry WITHOUT filing is refused AGAIN under the capped
			// refuse-until-filed semantics -- would go red if the gate
			// disarmed after one refusal (the superseded once-per-session
			// behaviour).
			const Agent::AgentSession::AgentGeometryScaffoldResult retry =
				sess->InsertGeometryScaffold( "sdf_column", "g2c2", 1.0, 0.5, 1.0 );
			Check( !retry.ok && retry.message.find( "file_build_plan" ) != std::string::npos,
			       "G2c/insert_geometry_scaffold RED-PROVE: the retry without filing is refused again "
			       "(refusal 2 of 3), not silently let through" );
			Check( sess->BuildPlanGateRefusalCount() == 2,
			       "G2c/insert_geometry_scaffold the counter reflects both refusals" );
			sess.reset(); pJob->release(); std::remove( tmp.c_str() );
		}
	}

	// replace_geometry_scaffold
	{
		const std::string tmp = TempPath( "agentcrud_g2c3.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "G2c/replace_geometry_scaffold fixture loads" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
			const std::string docBefore = sess->ReadDocument();
			const Agent::AgentSession::AgentGeometryScaffoldResult rs =
				sess->ReplaceGeometryScaffold( "obj_sph", "sdf_column", "g2c3", 1.0, 0.5, 1.0 );
			Check( !rs.ok, "G2c/replace_geometry_scaffold is REFUSED" );
			Check( rs.message.find( "file_build_plan" ) != std::string::npos,
			       "G2c/replace_geometry_scaffold the refusal names the tool" );
			Check( sess->ReadDocument() == docBefore, "G2c/replace_geometry_scaffold document untouched" );
			sess.reset(); pJob->release(); std::remove( tmp.c_str() );
		}
	}
}

static void TestBuildPlanGateIgnoresNonGeometry()
{
	std::printf( "G2d: non-geometry inserts do NOT trigger the gate...\n" );
	const std::string tmp = TempPath( "agentcrud_g2d.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "G2d fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );

	Check( sess->InsertChunk( "uniformcolor_painter\n{\n\tname g2d_p\n\tcolor 0.1 0.2 0.3\n}" ).applied,
	       "G2d a painter insert is NOT intercepted" );
	Check( sess->InsertChunk( "lambertian_material\n{\n\tname g2d_m\n\treflectance g2d_p\n}" ).applied,
	       "G2d a material insert is NOT intercepted" );
	Check( sess->InsertChunk( "omni_light\n{\n\tname g2d_l\n\tpower 50.0\n\tposition 2 2 2\n}" ).applied,
	       "G2d a light insert is NOT intercepted" );
	// insert_material_scaffold emits painters + a material and no geometry:
	// it must not burn the gate either.
	Check( sess->InsertMaterialScaffold( "rough_stone", "g2d", "0.5 0.5 0.5", 0.5, 1.0 ).ok,
	       "G2d insert_material_scaffold (painters + material, no geometry) is NOT intercepted" );
	Check( !sess->BuildPlanGateHasFired(),
	       "G2d MONEY ASSERTION: after four non-geometry authoring calls the gate is still ARMED -- "
	       "it triggers on GEOMETRY, not on editing" );

	// ...and the very next geometry insert DOES fire it, proving the negative
	// assertions above are not vacuous.
	const Agent::AgentChunkResult r = sess->InsertChunk( kG2GeometryChunk );
	Check( !r.applied && r.message.find( "file_build_plan" ) != std::string::npos,
	       "G2d RED-PROVE: the next GEOMETRY insert on the same session does fire the gate" );

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

static void TestBuildPlanAnyPlanAcceptedAndNonBinding()
{
	std::printf( "G2e/G2f: all-`primitive` is a complete plan; the plan is NON-BINDING...\n" );

	// G2e: a plan that declares `primitive` for every part is legal and
	// disarms the gate exactly like any other.  ANTI-GOODHART: the gate must
	// never be satisfiable only by declaring richness, or it teaches gaming.
	{
		const std::string tmp = TempPath( "agentcrud_g2e.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "G2e fixture loads" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
			std::vector<Agent::AgentSession::AgentBuildPlanEntry> plan;
			for( int i = 0; i < 4; ++i ) {
				Agent::AgentSession::AgentBuildPlanEntry e;
				e.element = "part" + std::to_string( i );
				e.pieces.push_back( "piece" );
				e.construction = "primitive";
				// G3a: `outline` is required; a distinct triangle per part so
				// the four sketches are not four copies of one shape.
				e.outline = "0 0; " + std::to_string( i + 1 ) + " 0; 0.5 1";
				plan.push_back( e );
			}
			const Agent::AgentSession::AgentBuildPlanResult pr = sess->FileBuildPlan( plan );
			Check( pr.ok, "G2e MONEY ASSERTION: a plan of `primitive` for EVERY part is accepted" );
			Check( pr.message.find( "part0: primitive" ) != std::string::npos,
			       "G2e the echo reports it back factually, with no grading of any kind" );
			Check( sess->InsertChunk( kG2GeometryChunk ).applied,
			       "G2e the all-primitive plan disarms the gate exactly like any other" );
			sess.reset(); pJob->release(); std::remove( tmp.c_str() );
		}
	}

	// G2f: declaring `sweep` and then inserting a box_geometry APPLIES.  This
	// is the measurement-critical behaviour: v1 is deliberately non-binding,
	// because if models declare richly and author primitives anyway, THAT is
	// the finding (compliance without competence) and enforcing would hide it.
	{
		const std::string tmp = TempPath( "agentcrud_g2f.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "G2f fixture loads" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
			std::vector<Agent::AgentSession::AgentBuildPlanEntry> plan;
			Agent::AgentSession::AgentBuildPlanEntry e; e.element = "body"; e.construction = "sweep";
			e.pieces.push_back( "piece" );      // S1: `pieces` is required, >= 1
			e.outline = "0 0; 1 0; 1 2; 0 2";   // G3a: `outline` is required
			plan.push_back( e );
			Check( sess->FileBuildPlan( plan ).ok, "G2f the sweep plan is filed" );
			const Agent::AgentChunkResult r = sess->InsertChunk( kG2GeometryChunk );
			Check( r.applied,
			       "G2f MONEY ASSERTION: `sweep` was declared and a box_geometry was inserted -- and it "
			       "APPLIES.  The declaration is NON-BINDING by design; refusing here would suppress the "
			       "exact signal (declare-rich, author-plain) this slice exists to measure" );
			Check( r.message.find( "file_build_plan" ) == std::string::npos &&
			       r.message.find( "sweep" ) == std::string::npos,
			       "G2f nothing anywhere in the result mentions the mismatch -- no nag, no advice" );
			sess.reset(); pJob->release(); std::remove( tmp.c_str() );
		}
	}
}

static void TestBuildPlanGateDisableSwitch()
{
	std::printf( "G2g: the launch-time disable switch turns the gate off completely...\n" );
	const std::string tmp = TempPath( "agentcrud_g2g.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "G2g fixture loads" );
	if( !pJob ) return;

	// The process default is already false for this binary (main()'s opt-out
	// is the SAME call `--agent-part-plan-gate=off` makes), so this session
	// snapshots a DISARMED gate -- exactly the gate-off measurement arm.
	Check( !Agent::AgentSession::BuildPlanGateDefaultEnabled(),
	       "G2g the process default is off (what --agent-part-plan-gate=off sets)" );
	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	Check( !sess->BuildPlanGateEnabled(), "G2g the session snapshotted the disabled default" );

	Check( sess->InsertChunk( kG2GeometryChunk ).applied,
	       "G2g MONEY ASSERTION: with the gate off, the first geometry insert applies -- no "
	       "interception, no plan needed" );
	Check( sess->InsertGeometryScaffold( "sdf_column", "g2g", 1.0, 0.5, 1.0 ).ok,
	       "G2g insert_geometry_scaffold is not intercepted either" );
	Check( !sess->BuildPlanGateHasFired(), "G2g the gate never fired" );

	// A session's posture is snapshotted at CONSTRUCTION -- flipping the
	// process default mid-session must not re-arm a running session.
	Agent::AgentSession::SetBuildPlanGateDefaultEnabled( true );
	Check( !sess->BuildPlanGateEnabled(),
	       "G2g a mid-session change of the process default does NOT re-arm this session" );
	Check( sess->InsertChunk( "box_geometry\n{\n\tname g2g_box2\n\twidth 1\n\theight 1\n\tdepth 1\n}" ).applied,
	       "G2g and it really is still off in behaviour, not just in the accessor" );
	Agent::AgentSession::SetBuildPlanGateDefaultEnabled( false );

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

//----------------------------------------------------------------------
// G2 fix-round (2026-08-10): the PATCH arm.
//
// Before this round the build-plan gate had exactly FOUR call sites, all
// INSERT verbs, and propose_patch / propose_patches had none -- while a
// param VALUE is spliced into the document as TEXT, so a value carrying
// `}` + a whole `box_geometry { ... }` block + a trailing `#` lands a REAL
// geometry chunk on serialization.  That is the identical VALUE-SPLICE
// bypass R1c's arm (b) exists to close for rasterizer chunks.  A session
// could therefore build a whole scene through propose_patch with no plan
// filed and the refusal counter still reading ZERO -- the instrument
// measuring nothing.  What these pin:
//   G2j  RED-PROOF: the splice patch is REFUSED, document byte-identical,
//        head unbumped, counter incremented
//   G2k  OVER-REFUSAL GUARD: editing an EXISTING geometry chunk's params
//        (a sphere's radius) is NOT a creation and must apply
//   G2l  a splice that introduces a NON-geometry chunk (a painter) is not
//        this gate's business and must apply
//   G2m  propose_patches: PER-ELEMENT, matching what the batch already
//        does for E1 / R1c refusals
//   G2n  ONE session-wide budget: patch refusals and insert refusals draw
//        on the SAME 3, and the 4th call anywhere is the give-up
//   G2o  the STAGED path: a ParamEdit staged while the gate was armed and
//        approved later cannot land geometry the delta now sees
//----------------------------------------------------------------------

//! The VALUE-SPLICE payload, aimed at `standard_object obj_sph`'s `name`.
//!
//! Shape, and why each piece is there: the legal value (`obj_sph`), then
//! the chunk's REMAINING params re-stated so the original chunk closes
//! COMPLETE, then `}`, then the smuggled chunk, then a fresh
//! `standard_object {` header that the original chunk's own trailing
//! `geometry`/`material` lines and closing brace go on to complete.  The
//! result is a document that still LOADS and DERIVES and simply has one
//! more chunk in it than the agent was allowed to create -- which is the
//! whole point: this is not a malformed-input probe, it is a legal edit
//! whose serialized bytes carry geometry.
//!
//! ONE LINE, deliberately: DocSetOrAddParamValue stores a value as
//! whitespace-separated pvalue TOKENS, so any newline in it comes back out
//! of SerializeCst as a space.  The single-line form is therefore what
//! actually lands, and writing it that way keeps the fixture honest about
//! the bytes under test.
//!
//! `name` is ValueKind::String, so the DERIVE layer accepts the whole blob
//! as a name and the edit COMMITS.  (A Double or Color slot rejects the
//! blob before serialization -- which is why R1c's own value-splice test
//! has to drive its gate function directly.  It is exactly that "most
//! splices are caught anyway" reasoning that let this hole stay open:
//! `most` is not a gate.)
static const char* const kG2SpliceGeometryValue =
	"obj_sph geometry sph material mat_diffuse } "
	"box_geometry { name sneaky width 1 height 1 depth 1 } "
	"standard_object { name obj_sph_tail";

//! The same shape, but splicing a PAINTER instead of geometry -- the
//! control that proves G2j's refusal is about the CATEGORY, not about the
//! splice mechanism.
static const char* const kG2SplicePainterValue =
	"obj_sph geometry sph material mat_diffuse } "
	"uniformcolor_painter { name g2_sneaky_p color 0.1 0.2 0.3 } "
	"standard_object { name obj_sph_tail";

#define G2_SPLICE_TARGET "obj_sph", "standard_object", "name"

static Agent::AgentSetPatch MakePatch( const char* target, const char* kind,
                                       const char* param, const char* value )
{
	Agent::AgentSetPatch p;
	p.target = target;
	p.kind   = kind;
	p.param  = param;
	p.value  = value;
	return p;
}

static void TestBuildPlanGatePatchArm()
{
	std::printf( "G2j-G2n: the build-plan gate's PATCH arm (value-splice bypass)...\n" );

	// -- G2j RED-PROOF ------------------------------------------------
	// Pre-fix this assertion set goes RED at the very first Check: the
	// patch APPLIES, a real box_geometry lands, the document changes, the
	// head bumps and BuildPlanGateRefusalCount() stays 0.
	{
		const std::string tmp = TempPath( "agentcrud_g2j.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "G2j fixture loads" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
			const std::string docBefore = sess->ReadDocument();
			const RISE::Cst::CstHeadVersion vBefore = sess->HeadVersion();

			const Agent::AgentPatchResult r = sess->ProposePatch(
				MakePatch( G2_SPLICE_TARGET, kG2SpliceGeometryValue ) );

			Check( !r.applied,
			       "G2j MONEY ASSERTION: a propose_patch whose VALUE splices a whole box_geometry "
			       "chunk into the document is REFUSED by the build-plan gate -- this is the bypass "
			       "the four insert-verb call sites left wide open" );
			Check( r.status == "rejected", "G2j the refusal reports status `rejected`" );
			Check( !r.retriable,
			       "G2j `retriable` is FALSE -- the GUI chat loops silently re-dispatch a retriable "
			       "refusal up to 5 times, which would burn all 3 refusals before the model saw one" );
			Check( r.message.find( "propose_patch refused" ) != std::string::npos,
			       "G2j the refusal names the VERB that was refused" );
			Check( r.message.find( "file_build_plan" ) != std::string::npos,
			       "G2j the refusal names the tool to call -- the SAME text the insert verbs emit" );
			Check( r.message.find( "primitive, csg, sweep, chain, displaced, mesh" ) != std::string::npos,
			       "G2j the refusal lists the CLOSED construction enum" );
			Check( r.message.find( "2 more calls will be refused" ) != std::string::npos,
			       "G2j the refusal states the ACCURATE remaining-refusal count" );
			Check( r.message.find( "`box_geometry`" ) != std::string::npos &&
			       r.message.find( "sneaky" ) != std::string::npos,
			       "G2j the refusal names the geometry chunk the splice WOULD have introduced "
			       "(the echoed name is the spliced chunk's `name` param verbatim -- on a "
			       "single-line splice that param legitimately swallows the rest of the line, and "
			       "echoing it as-authored is the honest report)" );
			Check( sess->ReadDocument() == docBefore,
			       "G2j MONEY ASSERTION: the document is BYTE-IDENTICAL -- no box_geometry landed" );
			Check( sess->HeadVersion() == vBefore, "G2j the head version did not move" );
			Check( sess->ReadDocument().find( "sneaky" ) == std::string::npos,
			       "G2j RED-PROVE (direct): the spliced chunk's name appears nowhere in the document" );
			Check( sess->BuildPlanGateRefusalCount() == 1,
			       "G2j MONEY ASSERTION: the SHARED refusal counter incremented -- before the fix it "
			       "stayed at 0 and the instrument measured nothing" );
			Check( !sess->BuildPlanFiled(), "G2j a refusal does not count as a filed plan" );

			// ...and after filing, the very same patch goes through: the gate
			// is a SEQUENCING check, not a content ban.
			Check( sess->FileBuildPlan( SamplePlan() ).ok, "G2j the plan is filed" );
			const Agent::AgentPatchResult r2 = sess->ProposePatch(
				MakePatch( G2_SPLICE_TARGET, kG2SpliceGeometryValue ) );
			Check( r2.applied,
			       "G2j MONEY ASSERTION: after file_build_plan the SAME patch applies -- the gate "
			       "sequences, it does not forbid" );
			Check( sess->ReadDocument().find( "box_geometry" ) != std::string::npos &&
			       sess->ReadDocument().find( "sneaky" ) != std::string::npos,
			       "G2j and the splice really did create a chunk, so the refusal above was not vacuous" );
			// STRONGEST form of "not vacuous": the engine's OWN canonical
			// parser -- the same ParseToCst every scene load, every gate and
			// every agent read goes through -- sees a REAL top-level
			// box_geometry chunk in the COMMITTED bytes.
			//
			// (A strict file RELOAD of those bytes does fail, because the
			// scene grammar wants chunk braces on their own lines and
			// DocSetOrAddParamValue stores a value as whitespace-separated
			// tokens, so the splice necessarily lands on ONE line.  That is a
			// separate robustness fact and emphatically NOT a defence: the
			// chunk is live in the retained Document, which is what every
			// agent verb reads, what the derive consumed, and what `save`
			// writes.  A gate whose only backstop is "the grammar might reject
			// it later" is not a gate -- the same "most splices are caught
			// anyway" reasoning is what let this hole stay open.)
			{
				const RISE::Cst::Document reparsed = RISE::Cst::ParseToCst( sess->ReadDocument() );
				bool sawBox = false;
				const int nItems = RISE::Cst::DocItemCount( reparsed );
				for( int i = 0; i < nItems && !sawBox; ++i ) {
					const RISE::Cst::NodeRef it =
						RISE::Cst::DocResolveNodeId( reparsed, RISE::Cst::DocNodeIdAt( reparsed, i ) );
					sawBox = it && it->kind == RISE::Cst::NodeKind::Chunk && it->role == "box_geometry";
				}
				Check( sawBox,
				       "G2j MONEY ASSERTION (canonical parse): the committed bytes carry a REAL "
				       "top-level box_geometry chunk -- exactly what the build-plan gate exists to "
				       "intercept, and exactly what the gate's delta detector sees" );
			}
			sess.reset(); pJob->release(); std::remove( tmp.c_str() );
		}
	}

	// -- G2k OVER-REFUSAL GUARD ---------------------------------------
	{
		const std::string tmp = TempPath( "agentcrud_g2k.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "G2k fixture loads" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
			const Agent::AgentPatchResult r = sess->ProposePatch(
				MakePatch( "sph", "sphere_geometry", "radius", "1.25" ) );
			Check( r.applied,
			       "G2k MONEY ASSERTION: editing an EXISTING geometry chunk's param is NOT a creation "
			       "-- the geometry-keyword multiset is unchanged, so the gate must not fire (delta, "
			       "not state: the E1 lesson)" );
			Check( !sess->BuildPlanGateHasFired(), "G2k the gate did not fire" );
			Check( sess->BuildPlanGateRefusalCount() == 0, "G2k no refusal was burned" );
			// A RENAME of an existing geometry chunk is not a creation either.
			const Agent::AgentPatchResult rn = sess->ProposePatch(
				MakePatch( "sph", "sphere_geometry", "name", "sph_renamed" ) );
			Check( !sess->BuildPlanGateHasFired(),
			       "G2k renaming an existing geometry chunk is not a creation either (the keyword "
			       "multiset is unchanged), whatever the derive layer then makes of the dangling "
			       "reference" );
			(void)rn;
			Check( sess->BuildPlanGateRefusalCount() == 0,
			       "G2k MONEY ASSERTION: after two edits that TOUCH geometry the counter is still 0" );
			// ...and the next genuine creation still fires, so the negatives
			// above are not vacuous.
			const Agent::AgentPatchResult rc = sess->ProposePatch(
				MakePatch( G2_SPLICE_TARGET, kG2SpliceGeometryValue ) );
			Check( !rc.applied && rc.message.find( "file_build_plan" ) != std::string::npos,
			       "G2k RED-PROVE: a genuine creation on the same session DOES fire" );
			sess.reset(); pJob->release(); std::remove( tmp.c_str() );
		}
	}

	// -- G2l NON-GEOMETRY SPLICE --------------------------------------
	{
		const std::string tmp = TempPath( "agentcrud_g2l.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "G2l fixture loads" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
			const Agent::AgentPatchResult r = sess->ProposePatch(
				MakePatch( G2_SPLICE_TARGET, kG2SplicePainterValue ) );
			Check( r.applied,
			       "G2l MONEY ASSERTION: the SAME splice mechanism introducing a PAINTER is not this "
			       "gate's business -- G2 triggers on ChunkCategory::Geometry, not on chunk creation" );
			Check( sess->ReadDocument().find( "g2_sneaky_p" ) != std::string::npos,
			       "G2l the painter really was created, so the positive above is not vacuous" );
			Check( !sess->BuildPlanGateHasFired(),
			       "G2l the gate is still ARMED after a non-geometry splice" );
			sess.reset(); pJob->release(); std::remove( tmp.c_str() );
		}
	}

	// -- G2m propose_patches: PER-ELEMENT -----------------------------
	// DECISION (G2 fix-round): the batch refuses PER-ELEMENT, matching what
	// ProposePatches already does for E1 and R1c refusals.  ProposePatches
	// is a thin loop over ProposePatch and is NOT atomic -- only a stale
	// baseHeadVersion on element 0 is batch-fatal (each element is its own
	// commit against the head as it then stands).  insert_chunkS refuses
	// ATOMICALLY because IT is atomic; making the patch batch atomic would
	// mean inventing a rollback the verb has never had.
	{
		const std::string tmp = TempPath( "agentcrud_g2m.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "G2m fixture loads" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
			std::vector<Agent::AgentSetPatch> batch;
			batch.push_back( MakePatch( "sph", "sphere_geometry", "radius", "0.9" ) );
			batch.push_back( MakePatch( G2_SPLICE_TARGET, kG2SpliceGeometryValue ) );
			batch.push_back( MakePatch( "mat_diffuse", "lambertian_material", "reflectance",
			                            "pnt_albedo" ) );
			const std::vector<Agent::AgentPatchResult> rs = sess->ProposePatches( batch, nullptr );
			Check( rs.size() == 3, "G2m one result per element" );
			if( rs.size() == 3 ) {
				Check( rs[0].applied, "G2m element 0 (an innocent geometry param edit) APPLIES" );
				Check( !rs[1].applied && rs[1].message.find( "file_build_plan" ) != std::string::npos,
				       "G2m MONEY ASSERTION: only the geometry-introducing element is refused" );
				Check( rs[2].applied,
				       "G2m element 2 still APPLIES -- per-element, exactly as E1/R1c refusals behave "
				       "in this batch verb" );
			}
			Check( sess->ReadDocument().find( "sneaky" ) == std::string::npos,
			       "G2m the refused element landed nothing" );
			Check( sess->BuildPlanGateRefusalCount() == 1,
			       "G2m the batch burned exactly ONE refusal -- one geometry-introducing element" );
			sess.reset(); pJob->release(); std::remove( tmp.c_str() );
		}
	}

	// -- G2n ONE SHARED BUDGET ----------------------------------------
	{
		const std::string tmp = TempPath( "agentcrud_g2n.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "G2n fixture loads" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );

			// refusal 1 -- through the PATCH arm
			const Agent::AgentPatchResult p1 = sess->ProposePatch(
				MakePatch( G2_SPLICE_TARGET, kG2SpliceGeometryValue ) );
			Check( !p1.applied && sess->BuildPlanGateRefusalCount() == 1, "G2n patch refusal is #1" );
			Check( p1.message.find( "2 more calls will be refused" ) != std::string::npos,
			       "G2n the patch refusal reports 2 remaining" );

			// refusal 2 -- through insert_chunk
			const Agent::AgentChunkResult i2 = sess->InsertChunk( kG2GeometryChunk );
			Check( !i2.applied && sess->BuildPlanGateRefusalCount() == 2,
			       "G2n MONEY ASSERTION: an INSERT refusal draws on the SAME counter the patch "
			       "refusal incremented -- one session-wide budget, not two" );
			Check( i2.message.find( "1 more call will be refused" ) != std::string::npos,
			       "G2n the insert refusal's remaining count continues the patch refusal's sequence" );

			// refusal 3 -- through insert_geometry_scaffold
			const Agent::AgentSession::AgentGeometryScaffoldResult g3 =
				sess->InsertGeometryScaffold( "sdf_column", "g2n", 1.0, 0.5, 1.0 );
			Check( !g3.ok && sess->BuildPlanGateRefusalCount() == 3, "G2n scaffold refusal is #3" );
			Check( !sess->BuildPlanGateGaveUp(), "G2n the gate has not given up yet" );

			// the 4th -- back through the PATCH arm -- is the GIVE-UP
			const Agent::AgentPatchResult p4 = sess->ProposePatch(
				MakePatch( G2_SPLICE_TARGET, kG2SpliceGeometryValue ) );
			Check( p4.applied,
			       "G2n MONEY ASSERTION: the 4th interception -- arriving through the PATCH arm -- is "
			       "the shared give-up, not a 4th refusal" );
			Check( sess->BuildPlanGateGaveUp(), "G2n the gate is now permanently disarmed" );
			Check( sess->BuildPlanGateRefusalCount() == 3, "G2n the counter stays at 3" );
			Check( p4.message.find( "build-plan gate" ) != std::string::npos &&
			       p4.message.find( "3 refusals" ) != std::string::npos &&
			       p4.message.find( "disarmed for this session" ) != std::string::npos,
			       "G2n MONEY ASSERTION: the give-up notice is folded into the PATCH result too -- "
			       "greppable in the payload a trajectory census reads" );
			Check( sess->ReadDocument().find( "sneaky" ) != std::string::npos,
			       "G2n the give-up call really did apply" );
			sess.reset(); pJob->release(); std::remove( tmp.c_str() );
		}
	}
}

//! G2o (fix-round 2026-08-10): the STAGED / resolve path.
//!
//! The insert verbs gate BEFORE their External staging branch and the two
//! disarm flags are permanent-once-set, so the ordinary direction is closed
//! by construction.  The residual this pins is the one HEAD-DEPENDENT
//! window a ParamEdit has: a patch whose target does not resolve at stage
//! time (so the delta sees nothing to refuse) can become geometry-
//! introducing once someone creates that target, and the Owner then
//! approves it.  ResolveProposal re-runs the same stateless delta.
static void TestBuildPlanGateStagedResolve()
{
	std::printf( "G2o: the build-plan gate's staged/resolve re-check...\n" );
	const std::string tmp = TempPath( "agentcrud_g2o.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "G2o fixture loads" );
	if( !pJob ) return;

	TestController c( *pJob, /*simulatedRenderMs*/ 0 );
	c.Start();
	std::unique_ptr<Agent::AgentSession> owner =
		Agent::AgentSession::WrapJob( pJob, Agent::AgentAuthority::Owner );
	owner->AttachController( &c );

	// (o1) STAGE TIME: an External session's geometry-introducing patch is
	// refused BEFORE it can reach the queue -- the gate sits ahead of the
	// authority branching, exactly as the insert verbs' arms do.
	{
		// WrapJobGateArmed wraps as Owner, so arm the default by hand for
		// exactly the width of this External construction (same idiom).
		Agent::AgentSession::SetBuildPlanGateDefaultEnabled( true );
		std::unique_ptr<Agent::AgentSession> ext =
			Agent::AgentSession::WrapJob( pJob, Agent::AgentAuthority::External );
		Agent::AgentSession::SetBuildPlanGateDefaultEnabled( false );
		ext->AttachController( &c );
		const Agent::AgentPatchResult r = ext->ProposePatch(
			MakePatch( G2_SPLICE_TARGET, kG2SpliceGeometryValue ) );
		Check( !r.applied && r.status == "rejected",
		       "G2o(o1) MONEY ASSERTION: an External geometry-introducing patch is refused BEFORE "
		       "staging -- the G2 arm sits ahead of the authority branch" );
		Check( r.message.find( "file_build_plan" ) != std::string::npos,
		       "G2o(o1) the refusal is the build-plan gate's, not the staging refusal" );
		Check( owner->ListProposals().empty(), "G2o(o1) and NOTHING was enqueued" );
	}

	// (o2) RESOLVE TIME: stage directly on the controller with the
	// armed-at-stage flag set (the shape a proposal has when it staged while
	// innocent -- e.g. its target did not resolve then), then approve it once
	// the delta DOES see geometry.  The re-check must refuse.
	{
		SceneEditController::AgentProposal p;
		p.kind       = SceneEditController::AgentProposalKind::ParamEdit;
		p.target     = String( "obj_sph" );
		p.entityKind = String( "standard_object" );
		p.param      = String( "name" );
		p.value      = String( kG2SpliceGeometryValue );
		p.hasExplicitBaseVersion   = false;
		p.buildPlanGateArmedAtStage = true;
		RISE::Cst::CstHeadVersion stagedHead{};
		const std::uint64_t id = c.StageProposal( p, &stagedHead );
		Check( id != 0, "G2o(o2) the innocent-at-stage proposal reaches the queue" );

		const std::string headBefore = owner->ReadDocument();
		const Agent::AgentSession::AgentResolveResult rr = owner->ResolveProposal( id, /*approve=*/true );
		Check( rr.ok, "G2o(o2) resolve runs (the id is found)" );
		Check( rr.status == "rejected",
		       "G2o(o2) MONEY ASSERTION: approving a staged ParamEdit that would NOW introduce "
		       "geometry is REFUSED at resolve time" );
		Check( rr.message.find( "resolve refused" ) != std::string::npos,
		       "G2o(o2) the message carries the resolve-refusal marker" );
		Check( rr.message.find( "file_build_plan" ) != std::string::npos &&
		       rr.message.find( "`box_geometry`" ) != std::string::npos,
		       "G2o(o2) the refusal names the remedy and the chunk it would have introduced" );
		Check( owner->ReadDocument() == headBefore,
		       "G2o(o2) MONEY ASSERTION: the live document never received the spliced geometry" );
	}

	// (o3) the SAME proposal shape with the flag CLEAR -- the ordinary case,
	// a proposal staged by a session whose gate was already disarmed -- must
	// APPLY.  Without this the re-check would be a blanket ban on geometry-
	// creating patches through the staged path.
	{
		SceneEditController::AgentProposal p;
		p.kind       = SceneEditController::AgentProposalKind::ParamEdit;
		p.target     = String( "obj_sph" );
		p.entityKind = String( "standard_object" );
		p.param      = String( "name" );
		p.value      = String( kG2SpliceGeometryValue );
		p.hasExplicitBaseVersion   = false;
		p.buildPlanGateArmedAtStage = false;
		RISE::Cst::CstHeadVersion stagedHead{};
		const std::uint64_t id = c.StageProposal( p, &stagedHead );
		Check( id != 0, "G2o(o3) the disarmed-at-stage proposal reaches the queue" );
		const Agent::AgentSession::AgentResolveResult rr = owner->ResolveProposal( id, /*approve=*/true );
		Check( rr.ok && rr.status == "applied",
		       "G2o(o3) MONEY ASSERTION: with the gate disarmed at stage time the SAME proposal "
		       "APPLIES -- the re-check is conditional, not a blanket ban" );
		Check( owner->ReadDocument().find( "sneaky" ) != std::string::npos,
		       "G2o(o3) and it really landed the chunk, so (o2)'s refusal was not vacuous" );
	}

	owner.reset();
	c.Stop();
	pJob->release();
	std::remove( tmp.c_str() );
}

static void TestBuildPlanWireShape()
{
	std::printf( "G2h: file_build_plan wire shape + param validation through the LIVE dispatcher...\n" );
	const std::string tmp = TempPath( "agentcrud_g2h.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "G2h fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
	Agent::AgentRpcDispatcher rpc( std::move( sess ) );

	// -32602, naming the enum, for every malformed shape.
	{
		const std::string resp = rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"file_build_plan\",\"params\":{}}" );
		Check( resp.find( "-32602" ) != std::string::npos, "G2h a missing 'parts' is -32602" );
		Check( resp.find( "primitive, csg, sweep, chain, displaced, mesh" ) != std::string::npos,
		       "G2h that error NAMES the accepted enum" );
	}
	{
		const std::string resp = rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"file_build_plan\",\"params\":{\"elements\":[]}}" );
		Check( resp.find( "-32602" ) != std::string::npos, "G2h an EMPTY 'elements' array is -32602" );
	}
	{
		const std::string resp = rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"file_build_plan\",\"params\":"
			"{\"elements\":[{\"element\":\"wing\",\"pieces\":[\"p\"],\"outline\":\"0 0; 1 0; 1 1; 0 1\"}]}}" );
		Check( resp.find( "-32602" ) != std::string::npos, "G2h a MISSING 'construction' is -32602" );
		Check( resp.find( "elements[0].construction" ) != std::string::npos,
		       "G2h that error names the offending INDEX and field" );
		Check( resp.find( "primitive, csg, sweep, chain, displaced, mesh" ) != std::string::npos,
		       "G2h and the accepted enum" );
	}
	{
		const std::string resp = rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"file_build_plan\",\"params\":"
			"{\"elements\":[{\"element\":\"wing\",\"pieces\":[\"p\"],\"construction\":\"sweep\","
			"\"outline\":\"0 0; 1 0; 1 1; 0 1\"},"
			"{\"element\":\"tail\",\"pieces\":[\"p\"],\"construction\":\"lathe\","
			"\"outline\":\"0 0; 1 0; 0.5 1\"}]}}" );
		Check( resp.find( "-32602" ) != std::string::npos, "G2h a construction OUTSIDE the enum is -32602" );
		Check( resp.find( "elements[1].construction" ) != std::string::npos &&
		       resp.find( "`lathe`" ) != std::string::npos,
		       "G2h that error names the index AND echoes the rejected value" );
		Check( resp.find( "primitive, csg, sweep, chain, displaced, mesh" ) != std::string::npos,
		       "G2h and the accepted enum" );
	}
	{
		const std::string resp = rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"file_build_plan\",\"params\":"
			"{\"elements\":[{\"element\":\"\",\"construction\":\"mesh\"}]}}" );
		Check( resp.find( "-32602" ) != std::string::npos && resp.find( "elements[0].element" ) != std::string::npos,
		       "G2h an EMPTY element name is -32602 naming the field" );
	}

	// A refused plan must NOT have disarmed the gate: the intercepted insert
	// still fires.  (A malformed filing that silently counted would be the
	// worst of both -- the model gets no plan and no gate.)
	{
		const std::string resp = rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"insert_chunk\",\"params\":{\"chunkText\":"
			"\"box_geometry\\n{\\n\\tname g2h_box\\n\\twidth 1\\n\\theight 1\\n\\tdepth 1\\n}\"}}" );
		Check( resp.find( "file_build_plan" ) != std::string::npos,
		       "G2h RED-PROVE: after five REFUSED filings the gate is still armed and fires" );
	}

	// The success envelope.
	{
		const std::string resp = rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"file_build_plan\",\"params\":"
			"{\"elements\":[{\"element\":\"wing\",\"pieces\":[\"p\"],\"construction\":\"sweep\",\"note\":\"membrane\","
			"\"outline\":\"0 0; 4 1; 3 2; -1 1.4\",\"view\":\"top\"},"
			"{\"element\":\"body\",\"pieces\":[\"p\"],\"construction\":\"primitive\","
			"\"outline\":\"0 0; 1 0; 1 1; 0 1\"}]}}" );
		Agent::JsonValue result;
		Check( JsonResultObj( resp, result ), "G2h file_build_plan returns a JSON-RPC result object" );
		Check( result.get( "filed" ).isBool() && result.get( "filed" ).asBool(), "G2h `filed` is true" );
		Check( !result.get( "replacedPreviousPlan" ).asBool( true ),
		       "G2h `replacedPreviousPlan` is false on the first filing" );
		Check( result.get( "elementCount" ).asNumber( -1 ) == 2.0, "G2h `elementCount` is 2" );
		Check( result.get( "elements" ).isArray() && result.get( "elements" ).size() == 2,
		       "G2h `elements` echoes one entry per declared element" );
		Check( result.get( "elements" ).at( 0 ).get( "element" ).asString() == "wing" &&
		       result.get( "elements" ).at( 0 ).get( "construction" ).asString() == "sweep" &&
		       result.get( "elements" ).at( 0 ).get( "note" ).asString() == "membrane",
		       "G2h each entry carries {element,pieces,construction,note}, in the order declared" );
		Check( result.get( "elements" ).at( 1 ).get( "note" ).asString().empty(),
		       "G2h an omitted note echoes as an empty string, never absent" );
		Check( result.get( "message" ).asString().find( "wing: sweep" ) != std::string::npos,
		       "G2h `message` is a factual echo of the plan" );
		Check( !result.has( "headVersion" ),
		       "G2h there is NO headVersion -- the call does not touch the document" );
	}

	// And now the same insert lands.
	{
		const std::string resp = rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"insert_chunk\",\"params\":{\"chunkText\":"
			"\"box_geometry\\n{\\n\\tname g2h_box\\n\\twidth 1\\n\\theight 1\\n\\tdepth 1\\n}\"}}" );
		Agent::JsonValue result;
		Check( JsonResultObj( resp, result ), "G2h the post-plan insert returns a result" );
		Check( result.get( "applied" ).asBool(),
		       "G2h MONEY ASSERTION (wire): after file_build_plan the SAME insert applies" );
	}

	pJob->release();
	std::remove( tmp.c_str() );
}

//----------------------------------------------------------------------
// G3a (2026-08-10): the SKETCH ARTIFACT -- build-plan schema v2.
//
// What these pin, in order:
//   G3a-a  a valid filing rasterizes one target per part: point count, area
//          fraction and aspect are the RIGHT numbers for known shapes (a
//          square fills ~0.85^2 of the canvas, a triangle ~half that), the
//          composite PNG really decodes at the tiled dimensions, and `view`
//          defaults to front
//   G3a-b  DETERMINISM: the same outline in two independent sessions gives
//          byte-identical masks AND byte-identical PNG bytes.  Without this
//          every downstream comparison (G3b's IoU) is unreproducible
//   G3a-c  every schema defect is a clean -32602 naming the part INDEX, and
//          NONE of them touches the gate's refusal counter (the G2h
//          invariant, extended to the outline-specific cases)
//   G3a-d  a self-intersecting BOWTIE is ACCEPTED and fills as TWO LOBES
//          under the even-odd rule -- rejecting it would refuse a legal
//          imagination, and filling it solid would be the wrong rule
//   G3a-e  a wide outline LETTERBOXES: the mask's own bbox carries the
//          authored aspect to within a pixel
//   G3a-f  re-filing REPLACES the target set completely -- a part dropped
//          from the plan leaves no stale sketch
//   G3a-g  the refusal text names the new fields and every claim it makes
//          is true of the schema the dispatcher actually enforces
//----------------------------------------------------------------------

//! A filed sketch's mask bbox, in pixels (empty mask -> w==h==0).
struct SketchMaskBBox { int x0 = 0, y0 = 0, x1 = -1, y1 = -1; int w = 0, h = 0; };

static SketchMaskBBox MaskBBox( const RISE::Agent::AgentSession::AgentElementSketch& s )
{
	SketchMaskBBox b;
	const int N = RISE::Agent::AgentSession::kElementSketchCanvas;
	bool any = false;
	for( int y = 0; y < N; ++y ) {
		for( int x = 0; x < N; ++x ) {
			if( !s.mask[ static_cast<std::size_t>( y ) * N + x ] ) continue;
			if( !any ) { b.x0 = b.x1 = x; b.y0 = b.y1 = y; any = true; }
			if( x < b.x0 ) b.x0 = x;
			if( x > b.x1 ) b.x1 = x;
			if( y < b.y0 ) b.y0 = y;
			if( y > b.y1 ) b.y1 = y;
		}
	}
	if( any ) { b.w = b.x1 - b.x0 + 1; b.h = b.y1 - b.y0 + 1; }
	return b;
}

//! Filled pixel count on one mask row.
static int MaskRowFilled( const RISE::Agent::AgentSession::AgentElementSketch& s, int row )
{
	const int N = RISE::Agent::AgentSession::kElementSketchCanvas;
	int n = 0;
	for( int x = 0; x < N; ++x )
		if( s.mask[ static_cast<std::size_t>( row ) * N + x ] ) ++n;
	return n;
}

static Agent::AgentSession::AgentBuildPlanEntry PlanEntry( const char* part, const char* cons,
                                                          const char* outline, const char* view = "" )
{
	Agent::AgentSession::AgentBuildPlanEntry e;
	e.element = part; e.construction = cons; e.outline = outline; e.view = view;
	e.pieces.push_back( "piece" );
	return e;
}

//! G3a-a / G3a-d / G3a-e: the rasterizer's numbers, on shapes whose answers
//! are known in closed form.
static void TestElementSketchRasterizer()
{
	std::printf( "G3a-a: the sketch rasterizer -- area/aspect/point facts, composite PNG, even-odd fill...\n" );
	const std::string tmp = TempPath( "agentcrud_g3a_a.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "G3a-a fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );

	std::vector<Agent::AgentSession::AgentBuildPlanEntry> plan;
	// 0: a unit SQUARE -- fills the fitted box entirely, so its area fraction
	//    must be kElementSketchFillFraction^2 (0.7225) to within pixel
	//    quantization.  This is the single most load-bearing number in the
	//    whole rasterizer: it pins the FIT, the CENTERING and the fill rule at
	//    once, and it is the constant G3b's comparison is calibrated against.
	plan.push_back( PlanEntry( "square", "primitive", "0 0; 1 0; 1 1; 0 1" ) );
	// 1: a TRIANGLE on the same bbox -- exactly HALF the square's area.
	plan.push_back( PlanEntry( "triangle", "csg", "0 0; 1 0; 0.5 1", "side" ) );
	// 2: a BOWTIE (self-intersecting) -- accepted, and even-odd fills it as
	//    two lobes meeting at a point, NOT as a solid quad.
	plan.push_back( PlanEntry( "bowtie", "chain", "0 0; 1 0; 0 1; 1 1" ) );
	// 3: a 4:1 WIDE rectangle -- letterboxed, aspect preserved.
	plan.push_back( PlanEntry( "wide", "sweep", "0 0; 4 0; 4 1; 0 1", "top" ) );

	const Agent::AgentSession::AgentBuildPlanResult pr = sess->FileBuildPlan( plan );
	Check( pr.ok, "G3a-a the four-build plan with outlines is accepted" );
	Check( pr.sketches.size() == 4, "G3a-a one target per part" );
	Check( sess->ElementSketches().size() == 4,
	       "G3a-a and the session HOLDS them as session-lifetime state" );
	if( pr.sketches.size() != 4 ) { sess.reset(); pJob->release(); std::remove( tmp.c_str() ); return; }

	const double fill2 = Agent::AgentSession::kElementSketchFillFraction *
	                     Agent::AgentSession::kElementSketchFillFraction;

	// --- 0: square -------------------------------------------------------
	{
		const Agent::AgentSession::AgentElementSketch& s = pr.sketches[0];
		Check( s.element == "square" && s.pointCount == 4, "G3a-a square: 4 points" );
		Check( s.view == "front",
		       "G3a-a MONEY ASSERTION: an omitted `view` resolves to front, never to empty" );
		Check( std::fabs( s.areaFraction - fill2 ) < 0.01,
		       "G3a-a MONEY ASSERTION: a square outline fills kElementSketchFillFraction^2 (~0.7225) of "
		       "the canvas -- pins the fit, the centering and the fill rule at once (got " +
		       std::to_string( s.areaFraction ) + ")" );
		Check( std::fabs( s.aspect - 1.0 ) < 1e-9, "G3a-a square: aspect 1.0" );
		const SketchMaskBBox b = MaskBBox( s );
		Check( std::abs( b.w - b.h ) <= 1, "G3a-a square: the mask bbox is square to within a pixel" );
	}
	// --- 1: triangle -----------------------------------------------------
	{
		const Agent::AgentSession::AgentElementSketch& s = pr.sketches[1];
		Check( s.pointCount == 3, "G3a-a triangle: 3 points" );
		Check( s.view == "side", "G3a-a triangle: an explicit `view` is recorded verbatim" );
		Check( std::fabs( s.areaFraction - fill2 * 0.5 ) < 0.01,
		       "G3a-a MONEY ASSERTION: a triangle on the same bbox fills HALF the square's area "
		       "(~0.361) -- the fill is a real scanline, not a bbox stamp (got " +
		       std::to_string( s.areaFraction ) + ")" );
	}
	// --- 2: bowtie (G3a-d) -----------------------------------------------
	{
		const Agent::AgentSession::AgentElementSketch& s = pr.sketches[2];
		Check( s.pointCount == 4, "G3a-d bowtie: accepted, 4 points -- self-intersection is NOT an error" );
		// Two triangles of half-height each: total area is half the bbox,
		// i.e. the same fraction as the triangle above.
		Check( std::fabs( s.areaFraction - fill2 * 0.5 ) < 0.01,
		       "G3a-d bowtie: even-odd gives the two lobes' combined area (~0.361), got " +
		       std::to_string( s.areaFraction ) );
		// The two-lobe SHAPE, which the area alone cannot distinguish from a
		// single triangle: wide near the top and bottom of the content, empty
		// at the waist.  Content spans rows ~19..236 (fill 0.85 of 256).
		Check( MaskRowFilled( s, 30 ) >= 150,
		       "G3a-d bowtie: the upper lobe is wide near the top of the content" );
		Check( MaskRowFilled( s, 226 ) >= 150,
		       "G3a-d bowtie: the lower lobe is wide near the bottom" );
		Check( MaskRowFilled( s, 128 ) <= 4,
		       "G3a-d MONEY ASSERTION: the WAIST is empty -- even-odd produced TWO LOBES, not the "
		       "solid quad a nonzero-winding fill would have given" );
	}
	// --- 3: wide (G3a-e) -------------------------------------------------
	{
		const Agent::AgentSession::AgentElementSketch& s = pr.sketches[3];
		Check( std::fabs( s.aspect - 4.0 ) < 1e-9, "G3a-e wide: the reported aspect is the authored 4.0" );
		Check( s.view == "top", "G3a-e wide: view top" );
		const SketchMaskBBox b = MaskBBox( s );
		const double edge   = static_cast<double>( Agent::AgentSession::kElementSketchCanvas );
		const double fitted = Agent::AgentSession::kElementSketchFillFraction * edge;   // 217.6
		Check( std::fabs( static_cast<double>( b.w ) - fitted ) <= 1.0,
		       "G3a-e wide: the LONG axis is fitted to kElementSketchFillFraction of the canvas" );
		Check( std::fabs( static_cast<double>( b.h ) - fitted / 4.0 ) <= 1.0,
		       "G3a-e MONEY ASSERTION: the short axis is LETTERBOXED to the authored 4:1 aspect, to "
		       "within a pixel -- a stretch-to-fit would have made it square (got h=" +
		       std::to_string( b.h ) + ")" );
		Check( b.y0 > 1 && b.y1 < Agent::AgentSession::kElementSketchCanvas - 2,
		       "G3a-e wide: and it is CENTERED -- margin above and below" );
	}

	// --- the composite PNG -----------------------------------------------
	Check( !pr.compositePng.empty(), "G3a-a the filing returns a composite PNG" );
	Check( pr.compositeWidth == 4u * static_cast<unsigned int>( Agent::AgentSession::kElementSketchCanvas ) &&
	       pr.compositeHeight == static_cast<unsigned int>( Agent::AgentSession::kElementSketchCanvas ),
	       "G3a-a four sketches tile into ONE row of four (1024x256)" );
	{
		DecodedLuma d;
		Check( DecodeRenderLuma( pr.compositePng, d ),
		       "G3a-a MONEY ASSERTION: the composite really DECODES through RISE's own PNG reader -- "
		       "not merely non-empty bytes" );
		Check( d.w == pr.compositeWidth && d.h == pr.compositeHeight,
		       "G3a-a and it decodes at exactly the reported dimensions" );
	}

	// --- the message: facts only -----------------------------------------
	Check( pr.message.find( "Sketches, in order: square, triangle, bowtie, wide" ) != std::string::npos,
	       "G3a-a the message states the tiling ORDER, so a model can map tile N to part N" );
	Check( pr.message.find( "4 points" ) != std::string::npos &&
	       pr.message.find( "area " ) != std::string::npos &&
	       pr.message.find( "aspect " ) != std::string::npos,
	       "G3a-a and echoes the per-part facts" );
	Check( pr.message.find( "consider" ) == std::string::npos &&
	       pr.message.find( "simple" ) == std::string::npos &&
	       pr.message.find( "detailed" ) == std::string::npos &&
	       pr.message.find( "should" ) == std::string::npos,
	       "G3a-a MEASUREMENT HYGIENE: the echo carries no advice and no value language about the "
	       "sketches -- this is the instrument, and a word like `simple` here would steer the very "
	       "distribution the census is about to read" );

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

//! G3a-b: byte-level determinism across independent sessions.
static void TestElementSketchDeterminism()
{
	std::printf( "G3a-b: the same outline rasterizes byte-identically in two fresh sessions...\n" );
	const char* const kOutline = "0.13 -2.5; 3.7 0.25; 2.05 4.9; -1.4 3.33; -2.2 0.1";

	std::vector<unsigned char> mask[2];
	std::vector<unsigned char> png[2];
	for( int trial = 0; trial < 2; ++trial ) {
		const std::string name = "agentcrud_g3a_b" + std::to_string( trial ) + ".RISEscene";
		const std::string tmp = TempPath( name.c_str() );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "G3a-b fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
		std::vector<Agent::AgentSession::AgentBuildPlanEntry> plan;
		plan.push_back( PlanEntry( "blob", "displaced", kOutline ) );
		const Agent::AgentSession::AgentBuildPlanResult pr = sess->FileBuildPlan( plan );
		Check( pr.ok && pr.sketches.size() == 1, "G3a-b the filing succeeds" );
		if( pr.sketches.size() == 1 ) { mask[trial] = pr.sketches[0].mask; png[trial] = pr.compositePng; }
		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}
	Check( !mask[0].empty() && mask[0] == mask[1],
	       "G3a-b MONEY ASSERTION: the mask bytes are IDENTICAL across two independent sessions -- "
	       "without this every IoU G3b reports is unreproducible" );
	Check( !png[0].empty() && png[0] == png[1],
	       "G3a-b and so are the encoded composite PNG bytes" );
}

//! G3a-c: every outline/view defect is a clean -32602 naming the index, and
//! NONE of them burns a gate refusal.
static void TestElementSketchWireRejections()
{
	std::printf( "G3a-c: outline/view -32602 shapes, and the gate counter stays untouched...\n" );
	const std::string tmp = TempPath( "agentcrud_g3a_c.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "G3a-c fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sessOwned = WrapJobGateArmed( pJob );
	Agent::AgentSession* sess = sessOwned.get();
	Agent::AgentRpcDispatcher rpc( std::move( sessOwned ) );

	struct Case { const char* params; const char* mustSay; const char* what; };
	static const Case kCases[] = {
		{ "{\"elements\":[{\"element\":\"wing\",\"pieces\":[\"p\"],\"construction\":\"sweep\"}]}",
		  "elements[0].outline", "a MISSING outline" },
		{ "{\"elements\":[{\"element\":\"a\",\"pieces\":[\"p\"],\"construction\":\"csg\",\"outline\":\"0 0; 1 0; 1 1; 0 1\"},"
		  "{\"element\":\"b\",\"pieces\":[\"p\"],\"construction\":\"csg\",\"outline\":\"0 0; 1 1\"}]}",
		  "elements[1].outline", "an outline with only 2 points" },
		{ "{\"elements\":[{\"element\":\"a\",\"pieces\":[\"p\"],\"construction\":\"csg\",\"outline\":\"0 0; nan 1; 1 1\"}]}",
		  "elements[0].outline", "a NON-FINITE coordinate" },
		{ "{\"elements\":[{\"element\":\"a\",\"pieces\":[\"p\"],\"construction\":\"csg\",\"outline\":\"0 0; 1 0; 2 0; 3 0\"}]}",
		  // G3a fix-round (2026-08-10): this is AXIS-DEGENERATE (every point
		  // shares y=0), not "collinear" -- a diagonal line of points (e.g.
		  // "0 0; 1 1; 2 2") is also collinear but has extent on both axes,
		  // so ParseElementOutlinePoints_ deliberately ACCEPTS it and it
		  // degrades gracefully to a near-empty mask.
		  "elements[0].outline", "an AXIS-DEGENERATE outline (zero-area bounding box)" },
		// G3a fix-round (2026-08-10) FIX 1: a bbox extent that is POSITIVE
		// (passes the zero-area check above) but SUBNORMAL overflows
		// RasterizeElementOutline_'s fit scale to +inf, turning every device
		// coordinate to NaN and the mask silently empty -- reported as an
		// honest-looking areaFraction 0.00 if it were ever let through.
		{ "{\"elements\":[{\"element\":\"a\",\"pieces\":[\"p\"],\"construction\":\"csg\",\"outline\":\"0 0; 1e-320 0; 0 1e-320\"}]}",
		  "elements[0].outline", "a SUBNORMAL-extent outline (bbox extent overflows the fit scale)" },
		{ "{\"elements\":[{\"element\":\"a\",\"pieces\":[\"p\"],\"construction\":\"csg\",\"outline\":\"0 0; 1; 1 1\"}]}",
		  "elements[0].outline", "a point that is not two numbers" },
		{ "{\"elements\":[{\"element\":\"a\",\"pieces\":[\"p\"],\"construction\":\"csg\",\"outline\":\"0 0; 1 0; 1 1\","
		  "\"view\":\"isometric\"}]}",
		  "elements[0].view", "a `view` outside the closed enum" },
	};
	int id = 100;
	for( const Case& c : kCases ) {
		const std::string resp = rpc.HandleLine(
			std::string( "{\"jsonrpc\":\"2.0\",\"id\":" ) + std::to_string( id++ ) +
			",\"method\":\"file_build_plan\",\"params\":" + c.params + "}" );
		const std::string p = std::string( "G3a-c " ) + c.what + ": ";
		Check( resp.find( "-32602" ) != std::string::npos, p + "is a clean -32602" );
		Check( resp.find( c.mustSay ) != std::string::npos,
		       p + "and the error names the offending part INDEX and field (" + c.mustSay + ")" );
		Check( !sess->BuildPlanFiled(), p + "no plan was recorded" );
		Check( sess->ElementSketches().empty(), p + "and no target was recorded" );
	}
	// The two RESOURCE BOUNDS, both wire-reachable and both -32602.  Unlike
	// every G2 field these grow with what the caller sends (one 64 KB mask
	// per part; a scanline fill per edge), so without them one call is an
	// unbounded allocation.  No honest plan comes near either.
	{
		std::string many = "{\"elements\":[";
		for( int i = 0; i < 65; ++i ) {
			if( i ) many += ",";
			many += "{\"element\":\"p" + std::to_string( i ) +
				"\",\"pieces\":[\"p\"],\"construction\":\"primitive\",\"outline\":\"0 0; 1 0; 1 1\"}";
		}
		many += "]}";
		const std::string resp = rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":150,\"method\":\"file_build_plan\",\"params\":" + many + "}" );
		Check( resp.find( "-32602" ) != std::string::npos && resp.find( "at most 64" ) != std::string::npos,
		       "G3a-c a 65-build plan is a -32602 naming the 64-part cap" );
		Check( !sess->BuildPlanFiled(), "G3a-c and nothing was recorded" );
	}
	{
		std::string pts;
		for( int i = 0; i < 513; ++i ) {
			if( i ) pts += "; ";
			pts += std::to_string( i ) + " " + std::to_string( ( i * 7 ) % 13 );
		}
		const std::string resp = rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":151,\"method\":\"file_build_plan\",\"params\":"
			"{\"elements\":[{\"element\":\"a\",\"pieces\":[\"p\"],\"construction\":\"csg\",\"outline\":\"" + pts + "\"}]}}" );
		Check( resp.find( "-32602" ) != std::string::npos &&
		       resp.find( "elements[0].outline" ) != std::string::npos &&
		       resp.find( "at most 512" ) != std::string::npos,
		       "G3a-c a 513-point outline is a -32602 naming the 512-point cap" );
		Check( !sess->BuildPlanFiled(), "G3a-c and nothing was recorded" );
	}

	Check( sess->BuildPlanGateRefusalCount() == 0 && !sess->BuildPlanGateHasFired(),
	       "G3a-c MONEY ASSERTION: nine rejected filings burned ZERO gate refusals -- a schema error "
	       "is not a gate interception, and the design's bounded-escape argument depends on it (G2h "
	       "pins the same property for the G2 fields)" );

	// RED-PROVE the pair: the gate is still armed and still fires.
	{
		const std::string resp = rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":200,\"method\":\"insert_chunk\",\"params\":{\"chunkText\":"
			"\"box_geometry\\n{\\n\\tname g3ac_box\\n\\twidth 1\\n\\theight 1\\n\\tdepth 1\\n}\"}}" );
		Check( resp.find( "file_build_plan" ) != std::string::npos,
		       "G3a-c RED-PROVE: after nine REFUSED filings the gate is still armed and fires" );
		Check( sess->BuildPlanGateRefusalCount() == 1,
		       "G3a-c and THAT interception is refusal #1 -- the counter starts here, not at 10" );
	}

	// A well-formed filing on the same session clears it, and carries an image.
	{
		const std::string resp = rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":201,\"method\":\"file_build_plan\",\"params\":"
			"{\"elements\":[{\"element\":\"body\",\"pieces\":[\"p\"],\"construction\":\"primitive\","
			"\"outline\":\"0 0; 2 0; 2 1; 0 1\"}]}}" );
		Agent::JsonValue result;
		Check( JsonResultObj( resp, result ), "G3a-c the corrected filing returns a result object" );
		Check( result.get( "filed" ).asBool( false ), "G3a-c and it files" );
		Check( result.get( "elements" ).at( 0 ).get( "view" ).asString() == "front",
		       "G3a-c the echoed `view` defaults to front on the wire too" );
		Check( result.get( "elements" ).at( 0 ).get( "pointCount" ).asNumber( -1 ) == 4.0,
		       "G3a-c the echoed pointCount is the parsed vertex count" );
		Check( result.get( "elements" ).at( 0 ).get( "outline" ).asString() == "0 0; 2 0; 2 1; 0 1",
		       "G3a-c and the outline comes back VERBATIM as authored" );
		Check( std::fabs( result.get( "elements" ).at( 0 ).get( "aspect" ).asNumber( -1 ) - 2.0 ) < 1e-9,
		       "G3a-c and the aspect fact is the authored 2:1" );
		Check( !result.get( "png_base64" ).asString().empty() &&
		       result.get( "compositeWidth" ).asNumber( -1 ) ==
		           static_cast<double>( Agent::AgentSession::kElementSketchCanvas ),
		       "G3a-c the composite rides the wire under the SAME png_base64 field name every other "
		       "image-bearing verb uses" );
		Check( result.get( "byteLength" ).asNumber( -1 ) > 0.0, "G3a-c with its byte length" );
	}
	{
		const std::string resp = rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":202,\"method\":\"insert_chunk\",\"params\":{\"chunkText\":"
			"\"box_geometry\\n{\\n\\tname g3ac_box\\n\\twidth 1\\n\\theight 1\\n\\tdepth 1\\n}\"}}" );
		Agent::JsonValue result;
		Check( JsonResultObj( resp, result ) && result.get( "applied" ).asBool(),
		       "G3a-c MONEY ASSERTION: refusal -> file (with outlines) -> proceed still works end to end" );
	}

	pJob->release();
	std::remove( tmp.c_str() );
}

//! G3a fix-round (2026-08-10) FIX 1: the new subnormal-extent floor
//! (kSketchMinExtent_ == 1e-9) rejects only the pathological case -- a
//! bounding-box extent that is small in absolute terms but nowhere near
//! subnormal must still rasterize NORMALLY.  If a future edit moves the
//! floor up carelessly (e.g. to "fix" this by rejecting anything under 1),
//! this goes red before any legitimately small, honestly-authored
//! silhouette does.
static void TestElementSketchSmallButSaneExtentStillRasterizes()
{
	std::printf( "G3a fix-round: a small-but-sane 0.001 extent still rasterizes normally...\n" );
	const std::string tmp = TempPath( "agentcrud_g3a_fix1.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "fix1 fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );

	std::vector<Agent::AgentSession::AgentBuildPlanEntry> plan;
	plan.push_back( PlanEntry( "tiny", "primitive", "0 0; 0.001 0; 0.001 0.001; 0 0.001" ) );
	const Agent::AgentSession::AgentBuildPlanResult pr = sess->FileBuildPlan( plan );
	Check( pr.ok, "fix1 a 0.001-unit square outline is ACCEPTED (extent is 1e6 times the 1e-9 floor)" );
	if( !pr.ok || pr.sketches.empty() ) { sess.reset(); pJob->release(); std::remove( tmp.c_str() ); return; }

	const Agent::AgentSession::AgentElementSketch& s = pr.sketches[0];
	const double fill2 = Agent::AgentSession::kElementSketchFillFraction *
	                     Agent::AgentSession::kElementSketchFillFraction;
	Check( std::fabs( s.areaFraction - fill2 ) < 0.01,
	       "fix1 MONEY ASSERTION: it rasterizes to the SAME ~0.7225 fill fraction as the unit square in "
	       "TestElementSketchRasterizer -- the fit is scale-invariant, so a small outline is not a degraded "
	       "outline (got " + std::to_string( s.areaFraction ) + ")" );
	Check( std::isfinite( s.aspect ) && std::fabs( s.aspect - 1.0 ) < 1e-6,
	       "fix1 and the aspect fact is finite and correct (1.0), not NaN/inf from an overflowed scale" );
	const SketchMaskBBox b = MaskBBox( s );
	Check( b.w > 0 && b.h > 0, "fix1 the mask actually has filled pixels, not the silently-empty mask "
	       "the pre-fix overflow would have produced" );

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

//! G3a fix-round (2026-08-10) FIX 3: composite tiling paths past the
//! previously-tested <= 4 sketches (single row), plus the ACCEPT side of
//! the two resource-cap fences whose REJECT side (65 parts, 513 points) is
//! already covered in TestElementSketchWireRejections.  Every fixture here is
//! built programmatically -- no pasted kilobyte outline lists.
static void TestElementSketchCompositeTilingAndAcceptBoundaries()
{
	std::printf( "G3a fix-round: composite multi-row tiling, truncation, and 64-part/512-point "
	             "accept boundaries...\n" );

	// --- 6 parts: composite wraps to a SECOND row ---------------------
	{
		const std::string tmp = TempPath( "agentcrud_g3a_fix3_6.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "fix3-6 fixture loads" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
			std::vector<Agent::AgentSession::AgentBuildPlanEntry> plan;
			for( int i = 0; i < 6; ++i )
				plan.push_back( PlanEntry( ( "p" + std::to_string( i ) ).c_str(), "primitive",
				                           "0 0; 1 0; 0.5 1" ) );
			const Agent::AgentSession::AgentBuildPlanResult pr = sess->FileBuildPlan( plan );
			Check( pr.ok, "fix3-6 the six-build plan is accepted" );
			Check( pr.compositeWidth == 4u * static_cast<unsigned int>( Agent::AgentSession::kElementSketchCanvas ) &&
			       pr.compositeHeight == 2u * static_cast<unsigned int>( Agent::AgentSession::kElementSketchCanvas ),
			       "fix3-6 MONEY ASSERTION: six sketches wrap to TWO rows of four -- 1024x512, not "
			       "1536x256 (a single wide row) or 1024x1024 (four rows)" );
			Check( pr.message.find( "Sketches, in order: p0, p1, p2, p3, p4, p5" ) != std::string::npos,
			       "fix3-6 the message states the tiling order, and it matches FILING order" );
			sess.reset();
			pJob->release();
		}
		std::remove( tmp.c_str() );
	}

	// --- 17 parts: composite draws exactly 16 tiles, truncation stated -
	{
		const std::string tmp = TempPath( "agentcrud_g3a_fix3_17.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "fix3-17 fixture loads" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
			std::vector<Agent::AgentSession::AgentBuildPlanEntry> plan;
			for( int i = 0; i < 17; ++i )
				plan.push_back( PlanEntry( ( "p" + std::to_string( i ) ).c_str(), "primitive",
				                           "0 0; 1 0; 0.5 1" ) );
			const Agent::AgentSession::AgentBuildPlanResult pr = sess->FileBuildPlan( plan );
			Check( pr.ok, "fix3-17 the seventeen-build plan is accepted" );
			Check( pr.compositeWidth == 4u * static_cast<unsigned int>( Agent::AgentSession::kElementSketchCanvas ) &&
			       pr.compositeHeight == 4u * static_cast<unsigned int>( Agent::AgentSession::kElementSketchCanvas ),
			       "fix3-17 MONEY ASSERTION: exactly 16 tiles are drawn (four full rows of four) -- "
			       "1024x1024, not a 17th partial row" );
			Check( pr.message.find( "first 16 of 17 sketches" ) != std::string::npos,
			       "fix3-17 the message carries the factual truncation note with correct arithmetic" );
			Check( sess->ElementSketches().size() == 17,
			       "fix3-17 MONEY ASSERTION: ALL 17 targets are still stored -- the composite's tile "
			       "cap bounds only the IMAGE, never what is recorded" );
			bool allFacts = true;
			for( int i = 0; i < 17; ++i )
				if( pr.message.find( "p" + std::to_string( i ) + ": primitive" ) == std::string::npos )
					allFacts = false;
			Check( allFacts,
			       "fix3-17 and facts are present for EVERY part, including the 17th that never got a tile" );
			sess.reset();
			pJob->release();
		}
		std::remove( tmp.c_str() );
	}

	// --- exactly 64 parts: the cap boundary, ACCEPTED -------------------
	{
		const std::string tmp = TempPath( "agentcrud_g3a_fix3_64.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "fix3-64 fixture loads" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
			std::vector<Agent::AgentSession::AgentBuildPlanEntry> plan;
			for( std::size_t i = 0; i < Agent::AgentSession::kBuildPlanMaxElements; ++i )
				plan.push_back( PlanEntry( ( "p" + std::to_string( i ) ).c_str(), "primitive",
				                           "0 0; 1 0; 0.5 1" ) );
			const Agent::AgentSession::AgentBuildPlanResult pr = sess->FileBuildPlan( plan );
			Check( pr.ok && pr.sketches.size() == Agent::AgentSession::kBuildPlanMaxElements,
			       "fix3-64 MONEY ASSERTION: EXACTLY 64 parts is accepted (the cap itself, not past "
			       "it -- 65 is already covered as a rejection in TestElementSketchWireRejections)" );
			sess.reset();
			pJob->release();
		}
		std::remove( tmp.c_str() );
	}

	// --- exactly 512 points: the cap boundary, ACCEPTED -----------------
	{
		const std::string tmp = TempPath( "agentcrud_g3a_fix3_512.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "fix3-512 fixture loads" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
			std::string pts;
			for( std::size_t i = 0; i < Agent::AgentSession::kElementOutlineMaxPoints; ++i ) {
				if( i ) pts += "; ";
				pts += std::to_string( i ) + " " + std::to_string( ( i * 7 ) % 13 );
			}
			std::vector<Agent::AgentSession::AgentBuildPlanEntry> plan;
			plan.push_back( PlanEntry( "many", "csg", pts.c_str() ) );
			const Agent::AgentSession::AgentBuildPlanResult pr = sess->FileBuildPlan( plan );
			Check( pr.ok && pr.sketches.size() == 1 &&
			       pr.sketches[0].pointCount == Agent::AgentSession::kElementOutlineMaxPoints,
			       "fix3-512 MONEY ASSERTION: EXACTLY 512 points is accepted (the cap itself, not "
			       "past it -- 513 is already covered as a rejection in TestElementSketchWireRejections)" );
			sess.reset();
			pJob->release();
		}
		std::remove( tmp.c_str() );
	}
}

//! G3a-f: re-filing REPLACES the whole target set.
static void TestElementSketchReplaceSemantics()
{
	std::printf( "G3a-f: re-filing replaces the target set completely...\n" );
	const std::string tmp = TempPath( "agentcrud_g3a_f.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "G3a-f fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );

	std::vector<Agent::AgentSession::AgentBuildPlanEntry> first;
	first.push_back( PlanEntry( "wing", "sweep",  "0 0; 3 1; 2 2; -1 1" ) );
	first.push_back( PlanEntry( "tail", "chain",  "0 0; 1 0; 0.5 2" ) );
	Check( sess->FileBuildPlan( first ).ok, "G3a-f the first plan files" );
	Check( sess->ElementSketches().size() == 2, "G3a-f two targets" );

	std::vector<Agent::AgentSession::AgentBuildPlanEntry> second;
	second.push_back( PlanEntry( "hull", "csg", "0 0; 5 0; 5 2; 0 2" ) );
	const Agent::AgentSession::AgentBuildPlanResult pr2 = sess->FileBuildPlan( second );
	Check( pr2.ok && pr2.replacedPreviousPlan, "G3a-f the re-filing is accepted and reports the replace" );
	Check( sess->ElementSketches().size() == 1 && sess->ElementSketches()[0].element == "hull",
	       "G3a-f MONEY ASSERTION: the target set is REPLACED wholesale -- `wing` and `tail` are gone, "
	       "so a part dropped from the plan can never leave a stale sketch behind for G3b to compare "
	       "against" );
	Check( pr2.compositeWidth == static_cast<unsigned int>( Agent::AgentSession::kElementSketchCanvas ),
	       "G3a-f and the composite is re-tiled for the NEW set (one tile, not three)" );
	Check( pr2.message.find( "every sketch filed with it" ) != std::string::npos,
	       "G3a-f the echo states the replacement covered the sketches, not only the plan" );

	// A rejected re-filing changes NOTHING -- all-or-nothing, and it must not
	// be a back door to clearing targets.
	std::vector<Agent::AgentSession::AgentBuildPlanEntry> bad;
	bad.push_back( PlanEntry( "ok",  "primitive", "0 0; 1 0; 1 1" ) );
	bad.push_back( PlanEntry( "bad", "primitive", "0 0; 1 0" ) );
	const Agent::AgentSession::AgentBuildPlanResult pr3 = sess->FileBuildPlan( bad );
	Check( !pr3.ok && pr3.message.find( "elements[1].outline" ) != std::string::npos,
	       "G3a-f a defect in part 1 rejects the WHOLE filing, naming the index" );
	Check( sess->ElementSketches().size() == 1 && sess->ElementSketches()[0].element == "hull" &&
	       sess->BuildPlan().size() == 1,
	       "G3a-f MONEY ASSERTION: the rejected filing left the previous plan AND targets untouched -- "
	       "no half-applied plan, no cleared targets" );

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

//! G3a-g: the refusal text names the new required fields, and every claim it
//! makes is TRUE of the schema the dispatcher enforces.  The second half is
//! the point: a refusal that told a model to do something the dispatcher then
//! -32602s is a P1 in this repo, so the test does not merely grep for words --
//! it FILES the exact example the refusal prints.
static void TestElementSketchRefusalText()
{
	std::printf( "G3a-g: the refusal names the outline/view schema and every claim in it is true...\n" );
	const std::string tmp = TempPath( "agentcrud_g3a_g.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "G3a-g fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );

	const Agent::AgentChunkResult r = sess->InsertChunk( kG2GeometryChunk );
	Check( !r.applied, "G3a-g the first geometry insert is refused" );
	Check( r.message.find( "`outline`" ) != std::string::npos,
	       "G3a-g the refusal NAMES the outline field -- without it a model files a G2-shaped plan "
	       "and gets a -32602 it was never warned about" );
	Check( r.message.find( "at least 3" ) != std::string::npos,
	       "G3a-g and states the 3-point minimum" );
	Check( r.message.find( "0 0; 1 0; 1 2; 0 2" ) != std::string::npos,
	       "G3a-g and prints a concrete example" );
	Check( r.message.find( "`view`" ) != std::string::npos &&
	       r.message.find( "front, side, top" ) != std::string::npos &&
	       r.message.find( "default front" ) != std::string::npos,
	       "G3a-g and describes the optional `view` with its default" );
	Check( r.message.find( "primitive, csg, sweep, chain, displaced, mesh" ) != std::string::npos &&
	       r.message.find( "`primitive` for every element is a complete plan" ) != std::string::npos &&
	       r.message.find( "does not constrain" ) != std::string::npos,
	       "G3a-g the G2 claims all survive the rewrite" );
	Check( r.message.find( "2 more calls will be refused" ) != std::string::npos,
	       "G3a-g and so does the accurate remaining-refusal count" );

	// EVERY CLAIM TRUE: file exactly what the refusal describes -- the printed
	// example outline, no `view` (it said view is optional) -- and it must be
	// accepted.  This is the assertion that catches a refusal drifting away
	// from the dispatcher's real schema.
	std::vector<Agent::AgentSession::AgentBuildPlanEntry> plan;
	plan.push_back( PlanEntry( "body", "primitive", "0 0; 1 0; 1 2; 0 2" ) );
	const Agent::AgentSession::AgentBuildPlanResult pr = sess->FileBuildPlan( plan );
	Check( pr.ok,
	       "G3a-g MONEY ASSERTION: a plan built by following the refusal LITERALLY -- its own example "
	       "outline, `primitive`, no view -- is accepted" );
	Check( pr.sketches.size() == 1 && pr.sketches[0].view == "front",
	       "G3a-g and the omitted view really does default to front, as the refusal claims" );
	Check( sess->InsertChunk( kG2GeometryChunk ).applied,
	       "G3a-g and the gate really does clear, as the refusal claims" );

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

//======================================================================
// Arc 77 Phase 2 (2026-08-11): imagine_scene + the two-condition gate.
//
// WHAT THESE PIN, and why each one is a P1 if it breaks:
//   IM-a  CAPABILITY.  A provider with no image generation answers with a
//         plain statement, holds no target, and -- crucially -- leaves the
//         gate as the shipped PLAN-ONLY gate.  A regression here would turn
//         every non-image provider's session into an unclearable gate.
//   IM-b  SUCCESS + REPLACE.  A capable provider's image becomes THE scene
//         target; imagining again replaces it wholesale.
//   IM-c  ANTI-STRANDING.  A PROVIDER failure disarms the imagine
//         requirement for the session, so the gate then clears on the plan
//         alone.  This is the rule that stops a network blip from bricking
//         a run; nothing else in the design substitutes for it.
//   IM-d  THE TWO-CONDITION GATE.  On a capable provider the refusal names
//         BOTH outstanding requirements, then only the one still missing,
//         and clears when both are met -- sharing ONE counter and ONE cap
//         with the plan half, whose value-splice arm and counter semantics
//         are untouched.
//   IM-e  THE CAP STILL BOUNDS IT.  Two conditions, still 3 refusals, still
//         a give-up on the 4th -- and the give-up notice names what was
//         actually missing rather than asserting "without a filed plan".
//   IM-f  A -32602 DISARMS NOTHING.  A mis-shaped call must not be a way to
//         switch the mechanism off; only a PROVIDER failure disarms.
//======================================================================

//! Mint a real, decodable PNG of a known size without adding a PNG encoder
//! to this test: file a `parts`-entry plan and take its composite sketch
//! echo, which is exactly kElementSketchCanvas x kElementSketchCanvas per tile.
//! One part -> 256x256, two parts -> 512x256, so a test can tell two canned
//! images apart by their dimensions alone.
static std::vector<unsigned char> MintCannedPng( Job* pJob, int partCount )
{
	std::unique_ptr<Agent::AgentSession> s = Agent::AgentSession::WrapJob( pJob );
	if( !s ) return std::vector<unsigned char>();
	std::vector<Agent::AgentSession::AgentBuildPlanEntry> parts;
	for( int i = 0; i < partCount; ++i ) {
		Agent::AgentSession::AgentBuildPlanEntry e;
		e.element         = "canned" + std::to_string( i );
		e.pieces.push_back( "piece" );
		e.construction = "primitive";
		e.outline      = "0 0; 1 0; 1 1; 0 1";
		parts.push_back( e );
	}
	return s->FileBuildPlan( parts ).compositePng;
}

//! A fake, host-installed generator -- the same seam AgentEvalRunner fills
//! from the live transport, so these tests drive the identical session code
//! a real provider does, with no network and no key.
static Agent::AgentSession::AgentImageGenerator MakeFakeImageGen(
	const std::vector<unsigned char>& png, int* callCount = nullptr )
{
	Agent::AgentSession::AgentImageGenerator g;
	g.supported    = true;
	g.providerName = "gemini";
	g.modelId      = "test-image-model";
	g.generate = [png, callCount]( const std::string& ) {
		Agent::AgentSession::AgentImageGenOutcome o;
		if( callCount ) ++*callCount;
		o.ok       = true;
		o.bytes    = png;
		o.mimeType = "image/png";
		return o;
	};
	return g;
}

static Agent::AgentSession::AgentImageGenerator MakeFailingImageGen( const char* why )
{
	Agent::AgentSession::AgentImageGenerator g;
	g.supported    = true;
	g.providerName = "gemini";
	g.modelId      = "test-image-model";
	const std::string reason = why;
	g.generate = [reason]( const std::string& ) {
		Agent::AgentSession::AgentImageGenOutcome o;
		o.ok    = false;
		o.error = reason;
		return o;
	};
	return g;
}

//! A geometry chunk with a CALLER-CHOSEN name.  The sub-blocks below share
//! one Job (loading a scene per block would triple these tests' cost), so a
//! block that actually LANDS a chunk must not collide with the next block's
//! insert -- a name collision would be rejected by the Job for a reason that
//! has nothing to do with the gate under test, and read as a gate failure.
static std::string ImagineBoxChunk( const char* name )
{
	return std::string( "box_geometry\n{\n\tname " ) + name +
		"\n\twidth 1.0\n\theight 1.0\n\tdepth 1.0\n}";
}

//! IM-a: no capability -> honest refusal, no target, and the gate is EXACTLY
//! today's plan-only gate.
static void TestImagineCapabilityRefusal()
{
	std::printf( "IM-a: an incapable provider refuses honestly and leaves the plan-only gate...\n" );
	const std::string tmp = TempPath( "agentcrud_im_a.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "IM-a fixture loads" );
	if( !pJob ) return;

	// (1) No generator installed at all -- every EXISTING construction site.
	{
		std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
		Check( !sess->ImagineCapable(), "IM-a a session with no host-installed generator is not capable" );
		const Agent::AgentSession::AgentImagineResult r = sess->ImagineScene( "a quiet courtyard" );
		Check( !r.ok, "IM-a ImagineScene refuses" );
		Check( r.capabilityRefusal, "IM-a and reports it as a CAPABILITY refusal" );
		Check( r.message.find( "does not generate images" ) != std::string::npos,
		       "IM-a the message says plainly what is missing" );
		Check( !sess->HasSceneTarget(), "IM-a no target is held" );
		Check( !sess->ImagineRequirementDisarmed(),
		       "IM-a MONEY ASSERTION: a capability refusal does NOT set the provider-failure disarm "
		       "-- the imagine half was never armed here, so claiming a disarm would be a false "
		       "statement about the session's own state" );

		// The gate is the shipped plan-only gate, word for word.
		const Agent::AgentChunkResult r1 = sess->InsertChunk( kG2GeometryChunk );
		Check( !r1.applied, "IM-a the geometry insert is still refused (the plan half)" );
		Check( r1.message.find( "no build plan has been filed for this session." ) != std::string::npos,
		       "IM-a MONEY ASSERTION: the refusal is the plan-only sentence -- an incapable provider "
		       "sees byte-identical behaviour to before this slice" );
		Check( r1.message.find( "imagine_scene" ) == std::string::npos,
		       "IM-a and it does NOT name imagine_scene, which cannot help here" );
		Check( sess->FileBuildPlan( SamplePlan() ).ok, "IM-a the plan files" );
		Check( sess->InsertChunk( kG2GeometryChunk ).applied,
		       "IM-a MONEY ASSERTION: the plan ALONE clears the gate on an incapable provider" );
	}

	// (2) A generator installed but explicitly not supported (the shape the
	//     host builds for anthropic/xai/local).
	{
		std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
		Agent::AgentSession::AgentImageGenerator g;
		g.supported    = false;
		g.providerName = "anthropic";
		sess->SetImageGenerator( g );
		Check( !sess->ImagineCapable(), "IM-a a supported=false generator is not capable" );
		const Agent::AgentSession::AgentImagineResult r = sess->ImagineScene( "anything" );
		Check( !r.ok && r.capabilityRefusal &&
		       r.message.find( "anthropic" ) != std::string::npos,
		       "IM-a the refusal NAMES the provider that cannot do it" );
	}

	std::remove( tmp.c_str() );
	pJob->release();
}

//! IM-b: a capable provider's image becomes the target; re-imagining
//! replaces it.
static void TestImagineSuccessAndReplace()
{
	std::printf( "IM-b: a generated image becomes the scene target, and re-imagining replaces it...\n" );
	const std::string tmp = TempPath( "agentcrud_im_b.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "IM-b fixture loads" );
	if( !pJob ) return;

	const std::vector<unsigned char> png1 = MintCannedPng( pJob, 1 );   // 256x256
	const std::vector<unsigned char> png2 = MintCannedPng( pJob, 2 );   // 512x256
	Check( !png1.empty() && !png2.empty(), "IM-b two distinguishable canned PNGs were minted" );

	std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
	int calls = 0;
	sess->SetImageGenerator( MakeFakeImageGen( png1, &calls ) );
	Check( sess->ImagineCapable(), "IM-b the session reports the installed capability" );

	const std::string docBefore = sess->ReadDocument();
	const Agent::AgentSession::AgentImagineResult r1 =
		sess->ImagineScene( "a cold blue dawn over a stone bridge" );
	Check( r1.ok, "IM-b the imagine succeeds" );
	Check( calls == 1, "IM-b the host generator was called exactly once" );
	Check( !r1.replacedPreviousTarget, "IM-b the first imagine replaced nothing" );
	Check( r1.width == 256 && r1.height == 256, "IM-b the target's dims are the generated image's" );
	Check( r1.png.size() >= 8 && r1.png[0] == 0x89 && r1.png[1] == 'P' && r1.png[2] == 'N' &&
	       r1.png[3] == 'G',
	       "IM-b the returned image is a real PNG -- the model SEES its own imagination" );
	Check( sess->HasSceneTarget() && sess->SceneTarget() != nullptr,
	       "IM-b the session now holds a scene target" );
	Check( sess->SceneTarget()->description == "a cold blue dawn over a stone bridge",
	       "IM-b MONEY ASSERTION: the target carries the model's OWN words verbatim -- the "
	       "description IS the imagining act" );
	Check( sess->SceneTarget()->rgb.size() ==
	       static_cast<std::size_t>( sess->SceneTarget()->width ) * sess->SceneTarget()->height * 3,
	       "IM-b and a decoded RGB representation the comparison can use" );
	Check( sess->ReadDocument() == docBefore,
	       "IM-b MONEY ASSERTION: the Document is BYTE-IDENTICAL -- the target is session state and "
	       "is never written into the scene" );
	Check( r1.message.find( "gemini/test-image-model" ) != std::string::npos,
	       "IM-b the echo names the provider and model that produced it" );

	const std::shared_ptr<const Agent::AgentSession::AgentSceneTarget> firstTarget = sess->SceneTarget();

	sess->SetImageGenerator( MakeFakeImageGen( png2 ) );
	const Agent::AgentSession::AgentImagineResult r2 = sess->ImagineScene( "warmer, at noon" );
	Check( r2.ok && r2.replacedPreviousTarget, "IM-b the second imagine REPLACES the target" );
	Check( sess->SceneTarget()->width == 512 && sess->SceneTarget()->height == 256,
	       "IM-b and the held target is now the NEW image (512x256, not 256x256)" );
	Check( firstTarget && firstTarget->width == 256,
	       "IM-b MONEY ASSERTION: a snapshot taken before the replace still describes the OLD "
	       "target -- publication replaces the pointer and never mutates a published pointee, "
	       "which is what makes an in-flight async render's snapshot safe" );

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

//! IM-c: a PROVIDER failure disarms the imagine requirement -- the
//! anti-stranding rule.
static void TestImagineProviderFailureDisarms()
{
	std::printf( "IM-c: a provider failure disarms the imagine requirement (anti-stranding)...\n" );
	const std::string tmp = TempPath( "agentcrud_im_c.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "IM-c fixture loads" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
	sess->SetImageGenerator( MakeFailingImageGen( "the image provider answered HTTP 503" ) );

	const Agent::AgentSession::AgentImagineResult r = sess->ImagineScene( "anything at all" );
	Check( !r.ok, "IM-c the imagine fails" );
	Check( !r.capabilityRefusal, "IM-c and it is NOT reported as a capability refusal" );
	Check( r.requirementDisarmed && sess->ImagineRequirementDisarmed(),
	       "IM-c MONEY ASSERTION: a PROVIDER failure DISARMS the imagine requirement -- a network "
	       "blip must never strand a session" );
	Check( r.message.find( "the image provider answered HTTP 503" ) != std::string::npos,
	       "IM-c the transport's header-free category is reported verbatim" );
	Check( r.message.find( "dropped for this session" ) != std::string::npos,
	       "IM-c and the disarm is stated as a fact in the result, not left silent" );
	Check( !sess->HasSceneTarget(), "IM-c no target was created" );

	// The gate now clears on the plan ALONE.
	const Agent::AgentChunkResult refused = sess->InsertChunk( kG2GeometryChunk );
	Check( !refused.applied, "IM-c the plan half still gates" );
	Check( refused.message.find( "imagine_scene" ) == std::string::npos,
	       "IM-c and the refusal no longer names imagine_scene -- the requirement is gone" );
	Check( sess->FileBuildPlan( SamplePlan() ).ok, "IM-c the plan files" );
	Check( sess->InsertChunk( kG2GeometryChunk ).applied,
	       "IM-c MONEY ASSERTION: with the requirement disarmed the gate clears on the plan alone" );

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

//! IM-d + IM-e: the two-condition gate, its refusal texts, its shared
//! counter and its unchanged cap.
//----------------------------------------------------------------------
// IM-g (Phase 2 review round, P2-2): the per-session spend cap.  Exactly
// kSceneImagineMaxPerSession calls reach the generator; the next is
// refused with the factual cap message, the generator is NOT invoked
// again, and the target from the last successful call is retained.
// Cap-hit can never strand the gate -- by construction every capped
// session already settled the imagine half (target exists, or the first
// provider failure disarmed it); asserted here via HasSceneTarget.
//----------------------------------------------------------------------
static void TestImaginePerSessionSpendCap()
{
	std::printf( "IM-g: the per-session image-generation spend cap refuses call %d factually...\n",
	             Agent::AgentSession::kSceneImagineMaxPerSession + 1 );
	const std::string tmp = TempPath( "agentcrud_im_g.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "IM-g fixture loads" );
	if( !pJob ) return;

	const std::vector<unsigned char> png1 = MintCannedPng( pJob, 1 );
	std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
	int calls = 0;
	sess->SetImageGenerator( MakeFakeImageGen( png1, &calls ) );

	for( int i = 0; i < Agent::AgentSession::kSceneImagineMaxPerSession; ++i ) {
		const Agent::AgentSession::AgentImagineResult r = sess->ImagineScene( "a scene" );
		Check( r.ok, "IM-g every call up to the cap succeeds" );
	}
	Check( calls == Agent::AgentSession::kSceneImagineMaxPerSession,
	       "IM-g the generator ran exactly kSceneImagineMaxPerSession times" );

	const Agent::AgentSession::AgentImagineResult over = sess->ImagineScene( "one more" );
	Check( !over.ok, "IM-g MONEY ASSERTION: the call past the cap is refused" );
	Check( calls == Agent::AgentSession::kSceneImagineMaxPerSession,
	       "IM-g MONEY ASSERTION: the generator was NOT invoked for the refused call -- no spend" );
	Check( over.message.find( "per-session image-generation cap" ) != std::string::npos,
	       "IM-g the refusal states the cap factually" );
	Check( sess->HasSceneTarget(),
	       "IM-g the previously generated target is retained -- the cap never strands the gate" );
	pJob->release();
}

static void TestImagineTwoConditionGate()
{
	std::printf( "IM-d/IM-e: the gate needs BOTH a plan and a target on a capable provider...\n" );
	const std::string tmp = TempPath( "agentcrud_im_d.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "IM-d fixture loads" );
	if( !pJob ) return;
	const std::vector<unsigned char> png = MintCannedPng( pJob, 1 );

	// (1) Both missing -> the refusal names both, then the plan alone is not
	//     enough, then imagining clears it.
	{
		std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
		sess->SetImageGenerator( MakeFakeImageGen( png ) );
		const std::string docBefore = sess->ReadDocument();

		const Agent::AgentChunkResult r1 = sess->InsertChunk( kG2GeometryChunk );
		Check( !r1.applied, "IM-d the first geometry insert is refused" );
		Check( r1.message.find( "file_build_plan" ) != std::string::npos &&
		       r1.message.find( "imagine_scene" ) != std::string::npos,
		       "IM-d MONEY ASSERTION: on a CAPABLE provider the refusal names BOTH tools" );
		Check( r1.message.find( "a plan is filed AND a scene target exists" ) != std::string::npos,
		       "IM-d and states BOTH clearing conditions -- the pre-Phase-2 'as soon as a plan is "
		       "filed' would now be a false claim in a model-facing payload" );
		Check( r1.message.find( "2 more calls will be refused before this gate stops intercepting" )
		       != std::string::npos,
		       "IM-d the shared counter is unchanged -- one counter, one cap, both halves" );
		Check( sess->ReadDocument() == docBefore, "IM-d the document is untouched" );

		Check( sess->FileBuildPlan( SamplePlan() ).ok, "IM-d the plan files" );
		const Agent::AgentChunkResult r2 = sess->InsertChunk( kG2GeometryChunk );
		Check( !r2.applied,
		       "IM-d MONEY ASSERTION: the plan ALONE does not clear the gate on a capable provider" );
		Check( r2.message.find( "no imagined scene target has been created for this session." )
		       != std::string::npos,
		       "IM-d and the refusal now names ONLY what is still missing" );
		Check( r2.message.find( "no build plan has been filed" ) == std::string::npos,
		       "IM-d -- it does not repeat a requirement already met" );
		Check( sess->BuildPlanGateRefusalCount() == 2,
		       "IM-d both refusals came out of the SAME counter" );

		Check( sess->ImagineScene( "a lit courtyard at dusk" ).ok, "IM-d the imagine succeeds" );
		Check( sess->InsertChunk( ImagineBoxChunk( "im_d_box" ) ).applied,
		       "IM-d MONEY ASSERTION: with BOTH conditions met the gate clears" );
	}

	// (2) The cap still bounds a two-condition gate, and the give-up notice
	//     names what was actually missing.
	{
		std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
		sess->SetImageGenerator( MakeFakeImageGen( png ) );
		const std::string capBox = ImagineBoxChunk( "im_e_cap_box" );
		for( int i = 0; i < 3; ++i ) {
			Check( !sess->InsertChunk( capBox ).applied,
			       "IM-e refusal " + std::to_string( i + 1 ) + " of 3" );
		}
		const Agent::AgentChunkResult r4 = sess->InsertChunk( capBox );
		Check( r4.applied,
		       "IM-e MONEY ASSERTION: still exactly 3 refusals then a give-up -- two conditions did "
		       "NOT double the cap, and no second counter was introduced" );
		Check( sess->BuildPlanGateGaveUp() && sess->BuildPlanGateRefusalCount() == 3,
		       "IM-e the give-up state is the shipped one" );
		Check( r4.message.find( "build-plan gate: not satisfied after 3 refusals" ) != std::string::npos,
		       "IM-e the census anchor for the give-up event is unchanged" );
		Check( r4.message.find( "a filed plan or an imagined scene target" ) != std::string::npos,
		       "IM-e MONEY ASSERTION: and the notice names what was ACTUALLY missing rather than "
		       "asserting 'without a filed plan' when neither had been done" );
	}

	// (3) The launch switch governs BOTH halves -- there is no second flag.
	{
		Agent::AgentSession::SetBuildPlanGateDefaultEnabled( false );
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
		sess->SetImageGenerator( MakeFakeImageGen( png ) );
		Check( sess->ImagineCapable(), "IM-e the generator is still installed with the gate off" );
		Check( sess->InsertChunk( ImagineBoxChunk( "im_e_gateoff_box" ) ).applied,
		       "IM-e MONEY ASSERTION: --agent-part-plan-gate=off disables the IMAGINE half too -- "
		       "one switch, both conditions" );
	}

	pJob->release();
	std::remove( tmp.c_str() );
}

//! IM-f: a schema error is a -32602 and disarms NOTHING.
static void TestImagineSchemaErrorDisarmsNothing()
{
	std::printf( "IM-f: a -32602 on imagine_scene disarms nothing...\n" );
	const std::string tmp = TempPath( "agentcrud_im_f.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "IM-f fixture loads" );
	if( !pJob ) return;
	const std::vector<unsigned char> png = MintCannedPng( pJob, 1 );

	std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
	sess->SetImageGenerator( MakeFakeImageGen( png ) );
	Agent::AgentRpcDispatcher rpc( std::move( sess ) );

	{
		const std::string resp = rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"imagine_scene\",\"params\":{}}" );
		Check( resp.find( "-32602" ) != std::string::npos, "IM-f a missing 'description' is -32602" );
		Check( resp.find( "description" ) != std::string::npos, "IM-f and the error names the field" );
	}
	{
		const std::string resp = rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"imagine_scene\","
			"\"params\":{\"description\":\"\"}}" );
		Check( resp.find( "-32602" ) != std::string::npos, "IM-f an EMPTY 'description' is -32602 too" );
	}
	Check( rpc.Session() != nullptr, "IM-f the dispatcher exposes its session" );
	if( rpc.Session() ) {
		Check( !rpc.Session()->ImagineRequirementDisarmed(),
		       "IM-f MONEY ASSERTION: a SCHEMA error does not disarm the imagine requirement -- only "
		       "a PROVIDER failure does, so a mis-shaped call is not a way to switch the mechanism "
		       "off" );
		Check( rpc.Session()->BuildPlanGateRefusalCount() == 0,
		       "IM-f and it does not burn a gate refusal either" );
		Check( !rpc.Session()->HasSceneTarget(), "IM-f and no target was created" );
	}

	// The well-formed call on the same dispatcher DOES work, and its result
	// carries the image under the shared png_base64 field name.
	{
		const std::string resp = rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"imagine_scene\","
			"\"params\":{\"description\":\"a copper kettle on a slate hearth\"}}" );
		Check( resp.find( "-32602" ) == std::string::npos, "IM-f the well-formed call is not an error" );
		Check( resp.find( "\"png_base64\"" ) != std::string::npos,
		       "IM-f MONEY ASSERTION: the generated image rides under the SAME png_base64 field every "
		       "other image-bearing verb uses, so every retention policy covers it with no second "
		       "code path" );
		Check( resp.find( "\"imagined\":true" ) != std::string::npos, "IM-f and reports imagined:true" );
		Check( rpc.Session() && rpc.Session()->HasSceneTarget(),
		       "IM-f the wire path really did install the target" );
	}

	pJob->release();
	std::remove( tmp.c_str() );
}

//----------------------------------------------------------------------
// S1 (2026-08-11): the STAGED BUILD PROTOCOL -- phases, attribution, the
// cross-element refusal, finish/reopen, and the compose-phase creation
// refusal.  Design: docs/agentic-redesign/78-staged-build-protocol.md.
//
// Every assertion below is about a MECHANISM, not a message: what the
// phase is, what is attributed to which element, which call is refused
// and whether the document moved.  The one text assertion (the refusal
// names the active element and the escape verb) is there because a
// refusal that does not say how to proceed is the over-refusal this
// design's sec 2.3 exists to prevent.
//----------------------------------------------------------------------

//! A two-element plan with named pieces -- the shape the wizard probe
//! produced, minus the wizard's eleven parts.
static std::vector<Agent::AgentSession::AgentBuildPlanEntry> TwoElementPlan()
{
	std::vector<Agent::AgentSession::AgentBuildPlanEntry> p;
	Agent::AgentSession::AgentBuildPlanEntry a;
	a.element = "wizard";
	a.pieces.push_back( "robe" );
	a.pieces.push_back( "hat" );
	a.construction = "csg";
	a.outline = "0 0; 2 0; 1.2 4; 0.8 4";
	p.push_back( a );
	Agent::AgentSession::AgentBuildPlanEntry b;
	b.element = "terrain";
	b.pieces.push_back( "ground" );
	b.construction = "displaced";
	b.outline = "0 0; 8 0; 8 1; 0 1";
	p.push_back( b );
	return p;
}

static std::string S1Box( const char* name )
{
	return std::string( "box_geometry\n{\n\tname " ) + name +
		"\n\twidth 1.0\n\theight 1.0\n\tdepth 1.0\n}";
}

static void TestBuildProtocolPhasesAndAttribution()
{
	std::printf( "S1a: plan -> pieces -> compose, with everything created attributed to the active element...\n" );
	const std::string tmp = TempPath( "agentcrud_s1a.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "S1a fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );

	Check( sess->BuildProtocolActive(), "S1a the protocol is active on a gate-armed session" );
	Check( sess->BuildPhase() == Agent::AgentSession::AgentBuildPhase::Plan,
	       "S1a a fresh session is in the PLAN phase" );
	Check( sess->ActiveElement().empty(), "S1a and has no active element" );

	// Filing enters PIECES with the FIRST element active.
	const Agent::AgentSession::AgentBuildPlanResult pr = sess->FileBuildPlan( TwoElementPlan() );
	Check( pr.ok, "S1a the two-element plan files" );
	Check( sess->BuildPhase() == Agent::AgentSession::AgentBuildPhase::Pieces,
	       "S1a filing enters the PIECES phase" );
	Check( sess->ActiveElement() == "wizard", "S1a with the FIRST element active" );
	Check( pr.message.find( "pieces phase" ) != std::string::npos &&
	       pr.message.find( "\"wizard\" is the active element" ) != std::string::npos,
	       "S1a and the filing echo states both facts" );

	// A chunk created in the window is attributed to it.
	Check( sess->InsertChunk( S1Box( "wizard_robe" ) ).applied, "S1a a geometry insert applies" );
	Check( sess->ChunkElement( "wizard_robe" ) == "wizard",
	       "S1a and is attributed to the ACTIVE element -- no naming convention, no volunteered field" );
	Check( sess->ChunkElement( "sph" ).empty(),
	       "S1a RED-PROVE: a PRE-EXISTING scene chunk carries no attribution" );

	// finish_element: the facts, the piece NAMING check, and the advance.
	const Agent::AgentSession::AgentFinishElementResult f1 = sess->FinishElement();
	Check( f1.ok && f1.element == "wizard", "S1a finish_element closes the active element" );
	Check( f1.chunks.size() == 1 && f1.chunks[0] == "wizard_robe",
	       "S1a and reports exactly what was attributed to it" );
	Check( f1.piecesNamed.size() == 1 && f1.piecesNamed[0] == "robe",
	       "S1a the piece whose name appears in a chunk NAME is reported as named" );
	Check( f1.piecesNotNamed.size() == 1 && f1.piecesNotNamed[0] == "hat",
	       "S1a and the one that does not is reported as not named" );
	Check( f1.message.find( "says nothing about what was built" ) != std::string::npos,
	       "S1a MEASUREMENT HYGIENE: the result states outright that the piece check is a NAME check, "
	       "never a judgement of the work" );
	Check( f1.nextElement == "terrain" && f1.phase == std::string( "pieces" ),
	       "S1a and the next element becomes active" );
	Check( sess->ActiveElement() == "terrain", "S1a (observable through the session too)" );
	Check( !f1.rendered && f1.isolateObject.empty(),
	       "S1a with no OBJECT attributed there is no isolate render, and the result says so" );
	Check( f1.message.find( "No renderable object" ) != std::string::npos,
	       "S1a -- stated, never silently absent" );

	// THE CROSS-ELEMENT REFUSAL.  An edit aimed at the wizard's chunk while
	// terrain is active is refused, the document is byte-identical, and the
	// refusal names both the active element and the way back.
	const std::string docBefore = sess->ReadDocument();
	Agent::AgentSetPatch patch;
	patch.target = "wizard_robe";
	patch.param  = "width";
	patch.value  = "2.0";
	const Agent::AgentPatchResult p1 = sess->ProposePatch( patch );
	Check( !p1.applied && p1.status == "rejected", "S1a a patch into ANOTHER element's chunk is refused" );
	Check( p1.message.find( "wizard" ) != std::string::npos &&
	       p1.message.find( "terrain" ) != std::string::npos &&
	       p1.message.find( "finish_element" ) != std::string::npos &&
	       p1.message.find( "reopen_element" ) != std::string::npos,
	       "S1a and the refusal names the owning element, the active element and BOTH escapes" );
	Check( sess->ReadDocument() == docBefore, "S1a RED-PROVE: the document is byte-identical" );
	Check( sess->BuildPhaseRefusalCount() == 1, "S1a one phase refusal is counted" );

	const Agent::AgentChunkResult rm = sess->RemoveChunk( "wizard_robe" );
	Check( !rm.applied, "S1a a REMOVE of another element's chunk is refused too (strictly more destructive)" );
	Check( sess->ReadDocument() == docBefore, "S1a RED-PROVE: still byte-identical" );
	Check( sess->BuildPhaseRefusalCount() == 2,
	       "S1a and the remove counted against the SAME shared budget the patch does" );

	// An UNATTRIBUTED chunk is editable from any window -- never refuse on
	// a chunk no element created.
	Agent::AgentSetPatch prePatch;
	prePatch.target = "sph";
	prePatch.param  = "radius";
	prePatch.value  = "0.9";
	Check( sess->ProposePatch( prePatch ).applied,
	       "S1a an UNATTRIBUTED (pre-existing) chunk is freely editable inside a window" );

	// reopen_element re-enters the wizard's window; the same patch now lands.
	const Agent::AgentSession::AgentReopenElementResult ro = sess->ReopenElement( "wizard" );
	Check( ro.ok && ro.element == "wizard" && ro.phase == std::string( "pieces" ),
	       "S1a reopen_element re-enters the element's window" );
	Check( ro.chunks.size() == 1 && ro.chunks[0] == "wizard_robe",
	       "S1a and reports what is already attributed to it" );
	Check( sess->ProposePatch( patch ).applied,
	       "S1a MONEY ASSERTION: the identical patch APPLIES once its element is active again" );
	Check( sess->BuildPhaseRefusalCount() == 2,
	       "S1a and reopening cost no refusal budget (the count is still the two spent above)" );

	// Finish both elements -> COMPOSE.
	Check( sess->FinishElement().ok, "S1a the reopened element finishes again" );
	Check( sess->ActiveElement() == "terrain", "S1a advancing lands on the still-unfinished element" );
	const Agent::AgentSession::AgentFinishElementResult f3 = sess->FinishElement();
	Check( f3.ok && f3.nextElement.empty() && f3.phase == std::string( "compose" ),
	       "S1a finishing the LAST element enters the compose phase" );
	Check( sess->BuildPhase() == Agent::AgentSession::AgentBuildPhase::Compose,
	       "S1a (observable through the session too)" );

	// COMPOSE: creating geometry is refused and names the escape; editing
	// any element's chunks, and creating a LIGHT, are allowed.
	const std::string docCompose = sess->ReadDocument();
	const Agent::AgentChunkResult composeInsert = sess->InsertChunk( S1Box( "late_box" ) );
	Check( !composeInsert.applied, "S1a creating geometry in the compose phase is refused" );
	Check( composeInsert.message.find( "compose phase" ) != std::string::npos &&
	       composeInsert.message.find( "reopen_element" ) != std::string::npos,
	       "S1a and the refusal states what compose is for and names reopen_element" );
	Check( sess->ReadDocument() == docCompose, "S1a RED-PROVE: the document is byte-identical" );
	Check( sess->ProposePatch( patch ).applied,
	       "S1a but editing ANY element's chunk is allowed in compose (composition adjusts parts)" );
	Check( sess->InsertChunk( "omni_light\n{\n\tname s1_key\n\tpower 40\n\tposition 2 2 2\n}" ).applied,
	       "S1a and a LIGHT can still be created there -- lighting is compose's stated job" );

	// The escape really works.
	Check( sess->ReopenElement( "terrain" ).ok, "S1a reopen_element is legal FROM compose" );
	Check( sess->InsertChunk( S1Box( "terrain_ground" ) ).applied,
	       "S1a MONEY ASSERTION: geometry creation is allowed again inside the reopened window" );
	Check( sess->ChunkElement( "terrain_ground" ) == "terrain",
	       "S1a and the new chunk is attributed to the reopened element" );
}

static void TestBuildProtocolExemptionsAndGiveUp()
{
	std::printf( "S1b: light/camera edits are never refused, and the phase refusals give up after 3...\n" );
	const std::string tmp = TempPath( "agentcrud_s1b.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "S1b fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
	Check( sess->FileBuildPlan( TwoElementPlan() ).ok, "S1b the plan files" );

	// A light created inside the wizard's window IS attributed to it...
	Check( sess->InsertChunk( "omni_light\n{\n\tname s1b_key\n\tpower 40\n\tposition 2 2 2\n}" ).applied,
	       "S1b a light created in the wizard's window applies" );
	Check( sess->ChunkElement( "s1b_key" ) == "wizard", "S1b and is attributed to it" );
	Check( sess->InsertChunk( S1Box( "wizard_robe" ) ).applied, "S1b so does a geometry chunk" );
	Check( sess->FinishElement().ok, "S1b the wizard finishes; terrain becomes active" );

	// ...but editing it from ANOTHER element's window is NOT refused: the
	// registry category (Light) is exempt in every phase.  Over-refusal is
	// the failure mode this design fears, and a model that cannot light its
	// own isolated element cannot see it.
	Agent::AgentSetPatch lightPatch;
	lightPatch.target = "s1b_key";
	lightPatch.param  = "power";
	lightPatch.value  = "60";
	Check( sess->ProposePatch( lightPatch ).applied,
	       "S1b MONEY ASSERTION: a LIGHT attributed to another element is still editable -- "
	       "the category exemption, not an accident of attribution" );
	Check( sess->BuildPhaseRefusalCount() == 0, "S1b and it cost no refusal" );

	// The GIVE-UP: three cross-element refusals, then the fourth proceeds
	// with a factual notice and nothing is refused for this reason again.
	Agent::AgentSetPatch patch;
	patch.target = "wizard_robe";
	patch.param  = "width";
	patch.value  = "2.0";
	for( int i = 1; i <= 3; ++i ) {
		const Agent::AgentPatchResult r = sess->ProposePatch( patch );
		Check( !r.applied, "S1b cross-element refusal " + std::to_string( i ) );
		Check( sess->BuildPhaseRefusalCount() == i, "S1b and the counter is exactly " + std::to_string( i ) );
		Check( !sess->BuildPhaseGaveUp(), "S1b and the phase rules have not given up yet" );
	}
	const Agent::AgentPatchResult gaveUp = sess->ProposePatch( patch );
	Check( gaveUp.applied, "S1b the FOURTH call is let through rather than refused a fourth time" );
	Check( sess->BuildPhaseGaveUp(), "S1b and the phase rules have given up for this session" );
	Check( gaveUp.message.find( "build phase: not satisfied after 3 refusals" ) != std::string::npos,
	       "S1b the give-up is VISIBLE in the payload a trajectory census reads, not only in a log" );
	Check( sess->BuildPhaseRefusalCount() == 3,
	       "S1b the give-up is not counted as a fourth refusal" );
	Check( sess->ProposePatch( patch ).applied, "S1b and nothing is refused for this reason afterwards" );
	// Attribution and the transitions keep working after a give-up: the
	// give-up stops the REFUSING, not the measuring.
	Check( sess->InsertChunk( S1Box( "terrain_rock" ) ).applied, "S1b inserts still apply" );
	Check( sess->ChunkElement( "terrain_rock" ) == "terrain",
	       "S1b and are still attributed -- a give-up disarms the refusals, not the census" );
	Check( sess->FinishElement().ok, "S1b finish_element still advances after a give-up" );
}

static void TestBuildProtocolIsolateRenderAndSwitchOff()
{
	std::printf( "S1c: finish_element returns the element's isolate render; --agent-build-protocol=off is total...\n" );
	{
		const std::string tmp = TempPath( "agentcrud_s1c.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "S1c fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
		Check( sess->FileBuildPlan( TwoElementPlan() ).ok, "S1c the plan files" );

		// A whole object -- geometry plus the standard_object that makes it
		// renderable -- so the payload-fact render has something to isolate.
		std::vector<std::string> chunks;
		chunks.push_back( S1Box( "wizard_robe" ) );
		chunks.push_back( "standard_object\n{\n\tname wizard_obj\n\tgeometry wizard_robe\n\tmaterial mat_diffuse\n}" );
		const std::vector<Agent::AgentChunkResult> ins = sess->InsertChunks( chunks );
		Check( ins.size() == 2 && ins[0].applied && ins[1].applied, "S1c both chunks land" );
		Check( sess->ChunkElement( "wizard_obj" ) == "wizard",
		       "S1c a BATCH insert attributes every chunk it lands, not just the first" );

		const Agent::AgentSession::AgentFinishElementResult f = sess->FinishElement();
		Check( f.ok, "S1c finish_element succeeds" );
		Check( f.isolateObject == "wizard_obj",
		       "S1c and picks the OBJECT attributed to the element (the geometry chunk is not one)" );
		Check( f.rendered && !f.png.empty() && f.width > 0 && f.height > 0,
		       "S1c MONEY ASSERTION: the result carries a real isolate render -- a look the model "
		       "did not have to ask for" );
		Check( f.width <= Agent::kAgentSurfaceMaxRenderEdge &&
		       f.height <= Agent::kAgentSurfaceMaxRenderEdge,
		       "S1c sized by the agent-surface cap, exactly like a model-issued render" );
	}
	// The switch: with the protocol off, filing changes no phase, nothing is
	// attributed, and no phase refusal can fire -- the session behaves
	// exactly as it did before this slice.
	{
		const std::string tmp = TempPath( "agentcrud_s1d.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "S1c/off fixture loads" );
		if( !pJob ) return;
		Agent::AgentSession::SetBuildProtocolDefaultEnabled( false );
		std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
		Agent::AgentSession::SetBuildProtocolDefaultEnabled( true );

		Check( !sess->BuildProtocolActive(), "S1c/off the protocol is inactive for this session" );
		Check( sess->FileBuildPlan( TwoElementPlan() ).ok, "S1c/off the plan still files (the gate still clears)" );
		Check( sess->BuildPhase() == Agent::AgentSession::AgentBuildPhase::Plan,
		       "S1c/off but no phase transition happens" );
		Check( sess->InsertChunk( S1Box( "off_box" ) ).applied, "S1c/off a geometry insert applies" );
		Check( sess->ChunkElement( "off_box" ).empty(), "S1c/off and nothing is attributed" );
		const Agent::AgentSession::AgentFinishElementResult f = sess->FinishElement();
		Check( !f.ok && f.message.find( "protocol is off" ) != std::string::npos,
		       "S1c/off finish_element does nothing and says why" );
		Check( !sess->ReopenElement( "wizard" ).ok, "S1c/off reopen_element does nothing either" );
		Check( sess->BuildPhaseRefusalCount() == 0, "S1c/off no phase refusal can fire" );
	}
	// And the OTHER switch: the plan gate's own flag turns the phases off
	// with it, because the phases are defined by the plan it produces.
	{
		const std::string tmp = TempPath( "agentcrud_s1e.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "S1c/gate-off fixture loads" );
		if( !pJob ) return;
		// The binary's own default is already gate-off (see main), so a plain
		// WrapJob is the gate-off session.
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
		Check( !sess->BuildProtocolActive(),
		       "S1c/gate-off --agent-part-plan-gate=off disables the phase machinery too" );
		Check( sess->FileBuildPlan( TwoElementPlan() ).ok, "S1c/gate-off the plan files" );
		Check( sess->BuildPhase() == Agent::AgentSession::AgentBuildPhase::Plan,
		       "S1c/gate-off and no phase transition happens" );
	}
}

//----------------------------------------------------------------------
// S1 fix-round (2026-08-11): the phase-refusal wiring at the call sites
// the first round left unproven, plus the erased-chunk attribution drop.
//
// The refusals above are exercised through propose_patch, insert_chunk
// and remove_chunk.  The other four arms -- insert_chunks, the two
// geometry scaffolds and remove_chunks -- were wired but never asserted,
// and each carries its own hand-written refusal shape (a per-element fan
// out, a pre-commit `ok=false` return, an all-or-nothing batch verdict),
// so "the shared helper works" proves nothing about them.  For each:
// the refusal names the RIGHT verb, the document is byte-identical, and
// the SHARED counter moved by exactly one -- once per CALL, not once per
// chunk in the batch.
//----------------------------------------------------------------------
static void TestBuildProtocolRefusalCallSites()
{
	std::printf( "S1e: the phase refusal at insert_chunks / both scaffolds / remove_chunks...\n" );

	// ---- CROSS-ELEMENT arm: replace_geometry_scaffold and remove_chunks.
	// Two refusals in one session, under the cap of three.
	{
		const std::string tmp = TempPath( "agentcrud_s1e_cross.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "S1e/cross fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
		Check( sess->FileBuildPlan( TwoElementPlan() ).ok, "S1e/cross the plan files" );

		std::vector<std::string> chunks;
		chunks.push_back( S1Box( "wz_geo" ) );
		chunks.push_back( "standard_object\n{\n\tname wz_obj\n\tgeometry wz_geo\n\tmaterial mat_diffuse\n}" );
		const std::vector<Agent::AgentChunkResult> ins = sess->InsertChunks( chunks );
		Check( ins.size() == 2 && ins[0].applied && ins[1].applied, "S1e/cross the wizard's object lands" );
		Check( sess->FinishElement().ok, "S1e/cross the wizard finishes; terrain becomes active" );

		const std::string docBefore = sess->ReadDocument();

		// remove_chunks -- the BATCH remove's cross-element arm.
		const Agent::AgentSession::AgentRemoveBatchResult rb =
			sess->RemoveChunks( std::vector<std::string>( 1, std::string( "wz_geo" ) ) );
		Check( !rb.applied && rb.status == "rejected",
		       "S1e/cross remove_chunks into ANOTHER element's chunk is refused" );
		Check( rb.message.find( "remove_chunks refused" ) != std::string::npos,
		       "S1e/cross and the refusal names remove_chunks, not the verb whose helper it shares" );
		Check( rb.targetResults.size() == 1 && !rb.targetResults[0].applied,
		       "S1e/cross the per-target verdict is fanned out too" );
		Check( sess->ReadDocument() == docBefore, "S1e/cross RED-PROVE: the document is byte-identical" );
		Check( sess->BuildPhaseRefusalCount() == 1, "S1e/cross the SHARED counter moved by exactly one" );

		// replace_geometry_scaffold -- its cross-element arm (arm (a)).
		const Agent::AgentSession::AgentGeometryScaffoldResult rg =
			sess->ReplaceGeometryScaffold( "wz_obj", "sdf_column", "s1ecross", 1.0, 0.5, 1.0 );
		Check( !rg.ok, "S1e/cross replace_geometry_scaffold on ANOTHER element's object is refused" );
		Check( rg.message.find( "replace_geometry_scaffold refused" ) != std::string::npos &&
		       rg.message.find( "terrain" ) != std::string::npos,
		       "S1e/cross and it names the verb and the active element" );
		Check( sess->ReadDocument() == docBefore, "S1e/cross RED-PROVE: still byte-identical" );
		Check( sess->BuildPhaseRefusalCount() == 2, "S1e/cross and the counter is exactly two" );
		Check( !sess->BuildPhaseGaveUp(), "S1e/cross with the budget not yet exhausted" );
	}

	// ---- COMPOSE arm: insert_chunks and both scaffolds.  Three refusals in
	// one session -- exactly the cap, so the give-up must NOT have fired.
	{
		const std::string tmp = TempPath( "agentcrud_s1e_compose.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "S1e/compose fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
		Check( sess->FileBuildPlan( TwoElementPlan() ).ok, "S1e/compose the plan files" );
		Check( sess->FinishElement().ok && sess->FinishElement().ok,
		       "S1e/compose both elements finish" );
		Check( sess->BuildPhase() == Agent::AgentSession::AgentBuildPhase::Compose,
		       "S1e/compose the session is in the compose phase" );

		const std::string docBefore = sess->ReadDocument();

		// insert_chunks -- TWO geometry chunks in ONE call.  The whole batch
		// is refused, and it costs ONE refusal, not one per chunk.
		std::vector<std::string> batch;
		batch.push_back( S1Box( "s1e_cb1" ) );
		batch.push_back( S1Box( "s1e_cb2" ) );
		const std::vector<Agent::AgentChunkResult> res = sess->InsertChunks( batch );
		Check( res.size() == 2 && !res[0].applied && !res[1].applied,
		       "S1e/compose the WHOLE insert_chunks batch is refused, not half-landed" );
		Check( res[0].message.find( "insert_chunks refused" ) != std::string::npos &&
		       res[0].message.find( "reopen_element" ) != std::string::npos,
		       "S1e/compose and the refusal names insert_chunks and the escape" );
		Check( sess->ReadDocument() == docBefore, "S1e/compose RED-PROVE: the document is byte-identical" );
		Check( sess->BuildPhaseRefusalCount() == 1,
		       "S1e/compose MONEY ASSERTION: a two-chunk batch costs ONE refusal -- a batching caller "
		       "spends the phase budget at the same rate a singular one does" );

		// insert_geometry_scaffold -- unconditionally geometry-creating.
		const Agent::AgentSession::AgentGeometryScaffoldResult gs =
			sess->InsertGeometryScaffold( "sdf_column", "s1egs", 1.0, 0.5, 1.0 );
		Check( !gs.ok, "S1e/compose insert_geometry_scaffold is refused in the compose phase" );
		Check( gs.message.find( "insert_geometry_scaffold refused" ) != std::string::npos &&
		       gs.message.find( "compose phase" ) != std::string::npos,
		       "S1e/compose and the refusal names the verb and states what compose is for" );
		Check( sess->ReadDocument() == docBefore, "S1e/compose RED-PROVE: still byte-identical" );
		Check( sess->BuildPhaseRefusalCount() == 2, "S1e/compose the counter is exactly two" );

		// replace_geometry_scaffold -- its COMPOSE arm (arm (b)).  Aimed at a
		// PRE-EXISTING object, so the cross-element arm cannot be what fires.
		const Agent::AgentSession::AgentGeometryScaffoldResult rg =
			sess->ReplaceGeometryScaffold( "obj_sph", "sdf_column", "s1ergs", 1.0, 0.5, 1.0 );
		Check( !rg.ok, "S1e/compose replace_geometry_scaffold is refused in the compose phase too" );
		Check( rg.message.find( "replace_geometry_scaffold refused" ) != std::string::npos &&
		       rg.message.find( "compose phase" ) != std::string::npos,
		       "S1e/compose and it is the COMPOSE refusal, on an UNATTRIBUTED target" );
		Check( sess->ReadDocument() == docBefore, "S1e/compose RED-PROVE: still byte-identical" );
		Check( sess->BuildPhaseRefusalCount() == 3, "S1e/compose the counter is exactly three" );
		Check( !sess->BuildPhaseGaveUp(),
		       "S1e/compose and three refusals is the cap, not the give-up -- the FOURTH gives up" );
	}
}

//----------------------------------------------------------------------
// S1 fix-round (2026-08-11, P1): replace_geometry_scaffold ERASES the
// orphaned previous geometry chunk.  Its attribution must go with it.
//
// A surviving entry can do exactly one thing: CheckElementWindowForEdit_
// is a pure attribution lookup that runs BEFORE any document read, so a
// later edit naming the erased chunk comes back as a cross-element
// refusal for a chunk that DOES NOT EXIST -- steering the model to
// reopen_element on something no window can fix, and spending one of the
// three shared refusal slots to do it.
//----------------------------------------------------------------------
static void TestBuildProtocolErasedGeometryAttribution()
{
	std::printf( "S1f: replace_geometry_scaffold drops the erased chunk's attribution...\n" );
	const std::string tmp = TempPath( "agentcrud_s1f.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "S1f fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
	Check( sess->FileBuildPlan( TwoElementPlan() ).ok, "S1f the plan files" );

	std::vector<std::string> chunks;
	chunks.push_back( S1Box( "wz_geo" ) );
	chunks.push_back( "standard_object\n{\n\tname wz_obj\n\tgeometry wz_geo\n\tmaterial mat_diffuse\n}" );
	const std::vector<Agent::AgentChunkResult> ins = sess->InsertChunks( chunks );
	Check( ins.size() == 2 && ins[0].applied && ins[1].applied, "S1f the wizard's object lands" );
	Check( sess->ChunkElement( "wz_geo" ) == "wizard", "S1f and the geometry is attributed to the wizard" );

	const Agent::AgentSession::AgentGeometryScaffoldResult rg =
		sess->ReplaceGeometryScaffold( "wz_obj", "sdf_column", "s1fnew", 1.0, 0.5, 1.0 );
	Check( rg.ok && rg.status == "applied", "S1f the form revision applies" );
	Check( rg.previousGeometryRemoved && rg.previousGeometryName == "wz_geo",
	       "S1f and it ERASED the now-unreferenced previous geometry" );
	Check( sess->ChunkElement( "wz_geo" ).empty(),
	       "S1f MONEY ASSERTION: the erased chunk's attribution went with it -- an entry naming a "
	       "chunk that no longer exists can only refuse a later edit unfixably" );
	Check( !sess->ChunkElement( rg.geometryName ).empty(),
	       "S1f while the REPLACEMENT geometry is attributed to the active element" );

	// The consequence, stated as behaviour: from another element's window, an
	// edit naming the erased chunk fails on RESOLUTION (it is not there), not
	// on attribution -- and costs no phase refusal.
	Check( sess->FinishElement().ok, "S1f the wizard finishes; terrain becomes active" );
	Agent::AgentSetPatch ghost;
	ghost.target = "wz_geo";
	ghost.param  = "width";
	ghost.value  = "2.0";
	const Agent::AgentPatchResult gp = sess->ProposePatch( ghost );
	Check( !gp.applied, "S1f a patch naming the erased chunk still fails" );
	Check( gp.message.find( "was created while the element" ) == std::string::npos,
	       "S1f RED-PROVE: but NOT as a cross-element refusal -- the model is not sent to "
	       "reopen_element for a chunk no window contains" );
	Check( sess->BuildPhaseRefusalCount() == 0,
	       "S1f and it spent none of the three shared refusal slots" );
}

//----------------------------------------------------------------------
// S1 fix-round (2026-08-11, P1): the DIAGNOSED (code-3) outcome MUTATES
// the document while reporting `applied == false`, so every attribution
// hook keys off AgentSession::ResultMutatedDocument_ (`applied || status
// == "diagnosed"`) rather than off `applied` alone.  Below is that
// predicate's executable guard, both directions.
//
// A code 3 cannot be synthesized from scene DATA -- it is the
// should-not-happen divergence where the dry-run passes but the real
// re-derive emits diagnostics.  So this reuses AgentLiveCommitTest's
// CodeThreeJob technique (Tests 6/14/20/27): override the chunk-CRUD
// virtuals, let the BASE call do the real D2 work (the Document really
// is mutated and the managers really are replaced -- the assertions
// below prove it against the live scene), and rewrite only the REPORTED
// code 2 -> 3.  Chunk CRUD is always D2-class, so no variant scene is
// needed to reach the rewrite.
//
// Arming is explicit per call rather than left on: each direction then
// red-proves exactly ONE hook site, and nothing else in the fixture
// (the plan filing, the setup insert, the finish) is running diagnosed.
//----------------------------------------------------------------------
class CodeThreeCrudJob : public Job
{
public:
	CodeThreeCrudJob() : Job(), mForceCodeThree( false ) {}
	void SetForceCodeThree( bool on ) { mForceCodeThree = on; }
	int ApplyCstInsertChunk( const char* chunkText, char* outKeyword, unsigned int keywordMax,
	                         char* outName, unsigned int nameMax, char* outDiag, unsigned int diagMax,
	                         int* outInsertedAt = nullptr ) override
	{
		const int base = Job::ApplyCstInsertChunk( chunkText, outKeyword, keywordMax,
		                                           outName, nameMax, outDiag, diagMax, outInsertedAt );
		return ( mForceCodeThree && base == 2 ) ? 3 : base;
	}
	int ApplyCstRemoveChunk( const char* target, const char* kind,
	                         char* outKeyword, unsigned int keywordMax, char* outDiag, unsigned int diagMax ) override
	{
		const int base = Job::ApplyCstRemoveChunk( target, kind, outKeyword, keywordMax, outDiag, diagMax );
		return ( mForceCodeThree && base == 2 ) ? 3 : base;
	}
private:
	bool mForceCodeThree;
};

//! Load `kScene` into a CodeThreeCrudJob (LoadScene's sibling -- it hard-codes
//! `new Job()`), force-code-three DISARMED so the load and the setup calls are
//! ordinary clean commits.
static CodeThreeCrudJob* LoadSceneCodeThree( const std::string& path )
{
	{ std::ofstream o( path.c_str(), std::ios::binary ); o << kScene; }
	CodeThreeCrudJob* pJob = new CodeThreeCrudJob();
	if( !pJob->LoadAsciiSceneViaCst( path.c_str() ) ) {
		pJob->release();
		std::remove( path.c_str() );
		return nullptr;
	}
	return pJob;
}

static void TestBuildProtocolDiagnosedAttribution()
{
	std::printf( "S1g: a DIAGNOSED (code-3) insert is still ATTRIBUTED, and a diagnosed remove still DROPS attribution...\n" );

	// (a) A chunk landed by a DIAGNOSED insert belongs to the active element.
	//     Pre-fix (`applied` alone) it landed UNATTRIBUTED -- invisible to
	//     finish_element and to the census, and freely editable from every
	//     other element's window, which is the "everything created in an
	//     element window is attributed to it" invariant broken outright.
	{
		const std::string tmp = TempPath( "agentcrud_s1g_insert.RISEscene" );
		CodeThreeCrudJob* pJob = LoadSceneCodeThree( tmp );
		Check( pJob != nullptr, "S1g(a) fixture loads into CodeThreeCrudJob via the CST path" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
		Check( sess->FileBuildPlan( TwoElementPlan() ).ok, "S1g(a) the plan files" );
		Check( sess->ActiveElement() == "wizard", "S1g(a) with the wizard active" );

		pJob->SetForceCodeThree( true );
		const Agent::AgentChunkResult ins = sess->InsertChunk( S1Box( "wz_diag_geo" ) );
		pJob->SetForceCodeThree( false );

		Check( !ins.applied && ins.status == "diagnosed" && ins.rawCode == 3,
		       "S1g(a) the insert is a code-3 diagnosed commit -- applied is FALSE, as the wire contract says" );
		Check( pJob->GetGeometries() && pJob->GetGeometries()->GetItem( "wz_diag_geo" ) != nullptr,
		       "S1g(a) RED-PROVE the premise: the chunk IS in the live derived scene despite applied==false "
		       "-- this is a mutation, not a reject" );
		Check( sess->ChunkElement( "wz_diag_geo" ) == "wizard",
		       "S1g(a) MONEY ASSERTION: it is attributed to the ACTIVE element (goes red if the hook keys "
		       "off `applied` instead of ResultMutatedDocument_)" );

		const Agent::AgentSession::AgentFinishElementResult f = sess->FinishElement();
		Check( f.ok && f.element == "wizard", "S1g(a) the wizard finishes" );
		Check( f.chunks.size() == 1 && f.chunks[0] == "wz_diag_geo",
		       "S1g(a) MONEY ASSERTION: finish_element reports the diagnosed chunk as the element's work "
		       "-- an unattributed chunk would be missing from this list entirely" );
		Check( sess->ActiveElement() == "terrain", "S1g(a) and terrain becomes active" );

		// The behavioural consequence: the wizard's chunk is now out of reach.
		const std::string docBefore = sess->ReadDocument();
		Agent::AgentSetPatch patch;
		patch.target = "wz_diag_geo";
		patch.param  = "width";
		patch.value  = "2.0";
		const Agent::AgentPatchResult p = sess->ProposePatch( patch );
		Check( !p.applied && p.status == "rejected",
		       "S1g(a) MONEY ASSERTION: an edit from ANOTHER element's window is refused -- the window rule "
		       "covers a diagnosed chunk exactly as it covers a cleanly-applied one" );
		Check( p.message.find( "was created while the element" ) != std::string::npos &&
		       p.message.find( "wizard" ) != std::string::npos &&
		       p.message.find( "reopen_element" ) != std::string::npos,
		       "S1g(a) and it is the CROSS-ELEMENT refusal, naming the owning element and the way back" );
		Check( sess->ReadDocument() == docBefore, "S1g(a) RED-PROVE: the refused patch left the document byte-identical" );
		Check( sess->BuildPhaseRefusalCount() == 1, "S1g(a) one phase refusal is counted" );
	}

	// (b) A chunk erased by a DIAGNOSED remove loses its attribution.  Pre-fix
	//     the entry survived the chunk, and CheckElementWindowForEdit_ is a
	//     pure attribution lookup that runs BEFORE any document read -- so a
	//     later edit came back as a cross-element refusal for a chunk that
	//     does not exist, steering the model to reopen_element on something no
	//     window can fix and spending a shared refusal slot to do it (the same
	//     failure S1f pins for the scaffold-replace path).
	{
		const std::string tmp = TempPath( "agentcrud_s1g_remove.RISEscene" );
		CodeThreeCrudJob* pJob = LoadSceneCodeThree( tmp );
		Check( pJob != nullptr, "S1g(b) fixture loads into CodeThreeCrudJob via the CST path" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
		Check( sess->FileBuildPlan( TwoElementPlan() ).ok, "S1g(b) the plan files" );

		// Setup insert is CLEAN (still disarmed), so this direction red-proves
		// the remove-side hook alone.
		Check( sess->InsertChunk( S1Box( "wz_doomed" ) ).applied, "S1g(b) the setup insert applies cleanly" );
		Check( sess->ChunkElement( "wz_doomed" ) == "wizard", "S1g(b) and is attributed to the wizard" );

		pJob->SetForceCodeThree( true );
		const Agent::AgentChunkResult rm = sess->RemoveChunk( "wz_doomed" );
		pJob->SetForceCodeThree( false );

		Check( !rm.applied && rm.status == "diagnosed" && rm.rawCode == 3,
		       "S1g(b) the remove is a code-3 diagnosed commit -- applied is FALSE" );
		Check( pJob->GetGeometries() && pJob->GetGeometries()->GetItem( "wz_doomed" ) == nullptr,
		       "S1g(b) RED-PROVE the premise: the chunk is GONE from the live derived scene despite "
		       "applied==false -- the erase really landed" );
		Check( sess->ChunkElement( "wz_doomed" ).empty(),
		       "S1g(b) MONEY ASSERTION: the erased chunk's attribution went with it (goes red if the hook "
		       "keys off `applied` instead of ResultMutatedDocument_)" );

		const Agent::AgentSession::AgentFinishElementResult f = sess->FinishElement();
		Check( f.ok && f.chunks.empty(),
		       "S1g(b) finish_element reports nothing attributed -- the only chunk was erased" );
		Check( sess->ActiveElement() == "terrain", "S1g(b) terrain becomes active" );

		Agent::AgentSetPatch ghost;
		ghost.target = "wz_doomed";
		ghost.param  = "width";
		ghost.value  = "2.0";
		const Agent::AgentPatchResult gp = sess->ProposePatch( ghost );
		Check( !gp.applied, "S1g(b) a patch naming the erased chunk still fails (it is not there)" );
		Check( gp.message.find( "was created while the element" ) == std::string::npos,
		       "S1g(b) MONEY ASSERTION: but NOT as a cross-element refusal -- a stale entry could only send "
		       "the model to reopen_element for a chunk no window contains" );
		Check( sess->BuildPhaseRefusalCount() == 0,
		       "S1g(b) and it spent none of the three shared refusal slots" );
	}
}

static void TestBuildProtocolWireShape()
{
	std::printf( "S1d: finish_element / reopen_element over the wire...\n" );
	const std::string tmp = TempPath( "agentcrud_s1wire.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "S1d fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
	Agent::AgentRpcDispatcher rpc( std::move( sess ) );

	{
		const std::string resp = rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"file_build_plan\",\"params\":"
			"{\"elements\":[{\"element\":\"wizard\",\"pieces\":[\"robe\",\"hat\"],"
			"\"construction\":\"csg\",\"outline\":\"0 0; 2 0; 1.2 4; 0.8 4\"},"
			"{\"element\":\"terrain\",\"pieces\":[\"ground\"],"
			"\"construction\":\"displaced\",\"outline\":\"0 0; 8 0; 8 1; 0 1\"}]}}" );
		Agent::JsonValue result;
		Check( JsonResultObj( resp, result ), "S1d file_build_plan returns a result object" );
		Check( result.get( "phase" ).asString() == "pieces" &&
		       result.get( "activeElement" ).asString() == "wizard",
		       "S1d the filing result carries the phase and the active element" );
		Check( result.get( "elements" ).at( 0 ).get( "pieces" ).isArray() &&
		       result.get( "elements" ).at( 0 ).get( "pieces" ).size() == 2,
		       "S1d and echoes each element's piece list" );
	}
	// pieces is REQUIRED on the wire, and its absence is a -32602 -- a SCHEMA
	// defect, which by contract never touches the gate's refusal counter.
	{
		const std::string resp = rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"file_build_plan\",\"params\":"
			"{\"elements\":[{\"element\":\"a\",\"construction\":\"csg\",\"outline\":\"0 0; 1 0; 1 1\"}]}}" );
		Check( resp.find( "-32602" ) != std::string::npos &&
		       resp.find( "elements[0].pieces" ) != std::string::npos,
		       "S1d a MISSING `pieces` is -32602 naming the index and the field" );
	}
	{
		const std::string resp = rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"finish_element\",\"params\":{}}" );
		Agent::JsonValue result;
		Check( JsonResultObj( resp, result ), "S1d finish_element returns a result object" );
		Check( result.get( "ok" ).asBool() && result.get( "element" ).asString() == "wizard",
		       "S1d it closes the active element" );
		Check( result.get( "phase" ).asString() == "pieces" &&
		       result.get( "nextElement" ).asString() == "terrain",
		       "S1d and reports the phase and the next element" );
		Check( result.get( "piecesNotNamed" ).isArray() &&
		       result.get( "piecesNotNamed" ).size() == 2,
		       "S1d with the piece checklist reported factually (nothing was built, so neither is named)" );
		Check( !result.has( "headVersion" ),
		       "S1d there is NO headVersion -- the call does not touch the document" );
	}
	{
		const std::string resp = rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"reopen_element\",\"params\":{\"element\":\"nope\"}}" );
		Agent::JsonValue result;
		Check( JsonResultObj( resp, result ), "S1d an unknown element is a RESULT, not an error envelope" );
		Check( !result.get( "ok" ).asBool() &&
		       result.get( "message" ).asString().find( "\"wizard\"" ) != std::string::npos,
		       "S1d ok:false, listing the filed element names -- a state mismatch, not a schema defect" );
	}
	{
		const std::string resp = rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"reopen_element\",\"params\":{}}" );
		Check( resp.find( "-32602" ) != std::string::npos,
		       "S1d a MISSING `element` is a param-shape defect: -32602" );
	}
	{
		const std::string resp = rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"reopen_element\",\"params\":{\"element\":\"wizard\"}}" );
		Agent::JsonValue result;
		Check( JsonResultObj( resp, result ), "S1d reopen_element returns a result object" );
		Check( result.get( "ok" ).asBool() && result.get( "phase" ).asString() == "pieces" &&
		       result.get( "previousPhase" ).asString() == "pieces",
		       "S1d and reports both the previous and the new phase" );
	}
	// S1 fix-round (2026-08-11): the CREATION verbs carry the attribution on
	// the wire.  Before this, "which chunks belong to element X" reached a
	// trajectory ONLY through finish_element's result -- so a run that ends
	// mid-element stated its attributed set nowhere at all, and design sec 4
	// item 2 ("is the wizard window actually spent on the wizard") is this
	// slice's central measurement.  The wizard is active again after the
	// reopen above.
	{
		const std::string resp = rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"insert_chunk\",\"params\":{\"chunkText\":"
			"\"box_geometry\\n{\\n\\tname s1d_probe\\n\\twidth 1\\n\\theight 1\\n\\tdepth 1\\n}\"}}" );
		Agent::JsonValue result;
		Check( JsonResultObj( resp, result ), "S1d insert_chunk returns a result object" );
		Check( result.get( "applied" ).asBool( false ), "S1d the insert applies inside the window" );
		Check( result.get( "element" ).asString() == "wizard",
		       "S1d MONEY ASSERTION: the chunk result names the element it was attributed to, so a run "
		       "that never reaches finish_element still states its attribution on the wire" );
	}
	// ...and it is OMITTED, not empty, when there is no attribution: a
	// REMOVE carries no element (the attribution is dropped with the chunk),
	// which is exactly the state that makes a chunk freely editable.
	{
		const std::string resp = rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"remove_chunk\",\"params\":"
			"{\"target\":\"s1d_probe\"}}" );
		Agent::JsonValue result;
		Check( JsonResultObj( resp, result ), "S1d remove_chunk returns a result object" );
		Check( result.get( "applied" ).asBool( false ), "S1d the remove applies" );
		Check( !result.has( "element" ),
		       "S1d RED-PROVE: the `element` key is OMITTED where there is no attribution -- an existing "
		       "caller's response shape is unchanged" );
	}
}

//----------------------------------------------------------------------
// S2 (2026-08-11): CLEAN-ROOM CONSTRUCTION -- build_element, place_element
// and the first-geometry refusal.
// Design: docs/agentic-redesign/79-clean-room-construction.md.
//
// EVERY TEST HERE USES A MOCKED TEXT COMPLETER.  No live provider call is
// made, ever: the completer is a canned-answer callable installed through
// the same AgentSession::SetTextCompleter seam the eval runner and both
// GUIs use, so the harness drives the byte-identical session path a real
// completion would, and the ASSERTIONS are about what the harness does
// with an answer -- which is the whole of what this slice owns.
//----------------------------------------------------------------------

//! A completer that answers with `answers[i]` on the i-th call and repeats
//! the last one thereafter, counting the calls it received.
static Agent::AgentSession::AgentTextCompleter MakeFakeCompleter(
	std::vector<std::string> answers, int* callsOut = nullptr,
	std::vector<std::string>* promptsOut = nullptr )
{
	Agent::AgentSession::AgentTextCompleter c;
	c.supported    = true;
	c.providerName = "mock";
	c.modelId      = "mock-builder-1";
	auto shared = std::make_shared<std::vector<std::string> >( std::move( answers ) );
	auto count  = std::make_shared<int>( 0 );
	c.complete = [shared, count, callsOut, promptsOut]( const std::string& prompt )
		-> Agent::AgentSession::AgentTextCompletionOutcome
	{
		Agent::AgentSession::AgentTextCompletionOutcome o;
		if( promptsOut ) promptsOut->push_back( prompt );
		const std::size_t idx = ( static_cast<std::size_t>( *count ) < shared->size() )
			? static_cast<std::size_t>( *count )
			: ( shared->empty() ? 0 : shared->size() - 1 );
		++( *count );
		if( callsOut ) *callsOut = *count;
		if( shared->empty() ) { o.error = "no canned answer"; return o; }
		o.ok   = true;
		o.text = ( *shared )[idx];
		return o;
	};
	return c;
}

//! A one-element plan, so the first element is active the moment it files.
static std::vector<Agent::AgentSession::AgentBuildPlanEntry> WizardOnlyPlan()
{
	std::vector<Agent::AgentSession::AgentBuildPlanEntry> p;
	Agent::AgentSession::AgentBuildPlanEntry a;
	a.element = "wizard";
	a.pieces.push_back( "robe" );
	a.pieces.push_back( "hat" );
	a.construction = "csg";
	a.outline = "0 0; 2 0; 1.2 4; 0.8 4";
	p.push_back( a );
	return p;
}

//! A well-formed builder answer: a painter, a material, an SDF geometry with
//! THREE part lines, and one standard_object -- every name prefixed
//! `wizard_`, and the object at the origin so the bbox is predictable.
static const char* const kGoodBuilderAnswer =
	"uniformcolor_painter\n{\n\tname wizard_robe_pnt\n\tcolor 0.3 0.2 0.5\n}\n"
	"lambertian_material\n{\n\tname wizard_robe_mat\n\treflectance wizard_robe_pnt\n}\n"
	"sdf_geometry\n{\n\tname wizard_body_sdf\n"
	"\tpart roundcone union 0  0 0.4 0  0 0 0  1 1 1  0.5 0.25 0.9  0\n"
	"\tpart sphere smin 0.1  0 1.6 0  0 0 0  1 1 1  0.3 0 0  0\n"
	"\tpart torus smin 0.05  0 1.85 0  0 0 0  1 1 1  0.45 0.06 0  0\n"
	"}\n"
	"standard_object\n{\n\tname wizard_obj\n\tgeometry wizard_body_sdf\n"
	"\tmaterial wizard_robe_mat\n\tposition 0 0 0\n}\n";

//! S2a: the happy path -- chunks land, they are attributed, the part count
//! and the realised bbox come back, and no retry was needed.
static void TestCleanRoomBuildElementHappyPath()
{
	std::printf( "S2a: build_element inserts a whole element, reports its bbox and part count...\n" );
	const std::string tmp = TempPath( "agentcrud_s2a.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "S2a fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );

	int calls = 0;
	std::vector<std::string> prompts;
	sess->SetTextCompleter( MakeFakeCompleter( { kGoodBuilderAnswer }, &calls, &prompts ) );
	Check( sess->BuildCapable(), "S2a the session reports the installed completer as a capability" );
	Check( sess->FileBuildPlan( WizardOnlyPlan() ).ok, "S2a the plan files" );
	Check( sess->ActiveElement() == "wizard", "S2a the wizard is active" );

	const Agent::AgentSession::AgentBuildElementResult r = sess->BuildElement( "wizard", 4.0 );
	Check( r.ok, "S2a build_element succeeds" );
	Check( calls == 1, "S2a MONEY ASSERTION: exactly ONE completion -- no retry on a clean answer" );
	Check( !r.retryRan, "S2a and the result says the retry did not run" );
	Check( r.chunksExtracted == 4, "S2a all four chunks were extracted" );
	Check( r.landed.size() == 4, "S2a and all four landed" );
	Check( r.rejected.empty(), "S2a with nothing rejected" );
	Check( r.sdfPartCount == 3,
	       "S2a MONEY ASSERTION: the SDF part count is the arc's headline measurement and is counted "
	       "from the CST of what actually landed (got " + std::to_string( r.sdfPartCount ) + ")" );
	Check( sess->ChunkElement( "wizard_body_sdf" ) == "wizard",
	       "S2a every landed chunk is attributed by the arc-78 machinery, unchanged" );
	Check( sess->ChunkElement( "wizard_robe_mat" ) == "wizard",
	       "S2a -- including the material" );
	Check( r.bboxValid, "S2a the realised world bbox is reported" );
	Check( r.bboxMax[1] > r.bboxMin[1],
	       "S2a and it has a real Y extent (the inventory the assembler never had)" );
	Check( r.message.find( "against the 4 requested" ) != std::string::npos,
	       "S2a the message states the realised height AGAINST the request, never as a pass/fail" );

	// THE PROMPT IS HOST-COMPOSED.  Everything the builder was told came
	// from this harness or from the descriptor registry.
	Check( prompts.size() == 1, "S2a one prompt was composed" );
	if( !prompts.empty() ) {
		const std::string& p = prompts[0];
		Check( p.find( Agent::AgentSession::LocalFrameContract() ) != std::string::npos,
		       "S2a MONEY ASSERTION: the local-frame contract is sent VERBATIM" );
		Check( p.find( "wizard_" ) != std::string::npos, "S2a the required prefix is stated" );
		Check( p.find( "robe" ) != std::string::npos && p.find( "hat" ) != std::string::npos,
		       "S2a the element's DECLARED PIECES are restated to the fresh context" );
		Check( p.find( "0 0; 2 0; 1.2 4; 0.8 4" ) != std::string::npos,
		       "S2a and so is the outline it sketched" );
		Check( p.find( "sdf_geometry" ) != std::string::npos &&
		       p.find( "standard_object" ) != std::string::npos,
		       "S2a the grammar comes from the descriptor registry (ReadSchema), not a second "
		       "hand-written copy that could drift from the parser" );
	}
}

//! S2b: a prefix violation is REJECTED, not renamed -- and an unbalanced
//! chunk is REPORTED, not dropped.  Both drive the one repair retry.
static void TestCleanRoomValidatedInsertion()
{
	std::printf( "S2b: prefix violations rejected without renaming; an unbalanced chunk reported...\n" );
	const std::string tmp = TempPath( "agentcrud_s2b.RISEscene" );

	// (1) PREFIX VIOLATION.  The second chunk is named without the prefix.
	{
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "S2b/prefix fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
		const std::string bad =
			"uniformcolor_painter\n{\n\tname wizard_pnt\n\tcolor 0.3 0.2 0.5\n}\n"
			"lambertian_material\n{\n\tname robe_mat\n\treflectance wizard_pnt\n}\n";
		sess->SetTextCompleter( MakeFakeCompleter( { bad } ) );
		Check( sess->FileBuildPlan( WizardOnlyPlan() ).ok, "S2b/prefix the plan files" );
		const Agent::AgentSession::AgentBuildElementResult r = sess->BuildElement( "wizard", 4.0 );
		Check( r.ok, "S2b/prefix the call still reports its outcome" );
		bool sawPrefixReject = false;
		for( std::size_t i = 0; i < r.rejected.size(); ++i )
			if( r.rejected[i].name == "robe_mat" ) sawPrefixReject = true;
		Check( sawPrefixReject, "S2b/prefix the wrongly-named chunk is REJECTED by name" );
		Check( sess->ReadDocument().find( "wizard_robe_mat" ) == std::string::npos,
		       "S2b/prefix MONEY ASSERTION: it was NOT renamed into the document -- renaming would "
		       "break the references the builder wrote between its own chunks" );
		Check( sess->ReadDocument().find( "\trobe_mat" ) == std::string::npos,
		       "S2b/prefix and it was not inserted under its own name either" );
	}

	// (2) UNBALANCED CHUNK -- the hand simulation's silent drop.
	{
		std::vector<std::string> chunks, problems;
		Agent::AgentSession::ExtractChunkTexts(
			"box_geometry\n{\n\tname wizard_a\n\twidth 1\n}\n"
			"sdf_geometry\n{\n\tname wizard_b\n\tpart sphere union 0 0 0 0 0 0 0 1 1 1 1 0 0 0\n",
			chunks, problems );
		Check( chunks.size() == 1, "S2b/unbalanced the closed chunk is still recovered" );
		Check( problems.size() == 1,
		       "S2b/unbalanced MONEY ASSERTION: the UNCLOSED chunk is REPORTED, never dropped -- the "
		       "hand simulation's most damaging failure mode" );
		if( !problems.empty() )
			Check( problems[0].find( "sdf_geometry" ) != std::string::npos &&
			       problems[0].find( "never closed" ) != std::string::npos,
			       "S2b/unbalanced and the report names the chunk and what was wrong" );
	}

	// (3) The extractor's other malformed shapes, and the canonical re-emit.
	{
		std::vector<std::string> chunks, problems;
		Agent::AgentSession::ExtractChunkTexts(
			"```\nHere is the element.\n{\n\tstray 1\n}\n}\n"
			"box_geometry { name wizard_c\n width 2 }\n```\n",
			chunks, problems );
		Check( chunks.size() == 1, "S2b/malformed only the real chunk is extracted" );
		if( !chunks.empty() ) {
			Check( chunks[0].find( "box_geometry\n{\n" ) == 0,
			       "S2b/malformed a same-line `keyword {` is re-emitted in the canonical "
			       "braces-on-their-own-lines form insert_chunk requires" );
		}
		Check( problems.size() == 2,
		       "S2b/malformed BOTH the keyword-less brace and the stray closing brace are reported "
		       "(got " + std::to_string( problems.size() ) + ")" );
		Check( chunks.size() == 1 || problems.size() == 2, "S2b/malformed (guard)" );
	}

	// (4) S2 fix-round (2026-08-11, P2): a comment between the keyword and
	// the opening brace -- both forms the CST lexer accepts -- must not
	// blind the backward keyword scan.  Before the fix, the backward scan
	// skipped whitespace only, stopped INSIDE the comment body, and
	// misreported both well-formed chunks below as keyword-less, silently
	// dropping them.
	{
		std::vector<std::string> chunks, problems;
		Agent::AgentSession::ExtractChunkTexts(
			"box_geometry # a trailing line comment\n{\n\tname wizard_lc\n\twidth 1\n}\n"
			"sdf_geometry /* a block comment */\n{\n\tname wizard_bc\n"
			"\tpart sphere union 0 0 0 0 0 0 0 1 1 1 1 0 0 0\n}\n",
			chunks, problems );
		Check( problems.empty(),
		       "S2b/comments MONEY ASSERTION: neither comment form is misreported as a keyword-less "
		       "brace (got " + std::to_string( problems.size() ) + " problem(s))" );
		Check( chunks.size() == 2,
		       "S2b/comments and BOTH chunks are extracted, not silently dropped "
		       "(got " + std::to_string( chunks.size() ) + ")" );
		if( chunks.size() == 2 ) {
			Check( chunks[0].find( "box_geometry\n{\n" ) == 0,
			       "S2b/comments the line-comment-preceded chunk keeps its `box_geometry` keyword" );
			Check( chunks[1].find( "sdf_geometry\n{\n" ) == 0,
			       "S2b/comments the block-comment-preceded chunk keeps its `sdf_geometry` keyword" );
		}
	}
}

//! S2c: the ONE repair retry -- it fires once and succeeds, and it fires
//! once and honestly reports a partial outcome.
static void TestCleanRoomRepairRetry()
{
	std::printf( "S2c: one repair retry, whether it succeeds or not...\n" );
	const std::string tmp = TempPath( "agentcrud_s2c.RISEscene" );

	// (1) RETRY SUCCEEDS.
	{
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "S2c/ok fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
		const std::string firstBad =
			"uniformcolor_painter\n{\n\tname pnt_wrong\n\tcolor 0.3 0.2 0.5\n}\n";
		int calls = 0;
		std::vector<std::string> prompts;
		sess->SetTextCompleter( MakeFakeCompleter( { firstBad, kGoodBuilderAnswer },
		                                           &calls, &prompts ) );
		Check( sess->FileBuildPlan( WizardOnlyPlan() ).ok, "S2c/ok the plan files" );
		const Agent::AgentSession::AgentBuildElementResult r = sess->BuildElement( "wizard", 4.0 );
		Check( calls == 2, "S2c/ok MONEY ASSERTION: exactly TWO completions -- one build, one retry" );
		Check( r.retryRan && r.retrySucceeded, "S2c/ok the retry ran and landed chunks" );
		Check( r.landed.size() == 4, "S2c/ok the corrected set landed whole" );
		Check( prompts.size() == 2 &&
		       prompts[1].find( "A PREVIOUS ANSWER TO THIS SAME REQUEST WAS PARTLY REJECTED" )
		           != std::string::npos,
		       "S2c/ok the retry prompt carries the rejection block" );
		Check( prompts.size() == 2 && prompts[1].find( "pnt_wrong" ) != std::string::npos,
		       "S2c/ok MONEY ASSERTION: it carries the EXACT rejection text, so the builder is "
		       "corrected by what happened rather than by a paraphrase" );
	}

	// (2) RETRY FAILS -- partial, honest, and NO second retry.
	{
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "S2c/partial fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
		const std::string halfGood =
			"uniformcolor_painter\n{\n\tname wizard_pnt\n\tcolor 0.3 0.2 0.5\n}\n"
			"lambertian_material\n{\n\tname nope_mat\n\treflectance wizard_pnt\n}\n";
		int calls = 0;
		sess->SetTextCompleter( MakeFakeCompleter( { halfGood, halfGood }, &calls ) );
		Check( sess->FileBuildPlan( WizardOnlyPlan() ).ok, "S2c/partial the plan files" );
		const Agent::AgentSession::AgentBuildElementResult r = sess->BuildElement( "wizard", 4.0 );
		Check( calls == 2,
		       "S2c/partial MONEY ASSERTION: the retry is capped at ONE -- a builder that keeps "
		       "returning the same defect is not asked a third time" );
		Check( r.retryRan && !r.retrySucceeded, "S2c/partial the retry ran and added nothing" );
		Check( r.landed.size() == 1 && r.landed[0] == "wizard_pnt",
		       "S2c/partial what landed, landed -- partial success is a first-class outcome" );
		bool sawReason = false;
		for( std::size_t i = 0; i < r.rejected.size(); ++i )
			if( r.rejected[i].name == "nope_mat" ) sawReason = true;
		Check( sawReason, "S2c/partial and every rejection is still named with its reason" );
		Check( r.message.find( "no second retry" ) != std::string::npos,
		       "S2c/partial the message says outright that there is no second retry" );
	}
}

//! S2d: the do-nothing cases -- wrong phase, wrong element, no capability.
static void TestCleanRoomBuildElementRefusals()
{
	std::printf( "S2d: build_element outside the pieces phase, and off the active element...\n" );
	const std::string tmp = TempPath( "agentcrud_s2d.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "S2d fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
	int calls = 0;
	sess->SetTextCompleter( MakeFakeCompleter( { kGoodBuilderAnswer }, &calls ) );

	// PLAN phase: no element is active yet.
	{
		const Agent::AgentSession::AgentBuildElementResult r = sess->BuildElement( "wizard", 4.0 );
		Check( !r.ok, "S2d build_element in the PLAN phase does nothing" );
		Check( r.message.find( "file_build_plan" ) != std::string::npos,
		       "S2d and names the verb that starts the build" );
		Check( calls == 0, "S2d MONEY ASSERTION: it never reached the provider, so it cost nothing" );
	}
	Check( sess->FileBuildPlan( TwoElementPlan() ).ok, "S2d the two-element plan files" );

	// A name that is not the ACTIVE element.
	{
		const Agent::AgentSession::AgentBuildElementResult r = sess->BuildElement( "terrain", 4.0 );
		Check( !r.ok, "S2d build_element for a non-active element does nothing" );
		Check( r.message.find( "reopen_element" ) != std::string::npos,
		       "S2d and names the escape that would make it active" );
		Check( calls == 0, "S2d still no provider call" );
	}
	// A bad height is refused before anything else.
	{
		const Agent::AgentSession::AgentBuildElementResult r = sess->BuildElement( "wizard", 0.0 );
		Check( !r.ok && calls == 0, "S2d a non-positive height does nothing and costs nothing" );
	}
	// COMPOSE phase.
	{
		Check( sess->FinishElement().ok, "S2d the wizard finishes" );
		Check( sess->FinishElement().ok, "S2d and so does terrain, entering compose" );
		Check( sess->BuildPhase() == Agent::AgentSession::AgentBuildPhase::Compose,
		       "S2d the session is in compose" );
		const Agent::AgentSession::AgentBuildElementResult r = sess->BuildElement( "wizard", 4.0 );
		Check( !r.ok, "S2d build_element in COMPOSE does nothing" );
		Check( r.message.find( "reopen_element" ) != std::string::npos,
		       "S2d and names the way back into an element window" );
		Check( calls == 0, "S2d and never reached the provider" );
	}

	// NO CAPABILITY -- an honest statement, and hand authoring is not blocked.
	{
		Job* pJob2 = LoadScene( kScene, TempPath( "agentcrud_s2d2.RISEscene" ) );
		Check( pJob2 != nullptr, "S2d/nocap fixture loads" );
		if( !pJob2 ) return;
		std::unique_ptr<Agent::AgentSession> s2 = WrapJobGateArmed( pJob2 );
		Check( !s2->BuildCapable(), "S2d/nocap a session with no completer is not build-capable" );
		Check( s2->FileBuildPlan( WizardOnlyPlan() ).ok, "S2d/nocap the plan files" );
		const Agent::AgentSession::AgentBuildElementResult r = s2->BuildElement( "wizard", 4.0 );
		Check( !r.ok && r.capabilityRefusal, "S2d/nocap it is a CAPABILITY refusal" );
		Check( r.message.find( "not blocked by this" ) != std::string::npos,
		       "S2d/nocap and says outright that hand authoring still works" );
		Check( s2->InsertChunk( S1Box( "wizard_hand" ) ).applied,
		       "S2d/nocap MONEY ASSERTION: with no builder to route to, the first-geometry refusal "
		       "never arms -- a mechanism cannot force a path that does not exist" );
	}
}

//! G2 fix-round (2026-08-11): build_element must consult the SAME build-plan
//! gate the other six construction verbs do, and consult it BEFORE spending a
//! provider completion -- not just before the InsertChunks call it makes with
//! the completion's result.  Design: docs/agentic-redesign/79-clean-room-
//! construction.md section 7.3 (the live-run defect this pins): the FIRST
//! build_element call on a session with a filed plan but no imagined scene
//! target ran the builder completion, got real geometry back, and had every
//! chunk refused by InsertChunks's own gate arm -- the provider call was paid
//! for and thrown away.  This test proves the fix: the gate now refuses
//! build_element itself, before any completion is issued, so nothing is
//! spent on a call the gate was always going to refuse.
static void TestCleanRoomBuildElementBuildPlanGate()
{
	std::printf( "S2/G2: build_element consults the build-plan gate BEFORE spending a completion...\n" );
	const std::string tmp = TempPath( "agentcrud_s2g2.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "S2/G2 fixture loads" );
	if( !pJob ) return;
	const std::vector<unsigned char> png = MintCannedPng( pJob, 1 );

	std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
	// A CAPABLE image generator, installed but never invoked yet -- this is
	// what makes ImagineRequirementActive_() true and therefore the gate's
	// imagine condition the one that actually fires, matching the live-run
	// defect exactly (a provider that supports imagine_scene, plan filed,
	// nothing imagined yet).
	sess->SetImageGenerator( MakeFakeImageGen( png ) );
	int calls = 0;
	sess->SetTextCompleter( MakeFakeCompleter( { kGoodBuilderAnswer }, &calls ) );
	Check( sess->BuildCapable(), "S2/G2 the session is build-capable" );

	Check( sess->FileBuildPlan( WizardOnlyPlan() ).ok, "S2/G2 the plan files" );
	Check( sess->ActiveElement() == "wizard", "S2/G2 the wizard is active" );
	Check( sess->BuildPhase() == Agent::AgentSession::AgentBuildPhase::Pieces,
	       "S2/G2 filing entered the pieces phase" );
	Check( !sess->HasSceneTarget(), "S2/G2 -- and, deliberately, nothing has been imagined yet" );

	const std::string docBefore = sess->ReadDocument();

	// (1) THE DEFECT, PINNED.  build_element is refused by the SAME gate
	//     insert_chunks enforces, and -- the point of this whole test --
	//     the refusal happens BEFORE the builder completion, not after.
	const Agent::AgentSession::AgentBuildElementResult r1 = sess->BuildElement( "wizard", 4.0 );
	Check( !r1.ok, "S2/G2 build_element is refused while no scene target exists" );
	Check( r1.message.find( "no imagined scene target has been created for this session." )
	       != std::string::npos,
	       "S2/G2 and the refusal is the build-plan gate's own text" );
	Check( r1.message.find( "imagine_scene" ) != std::string::npos,
	       "S2/G2 and names the tool that clears it" );
	Check( calls == 0,
	       "S2/G2 MONEY ASSERTION: NO provider completion was issued for a call the gate was always "
	       "going to refuse -- this is the regression the live run hit: paying for a completion and "
	       "then discarding every chunk it produced" );
	Check( r1.chunksExtracted == 0 && r1.landed.empty(),
	       "S2/G2 and nothing was extracted or landed -- the refusal returns before any of that runs" );
	Check( sess->ReadDocument() == docBefore,
	       "S2/G2 the document is byte-identical -- no mutation happened either" );
	Check( sess->BuildPlanGateRefusalCount() == 1,
	       "S2/G2 the refusal came out of the SAME shared counter insert_chunks/insert_chunk/"
	       "propose_patch/the geometry-scaffold verbs all use" );

	// (2) ONCE IMAGINED, build_element PROCEEDS NORMALLY -- past the gate,
	//     straight through to a real completion and a real insert.
	Check( sess->ImagineScene( "a robed wizard on a hilltop" ).ok, "S2/G2 the imagine succeeds" );
	Check( sess->HasSceneTarget(), "S2/G2 a scene target now exists" );

	const Agent::AgentSession::AgentBuildElementResult r2 = sess->BuildElement( "wizard", 4.0 );
	Check( r2.ok, "S2/G2 MONEY ASSERTION: with both conditions met build_element proceeds past the "
	              "gate exactly as before this fix" );
	Check( calls == 1, "S2/G2 and this is the FIRST completion actually issued -- exactly one, not "
	                   "wasted on the refused first attempt" );
	Check( !r2.landed.empty(), "S2/G2 and it actually landed geometry" );
}

//! S2e: the first-geometry refusal -- it fires, it names build_element, it
//! LIFTS once the element has content, and it never touches non-geometry.
static void TestCleanRoomFirstGeometryRefusal()
{
	std::printf( "S2e: the first geometry for an element must come from build_element...\n" );
	const std::string tmp = TempPath( "agentcrud_s2e.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "S2e fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
	sess->SetTextCompleter( MakeFakeCompleter( { kGoodBuilderAnswer } ) );
	Check( sess->FileBuildPlan( WizardOnlyPlan() ).ok, "S2e the plan files" );

	// A MATERIAL is never refused by this rule.
	Check( sess->InsertChunk(
		"uniformcolor_painter\n{\n\tname wizard_p0\n\tcolor 0.1 0.2 0.3\n}" ).applied,
	       "S2e a painter is never refused by the clean-room rule" );
	// ...but a painter counts as content, so re-arm by starting clean.
	Check( sess->RemoveChunk( "wizard_p0" ).applied, "S2e (and is removed again for the next arm)" );
	Check( sess->ElementChunks( "wizard" ).empty(), "S2e the element is empty again" );

	const std::string docBefore = sess->ReadDocument();
	const Agent::AgentChunkResult r1 = sess->InsertChunk( S1Box( "wizard_hand" ) );
	Check( !r1.applied && r1.status == "rejected",
	       "S2e MONEY ASSERTION: hand-authored geometry is refused while the element has no chunk" );
	Check( r1.message.find( "build_element" ) != std::string::npos,
	       "S2e and the refusal names build_element" );
	Check( sess->ReadDocument() == docBefore, "S2e the document is byte-identical" );
	Check( sess->BuildPhaseRefusalCount() == 1,
	       "S2e it burns exactly one slot of the SHARED phase counter (the third arm)" );

	// The BATCH surface refuses on the same terms, atomically.
	{
		std::vector<std::string> batch;
		batch.push_back( S1Box( "wizard_b1" ) );
		batch.push_back( "uniformcolor_painter\n{\n\tname wizard_b2\n\tcolor 1 1 1\n}" );
		const std::vector<Agent::AgentChunkResult> rs = sess->InsertChunks( batch );
		Check( rs.size() == 2 && !rs[0].applied && !rs[1].applied,
		       "S2e insert_chunks is refused as a WHOLE -- half an element is a state nobody asked for" );
		Check( sess->ReadDocument() == docBefore, "S2e and the document is still byte-identical" );
		Check( sess->BuildPhaseRefusalCount() == 2, "S2e one more slot of the same counter" );
	}

	// build_element itself is NEVER refused by the rule it arms.
	const Agent::AgentSession::AgentBuildElementResult br = sess->BuildElement( "wizard", 4.0 );
	Check( br.ok && br.landed.size() == 4,
	       "S2e MONEY ASSERTION: the clean room's own insertion is not refused by the clean-room rule" );

	// ...and now hand authoring is allowed, permanently, for this element.
	Check( sess->InsertChunk( S1Box( "wizard_hand" ) ).applied,
	       "S2e MONEY ASSERTION: once the element has content, refinement by hand is allowed" );
	Check( sess->InsertChunk( S1Box( "wizard_hand2" ) ).applied,
	       "S2e -- and stays allowed" );
	Check( sess->BuildPhaseRefusalCount() == 2, "S2e with no further refusals counted" );
}

//! S2f: protocol OFF disables the refusal with everything else.
static void TestCleanRoomProtocolOff()
{
	std::printf( "S2f: --agent-build-protocol=off disables the clean-room refusal too...\n" );
	const std::string tmp = TempPath( "agentcrud_s2f.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "S2f fixture loads" );
	if( !pJob ) return;

	Agent::AgentSession::SetBuildProtocolDefaultEnabled( false );
	std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
	Agent::AgentSession::SetBuildProtocolDefaultEnabled( true );

	sess->SetTextCompleter( MakeFakeCompleter( { kGoodBuilderAnswer } ) );
	Check( !sess->BuildProtocolActive(), "S2f the protocol is inactive for this session" );
	Check( sess->FileBuildPlan( WizardOnlyPlan() ).ok, "S2f the plan still files (the gate clears)" );
	Check( sess->InsertChunk( S1Box( "wizard_hand" ) ).applied,
	       "S2f MONEY ASSERTION: hand-authored geometry is NOT refused with the protocol off" );
	const Agent::AgentSession::AgentBuildElementResult r = sess->BuildElement( "wizard", 4.0 );
	Check( !r.ok, "S2f and build_element itself does nothing" );
	Check( r.message.find( "staged build protocol is off" ) != std::string::npos,
	       "S2f saying exactly that" );
	Check( !sess->PlaceElement( "wizard", "1 0 0" ).ok, "S2f place_element does nothing either" );
}

//! S2g: place_element offsets a MULTI-OBJECT element rigidly, and reports
//! what it could not fully place.
static void TestCleanRoomPlaceElement()
{
	std::printf( "S2g: place_element moves a multi-object element as one rigid transform...\n" );
	const std::string tmp = TempPath( "agentcrud_s2g.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "S2g fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );

	// A two-object element with its own internal offsets, authored through
	// build_element so the attribution is the real one.
	const std::string twoObjects =
		"uniformcolor_painter\n{\n\tname wizard_pnt\n\tcolor 0.3 0.2 0.5\n}\n"
		"lambertian_material\n{\n\tname wizard_mat\n\treflectance wizard_pnt\n}\n"
		"box_geometry\n{\n\tname wizard_body_geo\n\twidth 1\n\theight 2\n\tdepth 1\n}\n"
		"box_geometry\n{\n\tname wizard_hat_geo\n\twidth 1.4\n\theight 0.3\n\tdepth 1.4\n}\n"
		"standard_object\n{\n\tname wizard_body_obj\n\tgeometry wizard_body_geo\n"
		"\tmaterial wizard_mat\n\tposition 0 1 0\n}\n"
		"standard_object\n{\n\tname wizard_hat_obj\n\tgeometry wizard_hat_geo\n"
		"\tmaterial wizard_mat\n\tposition 0 2.15 0\n}\n";
	sess->SetTextCompleter( MakeFakeCompleter( { twoObjects } ) );
	Check( sess->FileBuildPlan( WizardOnlyPlan() ).ok, "S2g the plan files" );
	const Agent::AgentSession::AgentBuildElementResult br = sess->BuildElement( "wizard", 2.3 );
	Check( br.ok && br.landed.size() == 6, "S2g the two-object element lands" );

	const Agent::AgentSession::AgentPlaceElementResult pr =
		sess->PlaceElement( "wizard", "5 0 -2" );
	Check( pr.ok, "S2g place_element applies" );
	Check( pr.objects.size() == 2,
	       "S2g MONEY ASSERTION: EVERY standard_object attributed to the element is transformed, "
	       "which is the operation RISE's flat scene graph cannot express natively" );
	const std::string doc = sess->ReadDocument();
	Check( doc.find( "position 5 1 -2" ) != std::string::npos,
	       "S2g the body's own offset (0 1 0) is PRESERVED and added to the new base-centre" );
	Check( doc.find( "position 5 2.15 -2" ) != std::string::npos,
	       "S2g MONEY ASSERTION: so is the hat's -- the element moves rigidly, it does not collapse" );

	// A second placement composes against the first the same way (offset,
	// not accumulate-from-origin): the base-centre is where it is asked for.
	const Agent::AgentSession::AgentPlaceElementResult pr2 =
		sess->PlaceElement( "wizard", "0 0 0", "2" );
	Check( pr2.ok, "S2g a scaled placement applies" );
	const std::string doc2 = sess->ReadDocument();
	Check( doc2.find( "scale 2 2 2" ) != std::string::npos,
	       "S2g the uniform factor multiplies each object's scale" );
	Check( doc2.find( "position 10 2 -4" ) != std::string::npos,
	       "S2g and multiplies its offset from the element's origin, so the element scales about "
	       "its own base-centre" );

	// An element with nothing placeable says so, and changes nothing.
	{
		const std::string before = sess->ReadDocument();
		const Agent::AgentSession::AgentPlaceElementResult none =
			sess->PlaceElement( "not_an_element", "0 0 0" );
		Check( !none.ok, "S2g an unknown element does nothing" );
		Check( none.message.find( "not in the filed" ) != std::string::npos,
		       "S2g and lists what is filed" );
		Check( sess->ReadDocument() == before, "S2g with the document byte-identical" );
	}
}

//! G3 fix-round (2026-08-11): a live run built a 4-piece, 17-chunk element
//! and had EVERY chunk rejected, in two stages -- first for carrying no
//! `name` at all (the builder used the intended name as the chunk KEYWORD
//! instead), then, on the one repair retry, for carrying a QUOTED `name`
//! (`name "coral_reef_rock_painter"`).  ChunkParamString_ returns the raw
//! token text, so the quoted value came back WITH the quote characters, and
//! the prefix check's rejection message then LIED -- it claimed the name did
//! not begin with the required prefix when, quoted, it never could have.
//! Design: docs/agentic-redesign/79-clean-room-construction.md.  Three
//! things pinned here: (1) a quoted name that legitimately begins with the
//! prefix once stripped LANDS, under the bare name, and the strip is
//! DISCLOSED in the result message; (2) a quoted name that STILL fails the
//! prefix check after stripping reports the failure against the STRIPPED
//! (true) name, never the quoted one; (3) a TOTAL rejection (every chunk
//! refused) shows a short excerpt of what the builder actually returned --
//! and ONLY a total rejection, never a partial or full success.
static void TestCleanRoomBuildElementQuotedNameAndTotalRejection()
{
	std::printf( "S2/G3: quoted `name` values tolerated & disclosed; TOTAL rejection shows an excerpt...\n" );

	// (1) A quoted name that LANDS after stripping, and the strip is
	// DISCLOSED.  Cross-references between the chunks (reflectance,
	// geometry, material) stay BARE, matching the live-run shape: only the
	// DEFINITION side was quoted, and stripping it makes the definition
	// match the references the builder already wrote.
	{
		const std::string tmp = TempPath( "agentcrud_s2g3a.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "S2/G3a fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
		const std::string quotedGood =
			"uniformcolor_painter\n{\n\tname \"wizard_robe_pnt\"\n\tcolor 0.3 0.2 0.5\n}\n"
			"lambertian_material\n{\n\tname \"wizard_robe_mat\"\n\treflectance wizard_robe_pnt\n}\n"
			"sdf_geometry\n{\n\tname \"wizard_body_sdf\"\n"
			"\tpart roundcone union 0  0 0.4 0  0 0 0  1 1 1  0.5 0.25 0.9  0\n}\n"
			"standard_object\n{\n\tname \"wizard_obj\"\n\tgeometry wizard_body_sdf\n"
			"\tmaterial wizard_robe_mat\n\tposition 0 0 0\n}\n";
		int calls = 0;
		sess->SetTextCompleter( MakeFakeCompleter( { quotedGood }, &calls ) );
		Check( sess->FileBuildPlan( WizardOnlyPlan() ).ok, "S2/G3a the plan files" );
		const Agent::AgentSession::AgentBuildElementResult r = sess->BuildElement( "wizard", 4.0 );
		Check( calls == 1,
		       "S2/G3a MONEY ASSERTION: no retry needed -- the quoted names land on the FIRST attempt" );
		Check( r.ok && r.landed.size() == 4, "S2/G3a every chunk lands despite the quoted names" );
		Check( r.rejected.empty(), "S2/G3a nothing is rejected" );
		bool sawBareName = false;
		for( std::size_t i = 0; i < r.landed.size(); ++i )
			if( r.landed[i] == "wizard_robe_pnt" ) sawBareName = true;
		Check( sawBareName, "S2/G3a MONEY ASSERTION: it landed under the BARE name, not the quoted one" );
		Check( sess->ReadDocument().find( "\"wizard_robe_pnt\"" ) == std::string::npos,
		       "S2/G3a and the document itself carries no quote characters" );
		Check( sess->ChunkElement( "wizard_robe_pnt" ) == "wizard",
		       "S2/G3a attributed to the element under the bare name" );
		Check( r.message.find( "stripped a wrapping pair of double quotes" ) != std::string::npos,
		       "S2/G3a MONEY ASSERTION: the fix is DISCLOSED in the result message" );
		Check( r.message.find( "wizard_robe_pnt" ) != std::string::npos,
		       "S2/G3a and it names at least one of the chunks it fixed" );
	}

	// (2) A quoted name that STILL fails the prefix check after stripping --
	// the reported reason must be against the STRIPPED name, the TRUE
	// statement, never the quoted (false) one.
	{
		const std::string tmp = TempPath( "agentcrud_s2g3b.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "S2/G3b fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
		const std::string quotedBadPrefix =
			"uniformcolor_painter\n{\n\tname \"robe_pnt\"\n\tcolor 0.3 0.2 0.5\n}\n";
		sess->SetTextCompleter( MakeFakeCompleter( { quotedBadPrefix } ) );
		Check( sess->FileBuildPlan( WizardOnlyPlan() ).ok, "S2/G3b the plan files" );
		const Agent::AgentSession::AgentBuildElementResult r = sess->BuildElement( "wizard", 4.0 );
		Check( !r.rejected.empty(), "S2/G3b something was rejected" );
		bool sawStrippedReason = false;
		bool sawRejectedName = false;
		for( std::size_t i = 0; i < r.rejected.size(); ++i ) {
			if( r.rejected[i].reason.find( "\"robe_pnt\" does not begin" ) != std::string::npos )
				sawStrippedReason = true;
			if( r.rejected[i].name == "robe_pnt" ) sawRejectedName = true;
		}
		Check( sawStrippedReason,
		       "S2/G3b MONEY ASSERTION: the prefix-failure reason names the STRIPPED name \"robe_pnt\" "
		       "-- the true statement (before this fix it named the QUOTED form, a false claim)" );
		Check( sawRejectedName, "S2/G3b and the rejection's own `name` field is the stripped form too" );
	}

	// (3) A TOTAL rejection shows an excerpt of the builder's actual answer;
	// a partial or full success shows none.
	{
		// (3a) total: every chunk this element got, across both attempts, is
		// rejected (wrong prefix, not a missing answer -- so this is a real
		// total rejection, not the separate pure-provider-failure case).
		const std::string tmp = TempPath( "agentcrud_s2g3c.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "S2/G3c fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
		const std::string allBad =
			"uniformcolor_painter\n{\n\tname nope_pnt\n\tcolor 0.3 0.2 0.5\n}\n"
			"lambertian_material\n{\n\tname nope_mat\n\treflectance nope_pnt\n}\n";
		sess->SetTextCompleter( MakeFakeCompleter( { allBad, allBad } ) );
		Check( sess->FileBuildPlan( WizardOnlyPlan() ).ok, "S2/G3c the plan files" );
		const Agent::AgentSession::AgentBuildElementResult r = sess->BuildElement( "wizard", 4.0 );
		Check( r.ok && r.landed.empty(), "S2/G3c MONEY ASSERTION: a total rejection -- nothing landed" );
		Check( !r.rejected.empty(), "S2/G3c but chunks WERE rejected (not the pure provider-failure case)" );
		Check( r.message.find( "the builder's last answer" ) != std::string::npos,
		       "S2/G3c MONEY ASSERTION: the total-rejection excerpt is present" );
		Check( r.message.find( "nope_pnt" ) != std::string::npos ||
		       r.message.find( "nope_mat" ) != std::string::npos,
		       "S2/G3c and shows what the builder actually wrote" );
	}
	{
		// (3b) partial success: the excerpt must NOT appear.
		const std::string tmp = TempPath( "agentcrud_s2g3d.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "S2/G3d fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
		const std::string halfGood =
			"uniformcolor_painter\n{\n\tname wizard_pnt\n\tcolor 0.3 0.2 0.5\n}\n"
			"lambertian_material\n{\n\tname nope_mat\n\treflectance wizard_pnt\n}\n";
		sess->SetTextCompleter( MakeFakeCompleter( { halfGood, halfGood } ) );
		Check( sess->FileBuildPlan( WizardOnlyPlan() ).ok, "S2/G3d the plan files" );
		const Agent::AgentSession::AgentBuildElementResult r = sess->BuildElement( "wizard", 4.0 );
		Check( r.ok && !r.landed.empty(), "S2/G3d something landed -- a partial success" );
		Check( r.message.find( "the builder's last answer" ) == std::string::npos,
		       "S2/G3d MONEY ASSERTION: the total-rejection excerpt is NOT shown on a partial success" );
	}
	{
		// (3c) full success: no excerpt either.
		const std::string tmp = TempPath( "agentcrud_s2g3e.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "S2/G3e fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
		sess->SetTextCompleter( MakeFakeCompleter( { kGoodBuilderAnswer } ) );
		Check( sess->FileBuildPlan( WizardOnlyPlan() ).ok, "S2/G3e the plan files" );
		const Agent::AgentSession::AgentBuildElementResult r = sess->BuildElement( "wizard", 4.0 );
		Check( r.ok && r.rejected.empty(), "S2/G3e a clean full success" );
		Check( r.message.find( "the builder's last answer" ) == std::string::npos,
		       "S2/G3e and no excerpt on a full success either" );
	}
}

//! S2h: the wire shape of both verbs.
static void TestCleanRoomWireShape()
{
	std::printf( "S2h: build_element / place_element over JSON-RPC...\n" );
	const std::string tmp = TempPath( "agentcrud_s2h.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "S2h fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
	sess->SetTextCompleter( MakeFakeCompleter( { kGoodBuilderAnswer } ) );
	Check( sess->FileBuildPlan( WizardOnlyPlan() ).ok, "S2h the plan files" );
	Agent::AgentRpcDispatcher rpc( std::move( sess ) );

	{
		const std::string resp = rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"build_element\",\"params\":"
			"{\"element\":\"wizard\",\"height\":4.0}}" );
		Agent::JsonValue result;
		Check( JsonResultObj( resp, result ), "S2h build_element returns a result object" );
		Check( result.get( "ok" ).asBool( false ), "S2h ok:true" );
		Check( result.get( "sdfPartCount" ).asNumber( -1 ) == 3.0, "S2h sdfPartCount rides the wire" );
		Check( result.get( "landed" ).isArray() && result.get( "landed" ).size() == 4,
		       "S2h and so does the landed list" );
		Check( result.get( "bbox" ).isObject(),
		       "S2h the realised bbox is a structured fact, not only prose" );
		Check( result.get( "retryRan" ).asBool( true ) == false, "S2h retryRan is reported" );
	}
	// A schema defect is a clean -32602 that never touches a counter.
	{
		const std::string resp = rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"build_element\",\"params\":"
			"{\"element\":\"wizard\"}}" );
		Check( resp.find( "-32602" ) != std::string::npos,
		       "S2h a missing `height` is a schema error, not a refusal" );
	}
	{
		const std::string resp = rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"place_element\",\"params\":"
			"{\"element\":\"wizard\",\"position\":\"2 0 1\"}}" );
		Agent::JsonValue result;
		Check( JsonResultObj( resp, result ), "S2h place_element returns a result object" );
		Check( result.get( "ok" ).asBool( false ), "S2h ok:true" );
		Check( result.get( "objects" ).isArray() && result.get( "objects" ).size() == 1,
		       "S2h with the objects it transformed" );
		Check( result.get( "patchesApplied" ).asNumber( 0 ) >= 1.0, "S2h and the patch count" );
	}
	{
		const std::string resp = rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"place_element\",\"params\":"
			"{\"element\":\"wizard\"}}" );
		Check( resp.find( "-32602" ) != std::string::npos,
		       "S2h a missing `position` is a schema error" );
	}
}

//! S2i (S2 fix-round, 2026-08-11, P1): replace_geometry_scaffold used to
//! bypass the first-geometry clean-room gate entirely.  Unlike
//! insert_geometry_scaffold, it never routes through InsertChunk /
//! InsertChunks (it splices the CST and commits by its own path), so
//! without the fix it could land geometry against a PRE-EXISTING,
//! UNATTRIBUTED object -- never refused on its own, since the gate only
//! guards an element's FIRST chunk -- attribute the result to the active
//! element, and thereby permanently disarm the clean room for that element
//! without ever going through build_element.  Also covers the sibling
//! coverage the reviewer flagged as missing: insert_geometry_scaffold
//! against an empty active element IS refused (transitively, through
//! InsertChunks).
static void TestCleanRoomReplaceGeometryScaffoldGate()
{
	std::printf( "S2i: replace_geometry_scaffold and insert_geometry_scaffold both honour the "
	             "first-geometry clean-room gate...\n" );
	const std::string tmp = TempPath( "agentcrud_s2i.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "S2i fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
	sess->SetTextCompleter( MakeFakeCompleter( { kGoodBuilderAnswer } ) );
	Check( sess->FileBuildPlan( WizardOnlyPlan() ).ok, "S2i the plan files" );
	Check( sess->ElementChunks( "wizard" ).empty(), "S2i the wizard has no chunks yet" );

	// The sibling coverage: insert_geometry_scaffold is refused too, on the
	// same terms, transitively through InsertChunks.
	{
		const std::string docBefore = sess->ReadDocument();
		// InsertGeometryScaffold's `ok` means only "well-formed and
		// submitted" (the SAME "not a promise every chunk landed" hedge
		// InsertGeometryScaffoldResult's own doc states) -- the actual
		// per-chunk disposition, and the clean-room refusal, lives in
		// `chunkResults`, exactly as InsertChunks reports it directly.
		const Agent::AgentSession::AgentGeometryScaffoldResult sr =
			sess->InsertGeometryScaffold( "sweep_rail", "s2iIns", 1.0, 0.4, 1.0 );
		Check( !sr.chunkResults.empty(), "S2i insert_geometry_scaffold's request expanded to chunks" );
		bool allRefused = !sr.chunkResults.empty();
		bool anyNamesBuildElement = false;
		for( std::size_t k = 0; k < sr.chunkResults.size(); ++k ) {
			if( sr.chunkResults[k].applied ) allRefused = false;
			if( sr.chunkResults[k].message.find( "build_element" ) != std::string::npos )
				anyNamesBuildElement = true;
		}
		Check( allRefused,
		       "S2i MONEY ASSERTION: EVERY chunk insert_geometry_scaffold expands to is refused while "
		       "the element is empty -- transitively, through InsertChunks" );
		Check( anyNamesBuildElement, "S2i and the refusal names build_element" );
		Check( sess->ReadDocument() == docBefore, "S2i the document is byte-identical" );
		Check( sess->ElementChunks( "wizard" ).empty(),
		       "S2i the element is still empty -- the gate was not disarmed" );
	}

	const int refusalsBefore = sess->BuildPhaseRefusalCount();

	// THE BUG: replace_geometry_scaffold against `obj_sph`, a PRE-EXISTING
	// object in the fixture scene that is not attributed to any element.
	// Before the fix, this landed, attributed `obj_sph` to "wizard", and
	// permanently disarmed the clean room for it.
	{
		const std::string docBefore = sess->ReadDocument();
		const Agent::AgentSession::AgentGeometryScaffoldResult sr =
			sess->ReplaceGeometryScaffold( "obj_sph", "sweep_rail", "s2iRep", 1.0, 0.4, 1.0 );
		Check( !sr.ok,
		       "S2i MONEY ASSERTION: replace_geometry_scaffold against a pre-existing unattributed "
		       "object is REFUSED while the active element has no chunk of its own" );
		Check( sr.message.find( "build_element" ) != std::string::npos,
		       "S2i and the refusal names build_element" );
		Check( sess->ReadDocument() == docBefore,
		       "S2i MONEY ASSERTION: the document is byte-identical -- obj_sph's geometry slot was "
		       "NOT rebound" );
		Check( sess->ChunkElement( "obj_sph" ).empty(),
		       "S2i MONEY ASSERTION: obj_sph was NOT attributed to wizard" );
		Check( sess->ElementChunks( "wizard" ).empty(),
		       "S2i and the element still has zero chunks -- the gate was NOT permanently disarmed" );
		Check( sess->BuildPhaseRefusalCount() == refusalsBefore + 1,
		       "S2i the shared phase-refusal counter incremented exactly once" );
	}

	// Once the element has content -- landed the honest way, through
	// build_element -- the identical call succeeds: refinement by scaffold
	// is allowed once construction went through the clean room first.
	const Agent::AgentSession::AgentBuildElementResult br = sess->BuildElement( "wizard", 4.0 );
	Check( br.ok && !br.landed.empty(), "S2i build_element lands the element's first geometry" );

	{
		const Agent::AgentSession::AgentGeometryScaffoldResult sr =
			sess->ReplaceGeometryScaffold( "obj_sph", "sweep_rail", "s2iRep2", 1.0, 0.4, 1.0 );
		Check( sr.ok, "S2i MONEY ASSERTION: once the element has content, the same "
		       "replace_geometry_scaffold call succeeds" );
	}
}


//----------------------------------------------------------------------
// Arc 80 (2026-08-12): THE COMPOSE PHASE MAY NOT DELETE FORM.
//
// docs/agentic-redesign/79-clean-room-construction.md sec 8.2 measured the
// failure this closes: five builders produced 82 SDF parts, place_element
// converged in one call per element -- and then the COMPOSE phase removed
// all 31 objects, re-inserted them twice, removed 18 more, and rebuilt
// with ellipsoids and cylinders.  Two of 82 parts survived.  Nothing
// failed to apply; every removal was clean and deliberate.
//
// The rule is arc 78 sec 2.3's, extended one verb: ATTRIBUTE EVERYTHING,
// REFUSE ONLY ON FORM-BEARING CHUNKS.  Compose places; it does not
// destroy form.  Lights, cameras, film, rasterizers, materials and
// painters stay removable there, because a model that cannot re-light or
// re-aim cannot compose at all.
//----------------------------------------------------------------------

//! kScene plus a light and a second, unattributed object -- the compose
//! arm needs a NON-form-bearing chunk to prove it is still removable, and
//! kScene has no light of its own.
static const char* const kComposeRemoveScene =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 1\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 24\n\theight 24\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n"
	"omni_light\n{\n\tname key\n\tposition 0 2 4\n\tpower 40\n\tcolor 1 1 1\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_albedo\n\tcolor 0.5 0.5 0.5\n}\n\n"
	"lambertian_material\n{\n\tname mat_diffuse\n\treflectance pnt_albedo\n}\n\n"
	"sphere_geometry\n{\n\tname sph\n\tradius 0.8\n}\n\n"
	"standard_object\n{\n\tname obj_sph\n\tgeometry sph\n\tmaterial mat_diffuse\n}\n";

//! Take a gate-armed session from PLAN to COMPOSE with one geometry chunk
//! and one standard_object attributed to the first element.
static void ArcEightyToCompose( Agent::AgentSession& sess )
{
	sess.FileBuildPlan( TwoElementPlan() );
	sess.InsertChunk( S1Box( "wizard_body" ) );
	sess.InsertChunk( "standard_object\n{\n\tname wizard_obj\n\tgeometry wizard_body\n"
	                  "\tmaterial mat_diffuse\n}" );
	sess.FinishElement();
	sess.FinishElement();
}

static void TestComposePhaseRefusesRemovingForm()
{
	std::printf( "A80a: the compose phase refuses removing form-bearing chunks...\n" );
	const std::string tmp = TempPath( "agentcrud_a80a.RISEscene" );
	Job* pJob = LoadScene( kComposeRemoveScene, tmp );
	Check( pJob != nullptr, "A80a fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
	ArcEightyToCompose( *sess );
	Check( sess->BuildPhase() == Agent::AgentSession::AgentBuildPhase::Compose,
	       "A80a the session reaches the compose phase" );
	Check( sess->ChunkElement( "wizard_obj" ) == "wizard",
	       "A80a and the object it built is attributed to the wizard element" );

	const std::string docBefore = sess->ReadDocument();

	// (1) THE OBJECT.  This is the exact operation arc 79 sec 8.2 recorded
	//     31 times in one session.
	const Agent::AgentChunkResult r1 = sess->RemoveChunk( "wizard_obj", "" );
	Check( !r1.applied,
	       "A80a MONEY ASSERTION: removing a standard_object in the compose phase is REFUSED -- "
	       "the measured failure was a compose phase that removed all 31 objects and rebuilt "
	       "with primitives, with every operation applying cleanly" );
	Check( r1.message.find( "remove_chunk refused" ) != std::string::npos &&
	       r1.message.find( "form-bearing" ) != std::string::npos &&
	       r1.message.find( "reopen_element" ) != std::string::npos,
	       "A80a and the refusal names the verb, says what compose is for, and names the "
	       "deliberate way to do it anyway" );
	Check( sess->ReadDocument() == docBefore, "A80a RED-PROVE: the document is byte-identical" );
	Check( sess->BuildPhaseRefusalCount() == 1, "A80a it spends one shared refusal" );

	// (2) THE GEOMETRY behind it.
	const Agent::AgentChunkResult r2 = sess->RemoveChunk( "wizard_body", "" );
	Check( !r2.applied && r2.message.find( "form-bearing" ) != std::string::npos,
	       "A80a a GEOMETRY chunk is refused too -- the object places the form, the geometry IS "
	       "the form" );
	Check( sess->ReadDocument() == docBefore, "A80a RED-PROVE: still byte-identical" );
	Check( sess->BuildPhaseRefusalCount() == 2, "A80a the counter is exactly two" );

	// (3) THE LIGHT.  Arc 78 sec 2.3's governing rule, unchanged: a model
	//     that cannot re-light its own scene cannot compose it.
	const Agent::AgentChunkResult r3 = sess->RemoveChunk( "key", "" );
	Check( r3.applied,
	       "A80a MONEY ASSERTION: a LIGHT is still removable in the compose phase -- the rule is "
	       "ATTRIBUTE EVERYTHING, REFUSE ONLY ON FORM-BEARING CHUNKS, and lighting is exactly "
	       "what the compose phase is for" );
	Check( sess->BuildPhaseRefusalCount() == 2,
	       "A80a and an ALLOWED removal costs no refusal" );

	// (4) A PAINTER, for the same reason materials and painters are exempt
	//     from the cross-element arm: sharing one across elements is
	//     ordinary authoring.
	Check( sess->InsertChunk( "uniformcolor_painter\n{\n\tname a80_spare\n\tcolor 0.1 0.2 0.3\n}" ).applied,
	       "A80a a painter can be CREATED in compose (only geometry creation is refused)" );
	Check( sess->RemoveChunk( "a80_spare", "" ).applied,
	       "A80a and removed again" );

	// (5) A NAME THAT RESOLVES TO NOTHING is never refused by this rule --
	//     under-refusing is the correct direction of error.
	const Agent::AgentChunkResult r5 = sess->RemoveChunk( "a80_no_such_chunk", "" );
	Check( !r5.applied, "A80a removing an unknown chunk still fails" );
	Check( r5.message.find( "form-bearing" ) == std::string::npos,
	       "A80a MONEY ASSERTION: but NOT as a phase refusal -- an unresolvable name is left to "
	       "the remove itself, which reports it far better than a phase rule could" );
	Check( sess->BuildPhaseRefusalCount() == 2, "A80a and it costs no refusal" );

	pJob->release();
}

static void TestComposePhaseRemoveMixedBatchAndGiveUp()
{
	std::printf( "A80b: mixed remove_chunks batches, the shared cap, and the give-up...\n" );

	// ---- The MIXED batch: one light, one object.  WHOLE-CALL refusal. ----
	{
		const std::string tmp = TempPath( "agentcrud_a80b_mixed.RISEscene" );
		Job* pJob = LoadScene( kComposeRemoveScene, tmp );
		Check( pJob != nullptr, "A80b/mixed fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
		ArcEightyToCompose( *sess );
		const std::string docBefore = sess->ReadDocument();

		std::vector<std::string> targets;
		targets.push_back( "key" );          // a light -- removable on its own
		targets.push_back( "wizard_obj" );   // form-bearing -- not
		const Agent::AgentSession::AgentRemoveBatchResult br = sess->RemoveChunks( targets );
		Check( !br.applied,
		       "A80b MONEY ASSERTION: a MIXED batch is refused WHOLE -- remove_chunks is "
		       "all-or-nothing everywhere else in this verb, and tearing down the "
		       "non-form-bearing half of a batch would leave a scene the model never asked for" );
		Check( br.message.find( "1 of the 2 chunks named are form-bearing" ) != std::string::npos,
		       "A80b and the refusal says exactly how many of the named chunks triggered it" );
		Check( br.message.find( "removes all of its targets or none of them" ) != std::string::npos,
		       "A80b MONEY ASSERTION: and states the all-or-nothing outcome outright, so the "
		       "model knows what happened to the light it also named" );
		Check( sess->ReadDocument() == docBefore,
		       "A80b RED-PROVE: the document is byte-identical -- the LIGHT was not removed either" );
		Check( br.targetResults.size() == 2 && !br.targetResults[0].applied &&
		       !br.targetResults[1].applied,
		       "A80b and both per-target entries carry the batch verdict" );
		Check( sess->BuildPhaseRefusalCount() == 1,
		       "A80b a two-target batch costs ONE refusal, exactly as the creation arm does" );

		// A batch of ONLY non-form-bearing targets still applies.
		std::vector<std::string> ok;
		ok.push_back( "key" );
		Check( sess->RemoveChunks( ok ).applied,
		       "A80b an all-light batch applies untouched" );
		pJob->release();
	}

	// ---- The SHARED cap and the give-up: no model is ever stranded. ----
	{
		const std::string tmp = TempPath( "agentcrud_a80b_giveup.RISEscene" );
		Job* pJob = LoadScene( kComposeRemoveScene, tmp );
		Check( pJob != nullptr, "A80b/giveup fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
		ArcEightyToCompose( *sess );

		// Two refusals from the CREATION arm, one from the DELETE arm --
		// proving the counter is genuinely shared across the compose rules.
		Check( !sess->InsertChunk( S1Box( "a80_c1" ) ).applied, "A80b/giveup creation refusal 1" );
		Check( !sess->InsertChunk( S1Box( "a80_c2" ) ).applied, "A80b/giveup creation refusal 2" );
		Check( sess->BuildPhaseRefusalCount() == 2, "A80b/giveup two spent on creation" );
		Check( !sess->RemoveChunk( "wizard_obj", "" ).applied,
		       "A80b/giveup the delete arm refuses as the THIRD" );
		Check( sess->BuildPhaseRefusalCount() == 3,
		       "A80b MONEY ASSERTION: the delete arm shares ONE budget with the other compose "
		       "rules -- a model that cannot work the protocol should not have to exhaust two "
		       "separate budgets to get out of it" );
		Check( !sess->BuildPhaseGaveUp(), "A80b/giveup three is the cap, not the give-up" );

		const Agent::AgentChunkResult fourth = sess->RemoveChunk( "wizard_obj", "" );
		Check( fourth.applied,
		       "A80b MONEY ASSERTION: the FOURTH attempt GIVES UP and the removal PROCEEDS -- no "
		       "model is ever stranded by this rule" );
		Check( sess->BuildPhaseGaveUp(), "A80b/giveup the session has given up" );
		Check( fourth.message.find( "build phase" ) != std::string::npos,
		       "A80b/giveup and the give-up is stated in the call's own result, not only in a log" );
		Check( sess->RemoveChunk( "wizard_body", "" ).applied,
		       "A80b/giveup and nothing is refused for this reason afterwards" );
		pJob->release();
	}

	// ---- --agent-build-protocol=off disables it with everything else. ----
	{
		const std::string tmp = TempPath( "agentcrud_a80b_off.RISEscene" );
		Job* pJob = LoadScene( kComposeRemoveScene, tmp );
		Check( pJob != nullptr, "A80b/off fixture loads" );
		if( !pJob ) return;
		Agent::AgentSession::SetBuildProtocolDefaultEnabled( false );
		std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
		Agent::AgentSession::SetBuildProtocolDefaultEnabled( true );
		Check( !sess->BuildProtocolActive(), "A80b/off the protocol is inactive for this session" );
		Check( sess->RemoveChunk( "obj_sph", "" ).applied,
		       "A80b MONEY ASSERTION: with --agent-build-protocol=off a standard_object removal "
		       "applies with no interception at all -- the opt-out covers this arm exactly as it "
		       "covers the other three" );
		pJob->release();
	}

	// ---- The PIECES phase is untouched: an element may delete its own. ----
	{
		const std::string tmp = TempPath( "agentcrud_a80b_pieces.RISEscene" );
		Job* pJob = LoadScene( kComposeRemoveScene, tmp );
		Check( pJob != nullptr, "A80b/pieces fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
		Check( sess->FileBuildPlan( TwoElementPlan() ).ok, "A80b/pieces the plan files" );
		Check( sess->InsertChunk( S1Box( "wizard_body" ) ).applied, "A80b/pieces a geometry insert applies" );
		Check( sess->BuildPhase() == Agent::AgentSession::AgentBuildPhase::Pieces,
		       "A80b/pieces the session is in the pieces phase" );
		Check( sess->RemoveChunk( "wizard_body", "" ).applied,
		       "A80b MONEY ASSERTION: inside its OWN element window a model may still delete its "
		       "own geometry -- this arm fires only in compose, so revision during construction "
		       "is untouched" );
		Check( sess->BuildPhaseRefusalCount() == 0, "A80b/pieces and no refusal was spent" );
		pJob->release();
	}
}


//----------------------------------------------------------------------
// ARC 81 (2026-08-12): `light_scene` -- THE CLEAN-ROOM LIGHTING PASS,
// and the COMPOSE-phase refusal that forces its first use.
// Design: docs/agentic-redesign/81-creative-lighting-arc.md.
//
// THE MEASUREMENT BEHIND IT.  Against the hand-authored frontier benchmark
// on the same prompt, every agent run in this workstream produced 3-5
// lights spanning ~40x in power and used ONLY omni_light / spot_light /
// directional_light; the benchmark uses 8 lights spanning ~1000x, and
// ambient_light, hosek_wilkie_skylight and area/mesh lighting appear in NO
// agent run ever measured.  Lighting is authored in exactly the
// diluted-context regime arc 79's clean room relieved -- in the compose
// phase, 60-70 turns deep -- so it gets the same treatment.
//
// EVERY TEST HERE USES A MOCKED TEXT COMPLETER, through the same
// AgentSession::SetTextCompleter seam the arc-79 tests above use and for
// the same reason: no live provider call is ever made, and the assertions
// are about what the harness does with an answer.
//----------------------------------------------------------------------

//----------------------------------------------------------------------
// ARC 83 SLICE 1 (2026-08-12): the fixtures below are SPLIT ONE PER
// INTENT, because the verb is.
// Design: docs/agentic-redesign/83-staged-construction-plan.md sec 1, 2,
// 6.1; the measurement is 82 sec 10.4.
//
// `light_scene` is still ONE model-facing verb with the same name, the
// same params and the same result shape -- internally it is now a
// harness-driven loop: one PLANNING completion enumerating this scene's
// lighting intents, then ONE FRESH COMPLETION PER INTENT authoring
// EXACTLY ONE light source.  So a mocked run is a PLAN answer followed by
// one answer per intent, and the three answers below are arc 81's single
// six-chunk answer cut along exactly that seam: the same six chunks, the
// same four soloable lights afterwards, one light source at a time.
//----------------------------------------------------------------------

//! The PLAN step's answer: three lighting intents, one per line.  No
//! numbering and no prose, which is the shape the planning prompt asks
//! for; A83a covers the shapes it merely tolerates.
static const char* const kThreeIntentPlan =
	"sunlight arriving on the sphere from above and behind the camera\n"
	"a soft panel filling the shadowed left side of the sphere\n"
	"a low warm wash across the ground under the sphere\n";

//! A one-line plan, for the tests whose subject is what happens INSIDE
//! one intent rather than across the loop.
static const char* const kOneIntentPlan =
	"a single warm source above the sphere\n";

//! Intent 1's answer: ONE light source, a zero-area point light.
static const char* const kLightingAnswerKey =
	"omni_light\n{\n\tname lit_key\n\tposition 2 3 4\n\tcolor 1 0.95 0.9\n\tpower 60\n}\n";

//! Intent 2's answer: ONE light source, a complete AREA light (painter +
//! emissive material + geometry + object).  It matters twice over -- it
//! is the palette entry no agent run has ever used, and its geometry is
//! what proves light_scene's insertion is exempt from the compose-phase
//! creation ban.  Arc 83 exists so that a whole response can be spent on
//! exactly this four-chunk shape.
static const char* const kLightingAnswerArea =
	"uniformcolor_painter\n{\n\tname lit_panel_pnt\n\tcolor 1 0.8 0.6\n}\n"
	"lambertian_luminaire_material\n{\n\tname lit_panel_mat\n\texitance lit_panel_pnt\n"
	"\tmaterial none\n\tscale 20\n}\n"
	"box_geometry\n{\n\tname lit_panel_geo\n\twidth 2\n\theight 0.05\n\tdepth 1.5\n}\n"
	"standard_object\n{\n\tname lit_panel_obj\n\tgeometry lit_panel_geo\n"
	"\tmaterial lit_panel_mat\n\tposition 0 3 1\n}\n";

//! Intent 3's answer: ONE light source, a directional light.
static const char* const kLightingAnswerFill =
	"directional_light\n{\n\tname lit_fill\n\tdirection 0.3 0.7 0.6\n"
	"\tcolor 0.4 0.5 0.7\n\tpower 1.2\n}\n";

//! A completer for a clean three-intent run: the plan, then one answer
//! per intent.  FOUR completions, six chunks, four soloable lights --
//! the same end state arc 81's single answer produced, reached the way
//! arc 83 reaches it.
static Agent::AgentSession::AgentTextCompleter MakeLightingCompleter(
	int* callsOut = nullptr, std::vector<std::string>* promptsOut = nullptr )
{
	return MakeFakeCompleter( { kThreeIntentPlan, kLightingAnswerKey, kLightingAnswerArea,
	                            kLightingAnswerFill }, callsOut, promptsOut );
}

//! One hand-authored light chunk, for the gate assertions.
static std::string A81Light( const char* name )
{
	return std::string( "omni_light\n{\n\tname " ) + name +
		"\n\tposition 1 2 3\n\tcolor 1 1 1\n\tpower 30\n}";
}

//! A session on `kScene` that has reached the COMPOSE phase: the gate's
//! arming condition, and the phase light_scene is designed for.  The
//! element is finished empty on purpose -- nothing about the lighting rule
//! depends on what was built.
static std::unique_ptr<Agent::AgentSession> A81ComposeSession( Job* pJob )
{
	std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
	sess->FileBuildPlan( WizardOnlyPlan() );
	sess->FinishElement();
	return sess;
}

//! A81a: the happy path -- a whole lighting design lands, the prompt is
//! the composition this arc exists for, and each light's contribution is
//! MEASURED rather than asserted.
static void TestLightSceneHappyPath()
{
	std::printf( "A81a: light_scene designs a scene's lighting in a fresh context...\n" );
	const std::string tmp = TempPath( "agentcrud_a81a.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "A81a fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = A81ComposeSession( pJob );

	int calls = 0;
	std::vector<std::string> prompts;
	sess->SetTextCompleter( MakeLightingCompleter( &calls, &prompts ) );
	Check( sess->BuildPhase() == Agent::AgentSession::AgentBuildPhase::Compose,
	       "A81a the session is in the compose phase" );

	// WHAT IT COSTS.  Reported rather than asserted, the same way arc 80
	// reports the inventory's overhead: one completion (mocked to ~0 here,
	// so this figure is the HARNESS's share -- one inventory pass plus the
	// all-lights reference and one small render per light), paid ONCE per
	// scene rather than once per render, which is the entire reason N solo
	// renders are affordable at all.
	const std::chrono::steady_clock::time_point lt0 = std::chrono::steady_clock::now();
	const Agent::AgentSession::AgentLightSceneResult r = sess->LightScene();
	const std::chrono::steady_clock::time_point lt1 = std::chrono::steady_clock::now();
	std::printf( "[arc 81 cost] one light_scene call (24x24 scene, mocked completion): %.1f ms "
	             "-- one inventory pass + %d solo renders + 1 all-lights reference\n",
	             std::chrono::duration<double, std::milli>( lt1 - lt0 ).count(), r.soloedCount );
	Check( r.ok, "A81a light_scene succeeds" );
	Check( calls == 4,
	       "A81a MONEY ASSERTION: exactly FOUR completions for ONE call -- one PLAN plus one per "
	       "planned intent, and no retry on a clean answer. Arc 81 spent one completion on the "
	       "whole category; 82 sec 10.4 measured that a completion yields one response-worth of "
	       "output whatever is asked of it, so the category was the wrong unit" );
	Check( !r.retryRan, "A81a and the result says the retry did not run" );
	Check( r.chunksExtracted == 6, "A81a all six chunks were extracted, across the three intents" );
	Check( r.landed.size() == 6, "A81a and all six landed" );
	Check( r.rejected.empty(), "A81a with nothing rejected" );

	// ---- THE PLAN AND THE LOOP, as numbers the result carries.
	Check( r.intents.size() == 3 && r.intentsReturned == 3 && !r.intentsTruncated,
	       "A81a the plan step returned three intents and none was cut" );
	Check( r.perIntent.size() == 3 && r.intentsBuilt == 3,
	       "A81a one outcome per intent, and all three built something" );
	Check( r.completionsSpent == 4 && r.completionsSpent == calls,
	       "A81a MONEY ASSERTION: the completions the result REPORTS are the completions the "
	       "provider actually saw -- the per-session cap bounds CALLS, so a call that spends 1+N "
	       "completions has to say so or the spend becomes invisible" );
	Check( r.perIntent[0].completions == 1 && r.perIntent[1].completions == 1 &&
	       r.perIntent[2].completions == 1,
	       "A81a with one completion charged to each intent" );
	Check( r.perIntent[0].landed.size() == 1 && r.perIntent[1].landed.size() == 4 &&
	       r.perIntent[2].landed.size() == 1,
	       "A81a and each intent's landed chunks are reported per intent, not only in a total -- "
	       "83 sec 6's standing rule after 82 sec 10.1: never a category total that multiplies a "
	       "stable per-unit number by a variable unit count" );
	Check( r.areaLightsBuilt == 1 && r.zeroAreaLightsBuilt == 2 && r.skyLightsBuilt == 0 &&
	       r.otherLightsBuilt == 0,
	       "A81a MONEY ASSERTION: the FORM of each light built is counted -- one area light "
	       "(an emitting surface) and two zero-area idealizations. Area-light SHARE is arc 83's "
	       "falsifier (82 sec 10.4): if it does not move when each light gets its own completion, "
	       "cost was never the mechanism" );
	Check( r.perIntent[1].form == "area",
	       "A81a and the classification is read from what LANDED -- the emissive material is what "
	       "makes intent 2's four chunks an area light" );
	Check( sess->ReadDocument().find( "lit_key" ) != std::string::npos &&
	       sess->ReadDocument().find( "lit_panel_obj" ) != std::string::npos,
	       "A81a the lights are really in the document" );
	Check( sess->ReadDocument().find( "lit_panel_geo" ) != std::string::npos,
	       "A81a MONEY ASSERTION: the AREA light's GEOMETRY landed in the COMPOSE phase -- "
	       "light_scene's own insertion is exempt from the compose-phase creation ban, because "
	       "an emissive quad is a light source and that ban exists to stop a model replacing form "
	       "it already built" );
	Check( sess->BuildPhaseRefusalCount() == 0,
	       "A81a and no phase refusal was spent doing it" );

	// ---- THE PROMPTS.  Host-composed, and carrying the composition that is
	// the whole point of this arc: the arc-80 inventory, so the lighting is
	// designed against where the objects actually are.  Prompt 0 is the PLAN
	// and prompts 1..3 are the per-intent light requests; the palette
	// assertions below are about a per-intent prompt, because that is the one
	// that authors chunks.
	Check( prompts.size() == 4,
	       "A81a four prompts were composed -- the plan, then one per intent" );
	if( prompts.size() == 4 ) {
		Check( prompts[0].find( "LIGHTING INTENTS" ) != std::string::npos &&
		       prompts[0].find( "ONE INTENT PER LINE" ) != std::string::npos,
		       "A81a the FIRST prompt asks for lighting INTENTS, one per line" );
		Check( prompts[0].find( "at most 6 intents" ) != std::string::npos,
		       "A81a MONEY ASSERTION: and states the budget as a number -- a budget the model is "
		       "not told is a budget it can only discover by having its plan cut" );
		Check( prompts[0].find( "Example:" ) == std::string::npos &&
		       prompts[0].find( "clippedplane_geometry" ) == std::string::npos,
		       "A81a MONEY ASSERTION: the plan prompt carries NO palette and NO grammar -- it "
		       "writes no scene text, and arc 79 sec 1 measured that prepended text competes with "
		       "the construction it precedes" );
		Check( prompts[0].find( "SCENE INVENTORY" ) != std::string::npos &&
		       prompts[0].find( "THE CAMERA:" ) != std::string::npos,
		       "A81a while the scene facts it plans against ARE there" );
	}
	if( prompts.size() >= 2 ) {
		const std::string& p = prompts[1];
		Check( p.find( "ONE LIGHT SOURCE" ) != std::string::npos &&
		       p.find( "intent 1 of 3" ) != std::string::npos,
		       "A81a MONEY ASSERTION: a per-intent prompt asks for ONE LIGHT SOURCE and says which "
		       "intent of how many it is -- 83 sec 6.1: the unit must be sized so one response is "
		       "one unit's work, and for lighting that unit is one light, not one category" );
		Check( p.find( "sunlight arriving on the sphere" ) != std::string::npos,
		       "A81a and it carries that intent's own line from the plan" );
		Check( p.find( "SCENE INVENTORY" ) != std::string::npos &&
		       p.find( "obj_sph" ) != std::string::npos,
		       "A81a MONEY ASSERTION: the arc-80 SCENE INVENTORY is in the prompt, naming the "
		       "scene's actual objects -- this is the part build_element could not have, and it "
		       "is what makes the lighting designed for THIS scene rather than for a description "
		       "of one" );
		Check( p.find( "THE CAMERA:" ) != std::string::npos &&
		       p.find( "field of view" ) != std::string::npos,
		       "A81a the camera pose and FOV are stated" );
		Check( p.find( "WORLD BOUNDS" ) != std::string::npos,
		       "A81a and so are the scene's world bounds" );
		Check( p.find( "LIGHTS ALREADY IN THE SCENE (1)" ) != std::string::npos &&
		       p.find( "obj_emit -- emissive object" ) != std::string::npos,
		       "A81a and what is ALREADY lighting it, so the pass can design around it rather "
		       "than duplicate it -- including the fixture's emissive quad, which is a light "
		       "source with no light chunk and would be invisible to a naive light-manager scan" );

		// THE PALETTE -- ARC 81 FIX-ROUND (2026-08-12): ordered and weighted
		// by PHYSICS, and encoded as COPYABILITY rather than as advice.  The
		// area light is FIRST and is the only entry with a worked example;
		// the three zero-area kinds keep their schema and lose theirs;
		// ambient_light is gone entirely, because it is refused outright.
		Check( p.find( "omni_light" ) != std::string::npos &&
		       p.find( "spot_light" ) != std::string::npos &&
		       p.find( "directional_light" ) != std::string::npos &&
		       p.find( "hosek_wilkie_skylight" ) != std::string::npos,
		       "A81a the four light-chunk kinds this surface authors are all in the palette" );
		// ORDER, asserted through the palette's own numbering rather than
		// through raw find() positions: `hosek_wilkie_skylight` is legitimately
		// named earlier, in the NAMING paragraph (it is the one kind taking no
		// `name`), so a bare position compare would be testing the wrong thing.
		Check( p.find( "1. AREA / MESH LIGHT" ) != std::string::npos &&
		       p.find( "2. hosek_wilkie_skylight" ) != std::string::npos &&
		       p.find( "3. omni_light" ) != std::string::npos,
		       "A81a MONEY ASSERTION: the AREA light is palette entry ONE, ahead of the sky and "
		       "of the three idealizations -- most scenes are lit by an emitting surface, and the "
		       "palette encodes that by ORDER, not by telling the model what to prefer" );
		Check( p.find( "\texitance\tpnt_window\n" ) != std::string::npos &&
		       p.find( "clippedplane_geometry\n{\n" ) != std::string::npos &&
		       p.find( "\tsolar_elevation\t\t22\n" ) != std::string::npos,
		       "A81a MONEY ASSERTION: the area light carries a LITERAL, parseable four-chunk "
		       "example -- arc 79 sec 8.1 records an entire session's mechanism lost to a syntax "
		       "slip that prose did not prevent and a literal example did, and A81h proves this "
		       "particular one really parses" );
		Check( p.find( "omni_light\n{" ) == std::string::npos &&
		       p.find( "spot_light\n{" ) == std::string::npos &&
		       p.find( "directional_light\n{" ) == std::string::npos,
		       "A81a MONEY ASSERTION: omni / spot / directional carry NO worked example -- an "
		       "example is the one lever this workstream has measured moving what a model "
		       "writes, so what is copyable IS the policy; their registry schema is still there" );
		Check( p.find( "ZERO-AREA IDEALIZATIONS" ) != std::string::npos &&
		       p.find( "ZERO-AREA IDEALIZATIONS" ) < p.find( "3. omni_light" ),
		       "A81a and the three are stated as what they are -- zero-area, no penumbra, for "
		       "special cases -- as a FACT, not as a discouragement" );
		Check( p.find( "ambient_light" ) == std::string::npos,
		       "A81a MONEY ASSERTION: ambient_light is NOT in the palette at all. It is refused "
		       "on every creating path (A81g), and offering a model an option the surface will "
		       "then refuse is the false-clause class this design family exists to avoid" );
		Check( p.find( "FROM a lit surface TOWARD the light" ) != std::string::npos,
		       "A81a and the one convention a fresh context cannot recover from a parameter list "
		       "-- directional_light's direction sense -- is stated as fact" );

	}

	// NO ADVICE, ON EVERY PROMPT THIS CALL SENDS -- the plan prompt included,
	// which is where the temptation is largest now that something host-side
	// decides how many lights there will be.  Telling it to be dramatic, to
	// use many lights, or to spread its power range would contaminate the
	// very thing being measured, and advice measures ~0 in this workstream
	// anyway.  (Deliberately NOT "at least": the arc-80 inventory text these
	// prompts embed says "covered at least one pixel", which is a
	// measurement, not advice.)
	{
		static const char* const kBannedAdvice[] = {
			"dramatic", "many lights", "be bold", "contrast ratio",
			"key light", "three-point", "should use", "more lights" };
		for( std::size_t q = 0; q < prompts.size(); ++q )
			for( std::size_t i = 0; i < sizeof( kBannedAdvice ) / sizeof( kBannedAdvice[0] ); ++i ) {
				Check( prompts[q].find( kBannedAdvice[i] ) == std::string::npos,
				       std::string( "A81a MONEY ASSERTION: prompt " ) + std::to_string( q ) +
				       " never says \"" + kBannedAdvice[i] +
				       "\" -- it gives the palette, the scene, the mood and the syntax, and nothing "
				       "else; the light count and power range are what this arc MEASURES, so the "
				       "prompt must not put them there" );
			}
	}

	// ---- THE CONTRIBUTIONS.  Measured by soloing, which is affordable
	// because this call runs once per scene.
	Check( r.soloableLightCount == 4,
	       "A81a four light sources are soloable afterwards -- the two explicit lights the pass "
	       "added, the emissive panel it added, and the fixture's own emissive quad (got " +
	       std::to_string( r.soloableLightCount ) + ")" );
	Check( r.soloedCount == 4, "A81a and all four were actually soloed" );
	Check( r.contributions.size() == 4, "A81a with one entry each" );
	Check( r.allLightsMeanLuma > 0.0,
	       "A81a the all-lights reference frame is a real measurement" );
	bool everyOneMeasured = true, descending = true;
	double prev = 1e300;
	for( std::size_t i = 0; i < r.contributions.size(); ++i ) {
		if( !r.contributions[i].soloed ) everyOneMeasured = false;
		if( r.contributions[i].meanLuma > prev ) descending = false;
		prev = r.contributions[i].meanLuma;
	}
	Check( everyOneMeasured,
	       "A81a MONEY ASSERTION: every light's ACTUAL contribution was measured by rendering the "
	       "scene with only it lit -- N small renders are affordable here and nowhere else, "
	       "because light_scene runs once per scene rather than once per render" );
	Check( descending, "A81a and they come back brightest first" );
	Check( r.message.find( "Contribution of each light" ) != std::string::npos,
	       "A81a the report states what was measured" );
	Check( r.message.find( "not additive" ) != std::string::npos,
	       "A81a MONEY ASSERTION: and states outright that the solo figures do NOT sum to the "
	       "all-lights frame -- implying otherwise would be a false clause in a model-facing "
	       "payload, the class arc 79 sec 8.1 records the cost of" );

	// FACTS ONLY, exactly as the inventory and the tonal fact are.
	static const char* const kBannedVerdict[] = { "consider", "should", "too ", "flat", "needs" };
	for( std::size_t i = 0; i < sizeof( kBannedVerdict ) / sizeof( kBannedVerdict[0] ); ++i ) {
		Check( r.message.find( kBannedVerdict[i] ) == std::string::npos,
		       std::string( "A81a the result never says \"" ) + kBannedVerdict[i] +
		       "\" -- it reports what landed and what each light measures, and stops" );
	}
}

//! A81b: the admissibility rule and the ONE repair retry.  A lighting pass
//! that returns a camera, or geometry with nothing emissive to put on it,
//! has those chunks REJECTED with a reason -- never dropped silently -- and
//! that rejection text drives exactly one retry.
static void TestLightSceneAdmissibilityAndRetry()
{
	std::printf( "A81b: light_scene rejects what is not lighting, and repairs once...\n" );
	const std::string tmp = TempPath( "agentcrud_a81b.RISEscene" );

	// (1) A CAMERA and a BARE GEOMETRY are refused; the omni_light lands.
	{
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "A81b/reject fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = A81ComposeSession( pJob );

		// The painter is admissible and lands; the camera and the bare
		// geometry are not.  The light source itself is deliberately absent
		// from this answer, so the repair retry is free to supply one -- an
		// intent that has already landed a light source gets a SECOND-source
		// rejection instead, which is A83b's subject, not this one's.
		const std::string mixed =
			"uniformcolor_painter\n{\n\tname lit_ok_pnt\n\tcolor 1 1 1\n}\n"
			"pinhole_camera\n{\n\tname lit_cam\n\tlocation 0 0 9\n\tlookat 0 0 0\n\tfov 50\n}\n"
			"box_geometry\n{\n\tname lit_bare_geo\n\twidth 1\n\theight 1\n\tdepth 1\n}\n";
		int calls = 0;
		std::vector<std::string> prompts;
		// ARC 83: ONE intent, so this test's subject stays what happens
		// INSIDE an intent -- the plan, the flawed answer, the repair.
		sess->SetTextCompleter( MakeFakeCompleter( { kOneIntentPlan, mixed, kLightingAnswerArea },
		                                            &calls, &prompts ) );

		const Agent::AgentSession::AgentLightSceneResult r = sess->LightScene();
		Check( r.ok, "A81b/reject the call reports its outcome" );
		bool sawCameraReject = false, sawBareGeometryReject = false;
		for( std::size_t i = 0; i < r.rejected.size(); ++i ) {
			if( r.rejected[i].name == "lit_cam" ||
			    r.rejected[i].reason.find( "pinhole_camera" ) != std::string::npos )
				sawCameraReject = true;
			if( r.rejected[i].reason.find( "carrier of an emissive material" ) != std::string::npos )
				sawBareGeometryReject = true;
		}
		Check( sawCameraReject,
		       "A81b/reject MONEY ASSERTION: a camera is rejected -- re-aiming the camera is not "
		       "lighting, and this verb's insertion is exempt from the compose-phase creation "
		       "ban, so what it may admit has to be bounded here" );
		Check( sawBareGeometryReject,
		       "A81b/reject MONEY ASSERTION: geometry with no emissive material in the same answer "
		       "is rejected -- the exemption admits an area light's carrier, not arbitrary form" );
		Check( sess->ReadDocument().find( "lit_cam" ) == std::string::npos &&
		       sess->ReadDocument().find( "lit_bare_geo" ) == std::string::npos,
		       "A81b/reject and neither reached the document" );
		Check( sess->ReadDocument().find( "lit_ok_pnt" ) != std::string::npos,
		       "A81b/reject while the admissible chunk of the same answer landed -- partial success "
		       "is first-class, never an all-or-nothing on a per-chunk rule" );

		// THE ONE REPAIR RETRY, driven by the harness's own rejection text,
		// and PER INTENT since arc 83.
		Check( r.retryRan && r.perIntent.size() == 1 && r.perIntent[0].retryRan,
		       "A81b/reject the one repair retry ran, and is charged to the intent that needed it" );
		Check( calls == 3,
		       "A81b/reject MONEY ASSERTION: exactly THREE completions -- the plan, this intent's "
		       "one attempt and its one repair retry, then stop" );
		Check( r.perIntent[0].completions == 2 && r.completionsSpent == 3,
		       "A81b/reject and the intent is charged 2 of the 3" );
		Check( r.retrySucceeded, "A81b/reject and it landed more chunks" );
		Check( prompts.size() == 3 &&
		       prompts[2].find( "A PREVIOUS ANSWER TO THIS SAME REQUEST WAS PARTLY REJECTED" )
		       != std::string::npos &&
		       prompts[2].find( "carrier of an emissive material" ) != std::string::npos,
		       "A81b/reject MONEY ASSERTION: the retry is corrected by this harness's own rejection "
		       "text VERBATIM, not by a paraphrase of it" );
		Check( sess->ReadDocument().find( "lit_panel_obj" ) != std::string::npos,
		       "A81b/reject and the corrected answer's area light landed" );
		pJob->release();
	}

	// (2) A DUPLICATE NAME is rejected and NOT renamed -- the naming
	//     contract this verb actually has.  There is no `<element>_` prefix
	//     check: lights are scene-global and belong to no element, so the
	//     prefix idiom's precondition (an owning element whose members must
	//     be identifiable) does not hold.  UNIQUENESS is the real
	//     constraint, and InsertChunks already enforces it.
	{
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "A81b/dupe fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = A81ComposeSession( pJob );
		// The SAME name from TWO INTENTS: the second is a real collision
		// against the first, which is exactly the shape a collision against
		// pre-existing scene content has, and it is InsertChunks' own
		// duplicate-name rejection that catches it -- which IS this verb's
		// naming contract, since there is no prefix rule to catch it first.
		// ARC 83: across intents rather than within one answer, because two
		// lights in ONE answer are now stopped by the one-source rule first
		// (A83b) and would never reach the duplicate check.
		static const char* const kTwoIntentPlan =
			"a warm source above the sphere\na cool source behind the sphere\n";
		const std::string collidesA =
			"omni_light\n{\n\tname twice\n\tposition 1 2 3\n\tcolor 1 1 1\n\tpower 40\n}\n";
		const std::string collidesB =
			"omni_light\n{\n\tname twice\n\tposition 4 5 6\n\tcolor 1 1 1\n\tpower 80\n}\n";
		int calls = 0;
		sess->SetTextCompleter( MakeFakeCompleter( { kTwoIntentPlan, collidesA, collidesB },
		                                            &calls ) );
		const Agent::AgentSession::AgentLightSceneResult r = sess->LightScene();
		Check( r.landed.size() == 1, "A81b/dupe the first of the two landed" );
		Check( calls == 4 && r.perIntent.size() == 2 && r.perIntent[0].completions == 1 &&
		       r.perIntent[1].completions == 2,
		       "A81b/dupe MONEY ASSERTION: the second intent's collision cost the SECOND intent a "
		       "repair retry and cost the first one nothing -- the loop's failures are per unit" );
		Check( r.intentsBuilt == 1,
		       "A81b/dupe and exactly one of the two intents built something" );
		bool sawCollision = false;
		for( std::size_t i = 0; i < r.rejected.size(); ++i )
			if( r.rejected[i].reason.find( "twice" ) != std::string::npos ) sawCollision = true;
		Check( sawCollision,
		       "A81b/dupe MONEY ASSERTION: the colliding name is REJECTED and reported, never "
		       "silently renamed -- renaming would break the references the pass wrote between "
		       "its own chunks, which is why uniqueness (not a prefix) is this verb's naming rule" );
		Check( r.retryRan, "A81b/dupe and the rejection drove the one repair retry" );
		pJob->release();
	}

	// (3) A CHUNK WITHOUT the prefix any element would have imposed is
	//     accepted: proving the transplanted idiom really is absent, not
	//     merely unmentioned.
	{
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "A81b/noprefix fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = A81ComposeSession( pJob );
		sess->SetTextCompleter( MakeFakeCompleter( { kOneIntentPlan,
			"omni_light\n{\n\tname moonlight\n\tposition 0 9 2\n\tcolor 0.6 0.7 1\n\tpower 90\n}\n" } ) );
		const Agent::AgentSession::AgentLightSceneResult r = sess->LightScene();
		Check( r.ok && r.landed.size() == 1 && r.landed[0] == "moonlight",
		       "A81b/noprefix MONEY ASSERTION: a light named with NO element prefix lands -- lights "
		       "are scene-global, so the `<element>_` rule build_element enforces has no "
		       "precondition here and is deliberately absent" );
		pJob->release();
	}
}

//! A81c: the capability refusal and the per-session spend cap -- the two
//! bounds `build_element` carries, carried identically.
static void TestLightSceneCapabilityAndCap()
{
	std::printf( "A81c: light_scene's capability refusal and per-session cap...\n" );
	const std::string tmp = TempPath( "agentcrud_a81c.RISEscene" );

	{
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "A81c/cap-refusal fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = A81ComposeSession( pJob );
		Check( !sess->BuildCapable(), "A81c no completer is installed" );
		const std::string docBefore = sess->ReadDocument();
		const Agent::AgentSession::AgentLightSceneResult r = sess->LightScene();
		Check( !r.ok && r.capabilityRefusal,
		       "A81c a provider that cannot run a separate completion refuses honestly" );
		Check( r.message.find( "authoring light chunks directly" ) != std::string::npos,
		       "A81c and says outright that hand authoring is not blocked by it" );
		Check( sess->ReadDocument() == docBefore, "A81c with the document untouched" );
		pJob->release();
	}
	{
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "A81c/cap fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = A81ComposeSession( pJob );
		int calls = 0;
		// A completer that always FAILS, so each call is cheap and the cap
		// is what stops the sequence rather than the document filling up.
		Agent::AgentSession::AgentTextCompleter c;
		c.supported    = true;
		c.providerName = "mock";
		c.modelId      = "mock-lighting-1";
		c.complete = [&calls]( const std::string& ) -> Agent::AgentSession::AgentTextCompletionOutcome
		{
			++calls;
			Agent::AgentSession::AgentTextCompletionOutcome o;
			o.error = "mock refuses";
			return o;
		};
		sess->SetTextCompleter( c );

		Agent::AgentSession::AgentLightSceneResult last;
		for( int i = 0; i < Agent::AgentSession::kLightSceneMaxPerSession; ++i )
			last = sess->LightScene();
		// ARC 83: the PLAN step fails first, so the loop never starts and the
		// call costs exactly one completion.  There is no retry of the plan.
		Check( !last.ok && last.planFailure.find( "mock refuses" ) != std::string::npos &&
		       last.perIntent.empty() && last.completionsSpent == 1,
		       "A81c MONEY ASSERTION: a failed PLAN spends one completion and no per-intent one -- "
		       "the loop cannot run without units, and a second plan completion would double the "
		       "cheapest part of the call to guess at the reason" );
		Check( last.message.find( "no intent to build" ) != std::string::npos &&
		       last.message.find( "the plan step has no retry" ) != std::string::npos,
		       "A81c and the result says both facts rather than reporting an empty success" );
		Check( calls == Agent::AgentSession::kLightSceneMaxPerSession,
		       "A81c the four capped calls reached the provider exactly four times (got " +
		       std::to_string( calls ) + ")" );
		const int callsAtCap = calls;
		const Agent::AgentSession::AgentLightSceneResult over = sess->LightScene();
		Check( !over.ok && over.message.find( "per-session cap" ) != std::string::npos,
		       "A81c MONEY ASSERTION: the call past the per-session cap is refused and says so" );
		Check( over.message.find( "cap on CALLS" ) != std::string::npos,
		       "A81c MONEY ASSERTION: and says the cap counts CALLS -- since arc 83 one call spends "
		       "one planning completion plus one per intent, so 'calls' and 'completions' are "
		       "different numbers and a message that conflated them would be false" );
		Check( calls == callsAtCap,
		       "A81c and it reached the provider ZERO further times -- the cap bounds real money" );
		pJob->release();
	}
}

//! A81d: THE COMPOSE-PHASE GATE.  In compose, the first light-authoring
//! edit must come from light_scene; a hand-authored light chunk is refused
//! while it has not run, and permitted once it has.  It shares
//! RefuseForPhase_'s 3-refusal cap and give-up with the other three arms.
static void TestComposePhaseFirstLightRefusal()
{
	std::printf( "A81d: in compose, the first light must come from light_scene...\n" );
	const std::string tmp = TempPath( "agentcrud_a81d.RISEscene" );

	// (1) REFUSED, naming the verb; the batch surface refuses atomically.
	{
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "A81d/refuse fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = A81ComposeSession( pJob );
		sess->SetTextCompleter( MakeLightingCompleter() );

		const std::string docBefore = sess->ReadDocument();
		const Agent::AgentChunkResult r1 = sess->InsertChunk( A81Light( "hand_key" ) );
		Check( !r1.applied && r1.status == "rejected",
		       "A81d MONEY ASSERTION: a hand-authored light chunk is refused in compose while "
		       "light_scene has not run" );
		Check( r1.message.find( "light_scene" ) != std::string::npos,
		       "A81d and the refusal names light_scene" );
		Check( r1.kind == "omni_light" && r1.name == "hand_key",
		       "A81d with the identity echo every other refusal honours" );
		Check( sess->ReadDocument() == docBefore, "A81d the document is byte-identical" );
		Check( sess->BuildPhaseRefusalCount() == 1,
		       "A81d it burns exactly one slot of the SHARED phase counter (the fourth arm)" );

		// A NON-light chunk is untouched by this rule.
		Check( sess->InsertChunk(
			"uniformcolor_painter\n{\n\tname a81d_pnt\n\tcolor 0.2 0.3 0.4\n}" ).applied,
		       "A81d a painter is never refused by the lighting rule" );

		{
			std::vector<std::string> batch;
			batch.push_back( A81Light( "hand_b1" ) );
			batch.push_back( "uniformcolor_painter\n{\n\tname a81d_b2\n\tcolor 1 1 1\n}" );
			const std::vector<Agent::AgentChunkResult> rs = sess->InsertChunks( batch );
			Check( rs.size() == 2 && !rs[0].applied && !rs[1].applied,
			       "A81d insert_chunks is refused as a WHOLE -- half a batch is a state nobody "
			       "asked for" );
			Check( sess->BuildPhaseRefusalCount() == 2, "A81d one more slot of the same counter" );
		}

		// (2) light_scene ITSELF is never refused by the rule it arms, and
		//     running it LIFTS the refusal permanently.
		const Agent::AgentSession::AgentLightSceneResult lr = sess->LightScene();
		Check( lr.ok && lr.landed.size() == 6,
		       "A81d MONEY ASSERTION: the clean room's own insertion is not refused by the "
		       "clean-room rule" );
		Check( sess->InsertChunk( A81Light( "hand_after" ) ).applied,
		       "A81d MONEY ASSERTION: once light_scene has run, hand authoring is allowed -- "
		       "construction through the clean room, refinement in the model's hands" );
		Check( sess->InsertChunk( A81Light( "hand_after2" ) ).applied,
		       "A81d -- and stays allowed" );
		Check( sess->BuildPhaseRefusalCount() == 2, "A81d with no further refusals counted" );
		pJob->release();
	}

	// (3) THE CAP AND THE GIVE-UP.  Three refusals, then the phase rules
	//     stop intercepting -- shared with the other arms, so no model is
	//     ever stranded.
	{
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "A81d/giveup fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = A81ComposeSession( pJob );
		sess->SetTextCompleter( MakeLightingCompleter() );

		for( int i = 0; i < 3; ++i ) {
			const Agent::AgentChunkResult r =
				sess->InsertChunk( A81Light( ( "hand_" + std::to_string( i ) ).c_str() ) );
			Check( !r.applied, "A81d/giveup refusal " + std::to_string( i + 1 ) + " of 3" );
		}
		Check( sess->BuildPhaseRefusalCount() == 3, "A81d/giveup the cap is reached" );
		const Agent::AgentChunkResult r4 = sess->InsertChunk( A81Light( "hand_3" ) );
		Check( r4.applied,
		       "A81d/giveup MONEY ASSERTION: the fourth call is LET THROUGH -- the shared cap gives "
		       "up rather than refusing a model forever" );
		Check( r4.message.find( "stopped intercepting" ) != std::string::npos,
		       "A81d/giveup and the give-up notice rides the call's own result, so a trajectory "
		       "census sees the event" );
		pJob->release();
	}

	// (4) PROTOCOL OFF disables the refusal with everything else -- and
	//     light_scene itself still WORKS, because lighting needs no element
	//     window and a protocol-off session has no phases at all.
	{
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "A81d/off fixture loads" );
		if( !pJob ) return;
		Agent::AgentSession::SetBuildProtocolDefaultEnabled( false );
		std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
		Agent::AgentSession::SetBuildProtocolDefaultEnabled( true );
		sess->SetTextCompleter( MakeLightingCompleter() );
		Check( !sess->BuildProtocolActive(), "A81d/off the protocol is inactive for this session" );

		Check( sess->InsertChunk( A81Light( "hand_off" ) ).applied,
		       "A81d/off MONEY ASSERTION: a hand-authored light is NOT refused with the protocol "
		       "off -- the gate dies with --agent-build-protocol=off like its three siblings" );
		const Agent::AgentSession::AgentLightSceneResult lr = sess->LightScene();
		Check( lr.ok,
		       "A81d/off MONEY ASSERTION: and light_scene STILL WORKS with the protocol off -- "
		       "unlike build_element it needs no active element, and refusing it on a session "
		       "that has no phases at all would be exactly the over-refusal arc 78 sec 2.3 names "
		       "as this design family's worst failure mode" );
		pJob->release();
	}
}

//! A81e: THE SEAM.  Arc 78 sec 2.3 deliberately ALLOWS lights during
//! element windows -- a model that cannot light a part cannot see it.
//! Those lights must not be refused by this gate, and they must not
//! DISARM it either: the condition is "has light_scene run", not "does the
//! scene have lights".
static void TestPiecesPhaseLightsNeitherRefusedNorDisarming()
{
	std::printf( "A81e: pieces-phase lights are untouched by the compose gate, and do not disarm it...\n" );
	const std::string tmp = TempPath( "agentcrud_a81e.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "A81e fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
	sess->SetTextCompleter( MakeLightingCompleter() );
	Check( sess->FileBuildPlan( WizardOnlyPlan() ).ok, "A81e the plan files" );
	Check( sess->BuildPhase() == Agent::AgentSession::AgentBuildPhase::Pieces,
	       "A81e the session is in the pieces phase" );
	Check( sess->ElementChunks( "wizard" ).empty(),
	       "A81e with the active element still empty -- the arc-79 clean room is armed too" );

	// A WORK LIGHT inside an element window: allowed, and it does not even
	// trip the arc-79 first-geometry arm (which fires on GEOMETRY only).
	Check( sess->InsertChunk( A81Light( "work_light" ) ).applied,
	       "A81e MONEY ASSERTION: a light authored inside an element window is allowed -- arc 78 "
	       "sec 2.3's rule, which this arc must not take back" );
	Check( sess->BuildPhaseRefusalCount() == 0, "A81e and spends no refusal" );
	Check( sess->InsertChunk( A81Light( "work_light2" ) ).applied, "A81e -- and so is a second" );

	// Now into compose, with two lights already in the scene.
	Check( sess->FinishElement().ok, "A81e the element finishes" );
	Check( sess->BuildPhase() == Agent::AgentSession::AgentBuildPhase::Compose,
	       "A81e the session is in the compose phase" );
	const Agent::AgentChunkResult r = sess->InsertChunk( A81Light( "compose_hand" ) );
	Check( !r.applied && r.message.find( "light_scene" ) != std::string::npos,
	       "A81e MONEY ASSERTION: the two pieces-phase lights did NOT disarm the compose gate -- "
	       "it keys on whether light_scene has run, not on whether the scene has lights, so a "
	       "scene that entered compose carrying element-window work lights still gets its "
	       "lighting designed in a clean room" );
	Check( sess->BuildPhaseRefusalCount() == 1, "A81e and this is the first refusal spent" );

	// The existing lights are then reported TO the pass, so it can design
	// around them rather than duplicate them.
	std::vector<std::string> prompts;
	sess->SetTextCompleter( MakeLightingCompleter( nullptr, &prompts ) );
	const Agent::AgentSession::AgentLightSceneResult lr = sess->LightScene();
	Check( lr.ok, "A81e light_scene runs" );
	Check( !prompts.empty() &&
	       prompts[0].find( "work_light" ) != std::string::npos &&
	       prompts[0].find( "LIGHTS ALREADY IN THE SCENE (3)" ) != std::string::npos,
	       "A81e MONEY ASSERTION: and the lights that already exist are named in its prompt" );
	pJob->release();
}

//! A81f: light_scene over JSON-RPC -- the wire shape, and the one schema
//! error it has.
static void TestLightSceneWireShape()
{
	std::printf( "A81f: light_scene over JSON-RPC...\n" );
	const std::string tmp = TempPath( "agentcrud_a81f.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "A81f fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = A81ComposeSession( pJob );
	sess->SetTextCompleter( MakeLightingCompleter() );
	Agent::AgentRpcDispatcher rpc( std::move( sess ) );

	{
		const std::string resp = rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"light_scene\",\"params\":{}}" );
		Agent::JsonValue result;
		Check( JsonResultObj( resp, result ), "A81f light_scene returns a result object" );
		Check( result.get( "ok" ).asBool( false ), "A81f ok:true" );
		Check( result.get( "landed" ).isArray() && result.get( "landed" ).size() == 6,
		       "A81f the landed list rides the wire" );
		Check( result.get( "soloableLights" ).asNumber( -1 ) == 4.0,
		       "A81f and so does the soloable-light count" );
		Check( result.get( "contributions" ).isArray() &&
		       result.get( "contributions" ).size() == 4,
		       "A81f MONEY ASSERTION: the per-light contributions are a STRUCTURED fact, not only "
		       "prose" );
		Check( result.get( "contributions" ).at( 0 ).get( "soloed" ).asBool( false ) &&
		       result.get( "contributions" ).at( 0 ).has( "meanLuma" ),
		       "A81f with a real measurement on each measured entry" );
		Check( result.has( "allLightsMeanLuma" ),
		       "A81f and the all-lights reference it is read against" );
		Check( result.get( "retryRan" ).asBool( true ) == false, "A81f retryRan is reported" );

		// ---- ARC 83 SLICE 1: the plan and the loop ride the wire as
		// STRUCTURE, not only as prose in `message`.
		Check( result.get( "intentsPlanned" ).asNumber( -1 ) == 3.0 &&
		       result.get( "intentsBuilt" ).asNumber( -1 ) == 3.0 &&
		       result.get( "intentsTruncated" ).asBool( true ) == false,
		       "A81f the plan's shape rides the wire" );
		Check( result.get( "completions" ).asNumber( -1 ) == 4.0,
		       "A81f MONEY ASSERTION: so does the number of completions the call actually spent -- "
		       "a census that has to parse prose to learn the spend is a census that will drift" );
		Check( result.get( "areaLights" ).asNumber( -1 ) == 1.0 &&
		       result.get( "zeroAreaLights" ).asNumber( -1 ) == 2.0 &&
		       !result.has( "skyLights" ) && !result.has( "otherLights" ),
		       "A81f and the form tally, with the kinds that were never built OMITTED rather than "
		       "reported as zero" );
		Check( result.get( "intents" ).isArray() && result.get( "intents" ).size() == 3 &&
		       result.get( "intents" ).at( 1 ).get( "form" ).asString() == "area" &&
		       result.get( "intents" ).at( 1 ).get( "landed" ).size() == 4,
		       "A81f MONEY ASSERTION: with one entry PER INTENT carrying what that intent landed -- "
		       "83 sec 6's standing rule is per-unit numbers, and a total alone cannot show that "
		       "one of these three units bought a four-chunk area light" );
	}
	// The ONE schema defect: a non-string `notes`.  There are no required
	// params -- which scene gets lit is a property of the session.
	{
		const std::string resp = rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"light_scene\",\"params\":{\"notes\":7}}" );
		Check( resp.find( "-32602" ) != std::string::npos,
		       "A81f a non-string `notes` is a schema error" );
	}
}

//----------------------------------------------------------------------
// ARC 81 FIX-ROUND (2026-08-12): THE `ambient_light` BAN.
//
// House lighting policy from the project owner: ambient light is an
// anachronism and is never used.  Arc 81 shipped it as one of six equal
// palette entries and the first live run reached for it.  "NEVER" is
// implemented as a BLOCKING REFUSAL on every path that can create the
// chunk, and -- this is the part that is easy to get wrong -- it is NOT
// part of the staged build protocol's phase machinery: it consumes no
// part of the shared 3-refusal cap, cannot trigger the give-up, and is
// not disabled by --agent-build-protocol=off.  A permanent property of
// what this surface authors is not a sequencing gate.
//----------------------------------------------------------------------

//! One ambient_light chunk, the thing being refused.
static std::string A81Ambient( const char* name )
{
	return std::string( "ambient_light\n{\n\tname " ) + name +
		"\n\tcolor 0.15 0.2 0.3\n\tpower 0.4\n}";
}

//! The value-splice shape G2j's fixture documents in full (one line,
//! `name` is a String slot so the derive layer commits it), carrying an
//! ambient_light instead of a box_geometry.
static const char* const kA81AmbientSpliceValue =
	"obj_sph geometry sph material mat_diffuse } "
	"ambient_light { name spliced_amb color 1 1 1 power 1 } "
	"standard_object { name obj_sph_tail";

//! Every clause of the refusal a model reads, pinned once and reused by
//! each path -- the whole point of the shared DescribeAmbientLightBan is
//! that the message does not vary by where you hit it.
static void CheckAmbientBanMessage( const std::string& m, const char* where )
{
	Check( m.find( "not available through this surface" ) != std::string::npos,
	       std::string( "A81g/" ) + where + " the refusal states the kind is not available here" );
	Check( m.find( "casts no shadow ray" ) != std::string::npos &&
	       m.find( "neither a shadow nor any falloff" ) != std::string::npos,
	       std::string( "A81g/" ) + where + " MONEY ASSERTION: and states the PHYSICS behind it -- "
	       "no position, no direction, no shadow ray, so no shadow and no falloff. A prohibition "
	       "with no reason reads as an arbitrary rule" );
	Check( m.find( "AREA LIGHT" ) != std::string::npos &&
	       m.find( "lambertian_luminaire_material" ) != std::string::npos &&
	       m.find( "standard_object" ) != std::string::npos,
	       std::string( "A81g/" ) + where + " MONEY ASSERTION: and names the four-chunk chain that "
	       "fills the role -- a refusal that leaves a model with nothing to write instead is a "
	       "refusal it will spend turns arguing with" );
	// FACTS ONLY: it says what the renderer does and what to write instead.
	// It does not moralize, and it never says "you should".
	static const char* const kBannedMoralizing[] = {
		"should", "must", "please", "avoid", "prefer", "bad", "wrong", "never" };
	for( std::size_t i = 0; i < sizeof( kBannedMoralizing ) / sizeof( kBannedMoralizing[0] ); ++i )
		Check( m.find( kBannedMoralizing[i] ) == std::string::npos,
		       std::string( "A81g/" ) + where + " the refusal never says \"" +
		       kBannedMoralizing[i] + "\" -- it states facts and stops" );
}

static void TestAmbientLightIsAlwaysRefused()
{
	std::printf( "A81g: ambient_light is refused on every insertion path, unconditionally...\n" );
	const std::string tmp = TempPath( "agentcrud_a81g.RISEscene" );

	// (0) THE REGISTRY PIN.  The ban names ONE keyword; if the parser ever
	//     renames it the ban would silently become a no-op, so the rename
	//     has to fail a test instead.
	{
		const ChunkDescriptor* d = DescriptorForKeyword( String( "ambient_light" ) );
		Check( d != nullptr && d->category == ChunkCategory::Light,
		       "A81g/registry `ambient_light` is still the registry keyword this ban names, and "
		       "still a Light chunk -- a rename here would turn the refusal into a no-op" );
	}

	// (1) insert_chunk, with the BUILD PROTOCOL OFF.  This is the arm that
	//     proves the ban is not phase machinery: a protocol-off session has
	//     no phases at all, and every phase refusal dies with it.
	{
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "A81g/off fixture loads" );
		if( !pJob ) return;
		Agent::AgentSession::SetBuildProtocolDefaultEnabled( false );
		std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
		Agent::AgentSession::SetBuildProtocolDefaultEnabled( true );
		Check( !sess->BuildProtocolActive(), "A81g/off the protocol is inactive for this session" );

		const std::string docBefore = sess->ReadDocument();
		const Agent::AgentChunkResult r = sess->InsertChunk( A81Ambient( "amb_off" ) );
		Check( !r.applied && r.status == "rejected",
		       "A81g/off MONEY ASSERTION: an ambient_light insert is refused with "
		       "--agent-build-protocol=off -- the ban is a permanent property of what this "
		       "surface authors, not a sequencing gate that dies with the protocol" );
		Check( r.kind == "ambient_light" && r.name == "amb_off",
		       "A81g/off with the identity echo every other refusal honours" );
		Check( !r.retriable,
		       "A81g/off and NOT retriable -- the GUI chat loops auto-retry that flag silently, "
		       "so a retriable refusal is one the model never sees" );
		Check( sess->ReadDocument() == docBefore, "A81g/off the document is byte-identical" );
		CheckAmbientBanMessage( r.message, "off" );

		// The very same session authors an ORDINARY light with no trouble --
		// the ban is one keyword, not a mood about lights.
		Check( sess->InsertChunk( A81Light( "amb_off_ok" ) ).applied,
		       "A81g/off an omni_light is untouched by the ban" );
		pJob->release();
	}

	// (2) insert_chunk in COMPOSE, where the arc-81 first-light PHASE arm
	//     would also fire on a light chunk.  The ban runs FIRST and spends
	//     NOTHING, which is the whole reason for its ordering.
	{
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "A81g/cap fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = A81ComposeSession( pJob );
		sess->SetTextCompleter( MakeLightingCompleter() );
		Check( sess->BuildPhase() == Agent::AgentSession::AgentBuildPhase::Compose,
		       "A81g/cap the session is in the compose phase" );

		for( int i = 0; i < 5; ++i ) {
			const Agent::AgentChunkResult r =
				sess->InsertChunk( A81Ambient( ( "amb_" + std::to_string( i ) ).c_str() ) );
			Check( !r.applied, "A81g/cap ambient refusal " + std::to_string( i + 1 ) + " of 5" );
			Check( r.message.find( "light_scene" ) == std::string::npos,
			       "A81g/cap and it is the BAN talking, not the phase gate -- the refusal never "
			       "offers light_scene as the way to get an ambient_light, because there is none" );
		}
		Check( sess->BuildPhaseRefusalCount() == 0,
		       "A81g/cap MONEY ASSERTION: FIVE ambient refusals spent ZERO of the shared 3-refusal "
		       "phase cap -- a permanent prohibition must not consume a sequencing budget, and at "
		       "the old ordering these five would have burned the cap and tripped the give-up" );

		// ... and the phase machinery is still fully armed underneath.
		const Agent::AgentChunkResult ph = sess->InsertChunk( A81Light( "amb_phase" ) );
		Check( !ph.applied && ph.message.find( "light_scene" ) != std::string::npos,
		       "A81g/cap the compose first-light gate still fires on a real light chunk" );
		Check( sess->BuildPhaseRefusalCount() == 1,
		       "A81g/cap and THAT one is what spends a slot -- the cap was intact for it" );
		pJob->release();
	}

	// (3) insert_chunks: the WHOLE batch is refused, atomically.  A policy
	//     refusal is not an authoring failure; half a batch is a state
	//     nobody asked for.
	{
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "A81g/batch fixture loads" );
		if( !pJob ) return;
		Agent::AgentSession::SetBuildProtocolDefaultEnabled( false );
		std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
		Agent::AgentSession::SetBuildProtocolDefaultEnabled( true );

		const std::string docBefore = sess->ReadDocument();
		std::vector<std::string> batch;
		batch.push_back( "uniformcolor_painter\n{\n\tname a81g_pnt\n\tcolor 1 1 1\n}" );
		batch.push_back( A81Ambient( "amb_batch" ) );
		batch.push_back( A81Light( "a81g_omni" ) );
		const std::vector<Agent::AgentChunkResult> rs = sess->InsertChunks( batch );
		Check( rs.size() == 3 && !rs[0].applied && !rs[1].applied && !rs[2].applied,
		       "A81g/batch MONEY ASSERTION: one ambient_light anywhere in the batch refuses the "
		       "WHOLE batch -- the innocent painter and omni do NOT land" );
		Check( sess->ReadDocument() == docBefore,
		       "A81g/batch and the document is byte-identical" );
		Check( rs[0].message.find( "chunks[1]" ) != std::string::npos,
		       "A81g/batch the refusal names the offending INDEX, so the fix is one edit" );
		CheckAmbientBanMessage( rs[0].message, "batch" );
		pJob->release();
	}

	// (4) light_scene's OWN validated insertion.  The clean room is exempt
	//     from the phase rules it arms -- it is NOT exempt from this one --
	//     and the rejection carries the ban's text into the ONE repair
	//     retry, so the pass is told what to write instead in the same turn.
	{
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "A81g/cleanroom fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = A81ComposeSession( pJob );
		const std::string ambientAnswer =
			"ambient_light\n{\n\tname pass_amb\n\tcolor 0.2 0.2 0.3\n\tpower 0.5\n}\n"
			"omni_light\n{\n\tname pass_key\n\tposition 2 3 4\n\tcolor 1 1 1\n\tpower 50\n}\n";
		int calls = 0;
		std::vector<std::string> prompts;
		sess->SetTextCompleter( MakeFakeCompleter( { kOneIntentPlan, ambientAnswer,
		                                             kLightingAnswerKey },
		                                            &calls, &prompts ) );
		const Agent::AgentSession::AgentLightSceneResult r = sess->LightScene();

		bool sawAmbientReject = false;
		for( std::size_t i = 0; i < r.rejected.size(); ++i )
			if( r.rejected[i].kind == "ambient_light" ) {
				sawAmbientReject = true;
				CheckAmbientBanMessage( r.rejected[i].reason, "cleanroom" );
			}
		Check( sawAmbientReject,
		       "A81g/cleanroom MONEY ASSERTION: light_scene's own insertion is exempt from the "
		       "compose-phase creation ban and from the gate it arms -- and NOT from this ban" );
		Check( sess->ReadDocument().find( "pass_amb" ) == std::string::npos,
		       "A81g/cleanroom no ambient_light reached the document" );
		Check( sess->ReadDocument().find( "pass_key" ) != std::string::npos,
		       "A81g/cleanroom while the rest of the same answer landed -- never a silent drop, "
		       "never an all-or-nothing on a per-chunk rule" );
		Check( sess->ReadDocument().find( "pass_key" ) != std::string::npos &&
		       r.perIntent.size() == 1 && r.perIntent[0].form == "zero-area",
		       "A81g/cleanroom MONEY ASSERTION: the BANNED chunk did not consume this intent's one "
		       "light-source slot -- an answer of [ambient_light, omni_light] carries exactly one "
		       "light source this pass can land, and refusing the omni as a 'second' one would "
		       "punish the model twice for one mistake" );
		Check( r.retryRan && calls == 3 && prompts.size() == 3 &&
		       prompts[2].find( "casts no shadow ray" ) != std::string::npos,
		       "A81g/cleanroom MONEY ASSERTION: the ban's own text drives the ONE repair retry "
		       "VERBATIM, so the pass is corrected with the physics and the alternative in the "
		       "same turn rather than being told only that something was rejected" );
		pJob->release();
	}

	// (5) propose_patch's VALUE-SPLICE path -- a param value is spliced into
	//     the document as TEXT, the identical bypass R1c's arm (b) and the
	//     build-plan gate's patch arm each close for their own kinds.
	{
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "A81g/splice fixture loads" );
		if( !pJob ) return;
		Agent::AgentSession::SetBuildProtocolDefaultEnabled( false );
		std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
		Agent::AgentSession::SetBuildProtocolDefaultEnabled( true );

		const std::string docBefore = sess->ReadDocument();
		const Agent::AgentPatchResult r = sess->ProposePatch(
			MakePatch( G2_SPLICE_TARGET, kA81AmbientSpliceValue ) );
		Check( !r.applied && r.status == "rejected",
		       "A81g/splice MONEY ASSERTION: a propose_patch whose VALUE splices a whole "
		       "ambient_light chunk into the serialized document is refused -- a ban with no "
		       "patch arm is a ban with a documented hole" );
		Check( sess->ReadDocument() == docBefore,
		       "A81g/splice and the document is byte-identical" );
		Check( sess->ReadDocument().find( "spliced_amb" ) == std::string::npos,
		       "A81g/splice RED-PROVE (direct): the spliced chunk's name is nowhere in the document" );
		CheckAmbientBanMessage( r.message, "splice" );

		// The CONTROL: the same splice mechanism carrying something else is
		// not this ban's business, so the refusal is about the KIND rather
		// than about any value that happens to contain a brace.
		const Agent::AgentPatchResult ok = sess->ProposePatch(
			MakePatch( G2_SPLICE_TARGET, kG2SplicePainterValue ) );
		Check( ok.message.find( "not available through this surface" ) == std::string::npos,
		       "A81g/splice the SAME splice shape carrying a painter is not refused by THIS ban" );
		pJob->release();
	}
}

//! A81h: THE EXAMPLE PARSES.  The area light is the palette's one worked
//! example, and an example that does not parse is worse than none -- it
//! spends the model's repair retry on the harness's own typo.  So the
//! example is EXTRACTED FROM THE SHIPPED PROMPT (not retyped here, which
//! would only prove the copy parses) and pushed through light_scene's real
//! validated insertion.
static void TestPaletteAreaLightExampleParses()
{
	std::printf( "A81h: the palette's worked area-light example really parses...\n" );
	const std::string tmp = TempPath( "agentcrud_a81h.RISEscene" );

	// ---- Lift the example out of the prompt this surface actually sends.
	std::string example;
	{
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "A81h/compose fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = A81ComposeSession( pJob );
		std::vector<std::string> prompts;
		int calls = 0;
		sess->SetTextCompleter( MakeLightingCompleter( &calls, &prompts ) );
		sess->LightScene();
		// ARC 83: prompts[0] is the PLAN, which deliberately carries no
		// palette; the example lives in every PER-INTENT prompt, and it is
		// one of those that a model copies from.
		Check( prompts.size() == 4, "A81h the plan prompt and three per-intent prompts were composed" );
		if( prompts.size() >= 2 ) {
			const std::string& p = prompts[1];
			const std::size_t a = p.find( "Example:\n" );
			const std::size_t b = ( a == std::string::npos )
				? std::string::npos : p.find( "\n\n2. ", a );
			Check( a != std::string::npos && b != std::string::npos,
			       "A81h the area light's example block is locatable in the prompt" );
			if( a != std::string::npos && b != std::string::npos )
				example = p.substr( a + 9, b - ( a + 9 ) );
		}
		pJob->release();
	}
	Check( example.compare( 0, 21, "uniformcolor_painter\n" ) == 0,
	       "A81h and the block lifted is the FIRST palette entry's example, the area light" );
	if( example.empty() ) return;

	// ---- Push it through the real insertion path, verbatim.
	{
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "A81h/insert fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = A81ComposeSession( pJob );
		int calls = 0;
		sess->SetTextCompleter( MakeFakeCompleter( { kOneIntentPlan, example }, &calls ) );
		const Agent::AgentSession::AgentLightSceneResult r = sess->LightScene();

		Check( r.ok && r.chunksExtracted == 4,
		       "A81h the example is four chunks and all four were extracted" );
		Check( calls == 2 && r.perIntent.size() == 1 && r.perIntent[0].form == "area",
		       "A81h MONEY ASSERTION: and the example is EXACTLY ONE light source by this verb's own "
		       "one-source rule -- the four-chunk area chain the arc exists to make affordable is "
		       "not four lights, and a rule that counted it as four would refuse the palette's own "
		       "worked example" );
		Check( r.landed.size() == 4 && r.rejected.empty(),
		       "A81h MONEY ASSERTION: every chunk of the shipped example LANDS through the real "
		       "validated insertion -- painter, luminaire material, quad and object. A worked "
		       "example that does not parse is worse than no example at all" );
		Check( !r.retryRan,
		       "A81h with no repair retry -- nothing was rejected to repair" );
		const std::string doc = sess->ReadDocument();
		Check( doc.find( "pnt_window" )  != std::string::npos &&
		       doc.find( "window_mat" )  != std::string::npos &&
		       doc.find( "window_geo" )  != std::string::npos &&
		       doc.find( "window_obj" )  != std::string::npos,
		       "A81h and all four are really in the document by name" );
		Check( r.soloableLightCount >= 2,
		       "A81h the example's emissive object is soloable afterwards -- it is a real light "
		       "source, which is the claim the palette makes about it" );
		pJob->release();
	}
}

//----------------------------------------------------------------------
// ARC 83 SLICE 1 (2026-08-12): THE UNIT OF A LIGHTING CALL IS ONE LIGHT.
// Design: docs/agentic-redesign/83-staged-construction-plan.md sec 1, 2,
// 6.1; measurement 82 sec 10.4.
//
// The four tests below cover what the loop adds on top of everything arc
// 81 already pinned: the PLAN step and its budget, the one-light-per-
// intent rule, the independence of the intents from one another, and the
// composition that lets a later intent complement an earlier one.  Every
// one uses the same mocked-completer seam the arc-79 and arc-81 tests
// use: no live provider call is ever made.
//----------------------------------------------------------------------

//! A completer that answers the FIRST call with `plan` and every later
//! call with one distinct omni_light -- so an N-intent plan lands N
//! lights and the count that comes back is a fact about the loop rather
//! than about a canned list's length.
static Agent::AgentSession::AgentTextCompleter MakeIndexedLightCompleter(
	const std::string& plan, int* callsOut = nullptr,
	std::vector<std::string>* promptsOut = nullptr )
{
	Agent::AgentSession::AgentTextCompleter c;
	c.supported    = true;
	c.providerName = "mock";
	c.modelId      = "mock-lighting-1";
	auto count = std::make_shared<int>( 0 );
	c.complete = [plan, count, callsOut, promptsOut]( const std::string& prompt )
		-> Agent::AgentSession::AgentTextCompletionOutcome
	{
		if( promptsOut ) promptsOut->push_back( prompt );
		++( *count );
		if( callsOut ) *callsOut = *count;
		Agent::AgentSession::AgentTextCompletionOutcome o;
		o.ok = true;
		o.text = ( *count == 1 )
			? plan
			: ( "omni_light\n{\n\tname a83_light" + std::to_string( *count ) +
			    "\n\tposition 1 2 3\n\tcolor 1 1 1\n\tpower 30\n}\n" );
		return o;
	};
	return c;
}

//! A83a: THE PLAN STEP -- bounded at kLightIntentBudget, tolerant of the
//! punctuation a model reaches for, and TRUNCATING OUT LOUD.
static void TestLightSceneIntentPlanBudget()
{
	std::printf( "A83a: the lighting plan is bounded at the intent budget, and says when it cut...\n" );
	const std::string tmp = TempPath( "agentcrud_a83a.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "A83a fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = A81ComposeSession( pJob );

	// NINE lines of intent in three punctuations, wrapped in a fence and
	// carrying a blank line -- a plan that is merely differently punctuated
	// is not a failed plan.
	static const char* const kNineIntentPlan =
		"```\n"
		"- sunlight through the water above the sphere\n"
		"2. a soft fill on the left of the sphere\n"
		"* a rim behind the sphere\n"
		"\n"
		"a glow under the sphere\n"
		"a lamp inside the sphere\n"
		"a wash across the back wall\n"
		"a spark at the sphere's edge\n"
		"a faint bounce off the ground\n"
		"a cool source far behind the camera\n"
		"```\n";

	int calls = 0;
	std::vector<std::string> prompts;
	sess->SetTextCompleter( MakeIndexedLightCompleter( kNineIntentPlan, &calls, &prompts ) );
	const Agent::AgentSession::AgentLightSceneResult r = sess->LightScene();

	Check( r.ok, "A83a the call succeeds" );
	Check( r.intentsReturned == 9,
	       "A83a the plan really returned nine intents (got " +
	       std::to_string( r.intentsReturned ) + ")" );
	Check( static_cast<int>( r.intents.size() ) == Agent::AgentSession::kLightIntentBudget &&
	       r.intentsTruncated,
	       "A83a MONEY ASSERTION: the plan is cut to kLightIntentBudget and the result SAYS it was "
	       "cut -- an unbounded plan step is an unbounded provider bill, and a silent cut is the "
	       "false-clause class this design family exists to avoid" );
	Check( calls == 1 + Agent::AgentSession::kLightIntentBudget,
	       "A83a MONEY ASSERTION: exactly one completion per SURVIVING intent, plus the plan -- the "
	       "three that were cut cost nothing (got " + std::to_string( calls ) + ")" );
	Check( r.completionsSpent == calls,
	       "A83a and the reported spend is the real one" );
	Check( r.landed.size() == static_cast<std::size_t>( Agent::AgentSession::kLightIntentBudget ) &&
	       r.intentsBuilt == Agent::AgentSession::kLightIntentBudget,
	       "A83a six intents built six lights -- the counts the result reports are the counts that "
	       "landed" );
	Check( r.message.find( "the plan returned 9 intents against a budget of 6" ) != std::string::npos,
	       "A83a and the message states both numbers rather than showing a quietly short list" );

	// THE PUNCTUATION IS STRIPPED, so the intent a later prompt carries is
	// the intent and not a bullet.
	Check( r.intents.size() >= 3 &&
	       r.intents[0] == "sunlight through the water above the sphere" &&
	       r.intents[1] == "a soft fill on the left of the sphere" &&
	       r.intents[2] == "a rim behind the sphere",
	       "A83a a leading bullet, a leading number and a code fence are punctuation, not intents" );

	// AND THE BUDGET IS IN THE PROMPT.  A budget the model is not told is a
	// budget it can only discover by having its plan cut.
	Check( !prompts.empty() &&
	       prompts[0].find( "at most " + std::to_string( Agent::AgentSession::kLightIntentBudget ) +
	                        " intents" ) != std::string::npos,
	       "A83a the planning prompt states the budget as the same number the code enforces" );
	pJob->release();
}

//! A83b: EXACTLY ONE LIGHT SOURCE PER INTENT -- 83 sec 6.1, the part the
//! whole slice rests on.  A per-intent call that may author several lights
//! rebuilds the bottleneck this slice removes, so the rule is ENFORCED and
//! not merely requested.
static void TestLightSceneOneLightPerIntent()
{
	std::printf( "A83b: one intent authors exactly one light source...\n" );
	const std::string tmp = TempPath( "agentcrud_a83b.RISEscene" );

	// (1) TWO light chunks in one answer: the first lands, the second is
	//     reported -- and does NOT fire the repair retry.
	{
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "A83b/two fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = A81ComposeSession( pJob );
		const std::string twoLights =
			"omni_light\n{\n\tname a83_first\n\tposition 2 3 4\n\tcolor 1 1 1\n\tpower 50\n}\n"
			"spot_light\n{\n\tname a83_second\n\tposition 0 5 0\n\ttarget 0 0 0\n"
			"\tinner 10\n\touter 25\n\tcolor 1 1 1\n\tpower 40\n}\n";
		int calls = 0;
		sess->SetTextCompleter( MakeFakeCompleter( { kOneIntentPlan, twoLights }, &calls ) );
		const Agent::AgentSession::AgentLightSceneResult r = sess->LightScene();

		Check( r.landed.size() == 1 && r.landed[0] == "a83_first",
		       "A83b/two MONEY ASSERTION: exactly ONE light source lands from one intent's answer -- "
		       "the unit is one LIGHT, not one intent's worth of lights (83 sec 6.1)" );
		bool sawSecond = false;
		for( std::size_t i = 0; i < r.rejected.size(); ++i )
			if( r.rejected[i].name == "a83_second" &&
			    r.rejected[i].reason.find( "exactly ONE light source" ) != std::string::npos )
				sawSecond = true;
		Check( sawSecond,
		       "A83b/two the second is REPORTED with the reason, never silently dropped" );
		Check( sess->ReadDocument().find( "a83_second" ) == std::string::npos,
		       "A83b/two and it did not reach the document" );
		Check( !r.retryRan && calls == 2,
		       "A83b/two MONEY ASSERTION: and it does NOT fire the repair retry -- an answer whose "
		       "one light landed is not a broken answer, and spending a second completion to ask "
		       "for less would double this intent's cost for nothing" );
		Check( r.perIntent.size() == 1 && r.perIntent[0].form == "zero-area" &&
		       r.zeroAreaLightsBuilt == 1 && r.areaLightsBuilt == 0,
		       "A83b/two the form tally counts the light that landed, once" );
		pJob->release();
	}

	// (2) An AREA CHAIN plus an extra light: all FOUR chunks of the chain
	//     land and the extra is reported.  This is the case the rule has to
	//     get right -- four chunks are one light source, and a rule that
	//     counted chunks would refuse the very form arc 83 exists to buy.
	{
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "A83b/area fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = A81ComposeSession( pJob );
		const std::string areaPlusOne =
			std::string( kLightingAnswerArea ) +
			"omni_light\n{\n\tname a83_extra\n\tposition 9 9 9\n\tcolor 1 1 1\n\tpower 10\n}\n";
		int calls = 0;
		sess->SetTextCompleter( MakeFakeCompleter( { kOneIntentPlan, areaPlusOne }, &calls ) );
		const Agent::AgentSession::AgentLightSceneResult r = sess->LightScene();

		Check( r.landed.size() == 4,
		       "A83b/area MONEY ASSERTION: the four-chunk area chain lands WHOLE -- painter, "
		       "emissive material, geometry and object are ONE light source, and the one-source "
		       "rule counts sources rather than chunks" );
		Check( sess->ReadDocument().find( "lit_panel_obj" ) != std::string::npos &&
		       sess->ReadDocument().find( "a83_extra" ) == std::string::npos,
		       "A83b/area while the extra light chunk in the same answer did not land" );
		Check( r.areaLightsBuilt == 1 && r.zeroAreaLightsBuilt == 0,
		       "A83b/area and the intent is counted as ONE area light" );
		Check( !r.retryRan && calls == 2, "A83b/area with no retry fired by the extra" );
		pJob->release();
	}
}

//! A83c: A FAILING INTENT DOES NOT ABORT THE LOOP.  The remaining intents
//! still run, and the result says which one failed and why.
static void TestLightSceneFailedIntentDoesNotAbort()
{
	std::printf( "A83c: one intent's failure does not stop the others...\n" );
	const std::string tmp = TempPath( "agentcrud_a83c.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "A83c fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = A81ComposeSession( pJob );

	// A completer that fails for INTENT 2 ONLY, recognised by the prompt's
	// own "intent 2 of 3" line -- so the failure is targeted at a unit
	// rather than at a call count.
	int calls = 0;
	{
		Agent::AgentSession::AgentTextCompleter c;
		c.supported    = true;
		c.providerName = "mock";
		c.modelId      = "mock-lighting-1";
		auto count = std::make_shared<int>( 0 );
		c.complete = [count, &calls]( const std::string& prompt )
			-> Agent::AgentSession::AgentTextCompletionOutcome
		{
			++( *count );
			calls = *count;
			Agent::AgentSession::AgentTextCompletionOutcome o;
			if( *count == 1 ) { o.ok = true; o.text = kThreeIntentPlan; return o; }
			if( prompt.find( "intent 2 of 3" ) != std::string::npos ) {
				o.error = "mock refuses this intent";
				return o;
			}
			o.ok = true;
			o.text = "omni_light\n{\n\tname a83c_light" + std::to_string( *count ) +
				"\n\tposition 1 2 3\n\tcolor 1 1 1\n\tpower 30\n}\n";
			return o;
		};
		sess->SetTextCompleter( c );
	}

	const Agent::AgentSession::AgentLightSceneResult r = sess->LightScene();

	Check( r.ok, "A83c the call still reports an ok outcome -- two of three intents were built" );
	Check( r.perIntent.size() == 3,
	       "A83c MONEY ASSERTION: all three intents ran; the failure of the second did not abort "
	       "the loop" );
	Check( r.perIntent[0].built && !r.perIntent[1].built && r.perIntent[2].built,
	       "A83c with the middle one, and only the middle one, unbuilt" );
	Check( r.perIntent[1].failure.find( "mock refuses this intent" ) != std::string::npos,
	       "A83c and the reason it failed is recorded against THAT intent" );
	Check( r.intentsBuilt == 2 && r.landed.size() == 2,
	       "A83c two intents built one light each" );
	Check( r.perIntent[1].completions == 2 && r.perIntent[1].retryRan,
	       "A83c MONEY ASSERTION: the failing intent spent its own one repair retry and stopped "
	       "there -- the retry is per intent, and a provider failure does not become the next "
	       "intent's problem" );
	Check( calls == 5,
	       "A83c so the call spent 5 completions: the plan, one each for intents 1 and 3, and two "
	       "for the intent that failed (got " + std::to_string( calls ) + ")" );
	Check( r.message.find( "Intent 2 \"a soft panel filling the shadowed left side of the sphere\": "
	                       "nothing inserted" ) != std::string::npos &&
	       r.message.find( "mock refuses this intent" ) != std::string::npos,
	       "A83c MONEY ASSERTION: and the report names the intent that failed and what happened -- "
	       "a loop whose partial failure is invisible is a loop nobody can debug" );
	pJob->release();
}

//! A83d: WHAT AN EARLIER INTENT PLACED IS IN THE LATER INTENTS' PROMPTS,
//! with the intent it served -- which is what lets the later light
//! complement the earlier one rather than repeat it.
static void TestLightSceneLaterIntentsSeeEarlierLights()
{
	std::printf( "A83d: a later intent's prompt carries what the earlier intents placed...\n" );
	const std::string tmp = TempPath( "agentcrud_a83d.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "A83d fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = A81ComposeSession( pJob );

	std::vector<std::string> prompts;
	sess->SetTextCompleter( MakeLightingCompleter( nullptr, &prompts ) );
	const Agent::AgentSession::AgentLightSceneResult r = sess->LightScene();
	Check( r.ok && prompts.size() == 4, "A83d the plan and three per-intent prompts were composed" );
	if( prompts.size() != 4 ) { pJob->release(); return; }

	Check( prompts[1].find( "nothing yet -- this is the first intent of the plan" ) != std::string::npos,
	       "A83d the FIRST intent is told outright that nothing has been placed yet -- an empty "
	       "section is a fact, and omitting it would leave the model to guess" );
	Check( prompts[2].find( "WHAT THE EARLIER INTENTS OF THIS PLAN PLACED (1)" ) != std::string::npos &&
	       prompts[2].find( "\"sunlight arriving on the sphere from above and behind the camera\" "
	                        "-- lit_key" ) != std::string::npos,
	       "A83d MONEY ASSERTION: the SECOND intent's prompt names what the first placed AND the "
	       "intent it served -- the live 'lights already in the scene' list says what the scene "
	       "HAS, and only this section says what it was FOR, which is what a complementary light "
	       "is designed against" );
	Check( prompts[3].find( "WHAT THE EARLIER INTENTS OF THIS PLAN PLACED (2)" ) != std::string::npos &&
	       prompts[3].find( "lit_panel_obj" ) != std::string::npos,
	       "A83d and the third sees both, including every chunk of the second's area light" );
	Check( prompts[3].find( "intent 3 of 3" ) != std::string::npos,
	       "A83d each request also says which intent of how many it is" );

	// The LIVE section is still live: the area light the second intent
	// placed is a new emissive object, and the third intent's prompt sees it
	// as one.
	Check( prompts[3].find( "lit_panel_obj -- emissive object" ) != std::string::npos,
	       "A83d MONEY ASSERTION: and the 'lights already in the scene' section is re-measured per "
	       "intent, so an area light placed one intent ago appears as the light source it is" );

	// THE INVENTORY, by contrast, is measured ONCE -- so the prompt must not
	// claim it was measured "just now" at the third intent.
	Check( prompts[0].find( "measured from the live scene just now" ) != std::string::npos,
	       "A83d the PLAN prompt's inventory really was measured just now" );
	Check( prompts[3].find( "measured from the live scene at the start of this lighting pass" )
	       != std::string::npos &&
	       prompts[3].find( "measured from the live scene just now" ) == std::string::npos,
	       "A83d MONEY ASSERTION: while a per-intent prompt says WHEN its inventory was measured -- "
	       "it is measured once and reused, so 'just now' at the sixth intent would be a false "
	       "clause in a model-facing payload" );
	pJob->release();
}

//----------------------------------------------------------------------
// ARC 82 (2026-08-12): `populate_scene` -- THE CLEAN-ROOM POPULATION PASS,
// and the COMPOSE-phase RENDER refusal that forces its first use.
// Design: docs/agentic-redesign/82-population-arc.md.
//
// THE MEASUREMENT BEHIND IT.  The hand-authored frontier benchmark on the
// same prompt has 47 objects built from 18 distinct geometries -- 38 of the
// 47 are REPEATS of a geometry another object already uses -- and it uses no
// generator or instancing chunk of any kind: every repeat is an ordinary
// standard_object naming the same geometry and material at a different
// transform.  The best agent run to date has 16 objects and almost no reuse.
// The gap is not modelling; it is that the session builds one of each and
// stops.
//
// EVERY TEST HERE USES A MOCKED TEXT COMPLETER, through the same
// AgentSession::SetTextCompleter seam the arc-79 and arc-81 tests use: no
// live provider call is ever made, and the assertions are about what the
// harness does with an answer.
//----------------------------------------------------------------------

//! Three repeats of the fixture's OWN sphere -- the shape this verb exists
//! to produce, and every one of them references a geometry and a material
//! that already exist.
static const char* const kGoodPopulationAnswer =
	"standard_object\n{\n\tname pop_sph_a\n\tgeometry sph\n\tmaterial mat_diffuse\n"
	"\tposition 1.4 0 -0.6\n\tscale 0.6\n}\n"
	"standard_object\n{\n\tname pop_sph_b\n\tgeometry sph\n\tmaterial mat_diffuse\n"
	"\tposition -1.5 0.2 -1.1\n\torientation 0 40 0\n\tscale 0.45\n}\n"
	"standard_object\n{\n\tname pop_sph_c\n\tgeometry sph\n\tmaterial mat_diffuse\n"
	"\tposition 0.3 -0.9 -2.0\n\tscale 0.8\n}\n";

//! A scene with geometry and material but NO standard_object -- the one
//! state in which a population pass has nothing to work from.
static const char* const kNoObjectScene =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 8\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 24\n\theight 24\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_albedo\n\tcolor 0.5 0.5 0.5\n}\n\n"
	"lambertian_material\n{\n\tname mat_diffuse\n\treflectance pnt_albedo\n}\n\n"
	"sphere_geometry\n{\n\tname sph\n\tradius 0.8\n}\n";

//! A session on `kScene` that has reached the COMPOSE phase -- the phase
//! this verb's gate lives in.  A81ComposeSession's twin, kept separate so
//! neither arc's fixture can be changed on behalf of the other.
static std::unique_ptr<Agent::AgentSession> A82ComposeSession( Job* pJob )
{
	std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
	sess->FileBuildPlan( WizardOnlyPlan() );
	sess->FinishElement();
	return sess;
}

//! A render as a MODEL issues one: `fromAgentSurface` is what AgentRpc's
//! `render` handler sets, and it is the exact discriminator the gate keys
//! on.  Everything internal to AgentSession leaves it false.
static Agent::AgentRenderParams A82ModelRender()
{
	Agent::AgentRenderParams rp;
	rp.fromAgentSurface = true;
	rp.width  = 24;
	rp.height = 24;
	rp.samples = 1;
	return rp;
}

//! A82a: the happy path -- repeats land, the prompt carries the raw
//! material this arc exists to hand over, and the report states the object
//! count before and after.
static void TestPopulateSceneHappyPath()
{
	std::printf( "A82a: populate_scene places repeats of what the scene already has...\n" );
	const std::string tmp = TempPath( "agentcrud_a82a.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "A82a fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = A82ComposeSession( pJob );

	int calls = 0;
	std::vector<std::string> prompts;
	sess->SetTextCompleter( MakeFakeCompleter( { kGoodPopulationAnswer }, &calls, &prompts ) );

	const Agent::AgentSession::AgentPopulateSceneResult r = sess->PopulateScene();
	Check( r.ok, "A82a populate_scene succeeds" );
	Check( calls == 1, "A82a MONEY ASSERTION: exactly ONE completion -- no retry on a clean answer" );
	Check( !r.retryRan, "A82a and the result says the retry did not run" );
	Check( r.chunksExtracted == 3, "A82a all three chunks were extracted" );
	Check( r.created.size() == 3, "A82a and all three landed" );
	Check( r.rejected.empty(), "A82a with nothing rejected" );
	Check( r.created.size() == 3 && r.created[0].geometry == "sph" &&
	       r.created[0].material == "mat_diffuse",
	       "A82a MONEY ASSERTION: each creation records WHAT IT REPEATS -- the geometry and the "
	       "material it referenced, both of which already existed, which is the whole contract" );
	Check( r.objectCountBefore == 2 && r.objectCountAfter == 5,
	       "A82a MONEY ASSERTION: the scene's object count is measured from the object manager "
	       "BEFORE and AFTER, so the difference is a fact about the scene rather than a count of "
	       "what the pass believes it did (got " + std::to_string( r.objectCountBefore ) + " -> " +
	       std::to_string( r.objectCountAfter ) + ")" );
	Check( sess->ReadDocument().find( "pop_sph_a" ) != std::string::npos &&
	       sess->ReadDocument().find( "pop_sph_c" ) != std::string::npos,
	       "A82a the repeats are really in the document" );
	Check( sess->BuildPhaseRefusalCount() == 0,
	       "A82a MONEY ASSERTION: placing objects in COMPOSE spends no phase refusal -- arc 80's "
	       "creation ban fires on GEOMETRY, and placing an element is what compose is FOR" );

	// ---- THE PROMPT.  Host-composed, and carrying the raw material a
	// repeat is made from.
	Check( prompts.size() == 1, "A82a one prompt was composed" );
	if( !prompts.empty() ) {
		const std::string& p = prompts[0];
		Check( p.find( "SCENE INVENTORY" ) != std::string::npos &&
		       p.find( "obj_sph" ) != std::string::npos,
		       "A82a the arc-80 SCENE INVENTORY is in the prompt, naming the scene's actual "
		       "objects -- so a repeat can be placed in relation to real geometry" );
		Check( p.find( "THE CAMERA:" ) != std::string::npos &&
		       p.find( "field of view" ) != std::string::npos,
		       "A82a the camera pose and FOV are stated, so placements can fill the view that will "
		       "actually be rendered" );
		Check( p.find( "WORLD BOUNDS" ) != std::string::npos,
		       "A82a and so are the scene's world bounds" );
		Check( p.find( "THE GEOMETRIES AND MATERIALS THIS SCENE ALREADY HAS" ) != std::string::npos,
		       "A82a MONEY ASSERTION: the RAW MATERIAL listing is in the prompt -- a repeat must "
		       "reference an existing geometry and material, so the list of them IS the vocabulary" );
		Check( p.find( "sph -- material mat_diffuse; used by 1 object: obj_sph" ) != std::string::npos,
		       "A82a and each entry names the geometry, the material it is worn with, and the "
		       "objects currently using it" );
		Check( p.find( "quad_emit -- material mat_emit" ) != std::string::npos,
		       "A82a for every geometry the scene has, not only the first" );
		Check( p.find( "standard_object" ) != std::string::npos &&
		       p.find( "geometry" ) != std::string::npos,
		       "A82a the standard_object grammar comes from the descriptor registry" );

		// THE ONE WORKED EXAMPLE, built from this scene's own names.
		Check( p.find( "Example -- one more placement" ) != std::string::npos,
		       "A82a MONEY ASSERTION: the prompt carries exactly ONE worked example -- the lever "
		       "this workstream has measured moving what a model writes" );
		Check( p.find( "\tgeometry\tquad_emit\n" ) != std::string::npos ||
		       p.find( "\tgeometry\tsph\n" ) != std::string::npos,
		       "A82a MONEY ASSERTION: and the example names THIS scene's own geometry, not an "
		       "invented one -- an illustrative name would be copied and then rejected, spending "
		       "the one repair retry on the harness's own placeholder (A82g proves it inserts)" );
		Check( p.find( "\torientation\t" ) != std::string::npos &&
		       p.find( "\tscale\t\t" ) != std::string::npos &&
		       p.find( "\tposition\t" ) != std::string::npos,
		       "A82a and it shows a different position, orientation AND scale, which is what makes "
		       "a repeat a repeat rather than a duplicate" );

		// NO COUNT, NO EXHORTATION.  Object count is exactly what this arc
		// measures, so a number in the prompt would manufacture the result
		// instead of moving it.
		static const char* const kBannedPopulationAdvice[] = {
			"fill the scene", "as many", "many objects", "how many", "dozens", "a lot of",
			"be generous", "densely", "should add", "at least ten" };
		for( std::size_t i = 0;
		     i < sizeof( kBannedPopulationAdvice ) / sizeof( kBannedPopulationAdvice[0] ); ++i ) {
			Check( p.find( kBannedPopulationAdvice[i] ) == std::string::npos,
			       std::string( "A82a MONEY ASSERTION: the prompt never says \"" ) +
			       kBannedPopulationAdvice[i] + "\" -- it states the contract, the scene and the "
			       "vocabulary, and nothing else; the object count is what this arc MEASURES, so "
			       "the prompt must not put one there" );
		}
		// And no bare digit-plus-object instruction either.
		for( int n = 2; n <= 9; ++n ) {
			const std::string phrase = std::to_string( n ) + " objects";
			Check( p.find( phrase + " into" ) == std::string::npos &&
			       p.find( "add " + phrase ) == std::string::npos,
			       "A82a the prompt never asks for a specific number of objects" );
		}
	}

	// ---- THE REPORT.  Facts only, exactly as the inventory and the tonal
	// fact are.
	Check( r.message.find( "pop_sph_a (geometry sph, material mat_diffuse)" ) != std::string::npos,
	       "A82a the report states what each created object repeats" );
	Check( r.message.find( "had 2 objects before this call and 5 objects after it" )
	       != std::string::npos,
	       "A82a MONEY ASSERTION: and the before/after object count outright -- the number this arc "
	       "measures, stated rather than left to be inferred" );
	static const char* const kBannedPopulationVerdict[] = {
		"consider", "should", "too ", "sparse", "empty", "needs", "richer" };
	for( std::size_t i = 0;
	     i < sizeof( kBannedPopulationVerdict ) / sizeof( kBannedPopulationVerdict[0] ); ++i ) {
		Check( r.message.find( kBannedPopulationVerdict[i] ) == std::string::npos,
		       std::string( "A82a the result never says \"" ) + kBannedPopulationVerdict[i] +
		       "\" -- it reports what landed and what the counts are, and stops" );
	}
	pJob->release();
}

//! A82b: THE HARD CONTRACT.  Only standard_object, and only naming a
//! geometry and a material that already exist.  Everything else is
//! REJECTED with the reason -- never dropped -- and that text drives
//! exactly one repair retry.
static void TestPopulateSceneContractAndRetry()
{
	std::printf( "A82b: populate_scene creates standard_objects and nothing else...\n" );
	const std::string tmp = TempPath( "agentcrud_a82b.RISEscene" );

	// (1) A GEOMETRY chunk, an unknown geometry reference and an unknown
	//     material reference are each refused; the good repeat lands.
	{
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "A82b/reject fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = A82ComposeSession( pJob );

		const std::string mixed =
			"standard_object\n{\n\tname pop_ok\n\tgeometry sph\n\tmaterial mat_diffuse\n"
			"\tposition 1 0 -1\n}\n"
			"sphere_geometry\n{\n\tname pop_new_geo\n\tradius 0.3\n}\n"
			"uniformcolor_painter\n{\n\tname pop_new_pnt\n\tcolor 1 0 0\n}\n"
			"standard_object\n{\n\tname pop_ghost_geo\n\tgeometry ghost\n\tmaterial mat_diffuse\n}\n"
			"standard_object\n{\n\tname pop_ghost_mat\n\tgeometry sph\n\tmaterial ghost_mat\n}\n";
		int calls = 0;
		std::vector<std::string> prompts;
		sess->SetTextCompleter( MakeFakeCompleter( { mixed, kGoodPopulationAnswer },
		                                            &calls, &prompts ) );

		const Agent::AgentSession::AgentPopulateSceneResult r = sess->PopulateScene();
		Check( r.ok, "A82b/reject the call reports its outcome" );
		bool sawGeometryKind = false, sawPainterKind = false;
		bool sawUnknownGeometry = false, sawUnknownMaterial = false;
		for( std::size_t i = 0; i < r.rejected.size(); ++i ) {
			const std::string& why = r.rejected[i].reason;
			if( r.rejected[i].kind == "sphere_geometry" &&
			    why.find( "not part of a population pass" ) != std::string::npos )
				sawGeometryKind = true;
			if( r.rejected[i].kind == "uniformcolor_painter" &&
			    why.find( "not part of a population pass" ) != std::string::npos )
				sawPainterKind = true;
			if( why.find( "the geometry \"ghost\", which this scene does not have" ) != std::string::npos )
				sawUnknownGeometry = true;
			if( why.find( "the material \"ghost_mat\", which this scene does not have" ) != std::string::npos )
				sawUnknownMaterial = true;
		}
		Check( sawGeometryKind && sawPainterKind,
		       "A82b/reject MONEY ASSERTION: a geometry chunk and a painter chunk are BOTH rejected "
		       "-- populate_scene creates standard_object and nothing else, which is what stops it "
		       "becoming a second builder; new form belongs to build_element" );
		Check( sawUnknownGeometry,
		       "A82b/reject MONEY ASSERTION: a repeat naming a geometry this scene does not have is "
		       "rejected WITH THAT REASON, so the pass is told what was wrong rather than that "
		       "something was" );
		Check( sawUnknownMaterial,
		       "A82b/reject and the same for an unknown material" );
		const std::string doc = sess->ReadDocument();
		Check( doc.find( "pop_new_geo" )  == std::string::npos &&
		       doc.find( "pop_new_pnt" )  == std::string::npos &&
		       doc.find( "pop_ghost_geo" ) == std::string::npos &&
		       doc.find( "pop_ghost_mat" ) == std::string::npos,
		       "A82b/reject RED-PROVE: not one of the four reached the document" );
		Check( doc.find( "pop_ok" ) != std::string::npos,
		       "A82b/reject while the good repeat in the same answer landed -- never an "
		       "all-or-nothing on a per-chunk rule" );

		// THE ONE REPAIR RETRY, driven by this harness's own rejection text.
		Check( r.retryRan, "A82b/reject the one repair retry ran" );
		Check( calls == 2, "A82b/reject MONEY ASSERTION: exactly TWO completions -- one, then stop" );
		Check( r.retrySucceeded, "A82b/reject and it landed more chunks" );
		Check( prompts.size() == 2 &&
		       prompts[1].find( "A PREVIOUS ANSWER TO THIS SAME REQUEST WAS PARTLY REJECTED" )
		       != std::string::npos &&
		       prompts[1].find( "which this scene does not have" ) != std::string::npos,
		       "A82b/reject MONEY ASSERTION: the retry is corrected by this harness's own rejection "
		       "text VERBATIM, not by a paraphrase of it" );
		pJob->release();
	}

	// (2) A DUPLICATE NAME is rejected and NOT renamed -- InsertChunks'
	//     own contract, which is this verb's naming rule too.
	{
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "A82b/dupe fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = A82ComposeSession( pJob );
		const std::string collides =
			"standard_object\n{\n\tname obj_sph\n\tgeometry sph\n\tmaterial mat_diffuse\n"
			"\tposition 2 0 0\n}\n";
		sess->SetTextCompleter( MakeFakeCompleter( { collides, collides } ) );
		const Agent::AgentSession::AgentPopulateSceneResult r = sess->PopulateScene();
		Check( r.created.empty(),
		       "A82b/dupe MONEY ASSERTION: a repeat whose name is already taken by an existing "
		       "object is REJECTED, never silently renamed -- renaming would break the references "
		       "the pass wrote between its own chunks" );
		Check( !r.rejected.empty(), "A82b/dupe and it is reported" );
		Check( r.objectCountBefore == 2 && r.objectCountAfter == 2,
		       "A82b/dupe with the object count unmoved, measured both times" );
		pJob->release();
	}
}

//! A82c: the capability refusal, the per-session spend cap, and the
//! nothing-to-repeat case -- the three bounds, each of which must cost the
//! provider nothing.
static void TestPopulateSceneCapabilityCapAndNoStock()
{
	std::printf( "A82c: populate_scene's capability refusal, spend cap and empty-stock case...\n" );
	const std::string tmp = TempPath( "agentcrud_a82c.RISEscene" );

	{
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "A82c/cap-refusal fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = A82ComposeSession( pJob );
		Check( !sess->BuildCapable(), "A82c no completer is installed" );
		const std::string docBefore = sess->ReadDocument();
		const Agent::AgentSession::AgentPopulateSceneResult r = sess->PopulateScene();
		Check( !r.ok && r.capabilityRefusal,
		       "A82c a provider that cannot run a separate completion refuses honestly" );
		Check( r.message.find( "authoring standard_object chunks directly" ) != std::string::npos,
		       "A82c and says outright that hand authoring is not blocked by it" );
		Check( sess->ReadDocument() == docBefore, "A82c with the document untouched" );
		pJob->release();
	}
	{
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "A82c/cap fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = A82ComposeSession( pJob );
		int calls = 0;
		// A completer that always FAILS, so each call is cheap and the cap is
		// what stops the sequence rather than the document filling up.
		Agent::AgentSession::AgentTextCompleter c;
		c.supported    = true;
		c.providerName = "mock";
		c.modelId      = "mock-population-1";
		c.complete = [&calls]( const std::string& ) -> Agent::AgentSession::AgentTextCompletionOutcome
		{
			++calls;
			Agent::AgentSession::AgentTextCompletionOutcome o;
			o.error = "mock refuses";
			return o;
		};
		sess->SetTextCompleter( c );

		for( int i = 0; i < Agent::AgentSession::kPopulateSceneMaxPerSession; ++i )
			sess->PopulateScene();
		const int callsAtCap = calls;
		const Agent::AgentSession::AgentPopulateSceneResult over = sess->PopulateScene();
		Check( !over.ok && over.message.find( "per-session cap" ) != std::string::npos,
		       "A82c MONEY ASSERTION: the call past the per-session cap is refused and says so" );
		Check( calls == callsAtCap,
		       "A82c and it reached the provider ZERO further times -- the cap bounds real money" );
		pJob->release();
	}
	{
		const std::string tmp2 = TempPath( "agentcrud_a82c_nostock.RISEscene" );
		Job* pJob = LoadScene( kNoObjectScene, tmp2 );
		Check( pJob != nullptr, "A82c/nostock fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = A82ComposeSession( pJob );
		int calls = 0;
		sess->SetTextCompleter( MakeFakeCompleter( { kGoodPopulationAnswer }, &calls ) );
		const Agent::AgentSession::AgentPopulateSceneResult r = sess->PopulateScene();
		Check( !r.ok && r.message.find( "nothing for a placement to reference" ) != std::string::npos,
		       "A82c/nostock MONEY ASSERTION: a scene with no standard_object naming both a geometry "
		       "and a material has nothing to repeat, and the pass says so" );
		Check( calls == 0,
		       "A82c/nostock and it never called the provider -- sending a prompt whose vocabulary "
		       "list is empty would spend real money to be told what the harness already knows" );
		pJob->release();
	}
}

//! A82d: THE COMPOSE-PHASE RENDER GATE.  In compose, the FIRST full-scene
//! render is refused once and names populate_scene.  Renders in the pieces
//! phase are never touched, isolate renders are never touched, and the
//! refusal fires at most once whatever the model does next.
static void TestComposePhaseFirstRenderRefusal()
{
	std::printf( "A82d: in compose, the first render is gated on populate_scene...\n" );
	const std::string tmp = TempPath( "agentcrud_a82d.RISEscene" );

	// (1) REFUSED ONCE, naming the verb -- and ONLY once.
	{
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "A82d/refuse fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = A82ComposeSession( pJob );
		sess->SetTextCompleter( MakeFakeCompleter( { kGoodPopulationAnswer } ) );
		Check( sess->BuildPhase() == Agent::AgentSession::AgentBuildPhase::Compose,
		       "A82d the session is in the compose phase" );

		const Agent::AgentRenderResult r1 = sess->Render( A82ModelRender() );
		Check( !r1.ok,
		       "A82d MONEY ASSERTION: the first compose-phase render is refused while "
		       "populate_scene has not run -- population belongs before you judge the picture, and "
		       "the first compose render is the moment the model turns to judging it" );
		Check( r1.message.find( "populate_scene" ) != std::string::npos &&
		       r1.message.find( "render refused" ) != std::string::npos,
		       "A82d and the refusal names the verb and says what happened" );
		Check( sess->BuildPhaseRefusalCount() == 1,
		       "A82d it burns exactly one slot of the SHARED phase counter (the fifth arm)" );

		const Agent::AgentRenderResult r2 = sess->Render( A82ModelRender() );
		Check( r2.ok,
		       "A82d MONEY ASSERTION: the SECOND render proceeds even though populate_scene still "
		       "has not run -- this arm fires at most once per session, because a refused EDIT "
		       "leaves a model able to look at its scene while a refused RENDER leaves it blind" );
		Check( sess->BuildPhaseRefusalCount() == 1,
		       "A82d and no second slot of the shared counter was spent" );
		const Agent::AgentRenderResult r3 = sess->Render( A82ModelRender() );
		Check( r3.ok, "A82d -- and it stays lifted" );
		pJob->release();
	}

	// (2) RUNNING THE PASS lifts it too, on a session that has not spent
	//     the one-shot -- and the pass itself is never gated by the rule it
	//     arms, because it fires no render at all.
	{
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "A82d/lift fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = A82ComposeSession( pJob );
		sess->SetTextCompleter( MakeFakeCompleter( { kGoodPopulationAnswer } ) );

		const Agent::AgentSession::AgentPopulateSceneResult pr = sess->PopulateScene();
		Check( pr.ok && pr.created.size() == 3, "A82d/lift the pass runs and lands its repeats" );
		Check( sess->Render( A82ModelRender() ).ok,
		       "A82d/lift MONEY ASSERTION: once populate_scene has run, the very first render "
		       "proceeds -- population through the clean room, judgement in the model's hands" );
		Check( sess->BuildPhaseRefusalCount() == 0,
		       "A82d/lift and no refusal was ever spent" );
		pJob->release();
	}

	// (3) A FAILED PASS LIFTS IT TOO.  The gate keys on "has the pass
	//     reached the provider", not on "did anything land": refusing
	//     renders after the clean room's one turn would strand exactly the
	//     session whose provider failed.
	{
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "A82d/failed fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = A82ComposeSession( pJob );
		Agent::AgentSession::AgentTextCompleter c;
		c.supported    = true;
		c.providerName = "mock";
		c.modelId      = "mock-population-1";
		c.complete = []( const std::string& ) -> Agent::AgentSession::AgentTextCompletionOutcome
		{
			Agent::AgentSession::AgentTextCompletionOutcome o;
			o.error = "mock refuses";
			return o;
		};
		sess->SetTextCompleter( c );
		const Agent::AgentSession::AgentPopulateSceneResult pr = sess->PopulateScene();
		Check( !pr.ok, "A82d/failed the pass did not complete" );
		Check( sess->Render( A82ModelRender() ).ok,
		       "A82d/failed MONEY ASSERTION: and the render still proceeds -- a failed clean room "
		       "must not leave a session unable to look at its own scene" );
		pJob->release();
	}

	// (4) THE SEAM: pieces-phase renders are never gated, and neither are
	//     isolate renders or the session's own internal passes.
	{
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "A82d/pieces fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
		sess->SetTextCompleter( MakeFakeCompleter( { kGoodPopulationAnswer } ) );
		Check( sess->FileBuildPlan( WizardOnlyPlan() ).ok, "A82d/pieces the plan files" );
		Check( sess->BuildPhase() == Agent::AgentSession::AgentBuildPhase::Pieces,
		       "A82d/pieces the session is in the pieces phase" );
		Check( sess->Render( A82ModelRender() ).ok,
		       "A82d/pieces MONEY ASSERTION: a pieces-phase render is NEVER refused -- arc 78 sec "
		       "2.3's rule that a model must always be able to look at the part it is building, "
		       "which this arc must not take back" );
		Check( sess->BuildPhaseRefusalCount() == 0, "A82d/pieces and spends no refusal" );

		Check( sess->FinishElement().ok, "A82d/pieces the element finishes" );
		Check( sess->BuildPhase() == Agent::AgentSession::AgentBuildPhase::Compose,
		       "A82d/pieces the session is now in compose" );
		{
			// An ISOLATE render is looking at ONE part, not judging the
			// picture -- the same exclusion the arc-80 inventory makes.
			Agent::AgentRenderParams iso = A82ModelRender();
			iso.isolate = "obj_sph";
			Check( sess->Render( iso ).ok,
			       "A82d/pieces MONEY ASSERTION: an isolate render in COMPOSE is not refused either "
			       "-- it looks at one object, which is not judging the composed picture" );
			Check( sess->BuildPhaseRefusalCount() == 0, "A82d/pieces and spends no refusal" );
		}
		{
			// AN INTERNAL PASS.  `scene_inventory` runs a real render of its
			// own; every internal render in AgentSession leaves
			// `fromAgentSurface` false, which is exactly why the gate cannot
			// see them.
			const Agent::AgentSession::AgentSceneInventoryResult si = sess->SceneInventory();
			Check( si.ok,
			       "A82d/pieces MONEY ASSERTION: an INTERNAL render (the inventory's identity pass) "
			       "is never gated -- a diagnostic that could be refused would be a diagnostic that "
			       "stops working exactly when it is needed" );
			Check( sess->BuildPhaseRefusalCount() == 0, "A82d/pieces and spends no refusal" );
		}
		// ... and the model-issued full-scene render is still refused once.
		Check( !sess->Render( A82ModelRender() ).ok,
		       "A82d/pieces the gate is still armed for the render that IS judging the picture" );
		Check( sess->BuildPhaseRefusalCount() == 1, "A82d/pieces and THAT one spends the slot" );
		pJob->release();
	}

	// (5) PROTOCOL OFF disables the refusal with everything else -- and
	//     populate_scene itself still WORKS, because placing objects needs
	//     no element window and a protocol-off session has no phases at all.
	{
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "A82d/off fixture loads" );
		if( !pJob ) return;
		Agent::AgentSession::SetBuildProtocolDefaultEnabled( false );
		std::unique_ptr<Agent::AgentSession> sess = WrapJobGateArmed( pJob );
		Agent::AgentSession::SetBuildProtocolDefaultEnabled( true );
		sess->SetTextCompleter( MakeFakeCompleter( { kGoodPopulationAnswer } ) );
		Check( !sess->BuildProtocolActive(), "A82d/off the protocol is inactive for this session" );

		Check( sess->Render( A82ModelRender() ).ok,
		       "A82d/off MONEY ASSERTION: a render is NOT refused with --agent-build-protocol=off -- "
		       "the gate dies with the protocol like its four siblings" );
		const Agent::AgentSession::AgentPopulateSceneResult pr = sess->PopulateScene();
		Check( pr.ok,
		       "A82d/off MONEY ASSERTION: and populate_scene STILL WORKS with the protocol off -- "
		       "refusing it on a session that has no phases at all would be exactly the "
		       "over-refusal arc 78 sec 2.3 names as this design family's worst failure mode" );
		pJob->release();
	}
}

//! A82e: THE THREE-ARM CAP INTERACTION.  Five arms now share
//! RefuseForPhase_'s 3-refusal counter, three of which live in COMPOSE:
//! arc 80's delete ban, arc 81's first-light gate and arc 82's first-render
//! gate.  A session that hits all three must not be starved -- and the
//! render arm's one-shot is what bounds its share of the budget.
static void TestThreeComposeArmsShareOneCap()
{
	std::printf( "A82e: the three compose-phase arms share one cap without starving anyone...\n" );
	const std::string tmp = TempPath( "agentcrud_a82e.RISEscene" );

	// (1) ALL THREE FIRE, one slot each, then the FOURTH refusable call
	//     gives up and proceeds.
	{
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "A82e/all3 fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = A82ComposeSession( pJob );
		sess->SetTextCompleter( MakeFakeCompleter( { kGoodPopulationAnswer } ) );

		Check( !sess->InsertChunk( A81Light( "a82e_hand_key" ) ).applied,
		       "A82e/all3 arc 81's first-light arm refuses (slot 1)" );
		Check( sess->BuildPhaseRefusalCount() == 1, "A82e/all3 one slot spent" );
		Check( !sess->RemoveChunk( "obj_sph" ).applied,
		       "A82e/all3 arc 80's delete ban refuses (slot 2)" );
		Check( sess->BuildPhaseRefusalCount() == 2, "A82e/all3 two slots spent" );
		Check( !sess->Render( A82ModelRender() ).ok,
		       "A82e/all3 arc 82's first-render arm refuses (slot 3)" );
		Check( sess->BuildPhaseRefusalCount() == 3,
		       "A82e/all3 MONEY ASSERTION: all three COMPOSE arms draw on ONE shared budget -- a "
		       "model that cannot work the protocol must not have to exhaust three separate ones" );
		Check( !sess->BuildPhaseGaveUp(), "A82e/all3 three is the cap, not the give-up" );

		const Agent::AgentChunkResult r4 = sess->InsertChunk( A81Light( "a82e_hand_2" ) );
		Check( r4.applied && sess->BuildPhaseGaveUp(),
		       "A82e/all3 MONEY ASSERTION: the FOURTH refusable call GIVES UP and proceeds -- the "
		       "give-up is a GLOBAL release, so no arrangement of the three can strand a session" );
		Check( sess->Render( A82ModelRender() ).ok &&
		       sess->RemoveChunk( "obj_sph" ).applied,
		       "A82e/all3 and every arm has stopped intercepting, render included" );
		pJob->release();
	}

	// (2) THE RENDER ARM'S SHARE IS BOUNDED AT ONE.  Ten renders in a row
	//     spend one slot between them, leaving the other two arms their
	//     budget -- the property the one-shot exists to guarantee.
	{
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "A82e/bound fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = A82ComposeSession( pJob );
		sess->SetTextCompleter( MakeFakeCompleter( { kGoodPopulationAnswer } ) );

		int refusedRenders = 0;
		for( int i = 0; i < 10; ++i )
			if( !sess->Render( A82ModelRender() ).ok ) ++refusedRenders;
		Check( refusedRenders == 1,
		       "A82e/bound MONEY ASSERTION: TEN renders produce exactly ONE refusal (got " +
		       std::to_string( refusedRenders ) + ") -- without the one-shot the render arm alone "
		       "would burn the whole shared cap and trip the give-up, silently disarming arc 80's "
		       "delete ban and arc 81's light gate for the rest of the session" );
		Check( sess->BuildPhaseRefusalCount() == 1 && !sess->BuildPhaseGaveUp(),
		       "A82e/bound and two slots are left for the other two arms" );
		Check( !sess->InsertChunk( A81Light( "a82e_b_key" ) ).applied &&
		       !sess->RemoveChunk( "obj_sph" ).applied,
		       "A82e/bound which they then use, both still intercepting" );
		Check( sess->BuildPhaseRefusalCount() == 3, "A82e/bound the cap is reached, exactly" );
		pJob->release();
	}

	// (3) THE OPPOSITE ORDER: the other two arms exhaust the cap FIRST.
	//     The render arm then never refuses -- the failure direction is
	//     always "let through", never "blocked forever" -- and the give-up
	//     notice rides the render's own result so the event is visible.
	{
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "A82e/spent fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = A82ComposeSession( pJob );
		sess->SetTextCompleter( MakeFakeCompleter( { kGoodPopulationAnswer } ) );

		Check( !sess->InsertChunk( A81Light( "a82e_s1" ) ).applied, "A82e/spent slot 1" );
		Check( !sess->InsertChunk( A81Light( "a82e_s2" ) ).applied, "A82e/spent slot 2" );
		Check( !sess->InsertChunk( A81Light( "a82e_s3" ) ).applied, "A82e/spent slot 3" );
		Check( sess->BuildPhaseRefusalCount() == 3 && !sess->BuildPhaseGaveUp(),
		       "A82e/spent the cap is reached by the light arm alone" );

		const Agent::AgentRenderResult rr = sess->Render( A82ModelRender() );
		Check( rr.ok,
		       "A82e/spent MONEY ASSERTION: the render PROCEEDS -- an arm that arrives after the "
		       "shared budget is spent lets the call through and triggers the give-up; it can never "
		       "block one forever" );
		Check( sess->BuildPhaseGaveUp(), "A82e/spent and the session has given up" );
		Check( rr.message.find( "stopped intercepting" ) != std::string::npos,
		       "A82e/spent MONEY ASSERTION: with the give-up notice folded into the RENDER's own "
		       "result, so a trajectory census sees the event rather than only a log line" );
		pJob->release();
	}
}

//! A82f: populate_scene over JSON-RPC -- the wire shape, the one schema
//! error it has, and the gated render as a model actually meets it.
static void TestPopulateSceneWireShape()
{
	std::printf( "A82f: populate_scene over JSON-RPC...\n" );
	const std::string tmp = TempPath( "agentcrud_a82f.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "A82f fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = A82ComposeSession( pJob );
	sess->SetTextCompleter( MakeFakeCompleter( { kGoodPopulationAnswer } ) );
	Agent::AgentRpcDispatcher rpc( std::move( sess ) );

	// THE GATED RENDER, over the real wire -- the surface a model meets.
	{
		const std::string resp = rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"render\",\"params\":{\"width\":24,\"height\":24,\"samples\":1}}" );
		Agent::JsonValue result;
		Check( JsonResultObj( resp, result ), "A82f the gated render returns a result object" );
		Check( !result.get( "ok" ).asBool( true ),
		       "A82f MONEY ASSERTION: the first compose-phase render over the wire is refused" );
		Check( result.get( "message" ).asString().find( "populate_scene" ) != std::string::npos,
		       "A82f and the message names the verb that lifts it" );
	}
	{
		const std::string resp = rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"populate_scene\",\"params\":{}}" );
		Agent::JsonValue result;
		Check( JsonResultObj( resp, result ), "A82f populate_scene returns a result object" );
		Check( result.get( "ok" ).asBool( false ), "A82f ok:true" );
		Check( result.get( "created" ).isArray() && result.get( "created" ).size() == 3,
		       "A82f the created list rides the wire" );
		Check( result.get( "created" ).at( 0 ).get( "geometry" ).asString() == "sph" &&
		       result.get( "created" ).at( 0 ).get( "material" ).asString() == "mat_diffuse",
		       "A82f MONEY ASSERTION: with what each one REPEATS as a STRUCTURED fact, not only "
		       "prose" );
		Check( result.get( "objectsBefore" ).asNumber( -1 ) == 2.0 &&
		       result.get( "objectsAfter" ).asNumber( -1 ) == 5.0,
		       "A82f and the before/after object counts" );
		Check( result.get( "retryRan" ).asBool( true ) == false, "A82f retryRan is reported" );
	}
	{
		const std::string resp = rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"render\",\"params\":{\"width\":24,\"height\":24,\"samples\":1}}" );
		Agent::JsonValue result;
		Check( JsonResultObj( resp, result ), "A82f the post-pass render returns a result object" );
		Check( result.get( "ok" ).asBool( false ),
		       "A82f MONEY ASSERTION: and the render after the pass PROCEEDS over the same wire" );
	}
	// The ONE schema defect: a non-string `notes`.  There are no required
	// params -- which scene gets populated is a property of the session.
	{
		const std::string resp = rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"populate_scene\",\"params\":{\"notes\":7}}" );
		Check( resp.find( "-32602" ) != std::string::npos,
		       "A82f a non-string `notes` is a schema error" );
	}
}

//! A82g: THE EXAMPLE INSERTS.  populate_scene ships one worked example, and
//! an example that does not parse is worse than none -- it spends the
//! model's repair retry on the harness's own typo.  So the example is
//! EXTRACTED FROM THE SHIPPED PROMPT (not retyped here, which would only
//! prove the copy parses) and pushed through the real validated insertion.
//! A81h's approach, applied to this arc's one example.
static void TestPopulationExampleInserts()
{
	std::printf( "A82g: populate_scene's worked example really inserts...\n" );
	const std::string tmp = TempPath( "agentcrud_a82g.RISEscene" );

	// ---- Lift the example out of the prompt this surface actually sends.
	std::string example;
	{
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "A82g/compose fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = A82ComposeSession( pJob );
		std::vector<std::string> prompts;
		sess->SetTextCompleter( MakeFakeCompleter( { kGoodPopulationAnswer }, nullptr, &prompts ) );
		sess->PopulateScene();
		Check( prompts.size() == 1, "A82g a prompt was composed" );
		if( !prompts.empty() ) {
			const std::string& p = prompts[0];
			const std::string head = "orientation and scale:\n";
			const std::size_t a = p.find( head );
			const std::size_t b = ( a == std::string::npos )
				? std::string::npos : p.find( "\n\nWHAT THIS CALL WILL ACCEPT", a );
			Check( a != std::string::npos && b != std::string::npos,
			       "A82g the example block is locatable in the prompt" );
			if( a != std::string::npos && b != std::string::npos )
				example = p.substr( a + head.size(), b - ( a + head.size() ) );
		}
		pJob->release();
	}
	Check( example.compare( 0, 16, "standard_object\n" ) == 0,
	       "A82g and the block lifted is a standard_object chunk" );
	if( example.empty() ) return;

	// ---- Push it through the real insertion path, verbatim, on a FRESH
	//      copy of the same scene -- so what is proved is that the example
	//      the model is shown lands in the scene the model is shown it for.
	{
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "A82g/insert fixture loads" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = A82ComposeSession( pJob );
		sess->SetTextCompleter( MakeFakeCompleter( { example } ) );
		const Agent::AgentSession::AgentPopulateSceneResult r = sess->PopulateScene();

		Check( r.ok && r.chunksExtracted == 1,
		       "A82g the example is one chunk and it was extracted" );
		Check( r.created.size() == 1 && r.rejected.empty(),
		       "A82g MONEY ASSERTION: the shipped example LANDS through the real validated "
		       "insertion -- including the reference rule, which means the geometry and the "
		       "material it names really do exist in this scene. A worked example that gets "
		       "rejected is worse than no example at all" );
		Check( !r.retryRan, "A82g with no repair retry -- nothing was rejected to repair" );
		Check( r.objectCountAfter == r.objectCountBefore + 1,
		       "A82g and the scene really gained an object" );
		Check( !r.created.empty() && !r.created[0].geometry.empty() &&
		       sess->ReadDocument().find( r.created[0].name ) != std::string::npos,
		       "A82g which is in the document under the name the example used" );
		pJob->release();
	}
}

int main()
{
	// G2 (2026-08-10): the build-plan gate is ON by default in production (a
	// construction site nobody remembered to touch gets it -- the fail-safe
	// polarity).  This binary does not test the gate, and its fixtures insert
	// geometry directly, so opt OUT once here rather than at every session.
	// The gate's own coverage lives in AgentChunkCrudTest's G2 block, which
	// re-enables it explicitly per session.
	RISE::Agent::AgentSession::SetBuildPlanGateDefaultEnabled( false );
	std::printf( "=== AgentChunkCrudTest (Model-B F5 slice S2: insert_chunk / remove_chunk; R1a: remove_chunks) ===\n" );

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
	TestRasterizerAllowlistGate();
	TestActionableRemoveDiagnostics();
	TestRemoveChunksBatch();
	TestRemoveChunksWireShape();
	TestRemoveChunksLiveAndProposal();
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
	TestBuildPlanGateRefusesUntilFiledCapped();
	TestBuildPlanFiledMidRefusalSequenceClearsGate();
	TestBuildPlanFiledFirstNeverIntercepts();
	TestBuildPlanGateEveryTriggeringVerb();
	TestBuildPlanGateIgnoresNonGeometry();
	TestBuildPlanAnyPlanAcceptedAndNonBinding();
	TestBuildPlanGateDisableSwitch();
	TestBuildPlanGatePatchArm();        // G2 fix-round (2026-08-10): the value-splice bypass
	TestBuildPlanGateStagedResolve();   // G2 fix-round (2026-08-10): the staged/resolve re-check
	TestBuildPlanWireShape();
	// G3a (2026-08-10): the sketch artifact -- schema v2.
	TestElementSketchRasterizer();
	TestElementSketchDeterminism();
	TestElementSketchWireRejections();
	// G3a fix-round (2026-08-10): FIX 1 (subnormal-extent floor) and FIX 3
	// (multi-row/truncation composite tiling + accept boundaries).
	TestElementSketchSmallButSaneExtentStillRasterizes();
	TestElementSketchCompositeTilingAndAcceptBoundaries();
	TestElementSketchReplaceSemantics();
	TestElementSketchRefusalText();
	// Arc 77 Phase 2 (2026-08-11): the whole-scene imagined target.
	TestImagineCapabilityRefusal();
	TestImagineSuccessAndReplace();
	TestImagineProviderFailureDisarms();
	TestImagineTwoConditionGate();
	TestImagineSchemaErrorDisarmsNothing();
	TestImaginePerSessionSpendCap();
	// S1 (2026-08-11): the staged build protocol.
	TestBuildProtocolPhasesAndAttribution();
	TestBuildProtocolExemptionsAndGiveUp();
	TestBuildProtocolIsolateRenderAndSwitchOff();
	TestBuildProtocolRefusalCallSites();
	TestBuildProtocolErasedGeometryAttribution();
	TestBuildProtocolDiagnosedAttribution();
	TestBuildProtocolWireShape();
	// S2 (2026-08-11): clean-room construction.
	TestCleanRoomBuildElementHappyPath();
	TestCleanRoomValidatedInsertion();
	TestCleanRoomRepairRetry();
	TestCleanRoomBuildElementRefusals();
	TestCleanRoomBuildElementBuildPlanGate();
	TestCleanRoomFirstGeometryRefusal();
	TestCleanRoomProtocolOff();
	TestCleanRoomPlaceElement();
	TestCleanRoomBuildElementQuotedNameAndTotalRejection();
	TestCleanRoomWireShape();
	TestCleanRoomReplaceGeometryScaffoldGate();
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

	// R2 (2026-08-10): replace_geometry_scaffold -- one-call form revision.
	TestReplaceGeometryScaffoldFamilies();
	TestReplaceGeometryScaffoldRetainsSharedGeometry();
	TestReplaceGeometryScaffoldReportsDeeperOrphans();
	TestReplaceGeometryScaffoldVolumeBankRefused();
	TestReplaceGeometryScaffoldGeometryTargetDiagnosis();
	TestReplaceGeometryScaffoldUnknownAndAmbiguousTarget();
	TestReplaceGeometryScaffoldAtomicRefusals();
	TestReplaceGeometryScaffoldGatesAreDelta();
	TestReplaceGeometryScaffoldExternalAuthority();
	TestReplaceGeometryScaffoldWireShape();

	// Arc 80 (2026-08-12): the compose phase may not delete form.
	TestComposePhaseRefusesRemovingForm();
	TestComposePhaseRemoveMixedBatchAndGiveUp();

	// Arc 81 (2026-08-12): the clean-room lighting pass and its gate.
	TestLightSceneHappyPath();
	TestLightSceneAdmissibilityAndRetry();
	TestLightSceneCapabilityAndCap();
	TestComposePhaseFirstLightRefusal();
	TestPiecesPhaseLightsNeitherRefusedNorDisarming();
	TestLightSceneWireShape();
	TestAmbientLightIsAlwaysRefused();
	TestPaletteAreaLightExampleParses();
	// Arc 83 slice 1 (2026-08-12): the plan step, the one-light unit, the
	// loop's independence, and what a later intent is told.
	TestLightSceneIntentPlanBudget();
	TestLightSceneOneLightPerIntent();
	TestLightSceneFailedIntentDoesNotAbort();
	TestLightSceneLaterIntentsSeeEarlierLights();

	// Arc 82 (2026-08-12): the clean-room population pass and its render gate.
	TestPopulateSceneHappyPath();
	TestPopulateSceneContractAndRetry();
	TestPopulateSceneCapabilityCapAndNoStock();
	TestComposePhaseFirstRenderRefusal();
	TestThreeComposeArmsShareOneCap();
	TestPopulateSceneWireShape();
	TestPopulationExampleInserts();

	std::printf( "AgentChunkCrudTest: %d passed, %d failed\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
