//////////////////////////////////////////////////////////////////////
//
//  AgentChatLoopTest.cpp - Facet 5 slice B1: the sans-IO LLM chat-loop
//    core (AgentChatLoop + Anthropic/Gemini codecs).
//
//  Drives the WHOLE chat loop with NO network and NO LLM: canned raw
//  provider JSON fixtures play the model's side, and a LIVE
//  AgentRpcDispatcher (over the same inline native-v7 scene the slice-0c
//  test uses) executes the tool calls for real -- so the loop's
//  translation layer is proven against actual verb results, including a
//  real render + read_image PNG.
//
//  Coverage (T1..T8 of the slice brief):
//    T1  Anthropic request shape + API-KEY LEAK CHECK (key ONLY in the
//        x-api-key header value; nowhere else in url/headers/body).
//    T2  Anthropic tool loop end-to-end against the live dispatcher
//        (propose_patch really applies; the real result JSON rides back
//        in ONE user message with the matching tool_use_id).
//    T3  read_image packing: a real render's PNG lands as a REAL image
//        block (media_type image/png, valid base64 decoding to \x89PNG)
//        and the base64 is NOT duplicated into the textual part.
//    T8  Parallel tool_use: two calls in one assistant message -> both
//        executed -> BOTH tool_results in ONE user message.
//    T4  Gemini request shape (x-goog-api-key; functionDeclarations) +
//        loop (synthesized call_0/call_1 ids; functionResponse +
//        inlineData in the follow-up body; key-leak check).
//    T5  Multi-turn VERBATIM echo: a thinking block with a signature
//        round-trips byte-preserved into the next request body.
//    T6  Provider switch resets the transcript; same-provider
//        AddUserMessage preserves it.
//    T7  Hostile inputs: malformed JSON, missing content, unknown tool
//        name, non-200 with error body, the iteration cap at the
//        documented count.  No crashes; all ProviderError/graceful.
//
//  RED-PROVE evidence (development-time, reverted): (a) the T1 key-leak
//  check was run against a codec variant that put the key in the body
//  -> FAILED as expected; (b) T5 was run against a parse+re-serialize
//  echo (one byte of formatting lost) -> FAILED as expected.
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Agent/AgentChatLoop.h"
#include "../src/Library/Agent/AgentChatCodecs.h"
#include "../src/Library/Agent/AgentSession.h"
#include "../src/Library/Agent/AgentRpc.h"
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

// The same inline native-v7 scene the slice-0b/0c tests use: a lit diffuse
// sphere + an area emitter, PT at low spp with OIDN off, renders non-black.
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

//----------------------------------------------------------------------
// Fixture builders (realistic raw provider JSON, written by hand).
//----------------------------------------------------------------------

// An Anthropic assistant message whose content array is `contentJson`,
// with the given stop_reason.
static std::string AnthropicFixture( const std::string& contentJson, const char* stopReason )
{
	return std::string(
		"{\"id\":\"msg_01Fixture\",\"type\":\"message\",\"role\":\"assistant\","
		"\"model\":\"claude-sonnet-5\",\"content\":" ) + contentJson +
		",\"stop_reason\":\"" + stopReason + "\",\"stop_sequence\":null,"
		"\"usage\":{\"input_tokens\":128,\"output_tokens\":64}}";
}

// A Gemini generateContent response whose candidates[0].content is
// `contentJson` (a {role,parts} object).
static std::string GeminiFixture( const std::string& contentJson, const char* finishReason )
{
	return std::string( "{\"candidates\":[{\"content\":" ) + contentJson +
		",\"finishReason\":\"" + finishReason + "\",\"index\":0}],"
		"\"usageMetadata\":{\"promptTokenCount\":64,\"candidatesTokenCount\":32},"
		"\"modelVersion\":\"gemini-3.5-flash\"}";
}

//----------------------------------------------------------------------
// Small body-inspection helpers (parse the request body we built and
// walk it structurally -- no fragile escaped-substring matching).
//----------------------------------------------------------------------

static JsonValue ParseBody( const std::string& body )
{
	JsonValue root;
	std::string err;
	if( !JsonParse( body, root, err ) ) return JsonValue::MakeNull();
	return root;
}

static JsonValue LastArrayEntry( const JsonValue& root, const char* arrayKey )
{
	const JsonValue& arr = root.get( arrayKey );
	if( !arr.isArray() || arr.size() == 0 ) return JsonValue::MakeNull();
	return arr.at( arr.size() - 1 );
}

