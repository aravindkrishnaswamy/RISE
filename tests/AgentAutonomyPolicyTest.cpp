//////////////////////////////////////////////////////////////////////
//
//  AgentAutonomyPolicyTest.cpp - Secure-MCP slice 2: the headless
//    autonomy-policy regression guard.
//
//  Six in-process/subprocess suites:
//
//    (a) READ posture (in-process AgentRpcDispatcher constructed with
//        AgentAutonomy::Read): the single-item mutating verbs
//        (propose_patch, insert_chunk, remove_chunk) are refused with
//        the EXACT
//        {code:-32011, message, data:{verb,autonomy:"read"}} shape;
//        RED-PROVE the document is byte-UNCHANGED after each refused
//        attempt (a naive "check-then-mutate" implementation would still
//        mutate before noticing the refusal -- this proves the choke
//        point runs BEFORE dispatch, not as an after-the-fact undo).  All
//        9 read/render verbs (read_document, read_schema, read_skill,
//        validate, render, render_status, render_wait, render_cancel,
//        read_image) still work -- spot-check render + read_image
//        actually produce a real image (not just "no error").
//
//    (b) COMMIT posture: byte-IDENTICAL behavior to a no-param dispatcher
//        (the class default) -- spot-check propose_patch actually applies
//        under BOTH an explicit Commit dispatcher and a default-
//        constructed one, and the two produce the same result shape.
//
//    (c) MCP layer: tools/call propose_patch under Read -> isError:true
//        CallToolResult carrying the refusal text (NOT a top-level
//        JSON-RPC protocol error); tools/list under Read annotates the 3
//        mutating tools' descriptions with the
//        "[REFUSED under --agent-autonomy=read" note while KEEPING them
//        in the list (not hidden) -- RED-PROVE both halves: the note is
//        present under Read AND absent under Commit.  Secure-MCP slice 5b
//        fix round (P2-1): tools/list under Propose instead annotates the
//        SAME 3 tools with the DISTINCT "[NOTE under --agent-autonomy=
//        propose" staging note (never the Read refusal note) -- RED-PROVE
//        it is present under Propose, absent under Commit, and that
//        propose_patch's own description documents "staged" as a status.
//
//    (d) Launch-flag parsing: real-subprocess smoke (fork/pipe/execl,
//        mirroring AgentStdioSmokeTest.cpp's pattern) --
//        `--agent-stdio --agent-autonomy=read` refuses a propose_patch
//        over the REAL wire; RED-PROVE `--agent-autonomy=banana`
//        (malformed) exits nonzero with a loud stderr usage error rather
//        than silently defaulting to either posture.
//
//    (e) RED-PROVE deny-by-default (the load-bearing hardening item): a
//        HYPOTHETICAL verb name that is on NEITHER the read-safe allowlist
//        NOR the dispatch table -- simulating a future verb #13 that
//        mutates the Document but was never classified -- is REFUSED
//        (kAutonomyRefused, -32011) under Read, exactly like a real
//        mutating verb, even though nobody ever added it to any
//        blacklist.  Under Commit the SAME unknown verb name instead
//        falls through to the ordinary "method not found" (-32601) path,
//        proving the two codes are still distinguishable and the refusal
//        under Read is a POLICY gate, not an accidental byproduct of the
//        verb being unrecognized.
//
//    (f) Launch-flag parsing loud failures (subprocess): a BARE
//        `--agent-autonomy` (no `=value` at all) and the SPACE form
//        `--agent-autonomy read` both exit nonzero with a loud stderr
//        message naming the flag -- RED-PROVING neither silently falls
//        back to the default `read` posture nor silently swallows the
//        next token as the scene path.
//
//    (g) Secure-MCP slice 5b: AgentAutonomy::Propose -- propose_patch is
//        refused at the DISPATCHER under Read (kAutonomyRefused, never
//        reaches AgentSession) but REACHES the session under Propose (a
//        JSON-RPC success envelope, whatever AgentSession itself decides);
//        list_proposals stays read-safe under all three postures (a
//        headless session reports an empty array, not an error);
//        resolve_proposal is refused at the dispatcher under BOTH Read and
//        Propose (with autonomy-specific refusal data) and only reaches
//        the session under Commit. The External-authority STAGE path
//        itself (a live controller attached) is AgentProposalWireTest.
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Agent/AgentSession.h"
#include "../src/Library/Agent/AgentRpc.h"
#include "../src/Library/Agent/AgentMcpAdapter.h"
#include "../src/Library/Agent/Json.h"
#include "../src/Library/Agent/Base64.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
// Round-8 review P2: per-process temp filenames -- see WriteTemp below.
#ifdef _WIN32
	#include <process.h>
	#define getpid _getpid
#else
	#include <unistd.h>			// getpid()
#endif

using namespace RISE;
using namespace RISE::Agent;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const std::string& w )
{
	if( c ) ++g_pass;
	else { ++g_fail; std::printf( "  FAIL: %s\n", w.c_str() ); }
}

// Same tiny inline native-v7 scene the other agentic-surface tests use: a
// lit diffuse sphere, PT at low spp, OIDN off.
static const char* const kScene =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 8\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 24\n\theight 24\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_albedo\n\tcolor 0.5 0.5 0.5\n}\n\n"
	"lambertian_material\n{\n\tname mat_diffuse\n\treflectance pnt_albedo\n}\n\n"
	"sphere_geometry\n{\n\tname sph\n\tradius 0.8\n}\n\n"
	"standard_object\n{\n\tname obj_sph\n\tgeometry sph\n\tmaterial mat_diffuse\n}\n";

static std::string WriteTemp( const char* name, const std::string& text )
{
	const char* base = std::getenv( "TMPDIR" );
	std::string dir = base ? base : "/tmp";
	if( !dir.empty() && dir.back() != '/' ) dir += '/';
	// Round-8 review P2, reason CORRECTED in round 10: per-process filename.
	// The round-8 comment justified this by asserting that run_all_tests.sh
	// runs the suite in PARALLEL.  IT DOES NOT -- Phase 3 is a plain
	// sequential `for` loop that waits on each binary before starting the
	// next (only the BUILD phases pass -j), and run_all_tests.ps1 is
	// likewise sequential for execution.  The real justification is that
	// nothing stops two copies of THIS binary from running at once: a
	// developer runs it by hand while the suite runs, a stray earlier run
	// has not exited yet, or a repeat-run loop (`for i in $(seq 8); do
	// ./bin/tests/<name> & done` -- the usual way to chase a suspected
	// flake) launches several at once.  With a FIXED temp name those
	// processes clobber each other's scene file mid-load, which surfaces as
	// a bogus "the test is flaky / there is a race" failure -- that already
	// cost a reviewer hours once.  The pid prefix makes the path unique per
	// process.
	std::string path = dir + std::to_string( (long)getpid() ) + "_" + name;
	std::ofstream f( path.c_str(), std::ios::binary );
	if( !f ) return std::string();
	f.write( text.data(), (std::streamsize)text.size() );
	f.close();
	return path;
}

