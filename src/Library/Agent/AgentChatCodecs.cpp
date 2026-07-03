//////////////////////////////////////////////////////////////////////
//
//  AgentChatCodecs.cpp - provider wire-format codecs for the sans-IO
//    LLM chat loop (see AgentChatCodecs.h).
//
//  Layout:
//    (1) the SIX provider-neutral tool definitions (1:1 with the
//        AgentRpc verbs; parameter names/shapes mirror AgentRpc.cpp),
//    (2) a small raw-span JSON scanner (byte-exact extraction of the
//        assistant content from a response body, so provider-opaque
//        fields -- thinking-block signatures -- echo back VERBATIM),
//    (3) the Anthropic Messages API codec,
//    (4) the Gemini v1beta generateContent codec.
//
//  NO LOGGING anywhere in this file: request/response bodies may embed
//  scene content, and the API key must never reach a log.
//
//////////////////////////////////////////////////////////////////////

#include "AgentChatCodecs.h"

#include "Json.h"

#include <cstring>
#include <string>

namespace RISE
{
	namespace Agent
	{
		namespace
		{
			//------------------------------------------------------------------
			// (1) The provider-neutral tool definitions.
			//
			// One entry per JSON-RPC verb.  `schemaJson` is a JSON-Schema
			// object literal (the subset both providers accept); nullptr
			// means "no parameters" (Anthropic then gets an empty object
			// schema -- input_schema is mandatory there -- while Gemini
			// omits `parameters` entirely).  Descriptions are PRESCRIPTIVE:
			// they say WHEN to call, not just what the verb does.
			//------------------------------------------------------------------
			struct NeutralToolDef
			{
				const char* name;
				const char* description;
				const char* schemaJson;   // nullptr = no parameters
			};

			const NeutralToolDef kToolDefs[] =
			{
				{
					"read_document",
					"Read the current scene document. Call this FIRST, before any edit, "
					"to see the live .RISEscene text and the current headVersion "
					"{uuid,revision}. Always pass the headVersion you last read here as "
					"propose_patch's baseHeadVersion. Re-call after a conflict to rebase.",
					nullptr
				},
				{
					"read_schema",
					"Read the scene-language schema (chunk and parameter reference). Call "
					"with a chunk keyword to learn one chunk's parameters, or with no "
					"keyword for the whole grammar. Use this before proposing a patch "
					"whose parameter name or value format you are not sure about.",
					"{\"type\":\"object\",\"properties\":{"
						"\"keyword\":{\"type\":\"string\",\"description\":"
						"\"A single chunk keyword (e.g. sphere_geometry) to fetch just that chunk's schema; omit for the whole grammar.\"}"
					"}}"
				},
				{
					"validate",
					"Validate a CANDIDATE scene document without touching the live scene. "
					"Call this to check a document you are considering BEFORE proposing "
					"changes; an empty diagnostics list means the candidate is valid.",
					"{\"type\":\"object\",\"properties\":{"
						"\"text\":{\"type\":\"string\",\"description\":"
						"\"The complete candidate .RISEscene document text to check.\"}"
					"},\"required\":[\"text\"]}"
				},
				{
					"propose_patch",
					"Set one parameter of one named scene entity (the ONLY way to edit "
					"the live scene). Always pass the headVersion you last read as "
					"baseHeadVersion. If the result has status=conflict, re-call "
					"read_document and re-propose against the new headVersion. If "
					"retriable=true the refusal is transient -- retry the SAME patch "
					"after a moment. Only status=applied means the edit landed cleanly.",
					"{\"type\":\"object\",\"properties\":{"
						"\"target\":{\"type\":\"string\",\"description\":"
						"\"The entity NAME to edit (a chunk name from the document).\"},"
						"\"kind\":{\"type\":\"string\",\"description\":"
						"\"Optional entity KIND keyword (e.g. lambertian_material) to disambiguate a name clash.\"},"
						"\"param\":{\"type\":\"string\",\"description\":"
						"\"The parameter to set (e.g. radius, color, location).\"},"
						"\"value\":{\"type\":\"string\",\"description\":"
						"\"The new value as scene-language text (e.g. 0.9 0.1 0.1).\"},"
						"\"baseHeadVersion\":{\"type\":\"object\",\"description\":"
						"\"The headVersion from your last read_document -- pass it EVERY time so a stale edit is rejected as a conflict instead of clobbering.\","
						"\"properties\":{\"uuid\":{\"type\":\"number\"},\"revision\":{\"type\":\"number\"}},"
						"\"required\":[\"uuid\",\"revision\"]}"
					"},\"required\":[\"target\",\"param\",\"value\"]}"
				},
				{
					"render",
					"Re-render the live scene headlessly and return lean statistics "
					"(dimensions + linear per-channel means -- a stable image signature). "
					"Call after a successful propose_patch; compare the channel means "
					"against the previous render to confirm the edit changed the image.",
					"{\"type\":\"object\",\"properties\":{"
						"\"samples\":{\"type\":\"number\",\"description\":"
						"\"Optional sample-count override (currently advisory; the authored count is used).\"}"
					"}}"
				},
				{
					"read_image",
					"Fetch the LAST successful render as a PNG image so you can SEE the "
					"scene. Call after propose_patch + render to visually verify your "
					"edit did what you intended. Requires a prior successful render.",
					nullptr
				},
			};