// The one distinctive fake key used throughout; leak-checked everywhere.
static const char* const kApiKey = "sk-test-SECRETKEY-1234567890abcd";

// Assert the key appears ONLY as the exact value of `authHeader` -- not
// in the url, not in the body, not in any other header name/value.
static void CheckKeyOnlyInAuthHeader( const ChatHttpRequest& req, const char* authHeader,
                                      const char* label )
{
	Check( req.url.find( kApiKey ) == std::string::npos,
	       std::string( label ) + ": key does NOT appear in the url" );
	Check( req.body.find( kApiKey ) == std::string::npos,
	       std::string( label ) + ": key does NOT appear in the body" );
	bool sawAuth = false;
	bool leaked = false;
	for( std::size_t i = 0; i < req.headers.size(); ++i ) {
		const std::string& name = req.headers[i].first;
		const std::string& value = req.headers[i].second;
		if( name == authHeader ) {
			sawAuth = true;
			Check( value == kApiKey,
			       std::string( label ) + ": " + authHeader + " carries the exact key" );
			continue;
		}
		if( name.find( kApiKey ) != std::string::npos ||
		    value.find( kApiKey ) != std::string::npos ) leaked = true;
	}
	Check( sawAuth, std::string( label ) + ": the " + authHeader + " header is present" );
	Check( !leaked, std::string( label ) + ": key appears in NO other header" );
}

//----------------------------------------------------------------------
// T1: Anthropic request shape + key-leak check.
//----------------------------------------------------------------------
static void TestAnthropicRequestShape()
{
	std::printf( "T1: Anthropic request shape + key-leak check...\n" );
	AgentChatLoop loop;
	Check( loop.Provider() == ChatProvider::Anthropic, "default provider is Anthropic" );
	Check( loop.ModelId() == "claude-sonnet-5", "default Anthropic model id" );

	loop.AddUserMessage( "Make the sphere red" );
	const ChatHttpRequest req = loop.BuildRequest( kApiKey );

	Check( req.url == "https://api.anthropic.com/v1/messages", "url is the Messages API endpoint" );
	bool sawVersion = false;
	for( std::size_t i = 0; i < req.headers.size(); ++i )
		if( req.headers[i].first == "anthropic-version" && req.headers[i].second == "2023-06-01" )
			sawVersion = true;
	Check( sawVersion, "anthropic-version: 2023-06-01 header present" );
	CheckKeyOnlyInAuthHeader( req, "x-api-key", "T1" );

	// Body shape: model / max_tokens / system / all six tools / the user turn.
	JsonValue root = ParseBody( req.body );
	Check( root.isObject(), "body parses as JSON" );
	Check( root.get( "model" ).asString() == "claude-sonnet-5", "body carries the model id" );
	Check( root.get( "max_tokens" ).asNumber() == 8192.0, "body carries max_tokens 8192" );
	Check( !root.get( "system" ).asString().empty(), "body carries a non-empty system prompt" );
	Check( root.get( "system" ).asString().find( "co-edit" ) != std::string::npos ||
	       root.get( "system" ).asString().find( "CO-EDIT" ) != std::string::npos,
	       "system prompt is the co-editing prompt" );
	Check( !root.has( "thinking" ), "no thinking config is set (omitted = adaptive)" );

	const JsonValue& tools = root.get( "tools" );
	Check( tools.isArray() && tools.size() == 6, "body carries exactly six tools" );
	const char* expected[] = { "read_document", "read_schema", "validate",
	                           "propose_patch", "render", "read_image" };
	for( int t = 0; t < 6; ++t ) {
		bool found = false;
		for( std::size_t i = 0; i < tools.size(); ++i ) {
			if( tools.at( i ).get( "name" ).asString() == expected[t] ) {
				found = true;
				Check( tools.at( i ).get( "input_schema" ).isObject(),
				       std::string( "tool " ) + expected[t] + " has an input_schema object" );
				Check( !tools.at( i ).get( "description" ).asString().empty(),
				       std::string( "tool " ) + expected[t] + " has a description" );
			}
		}
		Check( found, std::string( "tool list includes " ) + expected[t] );
	}
	// Prescriptive descriptions: propose_patch instructs the headVersion protocol.
	for( std::size_t i = 0; i < tools.size(); ++i )
		if( tools.at( i ).get( "name" ).asString() == "propose_patch" )
			Check( tools.at( i ).get( "description" ).asString().find( "baseHeadVersion" ) != std::string::npos,
			       "propose_patch description instructs passing baseHeadVersion" );

	const JsonValue& msgs = root.get( "messages" );
	Check( msgs.isArray() && msgs.size() == 1, "body carries the one user message" );
	Check( msgs.at( 0 ).get( "role" ).asString() == "user", "message role is user" );
	Check( msgs.at( 0 ).get( "content" ).at( 0 ).get( "text" ).asString() == "Make the sphere red",
	       "message text round-trips" );
}