// Build a JSON-RPC request line for `method` with `params` under `id`.
static std::string Req( double id, const std::string& method, const JsonValue& params )
{
	JsonValue r = JsonValue::MakeObject();
	r.set( "jsonrpc", JsonValue::MakeString( "2.0" ) );
	r.set( "id", JsonValue::MakeNumber( id ) );
	r.set( "method", JsonValue::MakeString( method ) );
	r.set( "params", params );
	return JsonSerialize( r );
}

static std::string ReqToolCall( double id, const std::string& toolName, const JsonValue& arguments )
{
	JsonValue params = JsonValue::MakeObject();
	params.set( "name", JsonValue::MakeString( toolName ) );
	params.set( "arguments", arguments );
	return Req( id, "tools/call", params );
}

static JsonValue ParseResponse( const std::string& line, double expectId )
{
	JsonValue env; std::string err;
	const bool ok = JsonParse( line, env, err );
	Check( ok, "response line parses as JSON (" + ( ok ? std::string("ok") : err ) + ")" );
	if( !ok ) return JsonValue::MakeNull();
	Check( env.isObject(), "response is a JSON object" );
	Check( env.get( "jsonrpc" ).asString() == "2.0", "response carries jsonrpc==2.0" );
	Check( env.get( "id" ).asNumber( -999 ) == expectId, "response echoes the request id" );
	return env;
}

// read_document's `document` field, via the dispatcher, for byte-compare.
static std::string ReadDoc( AgentRpcDispatcher& d, double id )
{
	const std::string resp = d.HandleLine( Req( id, "read_document", JsonValue::MakeObject() ) );
	JsonValue env = ParseResponse( resp, id );
	return env.get( "result" ).get( "document" ).asString();
}

//----------------------------------------------------------------------
// (a) READ posture
//----------------------------------------------------------------------
static void TestReadPosture()
{
	std::printf( "--- (a) AgentAutonomy::Read posture ---\n" );
	const std::string scenePath = WriteTemp( "rise_autonomy_read.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the read-posture scene to a temp file" );

	std::unique_ptr<AgentSession> session = AgentSession::LoadFromFile( scenePath );
	Check( session != nullptr, "AgentSession::LoadFromFile loads the native-v7 scene" );
	if( !session ) return;

	AgentRpcDispatcher rpc( std::move( session ), AgentAutonomy::Read );

	const std::string docBefore = ReadDoc( rpc, 1 );
	Check( !docBefore.empty(), "read_document works under Read (baseline captured)" );

	// --- propose_patch refused ---
	{
		JsonValue params = JsonValue::MakeObject();
		params.set( "target", JsonValue::MakeString( "sph" ) );
		params.set( "param",  JsonValue::MakeString( "radius" ) );
		params.set( "value",  JsonValue::MakeString( "0.3" ) );
		const std::string resp = rpc.HandleLine( Req( 2, "propose_patch", params ) );
		JsonValue env = ParseResponse( resp, 2 );
		Check( env.has( "error" ), "propose_patch under Read is a JSON-RPC ERROR (not a success result)" );
		const JsonValue& err = env.get( "error" );
		Check( err.get( "code" ).asNumber( 0 ) == -32011.0,
		       "propose_patch refusal carries the exact code -32011" );
		Check( err.get( "message" ).asString().find( "--agent-autonomy=read" ) != std::string::npos,
		       "refusal message names the launch posture" );
		Check( err.get( "message" ).asString().find( "--agent-autonomy=commit" ) != std::string::npos,
		       "refusal message names the escape hatch (relaunch with commit)" );
		const JsonValue& data = err.get( "data" );
		Check( data.isObject(), "refusal error carries a structured 'data' object" );
		Check( data.get( "verb" ).asString() == "propose_patch",
		       "refusal data.verb == \"propose_patch\"" );
		Check( data.get( "autonomy" ).asString() == "read",
		       "refusal data.autonomy == \"read\"" );
	}
	// RED-PROVE: the document is byte-UNCHANGED after the refused propose_patch.
	Check( ReadDoc( rpc, 3 ) == docBefore,
	       "RED-PROVE: document is BYTE-IDENTICAL after the refused propose_patch (choke point runs BEFORE mutation)" );

	// --- insert_chunk refused ---
	{
		JsonValue params = JsonValue::MakeObject();
		params.set( "chunkText", JsonValue::MakeString(
			"uniformcolor_painter\n{\n\tname pnt_new\n\tcolor 1 0 0\n}\n" ) );
		const std::string resp = rpc.HandleLine( Req( 4, "insert_chunk", params ) );
		JsonValue env = ParseResponse( resp, 4 );
		Check( env.has( "error" ), "insert_chunk under Read is a JSON-RPC ERROR" );
		const JsonValue& err = env.get( "error" );
		Check( err.get( "code" ).asNumber( 0 ) == -32011.0, "insert_chunk refusal carries code -32011" );
		Check( err.get( "data" ).get( "verb" ).asString() == "insert_chunk",
		       "insert_chunk refusal data.verb == \"insert_chunk\"" );
		Check( err.get( "data" ).get( "autonomy" ).asString() == "read",
		       "insert_chunk refusal data.autonomy == \"read\"" );
	}
	Check( ReadDoc( rpc, 5 ) == docBefore,
	       "RED-PROVE: document is BYTE-IDENTICAL after the refused insert_chunk" );

	// --- remove_chunk refused ---
	{
		JsonValue params = JsonValue::MakeObject();
		params.set( "target", JsonValue::MakeString( "sph" ) );
		const std::string resp = rpc.HandleLine( Req( 6, "remove_chunk", params ) );
		JsonValue env = ParseResponse( resp, 6 );
		Check( env.has( "error" ), "remove_chunk under Read is a JSON-RPC ERROR" );
		const JsonValue& err = env.get( "error" );
		Check( err.get( "code" ).asNumber( 0 ) == -32011.0, "remove_chunk refusal carries code -32011" );
		Check( err.get( "data" ).get( "verb" ).asString() == "remove_chunk",
		       "remove_chunk refusal data.verb == \"remove_chunk\"" );
		Check( err.get( "data" ).get( "autonomy" ).asString() == "read",
		       "remove_chunk refusal data.autonomy == \"read\"" );
	}
	Check( ReadDoc( rpc, 7 ) == docBefore,
	       "RED-PROVE: document is BYTE-IDENTICAL after the refused remove_chunk" );

	// --- the 9 read/render verbs still work under Read ---
	std::printf( "  spot-checking all 9 read/render verbs remain available under Read...\n" );
	{
		const std::string resp = rpc.HandleLine( Req( 8, "read_schema", JsonValue::MakeObject() ) );
		JsonValue env = ParseResponse( resp, 8 );
		Check( !env.has( "error" ), "read_schema works under Read" );
	}
	{
		const std::string resp = rpc.HandleLine( Req( 9, "read_skill", JsonValue::MakeObject() ) );
		JsonValue env = ParseResponse( resp, 9 );
		Check( !env.has( "error" ), "read_skill works under Read" );
	}
	{
		JsonValue params = JsonValue::MakeObject();
		params.set( "text", JsonValue::MakeString( docBefore ) );
		const std::string resp = rpc.HandleLine( Req( 10, "validate", params ) );
		JsonValue env = ParseResponse( resp, 10 );
		Check( !env.has( "error" ), "validate works under Read" );
	}
	// DECIDED: render is allowed under Read -- it never mutates the
	// Document; it is the core value of a read-only observer.  Spot-check
	// it actually renders (ok==true, plausible dims), not just "no error".
	double renderJobId = -1.0;
	{
		const std::string resp = rpc.HandleLine( Req( 11, "render", JsonValue::MakeObject() ) );
		JsonValue env = ParseResponse( resp, 11 );
		Check( !env.has( "error" ), "render works under Read (does not mutate the Document)" );
		const JsonValue& result = env.get( "result" );
		Check( result.get( "ok" ).asBool(), "render under Read actually succeeds (ok==true)" );
		Check( result.get( "width" ).asNumber( 0 ) == 24.0, "render under Read produces the scene's real width" );
		Check( result.get( "height" ).asNumber( 0 ) == 24.0, "render under Read produces the scene's real height" );
		renderJobId = result.get( "renderJobId" ).asNumber( -1.0 );
	}
	{
		JsonValue params = JsonValue::MakeObject();
		params.set( "renderJobId", JsonValue::MakeNumber( renderJobId ) );
		const std::string resp = rpc.HandleLine( Req( 12, "render_status", params ) );
		JsonValue env = ParseResponse( resp, 12 );
		Check( !env.has( "error" ), "render_status works under Read" );
	}
	{
		JsonValue params = JsonValue::MakeObject();
		params.set( "renderJobId", JsonValue::MakeNumber( renderJobId ) );
		params.set( "timeoutMs", JsonValue::MakeNumber( 0 ) );
		const std::string resp = rpc.HandleLine( Req( 13, "render_wait", params ) );
		JsonValue env = ParseResponse( resp, 13 );
		Check( !env.has( "error" ), "render_wait works under Read" );
	}
	{
		const std::string resp = rpc.HandleLine( Req( 14, "render_cancel", JsonValue::MakeObject() ) );
		JsonValue env = ParseResponse( resp, 14 );
		Check( !env.has( "error" ), "render_cancel works under Read" );
	}
	// read_image: actually produces a real PNG image (spot-check the magic
	// bytes), not just "no error".
	{
		const std::string resp = rpc.HandleLine( Req( 15, "read_image", JsonValue::MakeObject() ) );
		JsonValue env = ParseResponse( resp, 15 );
		Check( !env.has( "error" ), "read_image works under Read" );
		const std::string b64 = env.get( "result" ).get( "png_base64" ).asString();
		Check( !b64.empty(), "read_image under Read returns a non-empty png_base64" );
		std::vector<unsigned char> png;
		Check( Base64Decode( b64, png ), "read_image's png_base64 decodes cleanly under Read" );
		Check( png.size() >= 8 &&
		       png[0] == 0x89 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G',
		       "read_image under Read produces a REAL PNG (magic bytes), not a stub" );
	}
	// read_document itself (the 9th) was already exercised throughout via ReadDoc().
	Check( !ReadDoc( rpc, 16 ).empty(), "read_document still works under Read at the end of the suite" );

	std::remove( scenePath.c_str() );
}