			const std::size_t kToolDefCount = sizeof( kToolDefs ) / sizeof( kToolDefs[0] );

			//! Anthropic-native tools array: [{name,description,input_schema},...].
			const std::string& AnthropicToolsJson()
			{
				static const std::string json = []() {
					std::string out = "[";
					for( std::size_t i = 0; i < kToolDefCount; ++i ) {
						if( i ) out += ",";
						out += "{\"name\":";
						JsonAppendEscapedString( out, kToolDefs[i].name );
						out += ",\"description\":";
						JsonAppendEscapedString( out, kToolDefs[i].description );
						out += ",\"input_schema\":";
						out += kToolDefs[i].schemaJson ? kToolDefs[i].schemaJson
						                               : "{\"type\":\"object\",\"properties\":{}}";
						out += "}";
					}
					out += "]";
					return out;
				}();
				return json;
			}

			//! Gemini-native function declarations: [{name,description[,parameters]},...].
			const std::string& GeminiFunctionDeclarationsJson()
			{
				static const std::string json = []() {
					std::string out = "[";
					for( std::size_t i = 0; i < kToolDefCount; ++i ) {
						if( i ) out += ",";
						out += "{\"name\":";
						JsonAppendEscapedString( out, kToolDefs[i].name );
						out += ",\"description\":";
						JsonAppendEscapedString( out, kToolDefs[i].description );
						if( kToolDefs[i].schemaJson ) {
							out += ",\"parameters\":";
							out += kToolDefs[i].schemaJson;
						}
						out += "}";
					}
					out += "]";
					return out;
				}();
				return json;
			}

			//------------------------------------------------------------------
			// (2) Raw-span JSON scanner.
			//
			// The assistant content must be echoed back BYTE-PRESERVED on
			// later requests (thinking-block signatures are opaque and
			// must round-trip unmodified), so we extract it as a raw byte
			// span of the response body rather than parse + re-serialize.
			// The keys we navigate by ("content", "candidates") never
			// contain escapes, so a raw key comparison is exact.
			//------------------------------------------------------------------

			const std::size_t kNpos = std::string::npos;