//----------------------------------------------------------------------
// T2: Anthropic tool loop end-to-end against a LIVE dispatcher.
//----------------------------------------------------------------------
static void TestAnthropicToolLoop( AgentRpcDispatcher& rpc )
{
	std::printf( "T2: Anthropic tool loop end-to-end (live dispatcher)...\n" );
	AgentChatLoop loop;
	loop.AddUserMessage( "Recolor the sphere albedo to red" );

	const std::string fixture = AnthropicFixture(
		"[{\"type\":\"text\",\"text\":\"Recoloring the sphere.\"},"
		"{\"type\":\"tool_use\",\"id\":\"toolu_01A\",\"name\":\"propose_patch\","
		"\"input\":{\"target\":\"pnt_albedo\",\"param\":\"color\",\"value\":\"0.9 0.1 0.1\"}}]",
		"tool_use" );

	ChatStepResult st = loop.HandleResponse( 200, fixture );
	Check( st.kind == ChatStepResult::Kind::ToolCalls, "tool_use fixture -> ToolCalls" );
	Check( st.toolCalls.size() == 1, "exactly one tool call" );
	if( st.toolCalls.size() != 1 ) return;
	Check( st.toolCalls[0].name == "propose_patch", "call name is propose_patch" );
	Check( st.toolCalls[0].id == "toolu_01A", "call id is the provider tool_use id" );

	// Translate to a JSON-RPC line and execute it for REAL.
	const std::string line = loop.ToolCallToJsonRpcLine( st.toolCalls[0], 1 );
	JsonValue lineJson = ParseBody( line );
	Check( lineJson.get( "method" ).asString() == "propose_patch", "rpc line carries the method" );
	Check( lineJson.get( "params" ).get( "target" ).asString() == "pnt_albedo",
	       "rpc line params carry the tool args" );
	const std::string resp = rpc.HandleLine( line );
	JsonValue respJson = ParseBody( resp );
	Check( respJson.get( "result" ).get( "applied" ).asBool(),
	       "the LIVE dispatcher really applied the patch" );

	loop.AddToolResult( st.toolCalls[0], resp );

	// The next request carries ONE user message with the matching
	// tool_use_id and the REAL result JSON.
	const ChatHttpRequest req = loop.BuildRequest( kApiKey );
	JsonValue root = ParseBody( req.body );
	JsonValue last = LastArrayEntry( root, "messages" );
	Check( last.get( "role" ).asString() == "user", "tool results ride in a user message" );
	const JsonValue& tr = last.get( "content" ).at( 0 );
	Check( tr.get( "type" ).asString() == "tool_result", "content[0] is a tool_result" );
	Check( tr.get( "tool_use_id" ).asString() == "toolu_01A", "tool_use_id matches the call" );
	Check( !tr.has( "is_error" ), "a success result carries no is_error" );
	const std::string resultText = tr.get( "content" ).at( 0 ).get( "text" ).asString();
	Check( resultText.find( "\"applied\":true" ) != std::string::npos,
	       "the REAL result JSON (applied:true) rides in the tool_result" );
	Check( resultText.find( "\"status\":\"applied\"" ) != std::string::npos,
	       "the REAL result JSON (status applied) rides in the tool_result" );

	// End the turn with an end_turn fixture -> FinalText.
	const std::string done = AnthropicFixture(
		"[{\"type\":\"text\",\"text\":\"Done: pnt_albedo is now red.\"}]", "end_turn" );
	ChatStepResult fin = loop.HandleResponse( 200, done );
	Check( fin.kind == ChatStepResult::Kind::FinalText, "end_turn fixture -> FinalText" );
	Check( fin.finalText == "Done: pnt_albedo is now red.", "final text extracted" );
	Check( loop.TranscriptSize() == 4, "transcript is user + assistant + tool-results + assistant" );
}