//----------------------------------------------------------------------
// (e) RED-PROVE deny-by-default: a HYPOTHETICAL, never-classified verb
// name (simulating a future verb #13) is refused under Read even though
// it appears on no allow-list AND no deny-list -- it simply isn't
// IsReadSafeVerb.  Under Commit the identical unknown name instead falls
// through to "method not found", proving the Read refusal is a genuine
// policy gate rather than an artifact of the verb being unrecognized.
//----------------------------------------------------------------------
static void TestDenyByDefaultForUnclassifiedVerb()
{
	std::printf( "--- (e) RED-PROVE deny-by-default for an unclassified verb ---\n" );

	// A verb name that exists in NEITHER AgentRpc.cpp's dispatch table NOR
	// any allow/deny list -- standing in for a future mutating verb that
	// was added to HandleLine's dispatch but never classified read-safe.
	const char* const kHypotheticalVerb = "delete_everything_v13";

	const std::string scenePathRead   = WriteTemp( "rise_autonomy_denybydefault_read.RISEscene", kScene );
	const std::string scenePathCommit = WriteTemp( "rise_autonomy_denybydefault_commit.RISEscene", kScene );
	Check( !scenePathRead.empty() && !scenePathCommit.empty(), "wrote both deny-by-default scenes" );

	std::unique_ptr<AgentSession> sessionRead   = AgentSession::LoadFromFile( scenePathRead );
	std::unique_ptr<AgentSession> sessionCommit = AgentSession::LoadFromFile( scenePathCommit );
	Check( sessionRead != nullptr && sessionCommit != nullptr, "both deny-by-default sessions load" );
	if( !sessionRead || !sessionCommit ) return;

	AgentRpcDispatcher rpcRead( std::move( sessionRead ), AgentAutonomy::Read );
	AgentRpcDispatcher rpcCommit( std::move( sessionCommit ), AgentAutonomy::Commit );

	// --- Under Read: the unclassified verb is REFUSED (-32011), not
	// "method not found" (-32601) -- deny-by-default proves itself: this
	// verb was never added to any mutating-verb blacklist (it doesn't
	// exist anywhere in AgentRpc.cpp), yet Read still refuses it because
	// it fails the read-safe allowlist check. ---
	{
		const std::string resp = rpcRead.HandleLine(
			Req( 1, kHypotheticalVerb, JsonValue::MakeObject() ) );
		JsonValue env = ParseResponse( resp, 1 );
		Check( env.has( "error" ), "RED-PROVE: unclassified verb under Read is a JSON-RPC error" );
		const JsonValue& err = env.get( "error" );
		Check( err.get( "code" ).asNumber( 0 ) == -32011.0,
		       "RED-PROVE: unclassified verb under Read is refused with kAutonomyRefused (-32011), "
		       "NOT method-not-found -- proving deny-by-default (it was never blacklisted, it simply "
		       "isn't on the read-safe allowlist)" );
		Check( err.get( "data" ).get( "verb" ).asString() == kHypotheticalVerb,
		       "RED-PROVE: refusal data.verb names the unclassified verb" );
		Check( err.get( "data" ).get( "autonomy" ).asString() == "read",
		       "RED-PROVE: refusal data.autonomy == \"read\"" );
	}

	// --- Under Commit: the SAME unclassified verb name is NOT refused by
	// the autonomy gate (Commit never gates) -- it falls through to the
	// ordinary "method not found" path, since the verb genuinely doesn't
	// exist in the dispatch table.  This proves the two error codes stay
	// distinguishable: Read's refusal above is a POLICY decision, not a
	// side effect of the verb being unrecognized. ---
	{
		const std::string resp = rpcCommit.HandleLine(
			Req( 1, kHypotheticalVerb, JsonValue::MakeObject() ) );
		JsonValue env = ParseResponse( resp, 1 );
		Check( env.has( "error" ), "unclassified verb under Commit is still a JSON-RPC error (unimplemented verb)" );
		const JsonValue& err = env.get( "error" );
		Check( err.get( "code" ).asNumber( 0 ) == -32601.0,
		       "unclassified verb under Commit is \"method not found\" (-32601), NOT the autonomy-refusal "
		       "code -- Commit performs no gating at all, so this distinguishes the Read result above as "
		       "a genuine policy refusal rather than coincidental overlap between the two error paths" );
	}

	std::remove( scenePathRead.c_str() );
	std::remove( scenePathCommit.c_str() );
}

