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
//        loop.  SELF-SUFFICIENT (runs its own render round).  Covers
//        both id regimes: a no-id functionCall -> synthesized call_0
//        with NO id echoed, and two functionCall.id-carrying calls
//        (Gemini 3.x) answered OUT OF ORDER -> each functionResponse
//        echoes the matching id; the read_image PNG rides INSIDE
//        functionResponse.parts[].inlineData (FunctionResponsePart);
//        key-leak checks on the first AND the follow-up request.
//    T5  Multi-turn VERBATIM echo: a thinking block with a signature
//        round-trips byte-preserved into the next request body.
//    T6  Provider switch resets the transcript; same-provider
//        AddUserMessage preserves it.
//    T7  Hostile inputs: malformed JSON, missing content, unknown tool
//        name, tool_use with no id (whole turn refused), non-200 with
//        error body, the iteration cap at the documented count.  No
//        crashes; all ProviderError/graceful.
//    T9  Wire invariant under partial rounds: a flush (BuildRequest or
//        AddUserMessage) SYNTHESIZES an is_error "not executed" result
//        for every unanswered pending call, so the transcript can never
//        replay an unanswered tool call; unknown-id / duplicate
//        AddToolResult are ignored (first result wins); a double
//        HandleResponse while calls are pending is refused.
//    T10 Dead-end surfacing: Anthropic max_tokens vs refusal get
//        DISTINCT actionable errors; a Gemini functionCall turn with
//        finishReason MAX_TOKENS is refused (truncated calls never
//        execute).  All record nothing.
//    T11 Hostile duplicate-key body: the parsed view (JsonValue::find,
//        last-set wins) and the raw-span echo (RawObjectMember, last
//        match) agree on which duplicate they take.
//    T12 BuildRequest on an empty transcript returns the documented
//        EMPTY request; hostile Gemini model ids are percent-escaped
//        in the URL path.
//    T13 Hostile-disposition gates: Anthropic tool_use blocks under
//        stop_reason end_turn, and a Gemini functionCall-keyed part
//        whose value is not an object, REFUSE the whole response
//        (record nothing; the next request is clean).
//    T14 Image retention: only the most recent read_image PNG stays
//        live; older ToolResults entries are rewritten with the image
//        elided (both providers; ids stay matched on the wire).  The
//        loop-written attach note is rewritten alongside the block
//        (never an RPC-owned note field); two images packed in ONE
//        flush keep only the LAST live (pre-elided at pack time).
//    T15 -ffast-math-safe non-finite guards: 1e999 tool args survive
//        JSON serialization as the documented `0` fallback (the
//        JSON-RPC line stays valid JSON); an `inf` numeric scene value
//        classifies as INVALID_VALUE in AgentSession validation.
//    T16 Duplicate-call-id refusal on BOTH providers (a repeated
//        Anthropic tool_use id or Gemini functionCall id refuses the
//        whole turn); Gemini synthesized ids skip collisions with ids
//        captured this turn.
//    T17 Gemini adjacent role:user contents merge into ONE content at
//        BuildRequest (functionResponse parts first) so the wire roles
//        always alternate.
//    T18 API-key header values are stripped of control characters on
//        both codecs (header-splice defense); multibyte UTF-8 model
//        ids percent-escape per byte.
//    T19 Degenerate empty-content final turns (Anthropic content:[] +
//        end_turn; Gemini STOP with missing/empty parts) are refused,
//        recording nothing.
//    T20 Gemini role-spoof gate: a candidate whose content.role is
//        present and != "model" (string or not, calls or text) is
//        refused outright -- otherwise the verbatim echo joins
//        BuildRequest's adjacent-user merge and the conversation
//        collapses into one wire-invalid user content.
//    T21 Loop-contract details: empty/whitespace AddUserMessage is a
//        documented no-op (nothing recorded, flushed, or reset);
//        direct Reset() clears transcript + pending + rounds but keeps
//        the provider/model; TranscriptAt out of range returns the
//        safe static empty entry.
//    Plus: ChatStepResult.assistantDisplayText on both success kinds,
//    ChatErrorKind asserted on every error path (Parse, Http,
//    Refusal, MaxTokens, IterationCap, Misuse, Provider), the system
//    prompt's parameter-only capability note (T1), and the Gemini
//    calls-require-explicit-STOP policy with text-only leniency (T10).
//
//  RED-PROVE evidence (development-time, reverted): (a) the T1 key-leak
//  check was run against a codec variant that put the key in the body
//  -> FAILED as expected; (b) T5 was run against a parse+re-serialize
//  echo (one byte of formatting lost) -> FAILED as expected; (c) the
//  suite was run with the flush synthesis disabled (old flush-what-
//  exists behaviour) -> exactly SIX checks failed as expected: T9(a)
//  "BOTH tool_use ids are answered in that one message" + "the
//  unanswered call carries a synthesized is_error 'not executed'
//  result", and T9(b) "user + assistant + SYNTHESIZED tool-results +
//  user", "the synthesized results ride BEFORE the new user message",
//  "the wire carries all four messages", "the interrupted call is
//  answered (is_error) before the user text"; (d) the suite was run
//  with the functionResponse.id echo dropped -> exactly TWO checks
//  failed as expected: T4 "read_document functionResponse echoes id
//  fc_doc_7 ..." and "read_image functionResponse echoes id fc_img_9
//  ...".  Round-2 additions: (e) the suite was run with the Anthropic
//  disposition gate disabled -> exactly SIX T13 checks failed
//  ("tool_use blocks under end_turn -> ProviderError ...", "...
//  errorKind Provider", "... names the offending stop_reason", "the
//  hostile turn records NOTHING ...", "the next BuildRequest carries
//  NO trace ...", "the next request carries only the user message") --
//  the recorded turn demonstrably replayed the unanswered tool_use;
//  (f) the suite was run with the image-elision pass disabled ->
//  exactly EIGHT T14 checks failed (both providers' "the OLD base64
//  rides ZERO times", "the elision text rides ...", "the old entry
//  carries NO image block/parts ...", "... elision note"); (g) the
//  suite was run with Json.cpp's SerializeNumber guard reverted to
//  std::isnan/std::isinf -> exactly TWO T15 checks failed ("the
//  non-numeric args survive ...", "the non-finite number serialized as
//  the documented fallback 0"), proving the intrinsic guard really IS
//  folded to dead code under the production -ffast-math flags and the
//  literal `inf` escapes into the serialized JSON.  Round-3 additions:
//  (h) the suite was run with the Anthropic duplicate-tool_use-id gate
//  disabled -> exactly FIVE T16 checks failed ("anthropic duplicate
//  tool_use ids -> ProviderError ...", "... errorKind Provider", "the
//  refusal names the repeated tool_use id", "... records NOTHING and
//  pends nothing", "the next BuildRequest carries NO trace ..."); (i)
//  the suite was run with the Gemini role-spoof gate disabled ->
//  exactly EIGHT T20 checks failed (both spoof cases + the non-string
//  role case), and notably "the next request carries only the user
//  message (merge untouched)" PASSED vacuously -- the spoofed turn had
//  merged into ONE user content, demonstrating the exact conversation
//  collapse the gate exists to prevent.
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Agent/AgentChatLoop.h"
#include "../src/Library/Agent/AgentChatCodecs.h"
#include "../src/Library/Agent/AgentSession.h"
#include "../src/Library/Agent/AgentDiagnostic.h"
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
	Check( root.get( "max_tokens" ).asNumber() == 16000.0,
	       "body carries max_tokens 16000 (adaptive-thinking + scene-doc headroom)" );
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
	// Prescriptive descriptions: propose_patch instructs the headVersion
	// protocol AND teaches status=diagnosed; read_image tells the truth
	// about the nothing-rendered-yet case; validate scopes "failure" to
	// error-severity diagnostics.
	for( std::size_t i = 0; i < tools.size(); ++i ) {
		const std::string name = tools.at( i ).get( "name" ).asString();
		const std::string desc = tools.at( i ).get( "description" ).asString();
		if( name == "propose_patch" ) {
			Check( desc.find( "baseHeadVersion" ) != std::string::npos,
			       "propose_patch description instructs passing baseHeadVersion" );
			Check( desc.find( "status=diagnosed" ) != std::string::npos &&
			       desc.find( "do not blindly re-propose" ) != std::string::npos,
			       "propose_patch description teaches status=diagnosed (applied but diagnosed; fix, don't re-propose)" );
		}
		if( name == "read_image" ) {
			Check( desc.find( "empty png_base64" ) != std::string::npos &&
			       desc.find( "byteLength 0" ) != std::string::npos,
			       "read_image description states the honest nothing-rendered behaviour (empty png_base64, byteLength 0)" );
			Check( desc.find( "Requires a prior successful render" ) == std::string::npos,
			       "read_image description no longer carries the false 'Requires' claim" );
		}
		if( name == "validate" ) {
			Check( desc.find( "error-severity" ) != std::string::npos,
			       "validate description scopes failure to error-severity diagnostics" );
		}
	}
	Check( root.get( "system" ).asString().find( "status=diagnosed" ) != std::string::npos,
	       "the system prompt teaches status=diagnosed" );
	Check( root.get( "system" ).asString().find( "adding or removing entities is not supported yet" ) != std::string::npos,
	       "the system prompt states the parameter-only capability scope (no entity add/remove)" );

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
	Check( st.assistantDisplayText == "Recoloring the sphere.",
	       "a ToolCalls result carries the assistant's interim display text" );
	Check( st.errorKind == ChatErrorKind::None, "a ToolCalls result carries errorKind None" );

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
	// tool_use_id and the REAL result JSON.  This is a FOLLOW-UP request
	// (one carrying tool results), so sweep it for key leaks too.
	const ChatHttpRequest req = loop.BuildRequest( kApiKey );
	CheckKeyOnlyInAuthHeader( req, "x-api-key", "T2-followup" );
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
	Check( fin.assistantDisplayText == fin.finalText,
	       "a FinalText result carries the display text too" );
	Check( fin.errorKind == ChatErrorKind::None, "a FinalText result carries errorKind None" );
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
	// Stronger: the probe occurs exactly ONCE in the WHOLE request body
	// (the image block) -- nowhere else at all.
	std::size_t occurrences = 0;
	for( std::size_t pos = req.body.find( probe ); pos != std::string::npos;
	     pos = req.body.find( probe, pos + 1 ) ) ++occurrences;
	Check( occurrences == 1, "the base64 probe occurs exactly ONCE in the whole request body" );
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
// T4: Gemini request shape + loop.  SELF-SUFFICIENT: performs its own
//     render round (no dependence on T3's cached render).  Covers BOTH
//     id regimes: a no-id functionCall (synthesized call_0; NO id field
//     echoed) and two id-carrying functionCalls (the Gemini 3.x shape)
//     answered OUT OF ORDER, whose functionResponses must echo the
//     matching ids.  read_image's PNG must ride INSIDE
//     functionResponse.parts[].inlineData (FunctionResponsePart).
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

	// Round A: a functionCall WITHOUT an id (the pre-3.x shape) ->
	// synthesized call_0.  Executes a REAL render so this test is
	// self-sufficient.
	{
		const std::string fxA = GeminiFixture(
			"{\"parts\":[{\"functionCall\":{\"name\":\"render\",\"args\":{}}}],\"role\":\"model\"}",
			"STOP" );
		ChatStepResult stA = loop.HandleResponse( 200, fxA );
		Check( stA.kind == ChatStepResult::Kind::ToolCalls && stA.toolCalls.size() == 1,
		       "no-id functionCall -> one ToolCall" );
		if( stA.toolCalls.size() != 1 ) return;
		Check( stA.toolCalls[0].id == "call_0" && stA.toolCalls[0].idSynthesized,
		       "absent functionCall.id -> synthesized call_0 (flagged synthesized)" );
		const std::string resp = rpc.HandleLine( loop.ToolCallToJsonRpcLine( stA.toolCalls[0], 6 ) );
		Check( ParseBody( resp ).get( "result" ).get( "ok" ).asBool(), "live render (gemini path) ok" );
		loop.AddToolResult( stA.toolCalls[0], resp );
	}

	// Round B: TWO functionCalls WITH provider ids (realistic Gemini 3.x
	// shape -- FunctionCall.id per the ai.google.dev/api reference),
	// answered OUT OF ORDER.
	const std::string fx = GeminiFixture(
		"{\"parts\":[{\"functionCall\":{\"id\":\"fc_doc_7\",\"name\":\"read_document\",\"args\":{}}},"
		"{\"functionCall\":{\"id\":\"fc_img_9\",\"name\":\"read_image\",\"args\":{}}}],\"role\":\"model\"}",
		"STOP" );
	ChatStepResult st = loop.HandleResponse( 200, fx );
	Check( st.kind == ChatStepResult::Kind::ToolCalls && st.toolCalls.size() == 2,
	       "gemini functionCalls -> two ToolCalls" );
	if( st.toolCalls.size() != 2 ) return;
	Check( st.toolCalls[0].id == "fc_doc_7" && !st.toolCalls[0].idSynthesized,
	       "functionCall.id fc_doc_7 is captured (NOT synthesized)" );
	Check( st.toolCalls[1].id == "fc_img_9" && !st.toolCalls[1].idSynthesized,
	       "functionCall.id fc_img_9 is captured (NOT synthesized)" );
	Check( st.toolCalls[0].name == "read_document" && st.toolCalls[1].name == "read_image",
	       "call names parsed" );

	// OUT OF ORDER on purpose: the image first, the document second.
	loop.AddToolResult( st.toolCalls[1], rpc.HandleLine( loop.ToolCallToJsonRpcLine( st.toolCalls[1], 7 ) ) );
	loop.AddToolResult( st.toolCalls[0], rpc.HandleLine( loop.ToolCallToJsonRpcLine( st.toolCalls[0], 8 ) ) );

	// FOLLOW-UP request (carries tool results): key-leak sweep it too.
	const ChatHttpRequest req = loop.BuildRequest( kApiKey );
	CheckKeyOnlyInAuthHeader( req, "x-goog-api-key", "T4-followup" );
	JsonValue root = ParseBody( req.body );
	JsonValue last = LastArrayEntry( root, "contents" );
	Check( last.get( "role" ).asString() == "user", "gemini tool results ride in a user turn" );
	const JsonValue& parts = last.get( "parts" );
	// Exactly TWO functionResponse parts; the PNG rides INSIDE
	// read_image's functionResponse.parts (FunctionResponsePart), NOT as
	// a bare sibling inlineData part.
	Check( parts.isArray() && parts.size() == 2,
	       "user turn carries exactly the two functionResponse parts" );
	std::string b64;
	bool sawDoc = false, sawImg = false;
	std::string imgResponseJson;
	for( std::size_t i = 0; i < parts.size(); ++i ) {
		const JsonValue& p = parts.at( i );
		Check( !p.has( "inlineData" ),
		       "no bare sibling inlineData part (the PNG rides inside functionResponse.parts)" );
		if( const JsonValue* fr = p.find( "functionResponse" ) ) {
			const std::string name = fr->get( "name" ).asString();
			if( name == "read_document" ) {
				sawDoc = true;
				Check( fr->get( "id" ).asString() == "fc_doc_7",
				       "read_document functionResponse echoes id fc_doc_7 (despite out-of-order answering)" );
				Check( fr->get( "response" ).get( "document" ).asString().find( "sphere_geometry" ) != std::string::npos,
				       "read_document functionResponse carries the REAL document" );
				Check( !fr->has( "parts" ), "a non-image functionResponse carries no parts array" );
			}
			if( name == "read_image" ) {
				sawImg = true;
				Check( fr->get( "id" ).asString() == "fc_img_9",
				       "read_image functionResponse echoes id fc_img_9 (despite out-of-order answering)" );
				imgResponseJson = JsonSerialize( fr->get( "response" ) );
				const JsonValue& frParts = fr->get( "parts" );
				Check( frParts.isArray() && frParts.size() == 1,
				       "functionResponse.parts carries one FunctionResponsePart" );
				const JsonValue& blob = frParts.at( 0 ).get( "inlineData" );
				Check( blob.get( "mimeType" ).asString() == "image/png",
				       "FunctionResponsePart.inlineData mimeType image/png" );
				b64 = blob.get( "data" ).asString();
			}
		}
	}
	Check( sawDoc && sawImg, "both functionResponses present" );
	Check( !b64.empty(), "the inlineData image is present inside functionResponse.parts" );
	std::vector<unsigned char> png;
	Check( Base64Decode( b64, png ) && png.size() >= 8 && png[0] == 0x89 && png[1] == 'P',
	       "inlineData decodes to a real PNG" );
	Check( imgResponseJson.find( b64.substr( 0, 48 ) ) == std::string::npos,
	       "base64 is STRIPPED from the functionResponse JSON (not double-sent)" );

	// The round-A (synthesized-id) functionResponse must carry NO id
	// field: contents = user, model A, results A, model B, results B.
	const JsonValue& contents = root.get( "contents" );
	Check( contents.size() == 5, "contents carry the full five-turn history" );
	const JsonValue* frA = contents.at( 2 ).get( "parts" ).at( 0 ).find( "functionResponse" );
	Check( frA != nullptr && frA->get( "name" ).asString() == "render" && !frA->has( "id" ),
	       "a synthesized-id functionResponse omits the id field entirely" );

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
		Check( st.errorKind == ChatErrorKind::Parse, "malformed body -> errorKind Parse" );
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
		Check( st.errorKind == ChatErrorKind::Refusal,
		       "a blocked prompt -> errorKind Refusal" );
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

	// Anthropic tool_use block with NO id: the whole turn is refused --
	// a block whose result could never be matched must not execute, and
	// tool_use_id:"" must never reach the wire.
	{
		AgentChatLoop loop;
		loop.AddUserMessage( "hi" );
		const std::string fx = AnthropicFixture(
			"[{\"type\":\"tool_use\",\"name\":\"render\",\"input\":{}}]", "tool_use" );
		ChatStepResult st = loop.HandleResponse( 200, fx );
		Check( st.kind == ChatStepResult::Kind::ProviderError,
		       "tool_use with no id -> ProviderError (never tool_use_id:\"\")" );
		Check( st.errorMessage.find( "no id" ) != std::string::npos,
		       "the error names the missing id" );
		Check( loop.TranscriptSize() == 1, "the id-less turn records nothing" );
	}

	// Non-200 with a provider error body.
	{
		AgentChatLoop loop;
		loop.AddUserMessage( "hi" );
		ChatStepResult st = loop.HandleResponse( 429,
			"{\"type\":\"error\",\"error\":{\"type\":\"rate_limit_error\",\"message\":\"rate limited\"}}" );
		Check( st.kind == ChatStepResult::Kind::ProviderError, "HTTP 429 -> ProviderError" );
		Check( st.errorKind == ChatErrorKind::Http, "HTTP 429 -> errorKind Http" );
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
		Check( tripped.errorKind == ChatErrorKind::IterationCap,
		       "the cap trip -> errorKind IterationCap" );
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

//----------------------------------------------------------------------
// T9: partial / absent tool results -- the flush SYNTHESIZES error
//     results so the wire invariant "every tool call answered in the
//     immediately-following user message" holds by construction, plus
//     the adjacent caller-contract guards (unknown/duplicate
//     AddToolResult ignored; double HandleResponse refused).
//----------------------------------------------------------------------
static void TestFlushSynthesis( AgentRpcDispatcher& rpc )
{
	std::printf( "T9: partial tool rounds synthesize error results (wire invariant)...\n" );

	// (a) Two pending calls, only ONE answered -> BuildRequest ships ONE
	// user message answering BOTH ids, and the transcript is NOT poisoned.
	{
		AgentChatLoop loop;
		loop.AddUserMessage( "read and render" );
		const std::string fx = AnthropicFixture(
			"[{\"type\":\"tool_use\",\"id\":\"toolu_ansA\",\"name\":\"read_document\",\"input\":{}},"
			"{\"type\":\"tool_use\",\"id\":\"toolu_ansB\",\"name\":\"render\",\"input\":{}}]",
			"tool_use" );
		ChatStepResult st = loop.HandleResponse( 200, fx );
		Check( st.kind == ChatStepResult::Kind::ToolCalls && st.toolCalls.size() == 2,
		       "two pending calls" );
		if( st.toolCalls.size() != 2 ) return;
		// Answer only the FIRST (an interrupt lands before the render).
		loop.AddToolResult( st.toolCalls[0],
			rpc.HandleLine( loop.ToolCallToJsonRpcLine( st.toolCalls[0], 10 ) ) );

		const ChatHttpRequest req = loop.BuildRequest( kApiKey );
		JsonValue root = ParseBody( req.body );
		JsonValue last = LastArrayEntry( root, "messages" );
		Check( last.get( "role" ).asString() == "user", "the flush produced ONE user message" );
		const JsonValue& content = last.get( "content" );
		Check( content.isArray() && content.size() == 2,
		       "BOTH tool_use ids are answered in that one message" );
		bool realA = false, synthB = false;
		for( std::size_t i = 0; i < content.size(); ++i ) {
			const JsonValue& tr = content.at( i );
			if( tr.get( "type" ).asString() != "tool_result" ) continue;
			const std::string id = tr.get( "tool_use_id" ).asString();
			const std::string text = tr.get( "content" ).at( 0 ).get( "text" ).asString();
			if( id == "toolu_ansA" )
				realA = !tr.has( "is_error" ) && text.find( "document" ) != std::string::npos;
			if( id == "toolu_ansB" )
				synthB = tr.get( "is_error" ).asBool() &&
				         text.find( "not executed" ) != std::string::npos;
		}
		Check( realA, "the answered call carries its REAL result (no is_error)" );
		Check( synthB, "the unanswered call carries a synthesized is_error 'not executed' result" );
		Check( loop.PendingToolCalls().empty(), "the flush cleared the pending set" );

		// The transcript is NOT poisoned: a normal round works right after.
		ChatStepResult fin = loop.HandleResponse( 200, AnthropicFixture(
			"[{\"type\":\"text\",\"text\":\"Understood.\"}]", "end_turn" ) );
		Check( fin.kind == ChatStepResult::Kind::FinalText,
		       "a subsequent normal round works (transcript not poisoned)" );
	}

	// (b) AddUserMessage with a pending unanswered call -> the same
	// synthesis, flushed BEFORE the new user message.
	{
		AgentChatLoop loop;
		loop.AddUserMessage( "render it" );
		const std::string fx = AnthropicFixture(
			"[{\"type\":\"tool_use\",\"id\":\"toolu_int1\",\"name\":\"render\",\"input\":{}}]",
			"tool_use" );
		ChatStepResult st = loop.HandleResponse( 200, fx );
		Check( st.kind == ChatStepResult::Kind::ToolCalls, "one pending call" );

		loop.AddUserMessage( "never mind, stop" );   // interrupt: NO result was added
		Check( loop.TranscriptSize() == 4,
		       "user + assistant + SYNTHESIZED tool-results + user" );
		Check( loop.TranscriptAt( 2 ).role == ChatTranscriptEntry::Role::ToolResults,
		       "the synthesized results ride BEFORE the new user message" );

		JsonValue root = ParseBody( loop.BuildRequest( kApiKey ).body );
		const JsonValue& msgs = root.get( "messages" );
		Check( msgs.isArray() && msgs.size() == 4, "the wire carries all four messages" );
		const JsonValue& tr = msgs.at( 2 ).get( "content" ).at( 0 );
		Check( tr.get( "tool_use_id" ).asString() == "toolu_int1" &&
		       tr.get( "is_error" ).asBool(),
		       "the interrupted call is answered (is_error) before the user text" );
	}

	// (c) Unknown-id and duplicate AddToolResult are IGNORED (first wins).
	{
		AgentChatLoop loop;
		loop.AddUserMessage( "two reads" );
		const std::string fx = AnthropicFixture(
			"[{\"type\":\"tool_use\",\"id\":\"toolu_c1\",\"name\":\"read_document\",\"input\":{}},"
			"{\"type\":\"tool_use\",\"id\":\"toolu_c2\",\"name\":\"read_schema\",\"input\":{}}]",
			"tool_use" );
		ChatStepResult st = loop.HandleResponse( 200, fx );
		if( st.toolCalls.size() != 2 ) { Check( false, "two pending calls expected" ); return; }

		ChatToolCall bogus;
		bogus.id = "toolu_never_issued";
		bogus.name = "render";
		loop.AddToolResult( bogus, "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"bogus\":true}}" );
		Check( loop.TranscriptSize() == 2 && loop.PendingToolCalls().size() == 2,
		       "an unknown-id result is IGNORED (no flush, nothing buffered against it)" );

		loop.AddToolResult( st.toolCalls[0], "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"which\":\"first\"}}" );
		Check( loop.TranscriptSize() == 2, "one of two answered -> no flush yet" );
		loop.AddToolResult( st.toolCalls[0], "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"which\":\"second\"}}" );
		Check( loop.TranscriptSize() == 2,
		       "a DUPLICATE result for an already-answered id is IGNORED (no premature flush)" );
		loop.AddToolResult( st.toolCalls[1], "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"ok\":true}}" );
		Check( loop.TranscriptSize() == 3, "answering the second call flushes" );

		const std::string body = loop.BuildRequest( kApiKey ).body;
		JsonValue root = ParseBody( body );
		JsonValue last = LastArrayEntry( root, "messages" );
		const JsonValue& packed = last.get( "content" );
		Check( packed.size() == 2, "exactly the two real tool_results (no bogus extra)" );
		std::string c1text;
		for( std::size_t i = 0; i < packed.size(); ++i )
			if( packed.at( i ).get( "tool_use_id" ).asString() == "toolu_c1" )
				c1text = packed.at( i ).get( "content" ).at( 0 ).get( "text" ).asString();
		Check( c1text.find( "\"which\":\"first\"" ) != std::string::npos,
		       "the FIRST result for toolu_c1 won" );
		Check( c1text.find( "second" ) == std::string::npos,
		       "the duplicate result was dropped" );
		Check( body.find( "toolu_never_issued" ) == std::string::npos &&
		       body.find( "bogus" ) == std::string::npos,
		       "the unknown-id result never reached the wire" );
	}

	// (d) A second HandleResponse while calls are pending is REFUSED and
	// records nothing; resolving the pending call recovers.
	{
		AgentChatLoop loop;
		loop.AddUserMessage( "go" );
		const std::string fx = AnthropicFixture(
			"[{\"type\":\"tool_use\",\"id\":\"toolu_dbl\",\"name\":\"read_document\",\"input\":{}}]",
			"tool_use" );
		ChatStepResult st = loop.HandleResponse( 200, fx );
		Check( st.kind == ChatStepResult::Kind::ToolCalls, "first HandleResponse consumed" );
		const std::size_t size = loop.TranscriptSize();

		ChatStepResult st2 = loop.HandleResponse( 200, fx );
		Check( st2.kind == ChatStepResult::Kind::ProviderError,
		       "a second HandleResponse while calls are pending is REFUSED" );
		Check( st2.errorKind == ChatErrorKind::Misuse,
		       "the double-HandleResponse refusal -> errorKind Misuse" );
		Check( st2.errorMessage.find( "pending" ) != std::string::npos,
		       "the refusal names the pending calls" );
		Check( loop.TranscriptSize() == size, "the refused response records NOTHING" );
		Check( loop.PendingToolCalls().size() == 1, "the pending set is untouched" );

		loop.AddToolResult( st.toolCalls[0], "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}" );
		ChatStepResult fin = loop.HandleResponse( 200, AnthropicFixture(
			"[{\"type\":\"text\",\"text\":\"done\"}]", "end_turn" ) );
		Check( fin.kind == ChatStepResult::Kind::FinalText,
		       "after resolving the pending call, HandleResponse works again" );
	}
}

//----------------------------------------------------------------------
// T10: provider dead-ends get DISTINCT, actionable errors; a truncated
//      Gemini call turn must not execute.
//----------------------------------------------------------------------
static void TestStopReasonDeadEnds()
{
	std::printf( "T10: max_tokens / refusal / truncated-call dead-ends...\n" );

	// Anthropic stop_reason max_tokens.
	{
		AgentChatLoop loop;
		loop.AddUserMessage( "hi" );
		ChatStepResult st = loop.HandleResponse( 200, AnthropicFixture(
			"[{\"type\":\"text\",\"text\":\"a truncated repl\"}]", "max_tokens" ) );
		Check( st.kind == ChatStepResult::Kind::ProviderError, "max_tokens -> ProviderError" );
		Check( st.errorKind == ChatErrorKind::MaxTokens, "max_tokens -> errorKind MaxTokens" );
		Check( st.errorMessage.find( "output-token cap" ) != std::string::npos &&
		       st.errorMessage.find( "narrower request" ) != std::string::npos,
		       "the max_tokens message is distinct and actionable" );
		Check( loop.TranscriptSize() == 1, "the truncated turn records nothing" );
	}

	// Anthropic stop_reason refusal -- a DIFFERENT message.
	{
		AgentChatLoop loop;
		loop.AddUserMessage( "hi" );
		ChatStepResult st = loop.HandleResponse( 200, AnthropicFixture(
			"[]", "refusal" ) );
		Check( st.kind == ChatStepResult::Kind::ProviderError, "refusal -> ProviderError" );
		Check( st.errorKind == ChatErrorKind::Refusal, "refusal -> errorKind Refusal" );
		Check( st.errorMessage.find( "declined" ) != std::string::npos,
		       "the refusal message says the provider declined" );
		Check( st.errorMessage.find( "output-token cap" ) == std::string::npos,
		       "refusal and max_tokens messages are DISTINCT" );
		Check( loop.TranscriptSize() == 1, "the refused turn records nothing" );
	}

	// Gemini functionCall + finishReason MAX_TOKENS: the truncated call
	// turn must NOT execute (mirrors the Anthropic stop_reason gate).
	{
		AgentChatLoop loop;
		loop.SetProvider( ChatProvider::Gemini );
		loop.AddUserMessage( "hi" );
		const std::string fx = GeminiFixture(
			"{\"parts\":[{\"functionCall\":{\"id\":\"fc_trunc\",\"name\":\"render\",\"args\":{}}}],\"role\":\"model\"}",
			"MAX_TOKENS" );
		ChatStepResult st = loop.HandleResponse( 200, fx );
		Check( st.kind == ChatStepResult::Kind::ProviderError,
		       "functionCall + MAX_TOKENS -> ProviderError (not ToolCalls)" );
		Check( st.errorKind == ChatErrorKind::MaxTokens,
		       "gemini MAX_TOKENS -> errorKind MaxTokens" );
		Check( st.errorMessage.find( "MAX_TOKENS" ) != std::string::npos &&
		       st.errorMessage.find( "truncated" ) != std::string::npos,
		       "the error names the finishReason and the truncation" );
		Check( loop.TranscriptSize() == 1 && loop.PendingToolCalls().empty(),
		       "the truncated call turn records nothing and pends nothing" );
	}

	// Gemini: a calls-bearing turn with an ABSENT finishReason is refused
	// (documented policy: finishReason is nominally optional in the proto,
	// so text-only turns stay lenient -- but a call turn without an
	// explicit STOP cannot be distinguished from one cut short mid-list).
	{
		AgentChatLoop loop;
		loop.SetProvider( ChatProvider::Gemini );
		loop.AddUserMessage( "hi" );
		const std::string fx =
			"{\"candidates\":[{\"content\":{\"parts\":[{\"functionCall\":"
			"{\"id\":\"fc_nofin\",\"name\":\"render\",\"args\":{}}}],\"role\":\"model\"},\"index\":0}]}";
		ChatStepResult st = loop.HandleResponse( 200, fx );
		Check( st.kind == ChatStepResult::Kind::ProviderError,
		       "a gemini functionCall turn with NO finishReason is refused" );
		Check( st.errorMessage.find( "finishReason" ) != std::string::npos,
		       "the refusal names the missing finishReason" );
		Check( loop.TranscriptSize() == 1 && loop.PendingToolCalls().empty(),
		       "the finishReason-less call turn records nothing and pends nothing" );

		// Text-only leniency is PRESERVED: the same absent finishReason
		// with a plain text part is still a FinalText.
		ChatStepResult fin = loop.HandleResponse( 200,
			"{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"ok\"}],\"role\":\"model\"},\"index\":0}]}" );
		Check( fin.kind == ChatStepResult::Kind::FinalText,
		       "a TEXT-ONLY turn without finishReason stays accepted (documented leniency)" );
	}
}

//----------------------------------------------------------------------
// T11: hostile duplicate-key body -- the parsed view (JsonValue::find,
//      last-set wins) and the raw-span echo (RawObjectMember, last
//      match) must agree on WHICH duplicate they take.
//----------------------------------------------------------------------
static void TestDuplicateKeyBody()
{
	std::printf( "T11: duplicate-key body -- parsed view and echoed span agree...\n" );
	AgentChatLoop loop;
	loop.AddUserMessage( "hi" );
	const std::string body =
		"{\"id\":\"msg_dup\",\"type\":\"message\",\"role\":\"assistant\","
		"\"content\":[{\"type\":\"text\",\"text\":\"DECOY-FIRST-VALUE\"}],"
		"\"content\":[{\"type\":\"text\",\"text\":\"the real reply\"}],"
		"\"stop_reason\":\"end_turn\",\"stop_sequence\":null}";
	ChatStepResult st = loop.HandleResponse( 200, body );
	Check( st.kind == ChatStepResult::Kind::FinalText, "the duplicate-key body still parses" );
	Check( st.finalText == "the real reply",
	       "the PARSED view takes the LAST duplicate (JsonValue::find semantics)" );

	loop.AddUserMessage( "go on" );
	const std::string reqBody = loop.BuildRequest( kApiKey ).body;
	Check( reqBody.find( "the real reply" ) != std::string::npos,
	       "the ECHOED span is the last duplicate too" );
	Check( reqBody.find( "DECOY-FIRST-VALUE" ) == std::string::npos,
	       "the first-duplicate decoy is NOT echoed (scanner agrees with the parser)" );
}

//----------------------------------------------------------------------
// T12: empty-transcript BuildRequest refusal + Gemini model-id escaping.
//----------------------------------------------------------------------
static void TestRequestGuards()
{
	std::printf( "T12: empty-transcript refusal + gemini model-id escaping...\n" );

	{
		AgentChatLoop loop;
		const ChatHttpRequest req = loop.BuildRequest( kApiKey );
		Check( req.url.empty() && req.headers.empty() && req.body.empty(),
		       "BuildRequest on an empty transcript returns the documented EMPTY request" );
	}

	{
		AgentChatLoop loop;
		loop.SetProvider( ChatProvider::Gemini, "evil/../model?key=x&y=1" );
		loop.AddUserMessage( "hi" );
		const ChatHttpRequest req = loop.BuildRequest( kApiKey );
		Check( req.url == "https://generativelanguage.googleapis.com/v1beta/models/"
		                  "evil%2F..%2Fmodel%3Fkey%3Dx%26y%3D1:generateContent",
		       "model-id characters outside [A-Za-z0-9._-] are percent-escaped in the URL path" );
		Check( req.url.find( '?' ) == std::string::npos,
		       "no query separator survives the escaping" );
	}

	{
		AgentChatLoop loop;
		loop.SetProvider( ChatProvider::Gemini );
		loop.AddUserMessage( "hi" );
		Check( loop.BuildRequest( kApiKey ).url ==
		       "https://generativelanguage.googleapis.com/v1beta/models/gemini-3.5-flash:generateContent",
		       "a normal model id passes through unchanged" );
	}
}

//----------------------------------------------------------------------
// T13: hostile-disposition gates -- tool calls that could never become
//      the pending set REFUSE the whole response (P2-A).  Without the
//      gate, an end_turn body carrying tool_use blocks is recorded as
//      FinalText, its calls are discarded, and every later request
//      replays an unanswered tool_use -> permanent 400.
//----------------------------------------------------------------------
static void TestHostileDispositionGates()
{
	std::printf( "T13: hostile disposition gates (tool calls under a non-call stop)...\n" );

	// Anthropic: tool_use blocks + stop_reason end_turn.
	{
		AgentChatLoop loop;
		loop.AddUserMessage( "hi" );
		const std::string fx = AnthropicFixture(
			"[{\"type\":\"text\",\"text\":\"Sneaky.\"},"
			"{\"type\":\"tool_use\",\"id\":\"toolu_evil\",\"name\":\"render\",\"input\":{}}]",
			"end_turn" );
		ChatStepResult st = loop.HandleResponse( 200, fx );
		Check( st.kind == ChatStepResult::Kind::ProviderError,
		       "tool_use blocks under end_turn -> ProviderError (whole response refused)" );
		Check( st.errorKind == ChatErrorKind::Provider,
		       "the hostile-disposition refusal -> errorKind Provider" );
		Check( st.errorMessage.find( "end_turn" ) != std::string::npos,
		       "the refusal names the offending stop_reason" );
		Check( loop.TranscriptSize() == 1 && loop.PendingToolCalls().empty(),
		       "the hostile turn records NOTHING and pends nothing" );

		// The next request is CLEAN: no orphaned tool_use rides the wire.
		const ChatHttpRequest req = loop.BuildRequest( kApiKey );
		Check( req.body.find( "toolu_evil" ) == std::string::npos,
		       "the next BuildRequest carries NO trace of the refused tool_use" );
		JsonValue root = ParseBody( req.body );
		Check( root.get( "messages" ).size() == 1,
		       "the next request carries only the user message" );

		// And a normal turn still works afterwards.
		ChatStepResult fin = loop.HandleResponse( 200, AnthropicFixture(
			"[{\"type\":\"text\",\"text\":\"ok\"}]", "end_turn" ) );
		Check( fin.kind == ChatStepResult::Kind::FinalText,
		       "a normal turn works after the refusal (transcript not poisoned)" );
	}

	// Gemini: a functionCall-keyed part whose value is NOT an object
	// would skip call collection but still ride in the raw echo --
	// the whole turn must be refused instead.
	{
		AgentChatLoop loop;
		loop.SetProvider( ChatProvider::Gemini );
		loop.AddUserMessage( "hi" );
		const std::string fx = GeminiFixture(
			"{\"parts\":[{\"functionCall\":\"EVIL-NOT-AN-OBJECT\"},"
			"{\"functionCall\":{\"id\":\"fc_ok\",\"name\":\"render\",\"args\":{}}}],\"role\":\"model\"}",
			"STOP" );
		ChatStepResult st = loop.HandleResponse( 200, fx );
		Check( st.kind == ChatStepResult::Kind::ProviderError,
		       "a malformed (non-object) functionCall part -> ProviderError (whole turn refused)" );
		Check( st.errorMessage.find( "malformed functionCall" ) != std::string::npos,
		       "the refusal names the malformed functionCall" );
		Check( loop.TranscriptSize() == 1 && loop.PendingToolCalls().empty(),
		       "the malformed gemini turn records NOTHING and pends nothing" );
		Check( loop.BuildRequest( kApiKey ).body.find( "EVIL-NOT-AN-OBJECT" ) == std::string::npos,
		       "the next request carries NO trace of the malformed part" );
	}
}

//----------------------------------------------------------------------
// T14: image retention (P2-B) -- only the MOST RECENT read_image PNG
//      stays live; older ToolResults entries are rewritten with the
//      image elided.  Uses fabricated read_image envelopes with two
//      DISTINCT base64 payloads so old-vs-new occurrence counting is
//      unambiguous.
//----------------------------------------------------------------------
static std::size_t CountOccurrences( const std::string& hay, const std::string& needle )
{
	std::size_t n = 0;
	for( std::size_t pos = hay.find( needle ); pos != std::string::npos;
	     pos = hay.find( needle, pos + 1 ) ) ++n;
	return n;
}

static void TestImageElision()
{
	std::printf( "T14: older read_image PNGs are elided (most recent stays live)...\n" );

	// Two distinct fake PNG payloads (IsImageResult only requires a
	// non-empty png_base64 string; the distinctive bytes make the
	// occurrence counting airtight).
	const std::vector<unsigned char> bytesA( 48, 0xA5 );
	std::vector<unsigned char> bytesB( 48, 0x5A );
	const std::string b64A = Base64Encode( bytesA );
	const std::string b64B = Base64Encode( bytesB );
	const std::string envA = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"png_base64\":\"" + b64A +
	                         "\",\"byteLength\":48}}";
	const std::string envB = "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"png_base64\":\"" + b64B +
	                         "\",\"byteLength\":48}}";

	// --- Anthropic ---
	{
		AgentChatLoop loop;
		loop.AddUserMessage( "show me twice" );
		const char* ids[] = { "toolu_imgA", "toolu_imgB" };
		const std::string* envs[] = { &envA, &envB };
		for( int r = 0; r < 2; ++r ) {
			const std::string fx = AnthropicFixture(
				std::string( "[{\"type\":\"tool_use\",\"id\":\"" ) + ids[r] +
				"\",\"name\":\"read_image\",\"input\":{}}]", "tool_use" );
			ChatStepResult st = loop.HandleResponse( 200, fx );
			if( st.toolCalls.size() != 1 ) { Check( false, "one read_image call expected" ); return; }
			loop.AddToolResult( st.toolCalls[0], *envs[r] );
		}

		const std::string body = loop.BuildRequest( kApiKey ).body;
		Check( CountOccurrences( body, b64B ) == 1,
		       "anthropic: the NEW base64 rides exactly once" );
		Check( CountOccurrences( body, b64A ) == 0,
		       "anthropic: the OLD base64 rides ZERO times (elided)" );
		Check( body.find( "image elided" ) != std::string::npos,
		       "anthropic: the elision text rides where the old image was" );

		// The rewritten entry stays wire-valid: same tool_use_id, no image
		// block, the elision note as a text block.
		JsonValue root = ParseBody( body );
		const JsonValue& msgs = root.get( "messages" );
		Check( msgs.size() == 5, "anthropic: user + 2x(assistant + tool-results)" );
		const JsonValue& oldTr = msgs.at( 2 ).get( "content" ).at( 0 );
		Check( oldTr.get( "type" ).asString() == "tool_result" &&
		       oldTr.get( "tool_use_id" ).asString() == "toolu_imgA",
		       "anthropic: the rewritten entry keeps its matching tool_use_id" );
		bool oldHasImage = false, oldHasNote = false;
		const JsonValue& oldBlocks = oldTr.get( "content" );
		for( std::size_t i = 0; i < oldBlocks.size(); ++i ) {
			if( oldBlocks.at( i ).get( "type" ).asString() == "image" ) oldHasImage = true;
			if( oldBlocks.at( i ).get( "type" ).asString() == "text" &&
			    oldBlocks.at( i ).get( "text" ).asString().find( "image elided" ) != std::string::npos )
				oldHasNote = true;
		}
		Check( !oldHasImage, "anthropic: the old entry carries NO image block any more" );
		Check( oldHasNote, "anthropic: the old entry carries the elision text block" );
		const JsonValue& newTr = msgs.at( 4 ).get( "content" ).at( 0 );
		bool newHasImage = false;
		for( std::size_t i = 0; i < newTr.get( "content" ).size(); ++i )
			if( newTr.get( "content" ).at( i ).get( "type" ).asString() == "image" ) newHasImage = true;
		Check( newHasImage, "anthropic: the NEWEST entry still carries its live image block" );

		// The loop-written ATTACH note is rewritten alongside the image:
		// only the LIVE entry may still say "attached"; the elided entry's
		// summary note must read elided (no contradictory context).
		Check( CountOccurrences( body, "the PNG is attached as an image block" ) == 1,
		       "anthropic: the stale attach note is rewritten (only the live entry says attached)" );
		bool oldNoteElided = false;
		for( std::size_t i = 0; i < oldBlocks.size(); ++i ) {
			if( oldBlocks.at( i ).get( "type" ).asString() != "text" ) continue;
			JsonValue summary = ParseBody( oldBlocks.at( i ).get( "text" ).asString() );
			if( summary.isObject() &&
			    summary.get( "note" ).asString().find( "image elided" ) != std::string::npos )
				oldNoteElided = true;
		}
		Check( oldNoteElided,
		       "anthropic: the elided entry's summary note field reads elided" );
	}

	// --- Gemini (the functionResponse.parts inlineData transport) ---
	{
		AgentChatLoop loop;
		loop.SetProvider( ChatProvider::Gemini );
		loop.AddUserMessage( "show me twice" );
		const char* ids[] = { "fc_imgA", "fc_imgB" };
		const std::string* envs[] = { &envA, &envB };
		for( int r = 0; r < 2; ++r ) {
			const std::string fx = GeminiFixture(
				std::string( "{\"parts\":[{\"functionCall\":{\"id\":\"" ) + ids[r] +
				"\",\"name\":\"read_image\",\"args\":{}}}],\"role\":\"model\"}", "STOP" );
			ChatStepResult st = loop.HandleResponse( 200, fx );
			if( st.toolCalls.size() != 1 ) { Check( false, "one gemini read_image call expected" ); return; }
			loop.AddToolResult( st.toolCalls[0], *envs[r] );
		}

		const std::string body = loop.BuildRequest( kApiKey ).body;
		Check( CountOccurrences( body, b64B ) == 1,
		       "gemini: the NEW base64 rides exactly once" );
		Check( CountOccurrences( body, b64A ) == 0,
		       "gemini: the OLD base64 rides ZERO times (elided)" );
		Check( body.find( "image elided" ) != std::string::npos,
		       "gemini: the elision text rides where the old image was" );

		JsonValue root = ParseBody( body );
		const JsonValue& contents = root.get( "contents" );
		Check( contents.size() == 5, "gemini: user + 2x(model + results)" );
		const JsonValue* oldFr = contents.at( 2 ).get( "parts" ).at( 0 ).find( "functionResponse" );
		Check( oldFr != nullptr && oldFr->get( "id" ).asString() == "fc_imgA",
		       "gemini: the rewritten functionResponse keeps its matching id" );
		Check( oldFr != nullptr && !oldFr->has( "parts" ),
		       "gemini: the old functionResponse carries NO inlineData parts any more" );
		Check( oldFr != nullptr &&
		       JsonSerialize( oldFr->get( "response" ) ).find( "image elided" ) != std::string::npos,
		       "gemini: the old response object carries the elision note" );
		const JsonValue* newFr = contents.at( 4 ).get( "parts" ).at( 0 ).find( "functionResponse" );
		Check( newFr != nullptr && newFr->get( "parts" ).isArray() &&
		       newFr->get( "parts" ).size() == 1,
		       "gemini: the NEWEST functionResponse still carries its live inlineData part" );
	}

	// --- Two images in ONE flush (Anthropic): only the LAST keeps a live
	// image block; the earlier one is packed PRE-ELIDED at pack time. ---
	{
		AgentChatLoop loop;
		loop.AddUserMessage( "show me both" );
		const std::string fx = AnthropicFixture(
			"[{\"type\":\"tool_use\",\"id\":\"toolu_two1\",\"name\":\"read_image\",\"input\":{}},"
			"{\"type\":\"tool_use\",\"id\":\"toolu_two2\",\"name\":\"read_image\",\"input\":{}}]",
			"tool_use" );
		ChatStepResult st = loop.HandleResponse( 200, fx );
		if( st.toolCalls.size() != 2 ) { Check( false, "two parallel read_image calls expected" ); return; }
		loop.AddToolResult( st.toolCalls[0], envA );
		loop.AddToolResult( st.toolCalls[1], envB );

		const std::string body = loop.BuildRequest( kApiKey ).body;
		Check( CountOccurrences( body, b64B ) == 1,
		       "anthropic two-in-one-flush: the LAST base64 rides exactly once" );
		Check( CountOccurrences( body, b64A ) == 0,
		       "anthropic two-in-one-flush: the earlier base64 is pre-elided at pack time" );
		JsonValue root = ParseBody( body );
		JsonValue last = LastArrayEntry( root, "messages" );
		const JsonValue& packed = last.get( "content" );
		bool firstHasImage = false, secondHasImage = false, firstNoted = false;
		for( std::size_t i = 0; i < packed.size(); ++i ) {
			const JsonValue& tr = packed.at( i );
			const std::string id = tr.get( "tool_use_id" ).asString();
			const JsonValue& blocks = tr.get( "content" );
			for( std::size_t j = 0; j < blocks.size(); ++j ) {
				const JsonValue& b = blocks.at( j );
				if( b.get( "type" ).asString() == "image" ) {
					if( id == "toolu_two1" ) firstHasImage = true;
					if( id == "toolu_two2" ) secondHasImage = true;
				}
				if( id == "toolu_two1" && b.get( "type" ).asString() == "text" &&
				    b.get( "text" ).asString().find( "image elided" ) != std::string::npos )
					firstNoted = true;
			}
		}
		Check( !firstHasImage && secondHasImage,
		       "anthropic two-in-one-flush: only the LAST result carries an image block" );
		Check( firstNoted,
		       "anthropic two-in-one-flush: the earlier result carries the elision text" );
	}

	// --- Two images in ONE flush (Gemini): same rule on the inlineData
	// transport. ---
	{
		AgentChatLoop loop;
		loop.SetProvider( ChatProvider::Gemini );
		loop.AddUserMessage( "show me both" );
		const std::string fx = GeminiFixture(
			"{\"parts\":[{\"functionCall\":{\"id\":\"fc_two1\",\"name\":\"read_image\",\"args\":{}}},"
			"{\"functionCall\":{\"id\":\"fc_two2\",\"name\":\"read_image\",\"args\":{}}}],\"role\":\"model\"}",
			"STOP" );
		ChatStepResult st = loop.HandleResponse( 200, fx );
		if( st.toolCalls.size() != 2 ) { Check( false, "two parallel gemini read_image calls expected" ); return; }
		loop.AddToolResult( st.toolCalls[0], envA );
		loop.AddToolResult( st.toolCalls[1], envB );

		const std::string body = loop.BuildRequest( kApiKey ).body;
		Check( CountOccurrences( body, b64B ) == 1,
		       "gemini two-in-one-flush: the LAST base64 rides exactly once" );
		Check( CountOccurrences( body, b64A ) == 0,
		       "gemini two-in-one-flush: the earlier base64 is pre-elided at pack time" );
		JsonValue root = ParseBody( body );
		JsonValue last = LastArrayEntry( root, "contents" );
		const JsonValue& parts = last.get( "parts" );
		const JsonValue* fr1 = parts.at( 0 ).find( "functionResponse" );
		const JsonValue* fr2 = parts.at( 1 ).find( "functionResponse" );
		Check( fr1 != nullptr && !fr1->has( "parts" ) &&
		       JsonSerialize( fr1->get( "response" ) ).find( "image elided" ) != std::string::npos,
		       "gemini two-in-one-flush: the earlier functionResponse has no parts and reads elided" );
		Check( fr2 != nullptr && fr2->get( "parts" ).isArray() && fr2->get( "parts" ).size() == 1,
		       "gemini two-in-one-flush: only the LAST functionResponse carries the live inlineData part" );
	}

	// --- An RPC result that already OWNS a "note" field (Gemini): the
	// attach note then lives under "image_note", and the elision rewrite
	// must target image_note, never the RPC-owned note. ---
	{
		AgentChatLoop loop;
		loop.SetProvider( ChatProvider::Gemini );
		loop.AddUserMessage( "noted" );
		const std::string envNote =
			"{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"png_base64\":\"" + b64A +
			"\",\"byteLength\":48,\"note\":\"rpc-owned-note\"}}";
		const char* ids[] = { "fc_noteA", "fc_noteB" };
		const std::string* envs[] = { &envNote, &envB };
		for( int r = 0; r < 2; ++r ) {
			const std::string fx = GeminiFixture(
				std::string( "{\"parts\":[{\"functionCall\":{\"id\":\"" ) + ids[r] +
				"\",\"name\":\"read_image\",\"args\":{}}}],\"role\":\"model\"}", "STOP" );
			ChatStepResult st = loop.HandleResponse( 200, fx );
			if( st.toolCalls.size() != 1 ) { Check( false, "one gemini read_image call expected (note case)" ); return; }
			loop.AddToolResult( st.toolCalls[0], *envs[r] );
		}
		JsonValue root = ParseBody( loop.BuildRequest( kApiKey ).body );
		const JsonValue* oldFr = root.get( "contents" ).at( 2 ).get( "parts" ).at( 0 ).find( "functionResponse" );
		Check( oldFr != nullptr &&
		       oldFr->get( "response" ).get( "note" ).asString() == "rpc-owned-note",
		       "gemini: an RPC-owned note field is NOT clobbered by the elision" );
		Check( oldFr != nullptr &&
		       oldFr->get( "response" ).get( "image_note" ).asString().find( "image elided" ) != std::string::npos,
		       "gemini: the attach note (image_note) is rewritten to the elision text" );
	}
}

//----------------------------------------------------------------------
// T15: -ffast-math-safe non-finite guards in the module's dependencies
//      (P2-C).  These paths run inside library objects compiled with
//      the production CXXFLAGS (-ffast-math on this platform), so a
//      std::isnan/isinf/isfinite guard would be folded away.
//----------------------------------------------------------------------
static void TestFastMathGuards()
{
	std::printf( "T15: -ffast-math-safe guards (Json non-finite fallback + validation)...\n" );

	// (a) A provider fixture whose tool args carry 1e999 (parses to
	// +inf).  JsonSerialize must emit the documented fallback `0`, NOT a
	// literal `inf` token -- otherwise argsJson stops being JSON and the
	// whole args object (note included) is silently dropped when the
	// JSON-RPC line is built.
	{
		AgentChatLoop loop;
		loop.AddUserMessage( "hi" );
		const std::string fx = AnthropicFixture(
			"[{\"type\":\"tool_use\",\"id\":\"toolu_inf\",\"name\":\"render\","
			"\"input\":{\"samples\":1e999,\"note\":\"keep-me\"}}]", "tool_use" );
		ChatStepResult st = loop.HandleResponse( 200, fx );
		Check( st.kind == ChatStepResult::Kind::ToolCalls && st.toolCalls.size() == 1,
		       "the 1e999-args fixture parses to one ToolCall" );
		if( st.toolCalls.size() != 1 ) return;
		const std::string line = loop.ToolCallToJsonRpcLine( st.toolCalls[0], 40 );
		JsonValue lj = ParseBody( line );
		Check( lj.isObject(), "the JSON-RPC line is VALID JSON (no literal inf escaped)" );
		Check( lj.get( "params" ).get( "note" ).asString() == "keep-me",
		       "the non-numeric args survive (nothing was silently dropped)" );
		Check( lj.get( "params" ).get( "samples" ).isNumber() &&
		       lj.get( "params" ).get( "samples" ).asNumber() == 0.0,
		       "the non-finite number serialized as the documented fallback 0" );
	}

	// (b) AgentSession validation: a declared numeric parameter whose
	// value strtod's to +inf must classify as INVALID_VALUE.  With a
	// folded std::isfinite the classification silently degrades to the
	// generic DERIVE_ERROR.
	{
		const std::string candidate =
			"RISE ASCII SCENE 7\n"
			"sphere_geometry\n{\n\tname s\n\tradius inf\n}\n";
		const std::vector<AgentDiagnostic> diags = AgentSession::ValidateText( candidate );
		bool sawInvalid = false;
		for( std::size_t i = 0; i < diags.size(); ++i )
			if( diags[i].code == "INVALID_VALUE" &&
			    diags[i].severity == AgentDiagnostic::Severity::Error ) sawInvalid = true;
		Check( !diags.empty(), "an out-of-range numeric value produces diagnostics" );
		Check( sawInvalid,
		       "the non-finite numeric value classifies as INVALID_VALUE (range guard alive under -ffast-math)" );
	}
}

//----------------------------------------------------------------------
// T16: duplicate-call-id refusal on BOTH providers + Gemini
//      synthesized-id collision handling.  Ids are the result-matching
//      key: without the gate a duplicate id is recorded + pended once,
//      AddToolResult ignores the second call as already-answered, the
//      flush synthesizes a SECOND result for the SAME id, and the wire
//      carries duplicate ids answered by duplicate results (permanent
//      400).
//----------------------------------------------------------------------
static void TestDuplicateIdRefusal()
{
	std::printf( "T16: duplicate-id refusal (both providers) + synthesized-id collisions...\n" );

	// A provider id literally named "call_1" + a no-id call: the
	// synthesized id must NOT collide.
	{
		AgentChatLoop loop;
		loop.SetProvider( ChatProvider::Gemini );
		loop.AddUserMessage( "hi" );
		const std::string fx = GeminiFixture(
			"{\"parts\":[{\"functionCall\":{\"id\":\"call_1\",\"name\":\"read_document\",\"args\":{}}},"
			"{\"functionCall\":{\"name\":\"render\",\"args\":{}}}],\"role\":\"model\"}", "STOP" );
		ChatStepResult st = loop.HandleResponse( 200, fx );
		Check( st.kind == ChatStepResult::Kind::ToolCalls && st.toolCalls.size() == 2,
		       "provider call_1 + no-id call -> two ToolCalls" );
		if( st.toolCalls.size() != 2 ) return;
		Check( st.toolCalls[0].id == "call_1" && !st.toolCalls[0].idSynthesized,
		       "the provider id call_1 is captured verbatim" );
		Check( st.toolCalls[1].idSynthesized && st.toolCalls[1].id != "call_1" &&
		       !st.toolCalls[1].id.empty(),
		       "the synthesized id skips the collision with the provider's call_1" );
	}

	// Duplicate provider ids: result matching would be ambiguous ->
	// refuse the whole turn, record nothing.
	{
		AgentChatLoop loop;
		loop.SetProvider( ChatProvider::Gemini );
		loop.AddUserMessage( "hi" );
		const std::string fx = GeminiFixture(
			"{\"parts\":[{\"functionCall\":{\"id\":\"dup_1\",\"name\":\"read_document\",\"args\":{}}},"
			"{\"functionCall\":{\"id\":\"dup_1\",\"name\":\"render\",\"args\":{}}}],\"role\":\"model\"}", "STOP" );
		ChatStepResult st = loop.HandleResponse( 200, fx );
		Check( st.kind == ChatStepResult::Kind::ProviderError,
		       "duplicate provider functionCall ids -> ProviderError (whole turn refused)" );
		Check( st.errorMessage.find( "dup_1" ) != std::string::npos,
		       "the refusal names the repeated id" );
		Check( loop.TranscriptSize() == 1 && loop.PendingToolCalls().empty(),
		       "the duplicate-id turn records NOTHING and pends nothing" );
	}

	// A provider id colliding with an id synthesized EARLIER this turn
	// is the same ambiguity -> refused too.
	{
		AgentChatLoop loop;
		loop.SetProvider( ChatProvider::Gemini );
		loop.AddUserMessage( "hi" );
		const std::string fx = GeminiFixture(
			"{\"parts\":[{\"functionCall\":{\"name\":\"read_document\",\"args\":{}}},"
			"{\"functionCall\":{\"id\":\"call_0\",\"name\":\"render\",\"args\":{}}}],\"role\":\"model\"}", "STOP" );
		ChatStepResult st = loop.HandleResponse( 200, fx );
		Check( st.kind == ChatStepResult::Kind::ProviderError,
		       "a provider id colliding with an earlier synthesized id -> refused" );
		Check( loop.TranscriptSize() == 1, "the colliding turn records nothing" );
	}

	// Anthropic: two tool_use blocks sharing an id -- same ambiguity,
	// same gate (P1-1 of review round 3).  Without it, the flush would
	// synthesize a second result for the SAME id -> permanent 400.
	{
		AgentChatLoop loop;
		loop.AddUserMessage( "hi" );
		const std::string fx = AnthropicFixture(
			"[{\"type\":\"tool_use\",\"id\":\"toolu_dup\",\"name\":\"read_document\",\"input\":{}},"
			"{\"type\":\"tool_use\",\"id\":\"toolu_dup\",\"name\":\"render\",\"input\":{}}]",
			"tool_use" );
		ChatStepResult st = loop.HandleResponse( 200, fx );
		Check( st.kind == ChatStepResult::Kind::ProviderError,
		       "anthropic duplicate tool_use ids -> ProviderError (whole turn refused)" );
		Check( st.errorKind == ChatErrorKind::Provider,
		       "the anthropic duplicate-id refusal -> errorKind Provider" );
		Check( st.errorMessage.find( "toolu_dup" ) != std::string::npos,
		       "the refusal names the repeated tool_use id" );
		Check( loop.TranscriptSize() == 1 && loop.PendingToolCalls().empty(),
		       "the anthropic duplicate-id turn records NOTHING and pends nothing" );
		Check( loop.BuildRequest( kApiKey ).body.find( "toolu_dup" ) == std::string::npos,
		       "the next BuildRequest carries NO trace of the refused duplicate-id turn" );

		// A normal turn still works afterwards (transcript not poisoned).
		ChatStepResult fin = loop.HandleResponse( 200, AnthropicFixture(
			"[{\"type\":\"text\",\"text\":\"ok\"}]", "end_turn" ) );
		Check( fin.kind == ChatStepResult::Kind::FinalText,
		       "a normal turn works after the anthropic duplicate-id refusal" );
	}
}

//----------------------------------------------------------------------
// T17: Gemini adjacent role:user contents merge at BuildRequest (P3-2)
//      -- an interrupt flush followed by user text must reach the wire
//      as ONE user content (functionResponse parts first).
//----------------------------------------------------------------------
static void TestGeminiUserRunMerge()
{
	std::printf( "T17: gemini adjacent user contents merge on the wire...\n" );
	AgentChatLoop loop;
	loop.SetProvider( ChatProvider::Gemini );
	loop.AddUserMessage( "render it" );
	const std::string fx = GeminiFixture(
		"{\"parts\":[{\"functionCall\":{\"id\":\"fc_m1\",\"name\":\"render\",\"args\":{}}}],\"role\":\"model\"}",
		"STOP" );
	ChatStepResult st = loop.HandleResponse( 200, fx );
	Check( st.kind == ChatStepResult::Kind::ToolCalls, "one pending gemini call" );

	// Interrupt: NO result -- AddUserMessage flushes a synthesized error
	// result, then appends the user text (two adjacent user entries in
	// the transcript).
	loop.AddUserMessage( "never mind, stop" );
	Check( loop.TranscriptSize() == 4,
	       "transcript holds user + model + synthesized results + user" );

	const std::string body = loop.BuildRequest( kApiKey ).body;
	JsonValue root = ParseBody( body );
	const JsonValue& contents = root.get( "contents" );
	Check( contents.isArray() && contents.size() == 3,
	       "the WIRE carries three contents (results + text merged into one user content)" );
	// Roles must alternate: user, model, user.
	Check( contents.at( 0 ).get( "role" ).asString() == "user" &&
	       contents.at( 1 ).get( "role" ).asString() == "model" &&
	       contents.at( 2 ).get( "role" ).asString() == "user",
	       "the wire roles alternate user/model/user" );
	const JsonValue& merged = contents.at( 2 ).get( "parts" );
	Check( merged.isArray() && merged.size() == 2,
	       "the merged user content carries exactly two parts" );
	const JsonValue* fr = merged.at( 0 ).find( "functionResponse" );
	Check( fr != nullptr && fr->get( "id" ).asString() == "fc_m1",
	       "the functionResponse part rides FIRST and answers the pending call" );
	Check( merged.at( 1 ).get( "text" ).asString() == "never mind, stop",
	       "the user text part rides after the functionResponse" );
}

//----------------------------------------------------------------------
// T18: header-splice defense (P3-3) -- control characters are stripped
//      from the API key before it reaches a header value, on BOTH
//      codecs; multibyte model ids percent-escape per byte.
//----------------------------------------------------------------------
static void TestHeaderAndUrlSanitizing()
{
	std::printf( "T18: api-key header sanitizing + multibyte model-id escaping...\n" );

	const std::string evilKey = std::string( "sk-bad\r\nx-evil: 1\x01tail" );
	const std::string cleaned = "sk-badx-evil: 1tail";

	{
		AgentChatLoop loop;   // Anthropic
		loop.AddUserMessage( "hi" );
		const ChatHttpRequest req = loop.BuildRequest( evilKey );
		bool ok = false, anyCtl = false;
		for( std::size_t i = 0; i < req.headers.size(); ++i ) {
			if( req.headers[i].first == "x-api-key" && req.headers[i].second == cleaned ) ok = true;
			for( std::size_t j = 0; j < req.headers[i].second.size(); ++j )
				if( static_cast<unsigned char>( req.headers[i].second[j] ) < 0x20 ) anyCtl = true;
		}
		Check( ok, "anthropic: CR/LF/control chars are STRIPPED from the key header value" );
		Check( !anyCtl, "anthropic: no header value carries any control character" );
	}

	{
		AgentChatLoop loop;
		loop.SetProvider( ChatProvider::Gemini );
		loop.AddUserMessage( "hi" );
		const ChatHttpRequest req = loop.BuildRequest( evilKey );
		bool ok = false, anyCtl = false;
		for( std::size_t i = 0; i < req.headers.size(); ++i ) {
			if( req.headers[i].first == "x-goog-api-key" && req.headers[i].second == cleaned ) ok = true;
			for( std::size_t j = 0; j < req.headers[i].second.size(); ++j )
				if( static_cast<unsigned char>( req.headers[i].second[j] ) < 0x20 ) anyCtl = true;
		}
		Check( ok, "gemini: CR/LF/control chars are STRIPPED from the key header value" );
		Check( !anyCtl, "gemini: no header value carries any control character" );
	}

	// Multibyte UTF-8 model id: each byte percent-escapes (%C3%A9 = e-acute).
	{
		AgentChatLoop loop;
		loop.SetProvider( ChatProvider::Gemini, "gem\xC3\xA9-mini" );
		loop.AddUserMessage( "hi" );
		Check( loop.BuildRequest( kApiKey ).url ==
		       "https://generativelanguage.googleapis.com/v1beta/models/gem%C3%A9-mini:generateContent",
		       "a multibyte UTF-8 model id percent-escapes per byte (%C3%A9)" );
	}
}

//----------------------------------------------------------------------
// T19: degenerate empty-content final turns are refused (P3-4) --
//      recording them poisons the echo (Anthropic rejects an assistant
//      message with no content blocks; a partless Gemini model turn is
//      equally invalid on replay).
//----------------------------------------------------------------------
static void TestDegenerateEmptyTurns()
{
	std::printf( "T19: degenerate empty-content final turns are refused...\n" );

	// Anthropic: content [] + end_turn.
	{
		AgentChatLoop loop;
		loop.AddUserMessage( "hi" );
		ChatStepResult st = loop.HandleResponse( 200, AnthropicFixture( "[]", "end_turn" ) );
		Check( st.kind == ChatStepResult::Kind::ProviderError,
		       "anthropic empty content + end_turn -> ProviderError" );
		Check( st.errorMessage.find( "EMPTY content" ) != std::string::npos,
		       "the refusal names the empty content array" );
		Check( loop.TranscriptSize() == 1, "the degenerate anthropic turn records NOTHING" );
	}

	// Gemini: finishReason STOP with NO content at all.
	{
		AgentChatLoop loop;
		loop.SetProvider( ChatProvider::Gemini );
		loop.AddUserMessage( "hi" );
		ChatStepResult st = loop.HandleResponse( 200,
			"{\"candidates\":[{\"finishReason\":\"STOP\",\"index\":0}]}" );
		Check( st.kind == ChatStepResult::Kind::ProviderError,
		       "gemini STOP with missing content -> ProviderError" );
		Check( st.errorMessage.find( "no content parts" ) != std::string::npos,
		       "the refusal names the missing parts" );
		Check( loop.TranscriptSize() == 1, "the missing-content gemini turn records NOTHING" );
	}

	// Gemini: finishReason STOP with an EMPTY parts array.
	{
		AgentChatLoop loop;
		loop.SetProvider( ChatProvider::Gemini );
		loop.AddUserMessage( "hi" );
		ChatStepResult st = loop.HandleResponse( 200, GeminiFixture(
			"{\"parts\":[],\"role\":\"model\"}", "STOP" ) );
		Check( st.kind == ChatStepResult::Kind::ProviderError,
		       "gemini STOP with empty parts -> ProviderError" );
		Check( loop.TranscriptSize() == 1, "the empty-parts gemini turn records NOTHING" );
	}
}

//----------------------------------------------------------------------
// T20: Gemini role-spoof gate (P1-2 of review round 3) -- a candidate
//      whose content.role is present and != "model" is refused OUTRIGHT.
//      Without the gate the spoofed content is echoed verbatim as the
//      assistant entry and BuildRequest's adjacent-user merge (which
//      classifies by parsing role from rawJson) folds it into the user
//      run: the conversation collapses into ONE user content with a
//      functionCall inside it -- wire-invalid, permanent poison.
//----------------------------------------------------------------------
static void TestGeminiRoleSpoofGate()
{
	std::printf( "T20: gemini role-spoofed model turns are refused...\n" );

	// A calls-bearing candidate spoofing role:"user".
	{
		AgentChatLoop loop;
		loop.SetProvider( ChatProvider::Gemini );
		loop.AddUserMessage( "hi" );
		const std::string fx = GeminiFixture(
			"{\"parts\":[{\"functionCall\":{\"id\":\"fc_spoof\",\"name\":\"render\",\"args\":{}}}],\"role\":\"user\"}",
			"STOP" );
		ChatStepResult st = loop.HandleResponse( 200, fx );
		Check( st.kind == ChatStepResult::Kind::ProviderError,
		       "content.role \"user\" on a model candidate -> ProviderError (whole turn refused)" );
		Check( st.errorKind == ChatErrorKind::Provider,
		       "the role-spoof refusal -> errorKind Provider" );
		Check( st.errorMessage.find( "\"user\"" ) != std::string::npos,
		       "the refusal names the spoofed role" );
		Check( loop.TranscriptSize() == 1 && loop.PendingToolCalls().empty(),
		       "the spoofed turn records NOTHING and pends nothing" );

		// The user merge is untouched: the next request carries the lone
		// user message and no functionCall ever joins a user content.
		const std::string body = loop.BuildRequest( kApiKey ).body;
		Check( body.find( "fc_spoof" ) == std::string::npos,
		       "the next request carries NO trace of the spoofed turn" );
		JsonValue root = ParseBody( body );
		Check( root.get( "contents" ).size() == 1 &&
		       root.get( "contents" ).at( 0 ).get( "role" ).asString() == "user",
		       "the next request carries only the user message (merge untouched)" );

		// A genuine model turn still works afterwards.
		ChatStepResult fin = loop.HandleResponse( 200, GeminiFixture(
			"{\"parts\":[{\"text\":\"ok\"}],\"role\":\"model\"}", "STOP" ) );
		Check( fin.kind == ChatStepResult::Kind::FinalText,
		       "a normal model turn works after the refusal (transcript not poisoned)" );
	}

	// A text-only spoofed turn is refused too (the echo would still join
	// the user merge on replay), and a NON-STRING role is equally hostile.
	{
		AgentChatLoop loop;
		loop.SetProvider( ChatProvider::Gemini );
		loop.AddUserMessage( "hi" );
		ChatStepResult st = loop.HandleResponse( 200, GeminiFixture(
			"{\"parts\":[{\"text\":\"sneaky\"}],\"role\":\"user\"}", "STOP" ) );
		Check( st.kind == ChatStepResult::Kind::ProviderError,
		       "a role-spoofed TEXT-ONLY turn is refused too" );
		Check( loop.TranscriptSize() == 1, "the text-only spoof records nothing" );

		ChatStepResult st2 = loop.HandleResponse( 200, GeminiFixture(
			"{\"parts\":[{\"text\":\"sneaky\"}],\"role\":123}", "STOP" ) );
		Check( st2.kind == ChatStepResult::Kind::ProviderError,
		       "a NON-STRING content.role is refused as well" );
	}
}

//----------------------------------------------------------------------
// T21: loop-contract details -- empty AddUserMessage no-op, direct
//      Reset() semantics, TranscriptAt bounds behaviour.
//----------------------------------------------------------------------
static void TestLoopContractDetails()
{
	std::printf( "T21: empty-message no-op, Reset(), TranscriptAt bounds...\n" );

	// (a) Empty / whitespace-only AddUserMessage is a documented NO-OP
	// (Anthropic hard-400s an empty text block).
	{
		AgentChatLoop loop;
		loop.AddUserMessage( "" );
		loop.AddUserMessage( " \t\r\n " );
		Check( loop.TranscriptSize() == 0,
		       "empty / whitespace-only user text records NOTHING" );
		Check( loop.BuildRequest( kApiKey ).url.empty(),
		       "the transcript stays empty -> BuildRequest still refuses" );

		// With calls pending, the no-op does NOT flush them either: the
		// caller sent nothing, so the turn state stays exactly as it was.
		loop.AddUserMessage( "hi" );
		const std::string fx = AnthropicFixture(
			"[{\"type\":\"tool_use\",\"id\":\"toolu_noop\",\"name\":\"render\",\"input\":{}}]",
			"tool_use" );
		ChatStepResult st = loop.HandleResponse( 200, fx );
		Check( st.kind == ChatStepResult::Kind::ToolCalls, "one call pending before the no-op" );
		const std::size_t size = loop.TranscriptSize();
		loop.AddUserMessage( "   " );
		Check( loop.TranscriptSize() == size && loop.PendingToolCalls().size() == 1,
		       "a whitespace-only message flushes NOTHING (turn state untouched)" );
	}

	// (b) Direct Reset(): transcript + pending calls/results + round
	// counter cleared; the provider/model selection is KEPT.
	{
		AgentChatLoop loop;
		loop.SetProvider( ChatProvider::Anthropic, "claude-sonnet-4-6" );
		loop.AddUserMessage( "hi" );
		const std::string fx = AnthropicFixture(
			"[{\"type\":\"tool_use\",\"id\":\"toolu_rstA\",\"name\":\"read_document\",\"input\":{}},"
			"{\"type\":\"tool_use\",\"id\":\"toolu_rstB\",\"name\":\"render\",\"input\":{}}]",
			"tool_use" );
		ChatStepResult st = loop.HandleResponse( 200, fx );
		Check( st.kind == ChatStepResult::Kind::ToolCalls && st.toolCalls.size() == 2,
		       "transcript + two pending calls before Reset" );
		if( st.toolCalls.size() == 2 )   // buffer ONE result so mPendingResults is non-empty too
			loop.AddToolResult( st.toolCalls[0], "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}" );
		Check( loop.ToolRoundCount() == 1, "round counter is non-zero before Reset" );

		loop.Reset();
		Check( loop.TranscriptSize() == 0, "Reset clears the transcript" );
		Check( loop.PendingToolCalls().empty(), "Reset clears the pending calls" );
		Check( loop.ToolRoundCount() == 0, "Reset clears the round counter" );
		Check( loop.Provider() == ChatProvider::Anthropic && loop.ModelId() == "claude-sonnet-4-6",
		       "Reset KEEPS the provider/model selection" );
		Check( loop.BuildRequest( kApiKey ).url.empty(),
		       "after Reset, BuildRequest refuses (no stray flush of dropped results)" );

		loop.AddUserMessage( "again" );
		Check( loop.TranscriptSize() == 1, "the loop is fully usable again after Reset" );
	}

	// (c) TranscriptAt out of range returns the documented safe static
	// empty entry (never throws) -- mirrors JsonValue::at.
	{
		AgentChatLoop loop;
		const ChatTranscriptEntry& e = loop.TranscriptAt( 12345 );
		Check( e.displayText.empty() && e.rawJson.empty() &&
		       e.role == ChatTranscriptEntry::Role::User && !e.carriesLiveImage,
		       "TranscriptAt out of range returns the safe empty entry" );
		loop.AddUserMessage( "one" );
		Check( loop.TranscriptAt( 0 ).displayText == "one" &&
		       loop.TranscriptAt( 1 ).rawJson.empty(),
		       "in-range access works; one-past-the-end is the safe empty entry" );
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
	TestFlushSynthesis( rpc );
	TestStopReasonDeadEnds();
	TestDuplicateKeyBody();
	TestRequestGuards();
	TestHostileDispositionGates();
	TestImageElision();
	TestFastMathGuards();
	TestDuplicateIdRefusal();
	TestGeminiUserRunMerge();
	TestHeaderAndUrlSanitizing();
	TestDegenerateEmptyTurns();
	TestGeminiRoleSpoofGate();
	TestLoopContractDetails();

	std::remove( scenePath.c_str() );
	std::printf( "=== AgentChatLoopTest: %d passed, %d failed ===\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