//----------------------------------------------------------------------
// T3: read_image packing (Anthropic) with a REAL render.
//----------------------------------------------------------------------
static void TestAnthropicReadImagePacking( AgentRpcDispatcher& rpc )
{
	std::printf( "T3: read_image packs a REAL PNG as an image block...\n" );
	AgentChatLoop loop;
	loop.AddUserMessage( "Render the scene and show me" );

	// Round 1: render (for real -- 24x24 at 8 spp).
	{
		const std::string fx = AnthropicFixture(
			"[{\"type\":\"tool_use\",\"id\":\"toolu_render\",\"name\":\"render\",\"input\":{}}]",
			"tool_use" );
		ChatStepResult st = loop.HandleResponse( 200, fx );
		Check( st.kind == ChatStepResult::Kind::ToolCalls && st.toolCalls.size() == 1,
		       "render fixture -> one ToolCall" );
		if( st.toolCalls.size() != 1 ) return;
		const std::string resp = rpc.HandleLine( loop.ToolCallToJsonRpcLine( st.toolCalls[0], 2 ) );
		Check( ParseBody( resp ).get( "result" ).get( "ok" ).asBool(), "live render ok" );
		loop.AddToolResult( st.toolCalls[0], resp );
	}

	// Round 2: read_image (fetches the cached PNG).
	{
		const std::string fx = AnthropicFixture(
			"[{\"type\":\"tool_use\",\"id\":\"toolu_img\",\"name\":\"read_image\",\"input\":{}}]",
			"tool_use" );
		ChatStepResult st = loop.HandleResponse( 200, fx );
		Check( st.kind == ChatStepResult::Kind::ToolCalls && st.toolCalls.size() == 1,
		       "read_image fixture -> one ToolCall" );
		if( st.toolCalls.size() != 1 ) return;
		const std::string resp = rpc.HandleLine( loop.ToolCallToJsonRpcLine( st.toolCalls[0], 3 ) );
		loop.AddToolResult( st.toolCalls[0], resp );
	}

	const ChatHttpRequest req = loop.BuildRequest( kApiKey );
	JsonValue root = ParseBody( req.body );
	JsonValue last = LastArrayEntry( root, "messages" );
	const JsonValue& tr = last.get( "content" ).at( 0 );
	Check( tr.get( "type" ).asString() == "tool_result", "read_image result is a tool_result" );

	// Find the image block + collect every text block of the message.
	std::string b64;
	std::vector<std::string> texts;
	const JsonValue& blocks = tr.get( "content" );
	for( std::size_t i = 0; i < blocks.size(); ++i ) {
		const JsonValue& b = blocks.at( i );
		if( b.get( "type" ).asString() == "image" ) {
			Check( b.get( "source" ).get( "type" ).asString() == "base64", "image source type base64" );
			Check( b.get( "source" ).get( "media_type" ).asString() == "image/png",
			       "image media_type is image/png" );
			b64 = b.get( "source" ).get( "data" ).asString();
		}
		else if( b.get( "type" ).asString() == "text" ) {
			texts.push_back( b.get( "text" ).asString() );
		}
	}
	Check( !b64.empty(), "an image block is present with base64 data" );
	std::vector<unsigned char> png;
	Check( Base64Decode( b64, png ), "image base64 decodes cleanly" );
	Check( png.size() >= 8 &&
	       png[0] == 0x89 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G',
	       "decoded bytes carry the PNG signature (a REAL render travelled)" );

	// The base64 must NOT also ride in the textual part (not double-sent).
	Check( !texts.empty(), "a textual summary part is present" );
	const std::string probe = b64.substr( 0, 48 );
	bool doubled = false;
	for( std::size_t i = 0; i < texts.size(); ++i )
		if( texts[i].find( probe ) != std::string::npos ) doubled = true;
	Check( !doubled, "the base64 is STRIPPED from the textual part (not double-sent)" );
	bool noted = false;
	for( std::size_t i = 0; i < texts.size(); ++i )
		if( texts[i].find( "byteLength" ) != std::string::npos ) noted = true;
	Check( noted, "the textual part keeps the non-image result fields (byteLength)" );
}