//----------------------------------------------------------------------
// Secure-MCP slice 5b: AgentAutonomy::Propose -- the wire posture reaches
// AgentSession for the single-item mutating verbs (propose_patch/
// insert_chunk/remove_chunk; the batch forms propose_patches and
// insert_chunks carry the identical posture and are covered by the
// tools/list suites below) rather than refusing them at the dispatcher
// choke point,
// but list_proposals stays read-safe under EVERY posture and
// resolve_proposal stays refused under BOTH Read and Propose (Commit-only).
// This test drives a HEADLESS session (no controller attached) under all
// three postures -- deliberately exercising the honest "External + no
// controller -> refused" leaf from AgentSession::ProposePatch's own doc
// (the same leaf RunAgentHttp's headless-CLI-propose limitation hits),
// distinct from the wire-level GATE this test's job is to red-prove: a
// propose_patch under Propose autonomy must actually REACH the session
// (and get the SESSION's own refusal message, not the dispatcher's
// kAutonomyRefused) -- versus under Read, where it never reaches the
// session at all (kAutonomyRefused fires first). The staging-actually-
// works path (a live controller attached) is AgentProposalWireTest.
//----------------------------------------------------------------------
static void TestProposePosture()
{
	std::printf( "--- (g) AgentAutonomy::Propose posture (Secure-MCP slice 5b) ---\n" );

	const std::string scenePathRead    = WriteTemp( "rise_autonomy_propose_read.RISEscene", kScene );
	const std::string scenePathPropose = WriteTemp( "rise_autonomy_propose_propose.RISEscene", kScene );
	const std::string scenePathCommit  = WriteTemp( "rise_autonomy_propose_commit.RISEscene", kScene );
	Check( !scenePathRead.empty() && !scenePathPropose.empty() && !scenePathCommit.empty(),
	       "wrote all three propose-posture scenes" );

	std::unique_ptr<AgentSession> sessionRead    = AgentSession::LoadFromFile( scenePathRead );
	std::unique_ptr<AgentSession> sessionPropose = AgentSession::LoadFromFile( scenePathPropose );
	std::unique_ptr<AgentSession> sessionCommit  = AgentSession::LoadFromFile( scenePathCommit );
	Check( sessionRead != nullptr && sessionPropose != nullptr && sessionCommit != nullptr,
	       "all three propose-posture sessions load" );
	if( !sessionRead || !sessionPropose || !sessionCommit ) return;

	AgentRpcDispatcher rpcRead(    std::move( sessionRead ),    AgentAutonomy::Read );
	AgentRpcDispatcher rpcPropose( std::move( sessionPropose ), AgentAutonomy::Propose );
	AgentRpcDispatcher rpcCommit(  std::move( sessionCommit ),  AgentAutonomy::Commit );

	auto proposeRadius = [&]( AgentRpcDispatcher& d, double id ) -> JsonValue {
		JsonValue params = JsonValue::MakeObject();
		params.set( "target", JsonValue::MakeString( "sph" ) );
		params.set( "param",  JsonValue::MakeString( "radius" ) );
		params.set( "value",  JsonValue::MakeString( "0.3" ) );
		const std::string resp = d.HandleLine( Req( id, "propose_patch", params ) );
		return ParseResponse( resp, id );
	};

	// --- Under Read: propose_patch refused at the DISPATCHER (kAutonomyRefused). ---
	{
		JsonValue env = proposeRadius( rpcRead, 1 );
		Check( env.has( "error" ), "propose_patch under Read is refused" );
		Check( env.get( "error" ).get( "code" ).asNumber( 0 ) == -32011.0,
		       "propose_patch under Read carries kAutonomyRefused (-32011) -- refused BEFORE reaching the session" );
	}

	// --- Under Propose: propose_patch REACHES the session (headless, no
	// controller attached here) -- it is NOT the dispatcher's
	// kAutonomyRefused; it is the SESSION's own honest headless refusal
	// (AgentSession::ProposePatch's "External + NO controller -> REFUSED"
	// leaf, reached via a plain Owner-authority AgentSession::LoadFromFile
	// default in THIS test -- see the note below). ---
	{
		JsonValue env = proposeRadius( rpcPropose, 2 );
		Check( !env.has( "error" ),
		       "RED-PROVE: propose_patch under Propose is a JSON-RPC SUCCESS envelope (dispatched, not "
		       "refused at the wire choke point) -- it reached AgentSession" );
		const JsonValue& result = env.get( "result" );
		// NOTE: AgentSession::LoadFromFile's default authority is Owner (this
		// test constructs sessionPropose with the 1-arg LoadFromFile, matching
		// every pre-5b call site) -- so under Propose autonomy alone, with an
		// Owner-authority session, the edit actually APPLIES (Owner always
		// commits directly, regardless of the wire's autonomy posture; see
		// AgentRpc.h's file header for why these two knobs are chosen
		// TOGETHER at a real construction site, e.g. commandconsole.cpp's
		// AuthorityForAutonomy, which this unit test intentionally bypasses
		// to isolate the DISPATCHER gate in isolation from the session's
		// authority). The load-bearing proof here is that dispatch reached
		// the session AT ALL under Propose (a real error would still be
		// !env.has("error") since ProposePatch's result is a structured
		// success payload either way) -- the External+controller STAGE path
		// itself is proven by AgentProposalWireTest.
		Check( result.get( "applied" ).asBool( false ) == true,
		       "propose_patch under Propose + Owner-authority session applies (dispatch gate passed; "
		       "authority is a SEPARATE knob -- see AgentProposalWireTest for the External+stage path)" );
	}

	// --- Under Commit: propose_patch applies (today's unrestricted behaviour). ---
	{
		JsonValue env = proposeRadius( rpcCommit, 3 );
		Check( !env.has( "error" ), "propose_patch under Commit is a success" );
		Check( env.get( "result" ).get( "applied" ).asBool( false ) == true,
		       "propose_patch under Commit applies" );
	}

	// --- list_proposals is read-safe under EVERY posture (headless -> empty
	// array, not an error, under all three). ---
	{
		const std::string respRead    = rpcRead.HandleLine(    Req( 4, "list_proposals", JsonValue::MakeObject() ) );
		const std::string respPropose = rpcPropose.HandleLine( Req( 4, "list_proposals", JsonValue::MakeObject() ) );
		const std::string respCommit  = rpcCommit.HandleLine(  Req( 4, "list_proposals", JsonValue::MakeObject() ) );
		JsonValue envRead    = ParseResponse( respRead,    4 );
		JsonValue envPropose = ParseResponse( respPropose, 4 );
		JsonValue envCommit  = ParseResponse( respCommit,  4 );
		Check( !envRead.has( "error" ),    "list_proposals works under Read" );
		Check( !envPropose.has( "error" ), "list_proposals works under Propose" );
		Check( !envCommit.has( "error" ),  "list_proposals works under Commit" );
		Check( envRead.get( "result" ).get( "proposals" ).isArray() &&
		       envRead.get( "result" ).get( "proposals" ).size() == 0,
		       "list_proposals reports an EMPTY array for a headless (no-controller) session, not an error" );
	}

	// --- resolve_proposal is refused under BOTH Read and Propose (at the
	// dispatcher), and reaches the session under Commit (where it then gets
	// a clean "no such id" refusal from AgentSession -- there was never
	// anything staged in this headless test). ---
	{
		JsonValue params = JsonValue::MakeObject();
		params.set( "proposalId", JsonValue::MakeNumber( 1.0 ) );
		params.set( "approve",    JsonValue::MakeBool( true ) );

		JsonValue envRead = ParseResponse(
			rpcRead.HandleLine( Req( 5, "resolve_proposal", params ) ), 5 );
		Check( envRead.has( "error" ), "resolve_proposal under Read is refused at the dispatcher" );
		Check( envRead.get( "error" ).get( "code" ).asNumber( 0 ) == -32011.0,
		       "resolve_proposal under Read carries kAutonomyRefused (-32011)" );

		JsonValue envPropose = ParseResponse(
			rpcPropose.HandleLine( Req( 5, "resolve_proposal", params ) ), 5 );
		Check( envPropose.has( "error" ),
		       "RED-PROVE: resolve_proposal under Propose is ALSO refused at the dispatcher "
		       "(deliberately excluded from Propose's mutating-verb extension)" );
		Check( envPropose.get( "error" ).get( "code" ).asNumber( 0 ) == -32011.0,
		       "resolve_proposal under Propose carries kAutonomyRefused (-32011), the SAME code as Read" );
		Check( envPropose.get( "error" ).get( "data" ).get( "autonomy" ).asString() == "propose",
		       "resolve_proposal's Propose refusal names autonomy=\"propose\" (distinct from Read's "
		       "own refusal, which names \"read\")" );

		JsonValue envCommit = ParseResponse(
			rpcCommit.HandleLine( Req( 5, "resolve_proposal", params ) ), 5 );
		Check( !envCommit.has( "error" ),
		       "resolve_proposal under Commit REACHES the session (not refused at the dispatcher)" );
		Check( envCommit.get( "result" ).get( "resolved" ).asBool( true ) == false,
		       "resolve_proposal under Commit, with nothing ever staged, gets a clean session-level "
		       "refusal (resolved:false, unknown id) -- not a dispatcher policy refusal" );
	}

	std::remove( scenePathRead.c_str() );
	std::remove( scenePathPropose.c_str() );
	std::remove( scenePathCommit.c_str() );
}

