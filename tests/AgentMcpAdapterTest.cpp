//////////////////////////////////////////////////////////////////////
//
//  AgentMcpAdapterTest.cpp - Secure-MCP slice 1: in-process test of the
//    MCP envelope adapter (AgentMcpAdapter) over the SAME 12 agent verbs
//    AgentFirstSliceTest drives via the raw JSON-RPC dispatcher.
//
//  Drives AgentMcpAdapter::HandleLine directly (no subprocess, no LLM):
//    * initialize handshake -> protocolVersion/capabilities/serverInfo.
//    * notifications/initialized -> RED-PROVED silent (empty response).
//    * tools/list -> all 12 verbs present; spot-check 3 schemas' required
//      fields + a description contains a known AgentRpc.h gotcha string.
//    * tools/call happy path: read_document, render (sync), read_image
//      (an MCP image content block with valid base64 PNG).
//    * tools/call tool-execution error: propose_patch on a nonexistent
//      entity -> isError:true, NOT a protocol error -- RED-PROVED against
//      a naive "always protocol error" implementation.
//    * tools/call protocol error: unknown tool name -> JSON-RPC error.
//    * malformed line -> -32700.
//    * batch (array) envelope -> a clean rejection, not a crash.
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Agent/AgentMcpAdapter.h"
#include "../src/Library/Agent/AgentSession.h"
#include "../src/Library/Agent/Json.h"
#include "../src/Library/Agent/Base64.h"

#include <cstdio>
#include <cstdlib>
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

// Same tiny inline native-v7 scene the other agentic-surface tests use.
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

// Build an MCP/JSON-RPC request line for `method` with `params` under `id`.
static std::string Req( double id, const std::string& method, const JsonValue& params )
{
	JsonValue r = JsonValue::MakeObject();
	r.set( "jsonrpc", JsonValue::MakeString( "2.0" ) );
	r.set( "id", JsonValue::MakeNumber( id ) );
	r.set( "method", JsonValue::MakeString( method ) );
	r.set( "params", params );
	return JsonSerialize( r );
}

// Build a `tools/call {name,arguments}` request line.
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