//----------------------------------------------------------------------
// T8: parallel tool_use -> BOTH results in ONE user message.
//----------------------------------------------------------------------
static void TestParallelToolUse( AgentRpcDispatcher& rpc )
{
	std::printf( "T8: parallel tool_use -> both results in ONE user message...\n" );
	AgentChatLoop loop;
	loop.AddUserMessage( "Read the scene and re-render it" );

	const std::string fx = AnthropicFixture(
		"[{\"type\":\"tool_use\",\"id\":\"toolu_parA\",\"name\":\"read_document\",\"input\":{}},"
		"{\"type\":\"tool_use\",\"id\":\"toolu_parB\",\"name\":\"render\",\"input\":{}}]",
		"tool_use" );
	ChatStepResult st = loop.HandleResponse( 200, fx );
	Check( st.kind == ChatStepResult::Kind::ToolCalls && st.toolCalls.size() == 2,
	       "two parallel tool calls parsed" );
	if( st.toolCalls.size() != 2 ) return;

	const std::size_t before = loop.TranscriptSize();
	loop.AddToolResult( st.toolCalls[0], rpc.HandleLine( loop.ToolCallToJsonRpcLine( st.toolCalls[0], 4 ) ) );
	Check( loop.TranscriptSize() == before, "first result alone does NOT flush yet" );
	loop.AddToolResult( st.toolCalls[1], rpc.HandleLine( loop.ToolCallToJsonRpcLine( st.toolCalls[1], 5 ) ) );
	Check( loop.TranscriptSize() == before + 1, "second result flushes ONE tool-results entry" );

	const ChatHttpRequest req = loop.BuildRequest( kApiKey );
	JsonValue root = ParseBody( req.body );
	JsonValue last = LastArrayEntry( root, "messages" );
	Check( last.get( "role" ).asString() == "user", "the packed entry is a user message" );
	const JsonValue& content = last.get( "content" );
	Check( content.isArray() && content.size() == 2, "ONE user message carries BOTH tool_results" );
	bool sawA = false, sawB = false;
	for( std::size_t i = 0; i < content.size(); ++i ) {
		const std::string id = content.at( i ).get( "tool_use_id" ).asString();
		if( id == "toolu_parA" ) sawA = true;
		if( id == "toolu_parB" ) sawB = true;
	}
	Check( sawA && sawB, "both tool_use_ids are answered in the same message" );
}