//----------------------------------------------------------------------
// (b) COMMIT posture: byte-identical to a no-param (class-default)
// dispatcher.
//----------------------------------------------------------------------
static void TestCommitPostureMatchesDefault()
{
	std::printf( "--- (b) AgentAutonomy::Commit == class default (back-compat) ---\n" );

	const std::string scenePathA = WriteTemp( "rise_autonomy_commit_a.RISEscene", kScene );
	const std::string scenePathB = WriteTemp( "rise_autonomy_commit_b.RISEscene", kScene );
	Check( !scenePathA.empty() && !scenePathB.empty(), "wrote both commit-posture scenes" );

	std::unique_ptr<AgentSession> sessionExplicit = AgentSession::LoadFromFile( scenePathA );
	std::unique_ptr<AgentSession> sessionDefault   = AgentSession::LoadFromFile( scenePathB );
	Check( sessionExplicit != nullptr && sessionDefault != nullptr,
	       "both sessions load the native-v7 scene" );
	if( !sessionExplicit || !sessionDefault ) return;

	// A: explicit AgentAutonomy::Commit.
	AgentRpcDispatcher rpcExplicit( std::move( sessionExplicit ), AgentAutonomy::Commit );
	// B: the CLASS DEFAULT (no autonomy argument at all) -- the exact
	// construction the GUI's live dispatcher and every pre-slice-2 test use.
	AgentRpcDispatcher rpcDefault( std::move( sessionDefault ) );

	auto proposeRadius = [&]( AgentRpcDispatcher& d, double id, const char* value ) -> JsonValue {
		JsonValue params = JsonValue::MakeObject();
		params.set( "target", JsonValue::MakeString( "sph" ) );
		params.set( "param",  JsonValue::MakeString( "radius" ) );
		params.set( "value",  JsonValue::MakeString( value ) );
		const std::string resp = d.HandleLine( Req( id, "propose_patch", params ) );
		return ParseResponse( resp, id );
	};

	JsonValue envExplicit = proposeRadius( rpcExplicit, 1, "0.3" );
	JsonValue envDefault  = proposeRadius( rpcDefault,  1, "0.3" );

	Check( !envExplicit.has( "error" ), "propose_patch under explicit Commit is a SUCCESS (not refused)" );
	Check( !envDefault.has( "error" ),  "propose_patch under the class-default dispatcher is a SUCCESS (not refused)" );
	Check( envExplicit.get( "result" ).get( "applied" ).asBool() == true,
	       "propose_patch APPLIES cleanly under explicit Commit" );
	Check( envDefault.get( "result" ).get( "applied" ).asBool() == true,
	       "propose_patch APPLIES cleanly under the class-default dispatcher" );
	Check( envExplicit.get( "result" ).get( "status" ).asString() ==
	       envDefault.get( "result" ).get( "status" ).asString(),
	       "explicit-Commit and class-default dispatchers produce the SAME status string" );

	// Byte-identical resulting documents (same starting scene, same patch).
	const std::string docExplicit = ReadDoc( rpcExplicit, 2 );
	const std::string docDefault  = ReadDoc( rpcDefault,  2 );
	Check( !docExplicit.empty() && docExplicit == docDefault,
	       "explicit-Commit and class-default dispatchers produce a BYTE-IDENTICAL document after the same patch" );

	std::remove( scenePathA.c_str() );
	std::remove( scenePathB.c_str() );
}