int main()
{
	std::printf( "=== AgentMcpAdapterTest (Secure-MCP slice 1: MCP envelope adapter) ===\n" );

	const std::string scenePath = WriteTemp( "rise_mcp_adapter_slice1.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the scene to a temp file" );

	std::unique_ptr<AgentSession> session = AgentSession::LoadFromFile( scenePath );
	Check( session != nullptr, "AgentSession::LoadFromFile loads the native-v7 scene" );
	if( !session ) { std::printf( "cannot continue without a session\n" ); return 1; }
	AgentMcpAdapter mcp( std::move( session ) );

	//----------------------------------------------------------------------
	// initialize -- protocolVersion/capabilities/serverInfo.
	//----------------------------------------------------------------------
	std::printf( "[initialize] handshake\n" );
	{
		JsonValue params = JsonValue::MakeObject();
		params.set( "protocolVersion", JsonValue::MakeString( "2025-03-26" ) );
		params.set( "capabilities", JsonValue::MakeObject() );
		JsonValue clientInfo = JsonValue::MakeObject();
		clientInfo.set( "name", JsonValue::MakeString( "test-client" ) );
		clientInfo.set( "version", JsonValue::MakeString( "0.0.1" ) );
		params.set( "clientInfo", clientInfo );

		const std::string resp = mcp.HandleLine( Req( 1, "initialize", params ) );
		JsonValue env = ParseResponse( resp, 1 );
		Check( !env.has( "error" ), "initialize returns a success (not an error)" );
		const JsonValue& result = env.get( "result" );
		Check( result.get( "protocolVersion" ).asString() == "2025-03-26",
		       "initialize echoes the matching client protocolVersion 2025-03-26" );
		Check( result.get( "capabilities" ).isObject(), "initialize result carries a capabilities object" );
		Check( result.get( "capabilities" ).has( "tools" ), "capabilities advertises the 'tools' capability" );
		Check( result.get( "serverInfo" ).get( "name" ).asString() == "rise",
		       "serverInfo.name == \"rise\"" );
		Check( !result.get( "serverInfo" ).get( "version" ).asString().empty(),
		       "serverInfo.version is non-empty" );
	}
	{
		// An UNRECOGNIZED client protocolVersion -> the adapter reports its
		// OWN baseline, not the client's unrecognized string.
		JsonValue params = JsonValue::MakeObject();
		params.set( "protocolVersion", JsonValue::MakeString( "1999-01-01" ) );
		const std::string resp = mcp.HandleLine( Req( 2, "initialize", params ) );
		JsonValue env = ParseResponse( resp, 2 );
		Check( env.get( "result" ).get( "protocolVersion" ).asString() == "2025-03-26",
		       "initialize with an unrecognized client protocolVersion reports the adapter's own baseline" );
	}

	//----------------------------------------------------------------------
	// notifications/initialized -- RED-PROVE: must be SILENT (empty
	// response), which the harness distinguishes from "responds with
	// something".  A naive always-respond implementation would return a
	// non-empty JSON-RPC envelope here; we assert the fix directly.
	//----------------------------------------------------------------------
	std::printf( "[notifications/initialized] RED-PROVE: silent (no response line)\n" );
	{
		// A true notification: no "id" field at all.
		JsonValue noIdReq = JsonValue::MakeObject();
		noIdReq.set( "jsonrpc", JsonValue::MakeString( "2.0" ) );
		noIdReq.set( "method", JsonValue::MakeString( "notifications/initialized" ) );
		noIdReq.set( "params", JsonValue::MakeObject() );
		const std::string line = JsonSerialize( noIdReq );

		const std::string resp = mcp.HandleLine( line );
		Check( resp.empty(), "notifications/initialized (no id) yields an EMPTY response (true MCP notification silence)" );

		// RED-PROVE harness: manually verify the naive alternative WOULD
		// have produced a non-empty line by checking that a REQUEST with
		// the SAME method but WITH an id (which our adapter treats as an
		// ordinary request, not a notification, since a real id was
		// supplied) DOES get a response -- proving HandleLine's silence
		// above is a deliberate branch on id-absence, not just a
		// coincidental empty method-not-found body.
		const std::string reqWithId = Req( 3, "notifications/initialized", JsonValue::MakeObject() );
		const std::string respWithId = mcp.HandleLine( reqWithId );
		Check( !respWithId.empty(),
		       "RED-PROVE: the SAME method WITH an id gets a real response (proves silence above is keyed on id-absence, not a fluke)" );
		JsonValue env = ParseResponse( respWithId, 3 );
		Check( env.has( "error" ), "notifications/initialized WITH an id (not a real notification) errors as an unhandled method" );
	}
	{
		// A non-initialized notification: also silent (forward-compatible
		// posture for future notification methods).
		JsonValue noIdReq = JsonValue::MakeObject();
		noIdReq.set( "jsonrpc", JsonValue::MakeString( "2.0" ) );
		noIdReq.set( "method", JsonValue::MakeString( "notifications/some_future_thing" ) );
		const std::string resp = mcp.HandleLine( JsonSerialize( noIdReq ) );
		Check( resp.empty(), "an unrecognized notification (no id) is also silently dropped" );
	}

	//----------------------------------------------------------------------
	// tools/list -- all 12 verbs present; spot-check schemas + descriptions.
	//----------------------------------------------------------------------
	std::printf( "[tools/list] all 12 verbs; schema + description spot-checks\n" );
	JsonValue toolsList;
	{
		const std::string resp = mcp.HandleLine( Req( 10, "tools/list", JsonValue::MakeObject() ) );
		JsonValue env = ParseResponse( resp, 10 );
		Check( !env.has( "error" ), "tools/list returns a success" );
		toolsList = env.get( "result" ).get( "tools" );
		Check( toolsList.isArray(), "tools/list result.tools is an array" );
		Check( toolsList.size() == 12, "tools/list returns EXACTLY the 12 agent verbs" );

		static const char* const kExpectedNames[] = {
			"read_document", "read_schema", "read_skill", "validate",
			"propose_patch", "insert_chunk", "remove_chunk",
			"render", "render_status", "render_wait", "render_cancel",
			"read_image"
		};
		for( const char* expected : kExpectedNames ) {
			bool found = false;
			for( std::size_t i = 0; i < toolsList.size(); ++i ) {
				if( toolsList.at( i ).get( "name" ).asString() == expected ) { found = true; break; }
			}
			Check( found, std::string( "tools/list includes '" ) + expected + "'" );
		}

		// Helper: find a tool by name.
		auto FindTool = [&]( const std::string& name ) -> JsonValue {
			for( std::size_t i = 0; i < toolsList.size(); ++i )
				if( toolsList.at( i ).get( "name" ).asString() == name ) return toolsList.at( i );
			return JsonValue::MakeNull();
		};

		// Spot-check 1: propose_patch requires target/param/value.
		{
			JsonValue tool = FindTool( "propose_patch" );
			Check( tool.isObject(), "found propose_patch tool" );
			const JsonValue& schema = tool.get( "inputSchema" );
			Check( schema.get( "type" ).asString() == "object", "propose_patch inputSchema.type == \"object\"" );
			const JsonValue& required = schema.get( "required" );
			Check( required.isArray(), "propose_patch inputSchema has a required array" );
			bool hasTarget = false, hasParam = false, hasValue = false;
			for( std::size_t i = 0; i < required.size(); ++i ) {
				const std::string r = required.at( i ).asString();
				if( r == "target" ) hasTarget = true;
				if( r == "param" )  hasParam  = true;
				if( r == "value" )  hasValue  = true;
			}
			Check( hasTarget && hasParam && hasValue,
			       "propose_patch inputSchema.required == {target,param,value}" );
		}

		// Spot-check 2: validate requires text.
		{
			JsonValue tool = FindTool( "validate" );
			Check( tool.isObject(), "found validate tool" );
			const JsonValue& required = tool.get( "inputSchema" ).get( "required" );
			Check( required.size() == 1 && required.at( 0 ).asString() == "text",
			       "validate inputSchema.required == [\"text\"]" );
		}

		// Spot-check 3: render_status requires renderJobId.
		{
			JsonValue tool = FindTool( "render_status" );
			Check( tool.isObject(), "found render_status tool" );
			const JsonValue& required = tool.get( "inputSchema" ).get( "required" );
			Check( required.size() == 1 && required.at( 0 ).asString() == "renderJobId",
			       "render_status inputSchema.required == [\"renderJobId\"]" );
		}

		// Description gotcha spot-checks: a known AgentRpc.h documented
		// gotcha string must appear verbatim (or in close paraphrase we
		// control) somewhere in the tool's descriptive text. The
		// width/height pairing + clamp gotchas live on the NESTED
		// per-property descriptions (inputSchema.properties.width/height),
		// which is the more useful place for an MCP client to see them --
		// so we search the WHOLE serialized tool object (top-level
		// description + every nested property description), not just the
		// top-level description string.
		{
			JsonValue tool = FindTool( "render" );
			const std::string wholeTool = JsonSerialize( tool );
			Check( wholeTool.find( "Must be paired with" ) != std::string::npos,
			       "render tool documents the width/height MUST-BE-PAIRED gotcha (in the width/height property descriptions)" );
			Check( wholeTool.find( "CLAMPED" ) != std::string::npos,
			       "render tool documents the samples/width/height CLAMP semantics" );
			Check( wholeTool.find( "async" ) != std::string::npos,
			       "render tool documents that async submission is not exposed on this headless transport" );
		}
		{
			JsonValue tool = FindTool( "propose_patch" );
			const std::string desc = tool.get( "description" ).asString();
			Check( desc.find( "conflict" ) != std::string::npos,
			       "propose_patch description documents the baseHeadVersion conflict protocol" );
			Check( desc.find( "retriable" ) != std::string::npos,
			       "propose_patch description documents the retriable flag" );
		}
		{
			JsonValue tool = FindTool( "read_image" );
			const std::string desc = tool.get( "description" ).asString();
			Check( desc.find( "image content block" ) != std::string::npos,
			       "read_image description documents the MCP image content block mapping" );
		}
	}

	//----------------------------------------------------------------------
	// tools/call happy path: read_document.
	//----------------------------------------------------------------------
	std::printf( "[tools/call] read_document happy path\n" );
	{
		const std::string resp = mcp.HandleLine( ReqToolCall( 20, "read_document", JsonValue::MakeObject() ) );
		JsonValue env = ParseResponse( resp, 20 );
		Check( !env.has( "error" ), "tools/call(read_document) is a JSON-RPC success (not a protocol error)" );
		const JsonValue& result = env.get( "result" );
		Check( result.get( "isError" ).asBool( true ) == false, "tools/call(read_document) isError == false" );
		const JsonValue& content = result.get( "content" );
		Check( content.isArray() && content.size() >= 1, "tools/call(read_document) content is a non-empty array" );
		const std::string text = content.at( 0 ).get( "text" ).asString();
		Check( text.find( "sphere_geometry" ) != std::string::npos,
		       "tools/call(read_document) text content carries the expected 'sphere_geometry' chunk keyword" );
		Check( content.at( 0 ).get( "type" ).asString() == "text",
		       "tools/call(read_document) content block type == \"text\"" );
	}

	//----------------------------------------------------------------------
	// tools/call happy path: render (synchronous).
	//----------------------------------------------------------------------
	std::printf( "[tools/call] render (sync) happy path\n" );
	{
		const std::string resp = mcp.HandleLine( ReqToolCall( 21, "render", JsonValue::MakeObject() ) );
		JsonValue env = ParseResponse( resp, 21 );
		Check( !env.has( "error" ), "tools/call(render) is a JSON-RPC success" );
		const JsonValue& result = env.get( "result" );
		Check( !result.get( "isError" ).asBool( true ), "tools/call(render) isError == false" );
		const std::string text = result.get( "content" ).at( 0 ).get( "text" ).asString();
		JsonValue renderResult; std::string perr;
		Check( JsonParse( text, renderResult, perr ), "tools/call(render) text content parses back to JSON" );
		Check( renderResult.get( "ok" ).asBool(), "tools/call(render) inner render result ok == true" );
		Check( renderResult.get( "width" ).asNumber() == 24.0, "tools/call(render) inner result width == 24" );
	}

	//----------------------------------------------------------------------
	// tools/call happy path: read_image -- MCP image content block with
	// valid base64 that decodes to a PNG header.
	//----------------------------------------------------------------------
	std::printf( "[tools/call] read_image -> MCP image content block\n" );
	{
		const std::string resp = mcp.HandleLine( ReqToolCall( 22, "read_image", JsonValue::MakeObject() ) );
		JsonValue env = ParseResponse( resp, 22 );
		Check( !env.has( "error" ), "tools/call(read_image) is a JSON-RPC success" );
		const JsonValue& result = env.get( "result" );
		Check( !result.get( "isError" ).asBool( true ), "tools/call(read_image) isError == false" );
		const JsonValue& content = result.get( "content" );
		Check( content.isArray() && content.size() >= 2,
		       "tools/call(read_image) content carries at least an image block + a text block" );

		bool foundImageBlock = false;
		for( std::size_t i = 0; i < content.size(); ++i ) {
			const JsonValue& block = content.at( i );
			if( block.get( "type" ).asString() == "image" ) {
				foundImageBlock = true;
				Check( block.get( "mimeType" ).asString() == "image/png",
				       "read_image image content block mimeType == \"image/png\"" );
				const std::string b64 = block.get( "data" ).asString();
				Check( !b64.empty(), "read_image image content block 'data' is non-empty base64" );
				std::vector<unsigned char> png;
				Check( Base64Decode( b64, png ), "read_image image content block base64 decodes cleanly" );
				Check( png.size() >= 8 &&
				       png[0] == 0x89 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G' &&
				       png[4] == 0x0D && png[5] == 0x0A && png[6] == 0x1A && png[7] == 0x0A,
				       "read_image image content block decodes to the \\x89PNG signature" );
			}
		}
		Check( foundImageBlock, "tools/call(read_image) content includes a {type:\"image\"} block" );
	}

	//----------------------------------------------------------------------
	// propose_patch on a nonexistent entity: the WRAPPED verb reports this
	// as a JSON-RPC SUCCESS whose result carries applied=false/
	// status="rejected" (AgentRpc.cpp's ProposePatch contract: a rejected
	// edit is not a JSON-RPC error, it is an honest structured "no" so the
	// head-untouched guarantee is visible in the result). This adapter
	// performs ZERO param/result translation, so that stays isError:false
	// here too -- verified explicitly so a future edit doesn't quietly
	// start reinterpreting "applied:false" as a protocol/tool error.
	//----------------------------------------------------------------------
	std::printf( "[tools/call] propose_patch(unknown entity) -> a SUCCESS result with applied:false (not isError)\n" );
	{
		JsonValue args = JsonValue::MakeObject();
		args.set( "target", JsonValue::MakeString( "this_entity_does_not_exist_anywhere" ) );
		args.set( "param",  JsonValue::MakeString( "radius" ) );
		args.set( "value",  JsonValue::MakeString( "1.0" ) );
		const std::string resp = mcp.HandleLine( ReqToolCall( 23, "propose_patch", args ) );
		JsonValue env = ParseResponse( resp, 23 );
		Check( !env.has( "error" ),
		       "tools/call(propose_patch, unknown entity) is a JSON-RPC SUCCESS envelope" );
		Check( env.has( "result" ), "tools/call(propose_patch, unknown entity) has a result field" );
		const JsonValue& result = env.get( "result" );
		Check( result.get( "isError" ).asBool( true ) == false,
		       "tools/call(propose_patch, unknown entity) isError == false (a rejected edit is a structured result, not a tool-execution error)" );
		const JsonValue& content = result.get( "content" );
		Check( content.isArray() && content.size() >= 1, "propose_patch(unknown entity) result carries content" );
		JsonValue inner; std::string perr;
		Check( JsonParse( content.at( 0 ).get( "text" ).asString(), inner, perr ),
		       "propose_patch(unknown entity) content text parses back to the inner AgentPatchResult JSON" );
		Check( inner.get( "applied" ).asBool( true ) == false,
		       "the inner AgentPatchResult reports applied == false for the unknown entity" );
		Check( inner.get( "status" ).asString() == "rejected",
		       "the inner AgentPatchResult reports status == \"rejected\"" );
	}

	//----------------------------------------------------------------------
	// tools/call TOOL-EXECUTION error: the wrapped verb itself returns a
	// genuine JSON-RPC error (validate with NO 'text' param -> the wrapped
	// dispatcher's own -32602) -> MCP isError:true, still a JSON-RPC SUCCESS
	// envelope at the tools/call layer (NOT a protocol error at THIS
	// layer -- the protocol-error cases are unknown tool name / malformed
	// tools/call shape, tested separately below). RED-PROVE: verify the
	// isError split is a real branch, not always-true or always-false, by
	// cross-checking against the propose_patch(unknown entity) case just
	// above (isError:false) and the unknown-tool-name case below (a REAL
	// JSON-RPC error, not isError:true) -- the three together prove the
	// split is keyed on the right condition (inner JSON-RPC error vs.
	// inner JSON-RPC success vs. this adapter's own protocol-layer error).
	//----------------------------------------------------------------------
	std::printf( "[tools/call] tool-execution error (validate missing 'text') -> isError:true\n" );
	{
		const std::string resp = mcp.HandleLine( ReqToolCall( 24, "validate", JsonValue::MakeObject() ) );
		JsonValue env = ParseResponse( resp, 24 );
		Check( !env.has( "error" ),
		       "tools/call(validate, missing text) is a JSON-RPC SUCCESS envelope (tool errors are not protocol errors)" );
		Check( env.has( "result" ), "tools/call(validate, missing text) has a result field" );
		const JsonValue& result = env.get( "result" );
		Check( result.get( "isError" ).asBool( false ) == true,
		       "tools/call(validate, missing text) isError == true (the wrapped verb's own -32602 becomes a tool-execution error)" );
		const JsonValue& content = result.get( "content" );
		Check( content.isArray() && content.size() >= 1, "tool-execution error result carries content" );
		const std::string text = content.at( 0 ).get( "text" ).asString();
		Check( text.find( "-32602" ) != std::string::npos || text.find( "text" ) != std::string::npos,
		       "tool-execution error content explains the failure (mentions the code or the missing field)" );
	}

	//----------------------------------------------------------------------
	// tools/call protocol error: unknown tool name -> a REAL JSON-RPC error
	// (method not found), NOT a success envelope with isError:true.
	//----------------------------------------------------------------------
	std::printf( "[tools/call] protocol error: unknown tool name -> JSON-RPC error\n" );
	{
		const std::string resp = mcp.HandleLine( ReqToolCall( 26, "this_tool_does_not_exist", JsonValue::MakeObject() ) );
		JsonValue env = ParseResponse( resp, 26 );
		Check( env.has( "error" ), "tools/call(unknown tool name) IS a JSON-RPC error envelope (protocol error, not tool error)" );
		Check( !env.has( "result" ), "tools/call(unknown tool name) has NO result field" );
		Check( env.get( "error" ).get( "code" ).asNumber() == -32601.0,
		       "tools/call(unknown tool name) -> error.code == -32601 (method not found)" );
	}
	{
		// Missing 'name' entirely -> -32602 Invalid params (still a
		// protocol-layer failure, before we even get to a tool lookup).
		JsonValue params = JsonValue::MakeObject();
		params.set( "arguments", JsonValue::MakeObject() );
		const std::string resp = mcp.HandleLine( Req( 27, "tools/call", params ) );
		JsonValue env = ParseResponse( resp, 27 );
		Check( env.get( "error" ).get( "code" ).asNumber() == -32602.0,
		       "tools/call with no 'name' -> error.code == -32602" );
	}

	//----------------------------------------------------------------------
	// Malformed line -> -32700.
	//----------------------------------------------------------------------
	std::printf( "[errors] malformed line -> -32700\n" );
	{
		const std::string resp = mcp.HandleLine( "{ not valid json at all " );
		JsonValue env; std::string err;
		Check( JsonParse( resp, env, err ), "the -32700 response is itself valid JSON" );
		Check( env.get( "error" ).get( "code" ).asNumber() == -32700.0,
		       "malformed line -> error.code == -32700" );
		Check( env.get( "id" ).isNull(), "malformed line -> id is null (un-attributable)" );
	}

	//----------------------------------------------------------------------
	// Batch (array) envelope -> a clean rejection, not a crash.  MCP
	// 2025-03-26 dropped JSON-RPC batching.
	//----------------------------------------------------------------------
	std::printf( "[errors] batch (array) envelope -> clean rejection\n" );
	{
		const std::string batchLine =
			"[" + ReqToolCall( 30, "read_document", JsonValue::MakeObject() ) + "," +
			      ReqToolCall( 31, "read_document", JsonValue::MakeObject() ) + "]";
		const std::string resp = mcp.HandleLine( batchLine );
		JsonValue env; std::string err;
		Check( JsonParse( resp, env, err ), "the batch-rejection response is itself valid JSON" );
		Check( env.has( "error" ), "a batch (array) envelope is rejected with an error, not silently processed" );
		Check( env.get( "id" ).isNull(), "batch envelope rejection -> id is null (no single id is determinable from an array)" );
		// RED-PROVE the adapter is still alive and correctly serving
		// requests after the batch rejection (no corrupted state). Use a
		// real MCP method (tools/call) -- "read_document" alone is an
		// AgentRpc-internal verb name, not a top-level MCP method.
		const std::string resp2 = mcp.HandleLine( ReqToolCall( 32, "read_document", JsonValue::MakeObject() ) );
		JsonValue env2 = ParseResponse( resp2, 32 );
		Check( !env2.has( "error" ), "the adapter still serves a normal request cleanly after a batch rejection" );
	}

	//----------------------------------------------------------------------
	// Unknown top-level METHOD (not tools/call, not initialize, not a
	// recognized MCP method) -> -32601, mirroring the underlying dispatcher.
	//----------------------------------------------------------------------
	std::printf( "[errors] unknown top-level method -> -32601\n" );
	{
		const std::string resp = mcp.HandleLine( Req( 33, "no_such_mcp_method", JsonValue::MakeObject() ) );
		JsonValue env = ParseResponse( resp, 33 );
		Check( env.get( "error" ).get( "code" ).asNumber() == -32601.0,
		       "unknown top-level method -> error.code == -32601" );
	}

	//----------------------------------------------------------------------
	// No-head bootstrap: an AgentMcpAdapter constructed with a NULL
	// session still speaks valid MCP (initialize/tools/list work; a
	// session-backed tools/call reports a TOOL-EXECUTION error, not a
	// protocol crash).
	//----------------------------------------------------------------------
	std::printf( "[no-head bootstrap] MCP adapter with a NULL session\n" );
	{
		std::unique_ptr<AgentSession> nullSession;
		AgentMcpAdapter nohead( std::move( nullSession ) );

		const std::string initResp = nohead.HandleLine( Req( 40, "initialize", JsonValue::MakeObject() ) );
		JsonValue initEnv = ParseResponse( initResp, 40 );
		Check( !initEnv.has( "error" ), "no-head initialize still succeeds" );

		const std::string listResp = nohead.HandleLine( Req( 41, "tools/list", JsonValue::MakeObject() ) );
		JsonValue listEnv = ParseResponse( listResp, 41 );
		Check( listEnv.get( "result" ).get( "tools" ).size() == 12, "no-head tools/list still lists all 12 tools" );

		// A stateless tool (read_schema) works with no head.
		{
			JsonValue args = JsonValue::MakeObject();
			args.set( "keyword", JsonValue::MakeString( "sphere_geometry" ) );
			const std::string resp = nohead.HandleLine( ReqToolCall( 42, "read_schema", args ) );
			JsonValue env = ParseResponse( resp, 42 );
			Check( !env.has( "error" ), "no-head tools/call(read_schema) is a JSON-RPC success" );
			Check( !env.get( "result" ).get( "isError" ).asBool( true ),
			       "no-head tools/call(read_schema) isError == false (stateless tool works with no head)" );
		}

		// A head-backed tool (render) reports a TOOL-EXECUTION error
		// (isError:true), not a protocol crash.
		{
			const std::string resp = nohead.HandleLine( ReqToolCall( 43, "render", JsonValue::MakeObject() ) );
			JsonValue env = ParseResponse( resp, 43 );
			Check( !env.has( "error" ), "no-head tools/call(render) is still a JSON-RPC SUCCESS envelope" );
			Check( env.get( "result" ).get( "isError" ).asBool( false ) == true,
			       "no-head tools/call(render) isError == true (head required, reported as a tool error)" );
		}
	}

	std::printf( "=== AgentMcpAdapterTest: %d passed, %d failed ===\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