//----------------------------------------------------------------------
// T4: Gemini request shape + loop (synthesized ids; functionResponse +
//     inlineData follow-up).  Relies on T3's render having populated the
//     session's cached PNG.
//----------------------------------------------------------------------
static void TestGemini( AgentRpcDispatcher& rpc )
{
	std::printf( "T4: Gemini request shape + tool loop...\n" );
	AgentChatLoop loop;
	loop.SetProvider( ChatProvider::Gemini );
	Check( loop.ModelId() == "gemini-3.5-flash", "default Gemini model id" );

	loop.AddUserMessage( "Show me the current rendering" );
	{
		const ChatHttpRequest req = loop.BuildRequest( kApiKey );
		Check( req.url == "https://generativelanguage.googleapis.com/v1beta/models/gemini-3.5-flash:generateContent",
		       "url is the v1beta generateContent endpoint" );
		CheckKeyOnlyInAuthHeader( req, "x-goog-api-key", "T4" );

		JsonValue root = ParseBody( req.body );
		Check( root.isObject(), "gemini body parses as JSON" );
		Check( root.get( "systemInstruction" ).get( "parts" ).at( 0 ).get( "text" ).asString() ==
		       AgentChatLoop::SystemPrompt(),
		       "systemInstruction carries the co-editing prompt" );
		const JsonValue& decls = root.get( "tools" ).at( 0 ).get( "functionDeclarations" );
		Check( decls.isArray() && decls.size() == 6, "six functionDeclarations" );
		bool sawPatch = false;
		for( std::size_t i = 0; i < decls.size(); ++i )
			if( decls.at( i ).get( "name" ).asString() == "propose_patch" ) {
				sawPatch = true;
				Check( decls.at( i ).get( "parameters" ).isObject(),
				       "propose_patch declaration carries parameters" );
			}
		Check( sawPatch, "functionDeclarations include propose_patch" );
		const JsonValue& contents = root.get( "contents" );
		Check( contents.isArray() && contents.size() == 1 &&
		       contents.at( 0 ).get( "role" ).asString() == "user",
		       "contents carry the one user turn" );
	}

	// A model turn with TWO functionCalls -> synthesized ids call_0/call_1.
	const std::string fx = GeminiFixture(
		"{\"parts\":[{\"functionCall\":{\"name\":\"read_document\",\"args\":{}}},"
		"{\"functionCall\":{\"name\":\"read_image\",\"args\":{}}}],\"role\":\"model\"}",
		"STOP" );
	ChatStepResult st = loop.HandleResponse( 200, fx );
	Check( st.kind == ChatStepResult::Kind::ToolCalls && st.toolCalls.size() == 2,
	       "gemini functionCalls -> two ToolCalls" );
	if( st.toolCalls.size() != 2 ) return;
	Check( st.toolCalls[0].id == "call_0" && st.toolCalls[1].id == "call_1",
	       "ids are synthesized call_0 / call_1" );
	Check( st.toolCalls[0].name == "read_document" && st.toolCalls[1].name == "read_image",
	       "call names parsed" );

	loop.AddToolResult( st.toolCalls[0], rpc.HandleLine( loop.ToolCallToJsonRpcLine( st.toolCalls[0], 6 ) ) );
	loop.AddToolResult( st.toolCalls[1], rpc.HandleLine( loop.ToolCallToJsonRpcLine( st.toolCalls[1], 7 ) ) );

	const ChatHttpRequest req = loop.BuildRequest( kApiKey );
	JsonValue root = ParseBody( req.body );
	JsonValue last = LastArrayEntry( root, "contents" );
	Check( last.get( "role" ).asString() == "user", "gemini tool results ride in a user turn" );
	const JsonValue& parts = last.get( "parts" );
	// read_document -> functionResponse; read_image -> functionResponse +
	// a separate inlineData image part in the SAME turn.
	Check( parts.isArray() && parts.size() == 3,
	       "user turn carries functionResponse x2 + inlineData (3 parts)" );
	std::string b64;
	bool sawDoc = false, sawImg = false;
	std::string imgResponseJson;
	for( std::size_t i = 0; i < parts.size(); ++i ) {
		const JsonValue& p = parts.at( i );
		if( const JsonValue* fr = p.find( "functionResponse" ) ) {
			const std::string name = fr->get( "name" ).asString();
			if( name == "read_document" ) {
				sawDoc = true;
				Check( fr->get( "response" ).get( "document" ).asString().find( "sphere_geometry" ) != std::string::npos,
				       "read_document functionResponse carries the REAL document" );
			}
			if( name == "read_image" ) {
				sawImg = true;
				imgResponseJson = JsonSerialize( fr->get( "response" ) );
			}
		}
		if( const JsonValue* blob = p.find( "inlineData" ) ) {
			Check( blob->get( "mimeType" ).asString() == "image/png", "inlineData mimeType image/png" );
			b64 = blob->get( "data" ).asString();
		}
	}
	Check( sawDoc && sawImg, "both functionResponses present (matched by name)" );
	Check( !b64.empty(), "inlineData image part present" );
	std::vector<unsigned char> png;
	Check( Base64Decode( b64, png ) && png.size() >= 8 && png[0] == 0x89 && png[1] == 'P',
	       "inlineData decodes to a real PNG" );
	Check( imgResponseJson.find( b64.substr( 0, 48 ) ) == std::string::npos,
	       "base64 is STRIPPED from the functionResponse JSON (not double-sent)" );

	// Final text turn.
	const std::string done = GeminiFixture(
		"{\"parts\":[{\"text\":\"Here is the current rendering.\"}],\"role\":\"model\"}", "STOP" );
	ChatStepResult fin = loop.HandleResponse( 200, done );
	Check( fin.kind == ChatStepResult::Kind::FinalText &&
	       fin.finalText == "Here is the current rendering.",
	       "gemini STOP text turn -> FinalText" );
}

//----------------------------------------------------------------------
// T5: multi-turn VERBATIM echo of provider-native assistant content.
//----------------------------------------------------------------------
static void TestVerbatimEcho()
{
	std::printf( "T5: assistant content echoes byte-preserved (signature intact)...\n" );
	AgentChatLoop loop;
	loop.AddUserMessage( "Think about the scene" );

	// Deliberately quirky formatting inside the content array (a space
	// after one colon, a \uXXXX escape): a parse + re-serialize echo
	// would normalize these; the raw-span echo must NOT.
	const std::string content =
		"[{\"type\":\"thinking\",\"thinking\":\"The sphere\\u2019s albedo is grey; plan: recolor.\","
		" \"signature\":\"EqQBCkYIAhABGAIiQPd8kZzXqm5S1==\"},"
		"{\"type\":\"text\",\"text\":\"I have a plan.\"}]";
	const std::string fx = AnthropicFixture( content, "end_turn" );

	ChatStepResult st = loop.HandleResponse( 200, fx );
	Check( st.kind == ChatStepResult::Kind::FinalText, "thinking+text end_turn -> FinalText" );
	Check( st.finalText == "I have a plan.", "display text is the text block only" );

	loop.AddUserMessage( "Go ahead" );
	const ChatHttpRequest req = loop.BuildRequest( kApiKey );
	Check( req.body.find( content ) != std::string::npos,
	       "the NEXT request echoes the assistant content array BYTE-PRESERVED "
	       "(signature + thinking + quirky formatting intact)" );
}