//----------------------------------------------------------------------
// (c) MCP layer
//----------------------------------------------------------------------
static void TestMcpLayer()
{
	std::printf( "--- (c) MCP layer: tools/call refusal + tools/list annotation ---\n" );

	const std::string scenePathRead    = WriteTemp( "rise_autonomy_mcp_read.RISEscene", kScene );
	const std::string scenePathCommit  = WriteTemp( "rise_autonomy_mcp_commit.RISEscene", kScene );
	const std::string scenePathPropose = WriteTemp( "rise_autonomy_mcp_propose.RISEscene", kScene );
	Check( !scenePathRead.empty() && !scenePathCommit.empty() && !scenePathPropose.empty(),
	       "wrote all three MCP-layer scenes" );

	std::unique_ptr<AgentSession> sessionRead    = AgentSession::LoadFromFile( scenePathRead );
	std::unique_ptr<AgentSession> sessionCommit  = AgentSession::LoadFromFile( scenePathCommit );
	std::unique_ptr<AgentSession> sessionPropose = AgentSession::LoadFromFile( scenePathPropose );
	Check( sessionRead != nullptr && sessionCommit != nullptr && sessionPropose != nullptr,
	       "all three MCP sessions load" );
	if( !sessionRead || !sessionCommit || !sessionPropose ) return;

	AgentMcpAdapter mcpRead(    std::move( sessionRead ),    AgentAutonomy::Read );
	AgentMcpAdapter mcpCommit(  std::move( sessionCommit ),  AgentAutonomy::Commit );
	AgentMcpAdapter mcpPropose( std::move( sessionPropose ), AgentAutonomy::Propose );

	// tools/call propose_patch under Read -> isError:true, NOT a protocol error.
	{
		JsonValue args = JsonValue::MakeObject();
		args.set( "target", JsonValue::MakeString( "sph" ) );
		args.set( "param",  JsonValue::MakeString( "radius" ) );
		args.set( "value",  JsonValue::MakeString( "0.3" ) );
		const std::string resp = mcpRead.HandleLine( ReqToolCall( 1, "propose_patch", args ) );
		JsonValue env = ParseResponse( resp, 1 );
		Check( !env.has( "error" ),
		       "tools/call(propose_patch) under Read is a JSON-RPC SUCCESS envelope (not a protocol error)" );
		const JsonValue& result = env.get( "result" );
		Check( result.get( "isError" ).asBool( false ) == true,
		       "tools/call(propose_patch) under Read carries isError:true" );
		const JsonValue& content = result.get( "content" );
		Check( content.isArray() && content.size() >= 1, "isError result carries a content array" );
		const std::string text = content.size() >= 1 ? content.at( 0 ).get( "text" ).asString() : std::string();
		Check( text.find( "-32011" ) != std::string::npos || text.find( "refused" ) != std::string::npos,
		       "isError content text carries the refusal (code -32011 or the word 'refused')" );
		Check( text.find( "read" ) != std::string::npos,
		       "isError content text mentions the read posture" );
	}

	// tools/list under Read: the 3 mutating tools stay PRESENT (not
	// hidden) but their description carries the annotation note; every
	// other tool's description is unannotated -- EXCEPT resolve_proposal
	// (Secure-MCP slice 5b), which carries its OWN distinct annotation
	// (kResolveProposalOwnerOnlyNote, "[OWNER-ONLY:...") since it is
	// refused for a session-authority reason, not the generic
	// "--agent-autonomy=read" reason the other three share.
	{
		const std::string resp = mcpRead.HandleLine( Req( 2, "tools/list", JsonValue::MakeObject() ) );
		JsonValue env = ParseResponse( resp, 2 );
		const JsonValue& tools = env.get( "result" ).get( "tools" );
		Check( tools.isArray() && tools.size() == 19,
		       "tools/list under Read STILL lists all 19 tools (mutating tools are ANNOTATED, not hidden)" );

		bool sawProposePatch = false, sawProposePatches = false, sawInsertChunk = false, sawInsertChunks = false, sawRemoveChunk = false;
		bool sawRender = false, sawListProposals = false, sawResolveProposal = false;
		int annotatedCount = 0;
		for( std::size_t i = 0; i < tools.size(); ++i ) {
			const JsonValue& t = tools.at( i );
			const std::string name = t.get( "name" ).asString();
			const std::string desc = t.get( "description" ).asString();
			const bool annotated = desc.find( "[REFUSED under --agent-autonomy=read" ) != std::string::npos;
			const bool ownerOnlyAnnotated = desc.find( "[OWNER-ONLY:" ) != std::string::npos;
			if( annotated ) ++annotatedCount;
			if( name == "propose_patch" )   { sawProposePatch   = true; Check( annotated, "propose_patch tool description is ANNOTATED under Read" ); }
			if( name == "propose_patches" ) { sawProposePatches = true; Check( annotated, "propose_patches tool description is ANNOTATED under Read" ); }
			if( name == "insert_chunk" )    { sawInsertChunk    = true; Check( annotated, "insert_chunk tool description is ANNOTATED under Read" ); }
			if( name == "insert_chunks" )   { sawInsertChunks   = true; Check( annotated, "insert_chunks tool description is ANNOTATED under Read" ); }
			if( name == "remove_chunk" )    { sawRemoveChunk    = true; Check( annotated, "remove_chunk tool description is ANNOTATED under Read" ); }
			if( name == "render" )          { sawRender         = true; Check( !annotated, "render tool description is NOT annotated under Read (it is allowed)" ); }
			if( name == "list_proposals" ) {
				sawListProposals = true;
				Check( !annotated && !ownerOnlyAnnotated, "list_proposals tool description is NOT annotated under Read (read-safe under every posture)" );
			}
			if( name == "resolve_proposal" ) {
				sawResolveProposal = true;
				Check( !annotated, "resolve_proposal is NOT annotated with the generic read-refusal note (it has its own)" );
				Check( ownerOnlyAnnotated, "resolve_proposal tool description IS annotated with the OWNER-ONLY note under Read" );
			}
		}
		Check( sawProposePatch && sawProposePatches && sawInsertChunk && sawInsertChunks && sawRemoveChunk && sawRender && sawListProposals && sawResolveProposal,
		       "all mutating tools + render + list_proposals + resolve_proposal were found in tools/list under Read" );
		Check( annotatedCount == 5, "EXACTLY 5 tool descriptions carry the generic read-refusal note under Read (the mutating set incl. propose_patches/insert_chunks, no more no less; resolve_proposal has its own distinct note)" );
	}

	// tools/list under Commit: no annotation anywhere (including
	// resolve_proposal's OWNER-ONLY note, since Commit is the ONE posture
	// where resolve_proposal actually reaches the session).
	{
		const std::string resp = mcpCommit.HandleLine( Req( 3, "tools/list", JsonValue::MakeObject() ) );
		JsonValue env = ParseResponse( resp, 3 );
		const JsonValue& tools = env.get( "result" ).get( "tools" );
		Check( tools.isArray() && tools.size() == 19, "tools/list under Commit lists all 19 tools" );
		int annotatedCount = 0;
		for( std::size_t i = 0; i < tools.size(); ++i ) {
			const std::string desc = tools.at( i ).get( "description" ).asString();
			if( desc.find( "[REFUSED under --agent-autonomy=read" ) != std::string::npos ) ++annotatedCount;
			if( desc.find( "[OWNER-ONLY:" ) != std::string::npos ) ++annotatedCount;
		}
		Check( annotatedCount == 0, "RED-PROVE: NO tool descriptions are annotated under Commit" );
	}

	// tools/call propose_patch under Commit -> applies cleanly (sanity:
	// the MCP layer's own refusal plumbing doesn't leak into Commit).
	{
		JsonValue args = JsonValue::MakeObject();
		args.set( "target", JsonValue::MakeString( "sph" ) );
		args.set( "param",  JsonValue::MakeString( "radius" ) );
		args.set( "value",  JsonValue::MakeString( "0.3" ) );
		const std::string resp = mcpCommit.HandleLine( ReqToolCall( 4, "propose_patch", args ) );
		JsonValue env = ParseResponse( resp, 4 );
		Check( !env.has( "error" ), "tools/call(propose_patch) under Commit is a success envelope" );
		Check( env.get( "result" ).get( "isError" ).asBool( true ) == false,
		       "tools/call(propose_patch) under Commit carries isError:false (applies cleanly)" );
	}

	// Secure-MCP slice 5b fix round (P2-1) RED-PROVE: tools/list under
	// Propose annotates the SAME 3 mutating tools with the DISTINCT
	// staging note (kAutonomyProposeNote, "...STAGES a proposal..."), never
	// the Read-only refusal note, and the staging note is ABSENT under
	// Commit -- this is the coverage hole the P2-1 review finding named:
	// under Propose, propose_patch/insert_chunk/remove_chunk's descriptions
	// used to be byte-identical to Commit's, giving an external MCP agent
	// no textual clue that its edit only STAGES rather than commits.
	{
		const std::string resp = mcpPropose.HandleLine( Req( 5, "tools/list", JsonValue::MakeObject() ) );
		JsonValue env = ParseResponse( resp, 5 );
		const JsonValue& tools = env.get( "result" ).get( "tools" );
		Check( tools.isArray() && tools.size() == 19, "tools/list under Propose lists all 19 tools" );

		bool sawProposePatch = false, sawProposePatches = false, sawInsertChunk = false, sawInsertChunks = false, sawRemoveChunk = false;
		int proposeNotedCount = 0, readNotedCount = 0;
		for( std::size_t i = 0; i < tools.size(); ++i ) {
			const JsonValue& t = tools.at( i );
			const std::string name = t.get( "name" ).asString();
			const std::string desc = t.get( "description" ).asString();
			const bool proposeNoted = desc.find( "[NOTE under --agent-autonomy=propose" ) != std::string::npos;
			const bool readNoted    = desc.find( "[REFUSED under --agent-autonomy=read" ) != std::string::npos;
			if( proposeNoted ) ++proposeNotedCount;
			if( readNoted )    ++readNotedCount;
			if( name == "propose_patch" ) {
				sawProposePatch = true;
				Check( proposeNoted, "propose_patch tool description carries the propose-staging note under Propose" );
				Check( !readNoted, "propose_patch tool description does NOT carry the read-refusal note under Propose" );
				Check( desc.find( "\"staged\"" ) != std::string::npos,
				       "propose_patch's own description documents \"staged\" as a possible status value" );
			}
			if( name == "propose_patches" ) {
				sawProposePatches = true;
				Check( proposeNoted, "propose_patches tool description carries the propose-staging note under Propose" );
			}
			if( name == "insert_chunk" ) {
				sawInsertChunk = true;
				Check( proposeNoted, "insert_chunk tool description carries the propose-staging note under Propose" );
			}
			if( name == "insert_chunks" ) {
				sawInsertChunks = true;
				Check( proposeNoted, "insert_chunks (batch) tool description carries the propose-staging note under Propose" );
			}
			if( name == "remove_chunk" ) {
				sawRemoveChunk = true;
				Check( proposeNoted, "remove_chunk tool description carries the propose-staging note under Propose" );
			}
		}
		Check( sawProposePatch && sawProposePatches && sawInsertChunk && sawInsertChunks && sawRemoveChunk,
		       "all 5 mutating tools were found in tools/list under Propose" );
		Check( proposeNotedCount == 5,
		       "RED-PROVE: EXACTLY 5 tool descriptions carry the propose-staging note under Propose" );
		Check( readNotedCount == 0,
		       "RED-PROVE: NO tool description carries the Read-only refusal note under Propose" );
	}

	// RED-PROVE the converse: under Commit, none of the 3 mutating tools
	// carry the propose-staging note either (already covered generically by
	// the "annotatedCount == 0 under Commit" check above for the read note
	// + owner-only note; this asserts the THIRD note is equally absent).
	{
		const std::string resp = mcpCommit.HandleLine( Req( 6, "tools/list", JsonValue::MakeObject() ) );
		JsonValue env = ParseResponse( resp, 6 );
		const JsonValue& tools = env.get( "result" ).get( "tools" );
		int proposeNotedCount = 0;
		for( std::size_t i = 0; i < tools.size(); ++i ) {
			const std::string desc = tools.at( i ).get( "description" ).asString();
			if( desc.find( "[NOTE under --agent-autonomy=propose" ) != std::string::npos ) ++proposeNotedCount;
		}
		Check( proposeNotedCount == 0, "RED-PROVE: NO tool descriptions carry the propose-staging note under Commit" );
	}

	std::remove( scenePathRead.c_str() );
	std::remove( scenePathCommit.c_str() );
	std::remove( scenePathPropose.c_str() );
}

