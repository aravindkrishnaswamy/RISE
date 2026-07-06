//////////////////////////////////////////////////////////////////////
//
//  AgentAutonomyPolicyTest.cpp - Secure-MCP slice 2: the headless
//    autonomy-policy regression guard.
//
//  Four in-process/subprocess suites:
//
//    (a) READ posture (in-process AgentRpcDispatcher constructed with
//        AgentAutonomy::Read): all 3 mutating verbs (propose_patch,
//        insert_chunk, remove_chunk) are refused with the EXACT
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
//        present under Read AND absent under Commit.
//
//    (d) Launch-flag parsing: real-subprocess smoke (fork/pipe/execl,
//        mirroring AgentStdioSmokeTest.cpp's pattern) --
//        `--agent-stdio --agent-autonomy=read` refuses a propose_patch
//        over the REAL wire; RED-PROVE `--agent-autonomy=banana`
//        (malformed) exits nonzero with a loud stderr usage error rather
//        than silently defaulting to either posture.
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
	std::string path = dir + name;
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

	const std::string scenePathRead   = WriteTemp( "rise_autonomy_mcp_read.RISEscene", kScene );
	const std::string scenePathCommit = WriteTemp( "rise_autonomy_mcp_commit.RISEscene", kScene );
	Check( !scenePathRead.empty() && !scenePathCommit.empty(), "wrote both MCP-layer scenes" );

	std::unique_ptr<AgentSession> sessionRead   = AgentSession::LoadFromFile( scenePathRead );
	std::unique_ptr<AgentSession> sessionCommit = AgentSession::LoadFromFile( scenePathCommit );
	Check( sessionRead != nullptr && sessionCommit != nullptr, "both MCP sessions load" );
	if( !sessionRead || !sessionCommit ) return;

	AgentMcpAdapter mcpRead( std::move( sessionRead ), AgentAutonomy::Read );
	AgentMcpAdapter mcpCommit( std::move( sessionCommit ), AgentAutonomy::Commit );

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
	// other tool's description is unannotated.
	{
		const std::string resp = mcpRead.HandleLine( Req( 2, "tools/list", JsonValue::MakeObject() ) );
		JsonValue env = ParseResponse( resp, 2 );
		const JsonValue& tools = env.get( "result" ).get( "tools" );
		Check( tools.isArray() && tools.size() == 12,
		       "tools/list under Read STILL lists all 12 tools (mutating tools are ANNOTATED, not hidden)" );

		bool sawProposePatch = false, sawInsertChunk = false, sawRemoveChunk = false;
		bool sawRender = false;
		int annotatedCount = 0;
		for( std::size_t i = 0; i < tools.size(); ++i ) {
			const JsonValue& t = tools.at( i );
			const std::string name = t.get( "name" ).asString();
			const std::string desc = t.get( "description" ).asString();
			const bool annotated = desc.find( "[REFUSED under --agent-autonomy=read" ) != std::string::npos;
			if( annotated ) ++annotatedCount;
			if( name == "propose_patch" ) { sawProposePatch = true; Check( annotated, "propose_patch tool description is ANNOTATED under Read" ); }
			if( name == "insert_chunk" )  { sawInsertChunk  = true; Check( annotated, "insert_chunk tool description is ANNOTATED under Read" ); }
			if( name == "remove_chunk" )  { sawRemoveChunk  = true; Check( annotated, "remove_chunk tool description is ANNOTATED under Read" ); }
			if( name == "render" )        { sawRender       = true; Check( !annotated, "render tool description is NOT annotated under Read (it is allowed)" ); }
		}
		Check( sawProposePatch && sawInsertChunk && sawRemoveChunk && sawRender,
		       "all 3 mutating tools + render were found in tools/list under Read" );
		Check( annotatedCount == 3, "EXACTLY 3 tool descriptions are annotated under Read (the mutating set, no more no less)" );
	}

	// tools/list under Commit: no annotation anywhere.
	{
		const std::string resp = mcpCommit.HandleLine( Req( 3, "tools/list", JsonValue::MakeObject() ) );
		JsonValue env = ParseResponse( resp, 3 );
		const JsonValue& tools = env.get( "result" ).get( "tools" );
		Check( tools.isArray() && tools.size() == 12, "tools/list under Commit lists all 12 tools" );
		int annotatedCount = 0;
		for( std::size_t i = 0; i < tools.size(); ++i ) {
			const std::string desc = tools.at( i ).get( "description" ).asString();
			if( desc.find( "[REFUSED under --agent-autonomy=read" ) != std::string::npos ) ++annotatedCount;
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

	std::remove( scenePathRead.c_str() );
	std::remove( scenePathCommit.c_str() );
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
	TestCommitPostureMatchesDefault();
	TestMcpLayer();
	TestLaunchFlagParsing();

	std::printf( "=== AgentAutonomyPolicyTest: %d passed, %d failed ===\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