			std::size_t SkipWs( const std::string& s, std::size_t i )
			{
				while( i < s.size() &&
				       ( s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r' ) ) ++i;
				return i;
			}

			//! `i` at the opening quote; returns one past the closing quote.
			std::size_t ScanRawString( const std::string& s, std::size_t i )
			{
				++i;
				while( i < s.size() ) {
					const char c = s[i];
					if( c == '\\' ) { i += 2; continue; }
					if( c == '"' ) return i + 1;
					++i;
				}
				return kNpos;
			}

			//! `i` at (or before, in whitespace) the first char of a JSON
			//! value; returns one past its end.  Balanced-bracket walk that
			//! skips strings; kNpos on malformation.
			std::size_t ScanRawValue( const std::string& s, std::size_t i )
			{
				i = SkipWs( s, i );
				if( i >= s.size() ) return kNpos;
				const char c = s[i];
				if( c == '"' ) return ScanRawString( s, i );
				if( c == '{' || c == '[' ) {
					int depth = 0;
					while( i < s.size() ) {
						const char d = s[i];
						if( d == '"' ) {
							i = ScanRawString( s, i );
							if( i == kNpos ) return kNpos;
							continue;
						}
						if( d == '{' || d == '[' ) ++depth;
						else if( d == '}' || d == ']' ) {
							--depth;
							if( depth == 0 ) return i + 1;
						}
						++i;
					}
					return kNpos;
				}
				// number / true / false / null: scan to a structural delimiter
				std::size_t j = i;
				while( j < s.size() && s[j] != ',' && s[j] != '}' && s[j] != ']' &&
				       s[j] != ' ' && s[j] != '\t' && s[j] != '\n' && s[j] != '\r' ) ++j;
				return ( j > i ) ? j : kNpos;
			}

			//! Find member `key` of the object starting at `objBegin`
			//! (whitespace allowed before the '{') and return the raw span
			//! of its value.  FIRST match wins (responses do not repeat keys).
			bool RawObjectMember( const std::string& s, std::size_t objBegin, const char* key,
			                      std::size_t& valBegin, std::size_t& valEnd )
			{
				std::size_t i = SkipWs( s, objBegin );
				if( i >= s.size() || s[i] != '{' ) return false;
				++i;
				const std::size_t keyLen = std::strlen( key );
				while( true ) {
					i = SkipWs( s, i );
					if( i >= s.size() || s[i] == '}' ) return false;
					if( s[i] != '"' ) return false;
					const std::size_t keyBegin = i + 1;
					const std::size_t afterKey = ScanRawString( s, i );
					if( afterKey == kNpos ) return false;
					const std::size_t rawKeyLen = ( afterKey - 1 ) - keyBegin;
					const bool match = ( rawKeyLen == keyLen ) &&
					                   ( s.compare( keyBegin, keyLen, key ) == 0 );
					i = SkipWs( s, afterKey );
					if( i >= s.size() || s[i] != ':' ) return false;
					++i;
					i = SkipWs( s, i );
					const std::size_t vEnd = ScanRawValue( s, i );
					if( vEnd == kNpos ) return false;
					if( match ) { valBegin = i; valEnd = vEnd; return true; }
					i = SkipWs( s, vEnd );
					if( i < s.size() && s[i] == ',' ) { ++i; continue; }
					return false;
				}
			}

			//! Raw span of element `index` of the array starting at `arrBegin`.
			bool RawArrayElement( const std::string& s, std::size_t arrBegin, std::size_t index,
			                      std::size_t& valBegin, std::size_t& valEnd )
			{
				std::size_t i = SkipWs( s, arrBegin );
				if( i >= s.size() || s[i] != '[' ) return false;
				++i;
				std::size_t n = 0;
				while( true ) {
					i = SkipWs( s, i );
					if( i >= s.size() || s[i] == ']' ) return false;
					const std::size_t vEnd = ScanRawValue( s, i );
					if( vEnd == kNpos ) return false;
					if( n == index ) { valBegin = i; valEnd = vEnd; return true; }
					++n;
					i = SkipWs( s, vEnd );
					if( i < s.size() && s[i] == ',' ) { ++i; continue; }
					return false;
				}
			}

			//------------------------------------------------------------------
			// Shared response/result helpers.
			//------------------------------------------------------------------

			ChatStepResult MakeProviderError( const std::string& message )
			{
				ChatStepResult r;
				r.kind = ChatStepResult::Kind::ProviderError;
				r.errorMessage = message;
				return r;
			}

			//! A ProviderError for a non-200 HTTP status, carrying the
			//! provider's error.message when the body parses (both providers
			//! use the {"error":{"message":...}} shape).  The raw body is
			//! deliberately NOT included (it may be large / arbitrary).
			ChatStepResult MakeHttpError( const char* provider, long status, const std::string& body )
			{
				std::string msg;
				JsonValue root;
				std::string perr;
				if( JsonParse( body, root, perr ) && root.isObject() ) {
					const JsonValue& m = root.get( "error" ).get( "message" );
					if( m.isString() ) msg = m.asString();
				}
				std::string full = std::string( provider ) + " HTTP " + std::to_string( status );
				if( !msg.empty() ) full += ": " + msg;
				else full += " (no parseable error message)";
				return MakeProviderError( full );
			}

			JsonValue MakeTextBlock( const std::string& text )
			{
				JsonValue b = JsonValue::MakeObject();
				b.set( "type", JsonValue::MakeString( "text" ) );
				b.set( "text", JsonValue::MakeString( text ) );
				return b;
			}

			//! For a read_image result: `result` minus the png_base64 field,
			//! plus a note that the image travels as a real image block/part.
			//! Stripping the base64 from the textual half keeps it from being
			//! double-sent.
			JsonValue StripPngBase64( const JsonValue& result, const char* note )
			{
				JsonValue summary = JsonValue::MakeObject();
				const std::vector<std::pair<std::string, JsonValue>>& members = result.members();
				for( std::size_t i = 0; i < members.size(); ++i ) {
					if( members[i].first == "png_base64" ) continue;
					summary.set( members[i].first, members[i].second );
				}
				summary.set( "note", JsonValue::MakeString( note ) );
				return summary;
			}

			//! True + the base64 payload iff this call is a read_image whose
			//! JSON-RPC result carries a non-empty png_base64 string.
			bool IsImageResult( const ChatToolCall& call, const JsonValue& result, std::string& outB64 )
			{
				if( call.name != "read_image" || !result.isObject() ) return false;
				const JsonValue* b64 = result.find( "png_base64" );
				if( !b64 || !b64->isString() || b64->asString().empty() ) return false;
				outB64 = b64->asString();
				return true;
			}
		}

		//======================================================================
		// (3) AnthropicChatCodec
		//======================================================================

		const char* AnthropicChatCodec::ProviderName() const { return "anthropic"; }

		const char* AnthropicChatCodec::DefaultModelId() const { return "claude-sonnet-5"; }

		std::string AnthropicChatCodec::MakeUserEntry( const std::string& text ) const
		{
			JsonValue msg = JsonValue::MakeObject();
			msg.set( "role", JsonValue::MakeString( "user" ) );
			JsonValue content = JsonValue::MakeArray();
			content.push_back( MakeTextBlock( text ) );
			msg.set( "content", content );
			return JsonSerialize( msg );
		}

		std::string AnthropicChatCodec::PackToolResults(
			const std::vector<std::pair<ChatToolCall, std::string>>& results ) const
		{
			// ONE user message carrying one tool_result block per tool_use --
			// Anthropic requires every tool_use of the assistant turn to be
			// answered in the SAME following user message.
			JsonValue contentArr = JsonValue::MakeArray();
			for( std::size_t i = 0; i < results.size(); ++i ) {
				const ChatToolCall& call = results[i].first;
				JsonValue env;
				std::string perr;
				const bool parsed = JsonParse( results[i].second, env, perr ) && env.isObject();

				bool isError = false;
				JsonValue blocks = JsonValue::MakeArray();
				if( !parsed ) {
					isError = true;
					blocks.push_back( MakeTextBlock( "tool transport error: the JSON-RPC response line did not parse as JSON" ) );
				}
				else if( const JsonValue* e = env.find( "error" ) ) {
					// JSON-RPC error envelope -> error tool result.
					isError = true;
					blocks.push_back( MakeTextBlock( JsonSerialize( *e ) ) );
				}
				else {
					const JsonValue& result = env.get( "result" );
					std::string b64;
					if( IsImageResult( call, result, b64 ) ) {
						blocks.push_back( MakeTextBlock( JsonSerialize(
							StripPngBase64( result, "the PNG is attached as an image block" ) ) ) );
						JsonValue source = JsonValue::MakeObject();
						source.set( "type", JsonValue::MakeString( "base64" ) );
						source.set( "media_type", JsonValue::MakeString( "image/png" ) );
						source.set( "data", JsonValue::MakeString( b64 ) );
						JsonValue img = JsonValue::MakeObject();
						img.set( "type", JsonValue::MakeString( "image" ) );
						img.set( "source", source );
						blocks.push_back( img );
					}
					else {
						blocks.push_back( MakeTextBlock( JsonSerialize( result ) ) );
					}
				}

				JsonValue tr = JsonValue::MakeObject();
				tr.set( "type", JsonValue::MakeString( "tool_result" ) );
				tr.set( "tool_use_id", JsonValue::MakeString( call.id ) );
				tr.set( "content", blocks );
				if( isError ) tr.set( "is_error", JsonValue::MakeBool( true ) );
				contentArr.push_back( tr );
			}

			JsonValue msg = JsonValue::MakeObject();
			msg.set( "role", JsonValue::MakeString( "user" ) );
			msg.set( "content", contentArr );
			return JsonSerialize( msg );
		}

		ChatHttpRequest AnthropicChatCodec::BuildRequest(
			const std::string& modelId,
			const std::string& apiKey,
			const std::string& systemPrompt,
			const std::vector<std::string>& rawEntries ) const
		{
			ChatHttpRequest r;
			r.url = "https://api.anthropic.com/v1/messages";
			// The key appears ONLY here, in the auth header.
			r.headers.push_back( std::make_pair( "content-type", "application/json" ) );
			r.headers.push_back( std::make_pair( "x-api-key", apiKey ) );
			r.headers.push_back( std::make_pair( "anthropic-version", "2023-06-01" ) );

			// The body is assembled as a string so assistant entries (raw
			// provider-native JSON) splice in VERBATIM.  No thinking config
			// is set (omitted = adaptive on models that support it).
			std::string body = "{\"model\":";
			JsonAppendEscapedString( body, modelId );
			body += ",\"max_tokens\":8192,\"system\":";
			JsonAppendEscapedString( body, systemPrompt );
			body += ",\"tools\":";
			body += AnthropicToolsJson();
			body += ",\"messages\":[";
			for( std::size_t i = 0; i < rawEntries.size(); ++i ) {
				if( i ) body += ",";
				body += rawEntries[i];
			}
			body += "]}";
			r.body = body;
			return r;
		}

		ChatParsedResponse AnthropicChatCodec::ParseResponse(
			long httpStatus, const std::string& rawBody ) const
		{
			ChatParsedResponse out;
			if( httpStatus != 200 ) {
				out.step = MakeHttpError( "anthropic", httpStatus, rawBody );
				return out;
			}

			JsonValue root;
			std::string perr;
			if( !JsonParse( rawBody, root, perr ) || !root.isObject() ) {
				out.step = MakeProviderError( "anthropic response did not parse as JSON: " + perr );
				return out;
			}
			const JsonValue& content = root.get( "content" );
			if( !content.isArray() ) {
				out.step = MakeProviderError( "anthropic response carries no content array" );
				return out;
			}

			std::string text;
			std::vector<ChatToolCall> calls;
			for( std::size_t i = 0; i < content.size(); ++i ) {
				const JsonValue& block = content.at( i );
				const std::string type = block.get( "type" ).asString();
				if( type == "text" ) {
					text += block.get( "text" ).asString();
				}
				else if( type == "tool_use" ) {
					ChatToolCall c;
					c.id = block.get( "id" ).asString();
					c.name = block.get( "name" ).asString();
					const JsonValue& input = block.get( "input" );
					c.argsJson = input.isObject() ? JsonSerialize( input ) : std::string( "{}" );
					calls.push_back( c );
				}
				// thinking / other block kinds: not displayed; the raw echo
				// below preserves them for the provider.
			}
			out.assistantDisplayText = text;

			const std::string stopReason = root.get( "stop_reason" ).asString();
			if( stopReason == "tool_use" && !calls.empty() ) {
				out.step.kind = ChatStepResult::Kind::ToolCalls;
				out.step.toolCalls = calls;
			}
			else if( stopReason == "end_turn" ) {
				out.step.kind = ChatStepResult::Kind::FinalText;
				out.step.finalText = text;
			}
			else {
				// refusal / max_tokens / anything unexpected -> error with
				// the stop_reason in the message.
				std::string msg = "anthropic stopped with stop_reason \"" + stopReason + "\"";
				if( stopReason == "tool_use" ) msg += " but no tool_use blocks were present";
				out.step = MakeProviderError( msg );
				return out;
			}

			// The assistant transcript entry: the content array as a RAW
			// byte span of the body (verbatim echo -- signatures intact).
			std::size_t b = 0, e = 0;
			if( RawObjectMember( rawBody, 0, "content", b, e ) ) {
				out.assistantEntryJson = "{\"role\":\"assistant\",\"content\":" +
				                         rawBody.substr( b, e - b ) + "}";
			}
			else {
				// Defensive fallback (should not happen for a body that
				// parsed above): re-serialize; loses byte-fidelity only.
				out.assistantEntryJson = "{\"role\":\"assistant\",\"content\":" +
				                         JsonSerialize( content ) + "}";
			}
			return out;
		}

		//======================================================================
		// (4) GeminiChatCodec
		//======================================================================

		const char* GeminiChatCodec::ProviderName() const { return "gemini"; }

		const char* GeminiChatCodec::DefaultModelId() const { return "gemini-3.5-flash"; }

		std::string GeminiChatCodec::MakeUserEntry( const std::string& text ) const
		{
			JsonValue part = JsonValue::MakeObject();
			part.set( "text", JsonValue::MakeString( text ) );
			JsonValue parts = JsonValue::MakeArray();
			parts.push_back( part );
			JsonValue msg = JsonValue::MakeObject();
			msg.set( "role", JsonValue::MakeString( "user" ) );
			msg.set( "parts", parts );
			return JsonSerialize( msg );
		}

		std::string GeminiChatCodec::PackToolResults(
			const std::vector<std::pair<ChatToolCall, std::string>>& results ) const
		{
			// ONE user turn: one functionResponse part per call (matched by
			// name + order -- Gemini calls carry no id), and for read_image
			// an ADDITIONAL inlineData image part right after its
			// functionResponse (the base64 is stripped from the JSON half).
			JsonValue parts = JsonValue::MakeArray();
			for( std::size_t i = 0; i < results.size(); ++i ) {
				const ChatToolCall& call = results[i].first;
				JsonValue env;
				std::string perr;
				const bool parsed = JsonParse( results[i].second, env, perr ) && env.isObject();

				JsonValue respObj = JsonValue::MakeObject();
				std::string b64;
				if( !parsed ) {
					respObj.set( "error", JsonValue::MakeString(
						"tool transport error: the JSON-RPC response line did not parse as JSON" ) );
				}
				else if( const JsonValue* e = env.find( "error" ) ) {
					respObj.set( "error", *e );
				}
				else {
					const JsonValue& result = env.get( "result" );
					if( IsImageResult( call, result, b64 ) ) {
						respObj = StripPngBase64( result, "the PNG is attached as an inline image part" );
					}
					else if( result.isObject() ) {
						respObj = result;
					}
					else {
						// functionResponse.response must be an object.
						respObj.set( "result", result );
					}
				}

				JsonValue fr = JsonValue::MakeObject();
				fr.set( "name", JsonValue::MakeString( call.name ) );
				fr.set( "response", respObj );
				JsonValue frPart = JsonValue::MakeObject();
				frPart.set( "functionResponse", fr );
				parts.push_back( frPart );

				if( !b64.empty() ) {
					JsonValue blob = JsonValue::MakeObject();
					blob.set( "mimeType", JsonValue::MakeString( "image/png" ) );
					blob.set( "data", JsonValue::MakeString( b64 ) );
					JsonValue imgPart = JsonValue::MakeObject();
					imgPart.set( "inlineData", blob );
					parts.push_back( imgPart );
				}
			}

			JsonValue msg = JsonValue::MakeObject();
			msg.set( "role", JsonValue::MakeString( "user" ) );
			msg.set( "parts", parts );
			return JsonSerialize( msg );
		}

		ChatHttpRequest GeminiChatCodec::BuildRequest(
			const std::string& modelId,
			const std::string& apiKey,
			const std::string& systemPrompt,
			const std::vector<std::string>& rawEntries ) const
		{
			ChatHttpRequest r;
			r.url = "https://generativelanguage.googleapis.com/v1beta/models/" +
			        modelId + ":generateContent";
			// The key appears ONLY here, in the auth header (NOT as the
			// ?key= query parameter the docs also allow -- a URL leaks into
			// logs/history far more easily than a header).
			r.headers.push_back( std::make_pair( "content-type", "application/json" ) );
			r.headers.push_back( std::make_pair( "x-goog-api-key", apiKey ) );

			std::string body = "{\"systemInstruction\":{\"parts\":[{\"text\":";
			JsonAppendEscapedString( body, systemPrompt );
			body += "}]},\"tools\":[{\"functionDeclarations\":";
			body += GeminiFunctionDeclarationsJson();
			body += "}],\"contents\":[";
			for( std::size_t i = 0; i < rawEntries.size(); ++i ) {
				if( i ) body += ",";
				body += rawEntries[i];
			}
			body += "]}";
			r.body = body;
			return r;
		}

		ChatParsedResponse GeminiChatCodec::ParseResponse(
			long httpStatus, const std::string& rawBody ) const
		{
			ChatParsedResponse out;
			if( httpStatus != 200 ) {
				out.step = MakeHttpError( "gemini", httpStatus, rawBody );
				return out;
			}

			JsonValue root;
			std::string perr;
			if( !JsonParse( rawBody, root, perr ) || !root.isObject() ) {
				out.step = MakeProviderError( "gemini response did not parse as JSON: " + perr );
				return out;
			}
			const JsonValue& candidates = root.get( "candidates" );
			if( !candidates.isArray() || candidates.size() == 0 ) {
				std::string msg = "gemini response carries no candidates";
				const JsonValue& em = root.get( "error" ).get( "message" );
				if( em.isString() ) msg += ": " + em.asString();
				const JsonValue& block = root.get( "promptFeedback" ).get( "blockReason" );
				if( block.isString() ) msg += " (blockReason " + block.asString() + ")";
				out.step = MakeProviderError( msg );
				return out;
			}

			const JsonValue& cand = candidates.at( 0 );
			const JsonValue& content = cand.get( "content" );
			const JsonValue& parts = content.get( "parts" );

			std::string text;
			std::vector<ChatToolCall> calls;
			for( std::size_t i = 0; i < parts.size(); ++i ) {
				const JsonValue& part = parts.at( i );
				if( const JsonValue* t = part.find( "text" ) ) {
					if( t->isString() ) text += t->asString();
				}
				if( const JsonValue* fc = part.find( "functionCall" ) ) {
					if( fc->isObject() ) {
						// Gemini function calls carry NO id: synthesize
						// "call_0", "call_1", ... per assistant turn;
						// results match by name + order.
						ChatToolCall c;
						c.id = "call_" + std::to_string( calls.size() );
						c.name = fc->get( "name" ).asString();
						const JsonValue& args = fc->get( "args" );
						c.argsJson = args.isObject() ? JsonSerialize( args ) : std::string( "{}" );
						calls.push_back( c );
					}
				}
			}
			out.assistantDisplayText = text;

			const std::string finishReason = cand.get( "finishReason" ).asString();
			if( !calls.empty() ) {
				out.step.kind = ChatStepResult::Kind::ToolCalls;
				out.step.toolCalls = calls;
			}
			else if( finishReason == "STOP" || finishReason.empty() ) {
				out.step.kind = ChatStepResult::Kind::FinalText;
				out.step.finalText = text;
			}
			else {
				// SAFETY / MAX_TOKENS / RECITATION / ... -> error with the
				// finishReason in the message.
				out.step = MakeProviderError( "gemini stopped with finishReason \"" + finishReason + "\"" );
				return out;
			}

			// Raw-span echo of candidates[0].content (verbatim -- preserves
			// provider-opaque fields such as thought signatures).
			std::size_t cb = 0, ce = 0, eb = 0, ee = 0, vb = 0, ve = 0;
			if( RawObjectMember( rawBody, 0, "candidates", cb, ce ) &&
			    RawArrayElement( rawBody, cb, 0, eb, ee ) &&
			    RawObjectMember( rawBody, eb, "content", vb, ve ) ) {
				if( content.has( "role" ) ) {
					out.assistantEntryJson = rawBody.substr( vb, ve - vb );
				}
				else {
					// Defensive: a content object with no role -- wrap the
					// raw parts span under an explicit model role.
					std::size_t pb = 0, pe = 0;
					if( RawObjectMember( rawBody, vb, "parts", pb, pe ) ) {
						out.assistantEntryJson = "{\"role\":\"model\",\"parts\":" +
						                         rawBody.substr( pb, pe - pb ) + "}";
					}
				}
			}
			if( out.assistantEntryJson.empty() ) {
				// Defensive fallback: re-serialize (loses byte-fidelity only).
				JsonValue msg = JsonValue::MakeObject();
				msg.set( "role", JsonValue::MakeString( "model" ) );
				msg.set( "parts", parts );
				out.assistantEntryJson = JsonSerialize( msg );
			}
			return out;
		}
	}
}