//----------------------------------------------------------------------
// (d) Launch-flag parsing: real-subprocess smoke.
//----------------------------------------------------------------------
#ifndef _WIN32

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

static void SplitLines( const std::string& blob, std::vector<std::string>& out )
{
	std::string cur;
	for( char c : blob ) {
		if( c == '\n' ) { out.push_back( cur ); cur.clear(); }
		else cur += c;
	}
	if( !cur.empty() ) out.push_back( cur );
}

// Spawn `./bin/rise` with `extraArgs` appended after the scene path (or, if
// scenePath is empty, with no positional arg at all -- used for the
// malformed-flag case, which should fail before ever touching a scene).
// Writes `stdinData` to the child's stdin then closes it (EOF). Captures
// stdout/stderr separately. Returns the child's exit code, or -1 on a
// harness-level failure (fork/pipe), in which case Check() has already
// recorded the failure.
static int RunChild( const std::vector<std::string>& args,
                      const std::string& stdinData,
                      std::string& outStdout,
                      std::string& outStderr )
{
	const char* kBin = "./bin/rise";
	int inPipe[2] = { -1, -1 }, outPipe[2] = { -1, -1 }, errPipe[2] = { -1, -1 };
	if( pipe( inPipe ) != 0 || pipe( outPipe ) != 0 || pipe( errPipe ) != 0 ) {
		Check( false, "created stdin/stdout/stderr pipes for subprocess" );
		return -1;
	}
	const pid_t pid = fork();
	if( pid < 0 ) {
		Check( false, "fork() succeeded" );
		return -1;
	}
	if( pid == 0 ) {
		// ---- child ----
		dup2( inPipe[0], STDIN_FILENO );
		dup2( outPipe[1], STDOUT_FILENO );
		dup2( errPipe[1], STDERR_FILENO );
		close( inPipe[0] );  close( inPipe[1] );
		close( outPipe[0] ); close( outPipe[1] );
		close( errPipe[0] ); close( errPipe[1] );
		unsetenv( "RISE_MEDIA_PATH" );

		std::vector<const char*> argv;
		argv.push_back( kBin );
		for( const std::string& a : args ) argv.push_back( a.c_str() );
		argv.push_back( nullptr );
		execv( kBin, const_cast<char* const*>( argv.data() ) );
		_exit( 127 );
	}

	// ---- parent ----
	close( inPipe[0] );
	close( outPipe[1] );
	close( errPipe[1] );

	if( !stdinData.empty() ) {
		const ssize_t wrote = write( inPipe[1], stdinData.data(), stdinData.size() );
		(void)wrote;
	}
	close( inPipe[1] );   // EOF

	// Drain stdout and stderr to avoid a pipe-buffer deadlock; non-blocking
	// alternation is overkill here (payload is tiny) so just read stdout to
	// EOF (the child exits promptly once stdin EOFs) then stderr.
	{
		char buf[4096]; ssize_t n;
		while( ( n = read( outPipe[0], buf, sizeof( buf ) ) ) > 0 ) outStdout.append( buf, (size_t)n );
	}
	close( outPipe[0] );
	{
		char buf[4096]; ssize_t n;
		while( ( n = read( errPipe[0], buf, sizeof( buf ) ) ) > 0 ) outStderr.append( buf, (size_t)n );
	}
	close( errPipe[0] );

	int status = 0;
	waitpid( pid, &status, 0 );
	return WIFEXITED( status ) ? WEXITSTATUS( status ) : -1;
}