//----------------------------------------------------------------------
// T6: provider switch resets the transcript; AddUserMessage preserves it.
//----------------------------------------------------------------------
static void TestProviderSwitch()
{
	std::printf( "T6: provider switch resets the transcript...\n" );
	AgentChatLoop loop;
	loop.AddUserMessage( "one" );
	loop.AddUserMessage( "two" );
	Check( loop.TranscriptSize() == 2, "same-provider AddUserMessage preserves the transcript" );

	loop.SetProvider( ChatProvider::Gemini );
	Check( loop.TranscriptSize() == 0, "switching provider RESETS the transcript" );
	Check( loop.Provider() == ChatProvider::Gemini, "provider switched" );

	loop.AddUserMessage( "three" );
	Check( loop.TranscriptSize() == 1, "post-switch messages accumulate" );

	loop.SetProvider( ChatProvider::Anthropic, "claude-sonnet-4-6" );
	Check( loop.TranscriptSize() == 0, "switching back resets again" );
	Check( loop.ModelId() == "claude-sonnet-4-6", "explicit model id honoured" );
	loop.SetProvider( ChatProvider::Anthropic );
	Check( loop.ModelId() == "claude-sonnet-5", "empty model id -> provider default" );
}

//----------------------------------------------------------------------
// T7: hostile inputs -- no crashes, all graceful.
//----------------------------------------------------------------------
static void TestHostileInputs( AgentRpcDispatcher& rpc )
{
	std::printf( "T7: hostile inputs (malformed / missing / unknown / non-200 / cap)...\n" );

	// Malformed JSON body.
	{
		AgentChatLoop loop;
		loop.AddUserMessage( "hi" );
		ChatStepResult st = loop.HandleResponse( 200, "{ this is not json" );
		Check( st.kind == ChatStepResult::Kind::ProviderError, "malformed body -> ProviderError" );
		Check( loop.TranscriptSize() == 1, "malformed body records NO assistant turn" );
	}

	// Missing content.
	{
		AgentChatLoop loop;
		loop.AddUserMessage( "hi" );
		ChatStepResult st = loop.HandleResponse( 200,
			"{\"id\":\"msg_x\",\"type\":\"message\",\"role\":\"assistant\",\"stop_reason\":\"end_turn\"}" );
		Check( st.kind == ChatStepResult::Kind::ProviderError, "missing content -> ProviderError" );
	}

	// Gemini: no candidates.
	{
		AgentChatLoop loop;
		loop.SetProvider( ChatProvider::Gemini );
		loop.AddUserMessage( "hi" );
		ChatStepResult st = loop.HandleResponse( 200,
			"{\"promptFeedback\":{\"blockReason\":\"SAFETY\"}}" );
		Check( st.kind == ChatStepResult::Kind::ProviderError, "no candidates -> ProviderError" );
		Check( st.errorMessage.find( "SAFETY" ) != std::string::npos,
		       "blockReason surfaces in the error message" );
	}

	// Unknown tool name: flows to the dispatcher, comes back -32601, and
	// is packed as an ERROR tool result -- gracefully self-correcting.
	{
		AgentChatLoop loop;
		loop.AddUserMessage( "hi" );
		const std::string fx = AnthropicFixture(
			"[{\"type\":\"tool_use\",\"id\":\"toolu_bogus\",\"name\":\"no_such_tool\",\"input\":{}}]",
			"tool_use" );
		ChatStepResult st = loop.HandleResponse( 200, fx );
		Check( st.kind == ChatStepResult::Kind::ToolCalls, "unknown tool still parses as a call" );
		if( st.toolCalls.size() == 1 ) {
			const std::string resp = rpc.HandleLine( loop.ToolCallToJsonRpcLine( st.toolCalls[0], 8 ) );
			loop.AddToolResult( st.toolCalls[0], resp );
			JsonValue root = ParseBody( loop.BuildRequest( kApiKey ).body );
			JsonValue last = LastArrayEntry( root, "messages" );
			const JsonValue& tr = last.get( "content" ).at( 0 );
			Check( tr.get( "is_error" ).asBool(), "-32601 envelope packs as is_error:true" );
			Check( tr.get( "content" ).at( 0 ).get( "text" ).asString().find( "-32601" ) != std::string::npos,
			       "the JSON-RPC error rides in the error tool result" );
		}
	}

	// Malformed tool args degrade to empty params (never throws).
	{
		AgentChatLoop loop;
		ChatToolCall bad;
		bad.id = "toolu_badargs";
		bad.name = "validate";
		bad.argsJson = "{ not json";
		const std::string line = loop.ToolCallToJsonRpcLine( bad, 9 );
		JsonValue lineJson = ParseBody( line );
		Check( lineJson.get( "params" ).isObject(), "malformed args -> empty params object" );
		const std::string resp = rpc.HandleLine( line );
		Check( ParseBody( resp ).get( "error" ).get( "code" ).asNumber() == -32602.0,
		       "dispatcher answers -32602 (flows back as an error result)" );
	}

	// Non-200 with a provider error body.
	{
		AgentChatLoop loop;
		loop.AddUserMessage( "hi" );
		ChatStepResult st = loop.HandleResponse( 429,
			"{\"type\":\"error\",\"error\":{\"type\":\"rate_limit_error\",\"message\":\"rate limited\"}}" );
		Check( st.kind == ChatStepResult::Kind::ProviderError, "HTTP 429 -> ProviderError" );
		Check( st.errorMessage.find( "429" ) != std::string::npos &&
		       st.errorMessage.find( "rate limited" ) != std::string::npos,
		       "error carries the status and the provider's error.message" );
	}

	// Iteration cap: exactly kMaxToolRoundsPerTurn tool rounds succeed in
	// one turn; the next one trips; AddUserMessage resets the counter.
	{
		AgentChatLoop loop;
		loop.AddUserMessage( "loop forever" );
		const std::string fx = AnthropicFixture(
			"[{\"type\":\"tool_use\",\"id\":\"toolu_spin\",\"name\":\"read_document\",\"input\":{}}]",
			"tool_use" );
		bool allOk = true;
		for( int i = 0; i < AgentChatLoop::kMaxToolRoundsPerTurn; ++i ) {
			ChatStepResult st = loop.HandleResponse( 200, fx );
			if( st.kind != ChatStepResult::Kind::ToolCalls ) allOk = false;
			if( st.toolCalls.size() == 1 )
				loop.AddToolResult( st.toolCalls[0], "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}" );
		}
		Check( allOk, "the documented 20 tool rounds all succeed" );
		Check( loop.ToolRoundCount() == AgentChatLoop::kMaxToolRoundsPerTurn,
		       "round counter reads the documented cap" );
		const std::size_t sizeAtCap = loop.TranscriptSize();
		ChatStepResult tripped = loop.HandleResponse( 200, fx );
		Check( tripped.kind == ChatStepResult::Kind::ProviderError,
		       "the 21st tool round trips the iteration cap" );
		Check( tripped.errorMessage.find( "iteration cap" ) != std::string::npos,
		       "cap error names the iteration cap" );
		Check( loop.TranscriptSize() == sizeAtCap, "the capped turn records nothing" );

		loop.AddUserMessage( "try again" );
		Check( loop.ToolRoundCount() == 0, "AddUserMessage resets the round counter" );
		ChatStepResult again = loop.HandleResponse( 200, fx );
		Check( again.kind == ChatStepResult::Kind::ToolCalls,
		       "after a new user message the loop runs again" );
	}
}

int main()
{
	std::printf( "=== AgentChatLoopTest (Facet 5 slice B1: sans-IO LLM chat loop) ===\n" );

	// ONE live dispatcher over the inline scene, shared by the tests that
	// execute verbs for real (T4's read_image reuses T3's cached render).
	const std::string scenePath = WriteTemp( "rise_agent_chatloop.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the scene to a temp file" );
	std::unique_ptr<AgentSession> session = AgentSession::LoadFromFile( scenePath );
	Check( session != nullptr, "AgentSession::LoadFromFile loads the native-v7 scene" );
	if( !session ) { std::printf( "cannot continue without a session\n" ); return 1; }
	AgentRpcDispatcher rpc( std::move( session ) );

	TestAnthropicRequestShape();
	TestAnthropicToolLoop( rpc );
	TestAnthropicReadImagePacking( rpc );
	TestParallelToolUse( rpc );
	TestGemini( rpc );
	TestVerbatimEcho();
	TestProviderSwitch();
	TestHostileInputs( rpc );

	std::remove( scenePath.c_str() );
	std::printf( "=== AgentChatLoopTest: %d passed, %d failed ===\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