static void TestLaunchFlagParsing()
{
	std::printf( "--- (d) launch-flag parsing (real subprocess) ---\n" );

	const char* kBin = "./bin/rise";
	{
		std::ifstream probe( kBin, std::ios::binary );
		Check( probe.good(), std::string( "rise binary exists at " ) + kBin );
		if( !probe.good() ) {
			std::printf( "  (build the CLI with `make -C build/make/rise all` before running this test)\n" );
			return;
		}
	}

	const std::string scenePath = WriteTemp( "rise_agent_autonomy_smoke.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the launch-flag smoke scene to a temp file" );

	// --- `--agent-stdio --agent-autonomy=read`: a real propose_patch over
	// the wire is refused with the exact code, and read_document/render
	// still work. ---
	{
		const std::string requests =
			"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"read_document\",\"params\":{}}\n"
			"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"propose_patch\",\"params\":{\"target\":\"sph\",\"param\":\"radius\",\"value\":\"0.3\"}}\n"
			"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"render\",\"params\":{}}\n";
		std::vector<std::string> args;
		args.push_back( "--agent-stdio" );
		args.push_back( "--agent-autonomy=read" );
		args.push_back( scenePath );
		std::string outStdout, outStderr;
		const int rc = RunChild( args, requests, outStdout, outStderr );
		Check( rc == 0, "`--agent-stdio --agent-autonomy=read` child exits 0 on stdin EOF" );

		std::vector<std::string> lines;
		SplitLines( outStdout, lines );
		Check( lines.size() >= 3, "child emitted 3 stdout response lines" );
		if( lines.size() >= 2 ) {
			JsonValue env; std::string perr;
			const bool ok = JsonParse( lines[1], env, perr );
			Check( ok, "propose_patch response line parses as JSON over the real wire" );
			Check( ok && env.has( "error" ),
			       "REAL WIRE: propose_patch under --agent-autonomy=read is refused (JSON-RPC error)" );
			if( ok && env.has( "error" ) ) {
				Check( env.get( "error" ).get( "code" ).asNumber( 0 ) == -32011.0,
				       "REAL WIRE: refusal carries code -32011" );
				Check( env.get( "error" ).get( "data" ).get( "verb" ).asString() == "propose_patch",
				       "REAL WIRE: refusal data.verb == \"propose_patch\"" );
			}
		}
		if( lines.size() >= 3 ) {
			JsonValue env; std::string perr;
			const bool ok = JsonParse( lines[2], env, perr );
			Check( ok && !env.has( "error" ) && env.get( "result" ).get( "ok" ).asBool( false ),
			       "REAL WIRE: render still succeeds under --agent-autonomy=read" );
		}
	}

	// --- RED-PROVE: `--agent-autonomy=banana` (malformed) exits nonzero
	// with a loud stderr usage error -- never a silent default to either
	// posture. ---
	{
		std::vector<std::string> args;
		args.push_back( "--agent-stdio" );
		args.push_back( "--agent-autonomy=banana" );
		args.push_back( scenePath );
		std::string outStdout, outStderr;
		const int rc = RunChild( args, std::string(), outStdout, outStderr );
		Check( rc != 0, "RED-PROVE: `--agent-autonomy=banana` exits NONZERO (not silently accepted)" );
		Check( outStderr.find( "--agent-autonomy" ) != std::string::npos,
		       "RED-PROVE: stderr names the offending flag" );
		Check( outStderr.find( "banana" ) != std::string::npos,
		       "RED-PROVE: stderr echoes the bad value 'banana'" );
		Check( outStdout.empty() || outStdout.find( "jsonrpc" ) == std::string::npos,
		       "malformed-flag failure does NOT emit a JSON-RPC frame on stdout (fails before the loop starts)" );
	}

	// --- (f) RED-PROVE: a BARE `--agent-autonomy` (no `=value` at all)
	// exits nonzero with a loud stderr usage error -- the pre-hardening
	// behaviour silently dropped this token entirely (fell through every
	// flag branch unmatched, consumed nothing) and proceeded with the
	// DEFAULT `read` posture as if the flag had never been given. ---
	{
		std::vector<std::string> args;
		args.push_back( "--agent-stdio" );
		args.push_back( "--agent-autonomy" );
		args.push_back( scenePath );
		std::string outStdout, outStderr;
		const int rc = RunChild( args, std::string(), outStdout, outStderr );
		Check( rc != 0, "RED-PROVE: bare `--agent-autonomy` (no '=value') exits NONZERO (not silently dropped)" );
		Check( outStderr.find( "--agent-autonomy" ) != std::string::npos,
		       "RED-PROVE: bare-flag stderr names the offending flag" );
		Check( outStdout.empty() || outStdout.find( "jsonrpc" ) == std::string::npos,
		       "bare-flag failure does NOT emit a JSON-RPC frame on stdout (fails before the loop starts)" );
	}

	// --- (f) RED-PROVE: the SPACE form `--agent-autonomy read` (as
	// opposed to the documented `--agent-autonomy=read`) exits nonzero
	// with a loud stderr usage error.  Pre-hardening this was WORSE than a
	// no-op: the bare `--agent-autonomy` token was silently dropped AND
	// the following `read` token was silently consumed as the positional
	// scene-file argument (displacing the real scene path), so the
	// process would have gone on to fail with a confusing "could not load
	// scene 'read'" error instead of a clear flag-usage message. ---
	{
		std::vector<std::string> args;
		args.push_back( "--agent-stdio" );
		args.push_back( "--agent-autonomy" );
		args.push_back( "read" );
		args.push_back( scenePath );
		std::string outStdout, outStderr;
		const int rc = RunChild( args, std::string(), outStdout, outStderr );
		Check( rc != 0, "RED-PROVE: space form `--agent-autonomy read` exits NONZERO (not silently accepted)" );
		Check( outStderr.find( "--agent-autonomy" ) != std::string::npos,
		       "RED-PROVE: space-form stderr names the offending flag" );
		Check( outStdout.empty() || outStdout.find( "jsonrpc" ) == std::string::npos,
		       "space-form failure does NOT emit a JSON-RPC frame on stdout (fails before the loop starts)" );
	}

	std::remove( scenePath.c_str() );
}

#else  // _WIN32

static void TestLaunchFlagParsing()
{
	std::printf( "--- (d) launch-flag parsing: POSIX-only (fork/pipe/execv); trivially passing on Windows ---\n" );
}

#endif

int main()
{
	std::printf( "=== AgentAutonomyPolicyTest (Secure-MCP slice 2: headless autonomy policy) ===\n" );

	TestReadPosture();
	TestDenyByDefaultForUnclassifiedVerb();
	TestProposePosture();
	TestCommitPostureMatchesDefault();
	TestMcpLayer();
	TestLaunchFlagParsing();

	std::printf( "=== AgentAutonomyPolicyTest: %d passed, %d failed ===\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
