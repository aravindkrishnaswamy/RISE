//////////////////////////////////////////////////////////////////////
//
//  AgentChatCodecs.cpp - provider wire-format codecs for the sans-IO
//    LLM chat loop (see AgentChatCodecs.h).
//
//  Layout:
//    (1) the NINE provider-neutral tool definitions (1:1 with the
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

#include "pch.h"
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
					"read_skill",
					"Read a scene-authoring skill (curated RISE how-to notes with "
					"verified scene snippets). Call with NO name first to list the "
					"available skills (name + one-line hook each); call with a name to "
					"read one BEFORE authoring or explaining scenes -- the skills carry "
					"the conventions (light directions, power semantics, painter "
					"wiring) that make first-try scenes render correctly.",
					"{\"type\":\"object\",\"properties\":{"
						"\"name\":{\"type\":\"string\",\"description\":"
						"\"A skill name from the index (e.g. scene-skeleton-and-conventions); omit to list all available skills.\"}"
					"}}"
				},
				{
					"validate",
					"Validate a CANDIDATE scene document without touching the live scene. "
					"Call this to check a document you are considering BEFORE proposing "
					"changes; no error-severity diagnostics means the candidate will load "
					"(warnings/info alone are not failures).",
					"{\"type\":\"object\",\"properties\":{"
						"\"text\":{\"type\":\"string\",\"description\":"
						"\"The complete candidate .RISEscene document text to check.\"}"
					"},\"required\":[\"text\"]}"
				},
				{
					"propose_patch",
					"Set one parameter of one named scene entity (the ONLY way to change "
					"a PARAMETER; insert_chunk/remove_chunk add and delete whole "
					"entities). Always pass the headVersion you last read as "
					"baseHeadVersion. If the result has status=conflict, re-call "
					"read_document and re-propose against the new headVersion. If "
					"retriable=true the refusal is transient -- retry the SAME patch "
					"after a moment. Only status=applied means the edit landed cleanly. "
					"status=diagnosed means the edit WAS applied but the re-derive "
					"produced diagnostics -- read them and fix the reported problem; do "
					"not blindly re-propose the same patch.",
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
					"insert_chunk",
					"ADD one new entity to the live scene by inserting a complete chunk. "
					"chunkText must be EXACTLY ONE `keyword { ... }` block with the braces "
					"on their own lines -- no scene header, no directives, no comments "
					"outside the chunk, one chunk per call. Order matters: an entity must "
					"appear EARLIER in the document than its consumer -- insert_chunk "
					"positions declaration chunks (painters, materials, geometry, "
					"shaders, media) before the objects that consume them automatically "
					"and appends everything else at the end; a rasterizer inserted this "
					"way becomes the ACTIVE integrator (matching save+reload). An insert "
					"that would leave the document unable to derive is refused cleanly. "
					"Unnamed film and rasterizer chunks can NEVER be removed through "
					"this surface once inserted -- insert them deliberately. The SOLE "
					"camera (even unnamed) IS removable via remove_chunk kind=\"camera\", "
					"so to SWAP cameras REMOVE the old one FIRST, THEN insert the "
					"replacement -- insert-first creates two cameras and strands the old "
					"unnamed one (a NAMED second camera stays removable by name) "
					"(the positional fallback requires exactly one). "
					"Use read_schema for the chunk's parameters; for a BIG "
					"addition, compose the full candidate document and validate it "
					"FIRST, then insert chunk by chunk. Always pass the headVersion you "
					"last read as baseHeadVersion. A duplicate (kind,name) is rejected "
					"-- pick a fresh name. status=applied means the entity is live (a "
					"full re-derive ran); render + read_image to verify.",
					"{\"type\":\"object\",\"properties\":{"
						"\"chunkText\":{\"type\":\"string\",\"description\":"
						"\"One complete chunk as scene-language text, e.g. omni_light\\n{\\nname key\\nposition 0 5 0\\ncolor 1 1 1\\npower 3.0\\n}\"},"
						"\"baseHeadVersion\":{\"type\":\"object\",\"description\":"
						"\"The headVersion from your last read_document -- pass it EVERY time so a stale edit is rejected as a conflict instead of clobbering.\","
						"\"properties\":{\"uuid\":{\"type\":\"number\"},\"revision\":{\"type\":\"number\"}},"
						"\"required\":[\"uuid\",\"revision\"]}"
					"},\"required\":[\"chunkText\"]}"
				},
				{
					"remove_chunk",
					"DELETE one entity (a whole chunk) from the live scene by name. "
					"Removal is whole-chunk only; the target is the chunk's bare name "
					"(the same addressing as propose_patch), with optional kind (the "
					"chunk keyword or a suffix like material) to narrow a name clash. "
					"A remove is rejected when the remaining document would not derive: "
					"usually the target is still REFERENCED by another chunk (retarget "
					"or remove the consumers first), or the document no longer derives "
					"in order -- read_document and validate to inspect. If a retarget "
					"is refused because the new entity sits later in the document, "
					"remove_chunk the consumer and re-insert it (it will be appended "
					"after the entity it references). Unnamed film and rasterizer "
					"chunks cannot be removed at all -- they have no addressable name; "
					"insert them deliberately. The SOLE camera CAN be removed even "
					"unnamed: pass kind=\"camera\" (the target resolves by position "
					"when exactly one camera exists). To SWAP cameras, remove the old "
					"one FIRST, then insert the replacement -- insert-first creates two "
					"cameras and strands the old unnamed one (a NAMED second camera "
					"stays removable by name). Always "
					"pass the headVersion you last read as baseHeadVersion. There is no "
					"rename verb -- the safe recipe: insert_chunk the renamed entity "
					"(declarations are positioned before their consumers), retarget its "
					"consumers via propose_patch, then remove_chunk the old one.",
					"{\"type\":\"object\",\"properties\":{"
						"\"target\":{\"type\":\"string\",\"description\":"
						"\"The bare NAME of the chunk to remove (a chunk name from the document).\"},"
						"\"kind\":{\"type\":\"string\",\"description\":"
						"\"Optional entity KIND keyword (e.g. omni_light) to disambiguate a name clash.\"},"
						"\"baseHeadVersion\":{\"type\":\"object\",\"description\":"
						"\"The headVersion from your last read_document -- pass it EVERY time so a stale edit is rejected as a conflict instead of clobbering.\","
						"\"properties\":{\"uuid\":{\"type\":\"number\"},\"revision\":{\"type\":\"number\"}},"
						"\"required\":[\"uuid\",\"revision\"]}"
					"},\"required\":[\"target\"]}"
				},
				{
					"render",
					"Re-render the live scene headlessly and return lean statistics "
					"(dimensions + linear per-channel means -- a stable image signature "
					"-- plus `integrator`, the ACTIVE rasterizer's chunk keyword, e.g. "
					"pathtracing_pel_rasterizer). Call after a successful propose_patch; "
					"compare the channel means against the previous render to confirm "
					"the edit changed the image. After inserting a rasterizer, check "
					"`integrator` to confirm which one is live. "
					"TOKEN ECONOMY: for modeling/placement checks use width/height 128-192 "
					"(NOT the full authored resolution) and read_image maxEdge ~192 -- a "
					"tiny preview is enough to confirm placement/shape/color and costs a "
					"fraction of the tokens and render time. Use `camera` to check the "
					"scene from 2-3 DIFFERENT ANGLES without editing the actual camera "
					"chunk -- the override is EPHEMERAL (restored after this one render) "
					"and never touches the document, so it's the cheap way to look at a "
					"scene from the side/above/behind before committing to a placement. "
					"Reserve full-size, full-sample renders (no width/height/camera "
					"override) for the FINAL verification once you're confident the edit "
					"is right.",
					"{\"type\":\"object\",\"properties\":{"
						"\"samples\":{\"type\":\"number\",\"description\":"
						"\"Optional sample-count override (currently advisory; the authored count is used).\"},"
						"\"width\":{\"type\":\"number\",\"description\":"
						"\"Optional TRANSIENT preview width in pixels, clamped to [16,512]. Must be paired with height -- if only one of width/height is given, NEITHER is applied and the render silently proceeds at the scene's authored dimensions (not rejected); check previewWidth/previewHeight in the result to confirm an override took. Does not touch the document -- use 128-192 for cheap placement checks.\"},"
						"\"height\":{\"type\":\"number\",\"description\":"
						"\"Optional TRANSIENT preview height in pixels, clamped to [16,512]. Must be paired with width -- see width's description for the unpaired behavior.\"},"
						"\"camera\":{\"type\":\"object\",\"description\":"
						"\"Optional EPHEMERAL camera-pose override for this ONE render only -- captured and restored automatically, never touches the document. Use to check the scene from a different angle.\","
						"\"properties\":{"
							"\"location\":{\"type\":\"string\",\"description\":\"Eye position, a string of EXACTLY 3 finite numbers \\\"x y z\\\" (space-separated, e.g. \\\"0 5 10\\\") -- required if camera is given. Malformed shapes (wrong token count, non-numeric component) are rejected with a clean error.\"},"
							"\"lookat\":{\"type\":\"string\",\"description\":\"Target point, a string of EXACTLY 3 finite numbers \\\"x y z\\\" -- required if camera is given. Same shape rule as location.\"},"
							"\"up\":{\"type\":\"string\",\"description\":\"Optional up vector, a string of EXACTLY 3 finite numbers \\\"x y z\\\"; defaults to the camera's current up. Same shape rule as location.\"},"
							"\"fov\":{\"type\":\"number\",\"description\":\"Optional field of view in degrees, EXCLUSIVE range (0, 180); defaults to the camera's current fov. 0, 180, negative, or non-finite values are rejected.\"}"
						"},\"required\":[\"location\",\"lookat\"]}"
					"}}"
				},
				{
					"read_image",
					"Fetch the LAST successful render as a PNG image so you can SEE the "
					"scene. Call after propose_patch + render to visually verify your "
					"edit did what you intended. If nothing has been rendered yet this "
					"returns an empty png_base64 (byteLength 0) -- call render first. "
					"TOKEN ECONOMY: pass maxEdge ~192 for a modeling/placement check -- the "
					"image is downscaled (no re-render) before being sent to you, so a "
					"quick look costs far fewer tokens than the full-resolution image. "
					"Omit maxEdge only for the final, full-detail look once you're done.",
					"{\"type\":\"object\",\"properties\":{"
						"\"maxEdge\":{\"type\":\"number\",\"description\":"
						"\"Optional long-edge bound in pixels, clamped to [16,1024]. Downscales (box filter, aspect-preserving, never upscales) the cached image before sending -- no re-render. Use ~192 for cheap modeling checks.\"}"
					"}}"
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
			//! of its value.  LAST match wins, deliberately matching
			//! JsonValue::find's last-set-wins rule -- so on a hostile
			//! body that repeats a key, the parsed view we act on and the
			//! raw span we echo back are the SAME value.
			bool RawObjectMember( const std::string& s, std::size_t objBegin, const char* key,
			                      std::size_t& valBegin, std::size_t& valEnd )
			{
				std::size_t i = SkipWs( s, objBegin );
				if( i >= s.size() || s[i] != '{' ) return false;
				++i;
				const std::size_t keyLen = std::strlen( key );
				bool found = false;
				while( true ) {
					i = SkipWs( s, i );
					if( i >= s.size() ) return false;
					if( s[i] == '}' ) return found;
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
					if( match ) { valBegin = i; valEnd = vEnd; found = true; }
					i = SkipWs( s, vEnd );
					if( i < s.size() && s[i] == ',' ) { ++i; continue; }
					if( i < s.size() && s[i] == '}' ) return found;
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

			//! Output-token budget for Anthropic requests.  Adaptive
			//! thinking on claude-sonnet-5 (the default model) counts
			//! against max_tokens, and tool results routinely carry whole
			//! scene documents into context -- 8192 was realistically
			//! exhaustible mid-reply.  16000 keeps the cap a safety net
			//! rather than a limit the loop actually hits.
			const int kAnthropicMaxTokens = 16000;

			//! Strip every control character (< 0x20 -- CR, LF, TAB, ...)
			//! from a caller-supplied string before splicing it into an
			//! HTTP header VALUE, so a pasted API key carrying a stray
			//! newline cannot smuggle extra headers into the request
			//! (header-injection defense; a stripped key simply fails
			//! auth, which is the honest outcome).
			std::string SanitizeHeaderValue( const std::string& v )
			{
				std::string out;
				out.reserve( v.size() );
				for( std::size_t i = 0; i < v.size(); ++i ) {
					const unsigned char c = static_cast<unsigned char>( v[i] );
					if( c >= 0x20 ) out += v[i];
				}
				return out;
			}

			//! Percent-escape any modelId character outside [A-Za-z0-9._-]
			//! before splicing it into a URL path segment, so a hostile or
			//! mistyped model id cannot alter the request path or smuggle
			//! query parameters.
			std::string SanitizeModelIdForUrl( const std::string& modelId )
			{
				static const char* const kHex = "0123456789ABCDEF";
				std::string out;
				out.reserve( modelId.size() );
				for( std::size_t i = 0; i < modelId.size(); ++i ) {
					const char c = modelId[i];
					const bool ok = ( c >= 'A' && c <= 'Z' ) || ( c >= 'a' && c <= 'z' ) ||
					                ( c >= '0' && c <= '9' ) ||
					                c == '.' || c == '_' || c == '-';
					if( ok ) {
						out += c;
					}
					else {
						const unsigned char u = static_cast<unsigned char>( c );
						out += '%';
						out += kHex[( u >> 4 ) & 0xF];
						out += kHex[u & 0xF];
					}
				}
				return out;
			}

			ChatStepResult MakeProviderError( ChatErrorKind kind, const std::string& message )
			{
				ChatStepResult r;
				r.kind = ChatStepResult::Kind::ProviderError;
				r.errorKind = kind;
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
				return MakeProviderError( ChatErrorKind::Http, full );
			}

			//! The text that replaces an elided (superseded) image block or
			//! part -- see AgentChatLoop.h "IMAGE RETENTION".
			const char* const kImageElidedNote =
				"[image elided -- superseded by a newer render]";

			//! The text that replaces an elided user reference-image
			//! attachment once the live-image cap is exceeded -- see
			//! AgentChatLoop.h "USER IMAGE RETENTION".  Deliberately
			//! distinct wording from kImageElidedNote (a different policy:
			//! capped-oldest-first, not most-recent-only) and actionable
			//! ("re-attach if needed") since, unlike a read_image result
			//! the loop can always regenerate by rendering again, a user
			//! photo is gone for good unless the user attaches it again.
			const char* const kUserImageElidedNote =
				"[reference image elided -- re-attach if needed]";

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
				// Check-then-set: JsonValue::set APPENDS members, so blindly
				// setting "note" would serialize a DUPLICATE key if the RPC
				// result ever grows a note field of its own.
				summary.set( summary.has( "note" ) ? "image_note" : "note",
				             JsonValue::MakeString( note ) );
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

			//! Rewrite the attach note inside a StripPngBase64-produced
			//! textual summary so an elided image is not described as
			//! "attached" (contradictory context for the model).  The
			//! summary is LOOP-OWNED JSON (StripPngBase64 wrote it), so
			//! parse + rewrite is legal.  Prefers "image_note" -- the key
			//! StripPngBase64 falls back to when the RPC result already
			//! owned a "note" of its own -- so an RPC-owned note field is
			//! never clobbered.  Text that does not parse as an object, or
			//! carries neither key, is returned unchanged.
			std::string RewriteElidedSummaryText( const std::string& text )
			{
				JsonValue obj;
				std::string perr;
				if( !JsonParse( text, obj, perr ) || !obj.isObject() ) return text;
				// INVARIANT: this prefer-image_note-else-note order matches
				// StripPngBase64's collision fallback (which writes the attach
				// note under "image_note" ONLY when the RPC result already
				// owns a "note").  If an RPC result ever grows an
				// "image_note" field of its own WITHOUT a "note", the two
				// rules diverge (this rewrite would clobber the RPC's field)
				// -- keep the key-choice rules in lockstep.
				const char* key = obj.has( "image_note" ) ? "image_note"
				                : obj.has( "note" )       ? "note"
				                : nullptr;
				if( !key ) return text;
				JsonValue out = JsonValue::MakeObject();
				const std::vector<std::pair<std::string, JsonValue>>& mem = obj.members();
				for( std::size_t i = 0; i < mem.size(); ++i ) {
					out.set( mem[i].first, mem[i].first == key
					         ? JsonValue::MakeString( kImageElidedNote )
					         : mem[i].second );
				}
				return JsonSerialize( out );
			}
		}

		bool ChatToolResultCarriesImage( const ChatToolCall& call,
		                                 const std::string& rawJsonRpcResponseLine )
		{
			// Same predicate the pack paths apply: a parseable JSON-RPC
			// SUCCESS envelope (an error envelope never packs an image)
			// whose result is a read_image payload with non-empty
			// png_base64.
			JsonValue env;
			std::string perr;
			if( !JsonParse( rawJsonRpcResponseLine, env, perr ) || !env.isObject() ) return false;
			if( env.find( "error" ) ) return false;
			std::string b64;
			return IsImageResult( call, env.get( "result" ), b64 );
		}

		int ChatUserEntryLiveImageCount( const std::string& userEntryJson )
		{
			JsonValue root;
			std::string perr;
			if( !JsonParse( userEntryJson, root, perr ) || !root.isObject() ) return 0;

			int count = 0;
			// Anthropic shape: {"role":"user","content":[{"type":"image",...},...]}
			const JsonValue& content = root.get( "content" );
			if( content.isArray() ) {
				for( std::size_t i = 0; i < content.size(); ++i )
					if( content.at( i ).get( "type" ).asString() == "image" ) ++count;
			}
			// Gemini shape: {"role":"user","parts":[{"inlineData":{...}},...]}
			const JsonValue& parts = root.get( "parts" );
			if( parts.isArray() ) {
				for( std::size_t i = 0; i < parts.size(); ++i )
					if( parts.at( i ).get( "inlineData" ).isObject() ) ++count;
			}
			return count;
		}

		//======================================================================
		// (3) AnthropicChatCodec
		//======================================================================

		const char* AnthropicChatCodec::ProviderName() const { return "anthropic"; }

		const char* AnthropicChatCodec::DefaultModelId() const { return "claude-sonnet-5"; }

		std::string AnthropicChatCodec::MakeUserEntry(
			const std::string& text, const std::vector<ChatAttachment>& attachments ) const
		{
			JsonValue msg = JsonValue::MakeObject();
			msg.set( "role", JsonValue::MakeString( "user" ) );
			JsonValue content = JsonValue::MakeArray();
			// Images FIRST, then the caption -- see the header note on
			// MakeUserEntry (an empty attachments vector reproduces the
			// pre-attachment byte shape exactly: one text block, nothing
			// else, since the loop below then simply doesn't run).
			for( std::size_t i = 0; i < attachments.size(); ++i ) {
				JsonValue source = JsonValue::MakeObject();
				source.set( "type", JsonValue::MakeString( "base64" ) );
				source.set( "media_type", JsonValue::MakeString( attachments[i].mimeType ) );
				source.set( "data", JsonValue::MakeString( attachments[i].base64Data ) );
				JsonValue img = JsonValue::MakeObject();
				img.set( "type", JsonValue::MakeString( "image" ) );
				img.set( "source", source );
				content.push_back( img );
			}
			// Anthropic hard-400s an EMPTY text block -- an attachment-
			// only message (no caption) must therefore omit the text
			// block entirely rather than send {"type":"text","text":""}.
			// A text-only call (the common case) is unaffected: text is
			// non-empty by the loop's blank-message no-op contract.
			if( !text.empty() ) content.push_back( MakeTextBlock( text ) );
			msg.set( "content", content );
			return JsonSerialize( msg );
		}

		std::string AnthropicChatCodec::RewriteElidedUserImages(
			const std::string& userEntryJson, int countToElide ) const
		{
			// Parse + regenerate is LEGAL here: this entry was produced by
			// MakeUserEntry above (loop-generated), never by a provider --
			// only assistant entries carry the byte-preservation contract.
			if( countToElide <= 0 ) return userEntryJson;
			JsonValue root;
			std::string perr;
			if( !JsonParse( userEntryJson, root, perr ) || !root.isObject() ) return userEntryJson;
			const JsonValue& content = root.get( "content" );
			if( !content.isArray() ) return userEntryJson;

			bool changed = false;
			int remaining = countToElide;
			JsonValue newContent = JsonValue::MakeArray();
			for( std::size_t i = 0; i < content.size(); ++i ) {
				const JsonValue& b = content.at( i );
				if( b.get( "type" ).asString() == "image" && remaining > 0 ) {
					newContent.push_back( MakeTextBlock( kUserImageElidedNote ) );
					--remaining;
					changed = true;
				}
				else {
					newContent.push_back( b );
				}
			}
			if( !changed ) return userEntryJson;

			JsonValue newRoot = JsonValue::MakeObject();
			const std::vector<std::pair<std::string, JsonValue>>& mem = root.members();
			for( std::size_t i = 0; i < mem.size(); ++i )
				newRoot.set( mem[i].first, mem[i].first == "content" ? newContent : mem[i].second );
			return JsonSerialize( newRoot );
		}

		std::string AnthropicChatCodec::PackToolResults(
			const std::vector<std::pair<ChatToolCall, std::string>>& results ) const
		{
			// ONE user message carrying one tool_result block per tool_use --
			// Anthropic requires every tool_use of the assistant turn to be
			// answered in the SAME following user message.
			//
			// IMAGE RETENTION, entry-internal half: within one flush only
			// the LAST image-bearing result keeps a live image block --
			// earlier ones are packed PRE-ELIDED (summary note + elision
			// text, exactly what RewriteElidedImages would later produce)
			// so the "one live image globally" rule holds even when one
			// assistant turn requested read_image twice.
			std::size_t lastImage = results.size();
			for( std::size_t i = 0; i < results.size(); ++i ) {
				if( ChatToolResultCarriesImage( results[i].first, results[i].second ) )
					lastImage = i;
			}

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
						if( i == lastImage ) {
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
							// Superseded within this very flush: pack it
							// already elided (see the header comment above).
							blocks.push_back( MakeTextBlock( JsonSerialize(
								StripPngBase64( result, kImageElidedNote ) ) ) );
							blocks.push_back( MakeTextBlock( kImageElidedNote ) );
						}
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

		std::string AnthropicChatCodec::RewriteElidedImages( const std::string& packedEntryJson ) const
		{
			// Parse + regenerate is LEGAL here: this entry was produced by
			// PackToolResults above (loop-generated), not by a provider --
			// only assistant entries carry the byte-preservation contract.
			JsonValue root;
			std::string perr;
			if( !JsonParse( packedEntryJson, root, perr ) || !root.isObject() ) return packedEntryJson;
			const JsonValue& content = root.get( "content" );
			if( !content.isArray() ) return packedEntryJson;

			bool changed = false;
			JsonValue newContent = JsonValue::MakeArray();
			for( std::size_t i = 0; i < content.size(); ++i ) {
				const JsonValue& tr = content.at( i );
				const JsonValue& blocks = tr.get( "content" );
				if( tr.get( "type" ).asString() != "tool_result" || !blocks.isArray() ) {
					newContent.push_back( tr );
					continue;
				}
				// Swap each {type:"image",...} element for the elision text
				// block; every other member of the tool_result (tool_use_id
				// included) is copied through, so the rewritten entry stays
				// wire-valid.  In an image-bearing tool_result the textual
				// summary's attach note ("the PNG is attached as an image
				// block") is rewritten too -- it is loop-written content
				// (StripPngBase64 produced it) and leaving it would show
				// the model a note contradicting the elided block.
				bool hasImage = false;
				for( std::size_t j = 0; j < blocks.size(); ++j )
					if( blocks.at( j ).get( "type" ).asString() == "image" ) hasImage = true;
				if( !hasImage ) {
					newContent.push_back( tr );
					continue;
				}
				changed = true;
				JsonValue newBlocks = JsonValue::MakeArray();
				for( std::size_t j = 0; j < blocks.size(); ++j ) {
					const JsonValue& b = blocks.at( j );
					const std::string btype = b.get( "type" ).asString();
					if( btype == "image" ) {
						newBlocks.push_back( MakeTextBlock( kImageElidedNote ) );
					}
					else if( btype == "text" ) {
						newBlocks.push_back( MakeTextBlock(
							RewriteElidedSummaryText( b.get( "text" ).asString() ) ) );
					}
					else {
						newBlocks.push_back( b );
					}
				}
				JsonValue newTr = JsonValue::MakeObject();
				const std::vector<std::pair<std::string, JsonValue>>& mem = tr.members();
				for( std::size_t j = 0; j < mem.size(); ++j )
					newTr.set( mem[j].first, mem[j].first == "content" ? newBlocks : mem[j].second );
				newContent.push_back( newTr );
			}
			if( !changed ) return packedEntryJson;

			JsonValue newRoot = JsonValue::MakeObject();
			const std::vector<std::pair<std::string, JsonValue>>& mem = root.members();
			for( std::size_t i = 0; i < mem.size(); ++i )
				newRoot.set( mem[i].first, mem[i].first == "content" ? newContent : mem[i].second );
			return JsonSerialize( newRoot );
		}

		ChatHttpRequest AnthropicChatCodec::BuildRequest(
			const std::string& modelId,
			const std::string& apiKey,
			const std::string& systemPrompt,
			const std::vector<std::string>& rawEntries ) const
		{
			ChatHttpRequest r;
			r.url = "https://api.anthropic.com/v1/messages";
			// The key appears ONLY here, in the auth header -- control
			// characters stripped so a pasted key cannot inject headers.
			r.headers.push_back( std::make_pair( "content-type", "application/json" ) );
			r.headers.push_back( std::make_pair( "x-api-key", SanitizeHeaderValue( apiKey ) ) );
			r.headers.push_back( std::make_pair( "anthropic-version", "2023-06-01" ) );

			// The body is assembled as a string so assistant entries (raw
			// provider-native JSON) splice in VERBATIM.  No thinking config
			// is set (omitted = adaptive on models that support it).
			std::string body = "{\"model\":";
			JsonAppendEscapedString( body, modelId );
			body += ",\"max_tokens\":" + std::to_string( kAnthropicMaxTokens ) + ",\"system\":";
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
				out.step = MakeHttpError( ProviderName(), httpStatus, rawBody );
				return out;
			}

			JsonValue root;
			std::string perr;
			if( !JsonParse( rawBody, root, perr ) || !root.isObject() ) {
				out.step = MakeProviderError( ChatErrorKind::Parse,
					"anthropic response did not parse as JSON: " + perr );
				return out;
			}
			const JsonValue& content = root.get( "content" );
			if( !content.isArray() ) {
				out.step = MakeProviderError( ChatErrorKind::Provider,
					"anthropic response carries no content array" );
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
					if( c.id.empty() ) {
						// A tool_use with no id could never be answered (the
						// tool_result must echo tool_use_id) -- executing the
						// rest of the turn would ship an unanswerable block,
						// so refuse the WHOLE response instead of emitting
						// tool_use_id:"" downstream.
						out.step = MakeProviderError( ChatErrorKind::Provider,
							"anthropic tool_use block carries no id; refusing the turn (its result could never be matched)" );
						return out;
					}
					// Duplicate ids within one turn make result matching
					// ambiguous -- ids are the result-matching key and must
					// be unique.  Without this gate, AddToolResult would
					// ignore the second call as already-answered, the flush
					// would synthesize a SECOND result for the SAME id, and
					// the wire would carry duplicate tool_use ids answered by
					// duplicate tool_use_id results (a permanent 400).
					// Refuse the whole turn instead (mirrors the Gemini
					// duplicate-functionCall-id gate).
					for( std::size_t k = 0; k < calls.size(); ++k ) {
						if( calls[k].id == c.id ) {
							out.step = MakeProviderError( ChatErrorKind::Provider,
								"anthropic response repeats tool_use id \"" + c.id +
								"\" -- refusing the turn (results could not be matched unambiguously)" );
							return out;
						}
					}
					const JsonValue& input = block.get( "input" );
					c.argsJson = input.isObject() ? JsonSerialize( input ) : std::string( "{}" );
					calls.push_back( c );
				}
				// thinking / other block kinds: not displayed; the raw echo
				// below preserves them for the provider.
			}

			const std::string stopReason = root.get( "stop_reason" ).asString();
			if( stopReason == "tool_use" ) {
				if( calls.empty() ) {
					out.step = MakeProviderError( ChatErrorKind::Provider,
						"anthropic stopped with stop_reason \"tool_use\" but no tool_use blocks were present" );
					return out;
				}
				out.step.kind = ChatStepResult::Kind::ToolCalls;
				out.step.toolCalls = calls;
			}
			else if( stopReason == "max_tokens" ) {
				// Distinct, actionable dead-end message (the reply is
				// discarded -- transcript consistency: record nothing).
				out.step = MakeProviderError( ChatErrorKind::MaxTokens,
					"anthropic: the response hit the output-token cap (stop_reason max_tokens) -- "
					"the truncated reply was discarded; try a narrower request" );
				return out;
			}
			else if( stopReason == "refusal" ) {
				out.step = MakeProviderError( ChatErrorKind::Refusal,
					"anthropic: the provider declined this request (stop_reason refusal)" );
				return out;
			}
			else if( !calls.empty() ) {
				// WIRE-INVARIANT GATE: tool_use blocks under a non-tool_use
				// stop_reason (a hostile or corrupted body).  Recording this
				// turn would echo tool_use blocks the loop never pends --
				// so no tool_result could EVER answer them and every later
				// request would replay an unanswered tool_use (a permanent
				// 400).  Refuse the WHOLE response; record nothing.
				out.step = MakeProviderError( ChatErrorKind::Provider,
					"anthropic response carries tool_use blocks under stop_reason \"" + stopReason +
					"\" -- refusing the turn (its calls would be recorded but never answerable)" );
				return out;
			}
			else if( stopReason == "end_turn" ) {
				if( content.size() == 0 ) {
					// A degenerate empty-content final turn: recording
					// {"content":[]} poisons the echo (the API rejects an
					// assistant message with no content blocks).
					out.step = MakeProviderError( ChatErrorKind::Provider,
						"anthropic ended the turn with an EMPTY content array -- refusing the degenerate turn" );
					return out;
				}
				out.step.kind = ChatStepResult::Kind::FinalText;
				out.step.finalText = text;
			}
			else {
				// pause_turn / anything unexpected -> error with the
				// stop_reason in the message.
				out.step = MakeProviderError( ChatErrorKind::Provider,
					"anthropic stopped with stop_reason \"" + stopReason + "\"" );
				return out;
			}
			out.assistantDisplayText = text;
			out.step.assistantDisplayText = text;

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

		std::string GeminiChatCodec::MakeUserEntry(
			const std::string& text, const std::vector<ChatAttachment>& attachments ) const
		{
			JsonValue parts = JsonValue::MakeArray();
			// Images FIRST, then the caption -- mirrors the Anthropic
			// codec (see the interface doc on MakeUserEntry); an empty
			// attachments vector reproduces the pre-attachment byte shape
			// exactly (one text part, nothing else).
			for( std::size_t i = 0; i < attachments.size(); ++i ) {
				JsonValue blob = JsonValue::MakeObject();
				blob.set( "mimeType", JsonValue::MakeString( attachments[i].mimeType ) );
				blob.set( "data", JsonValue::MakeString( attachments[i].base64Data ) );
				JsonValue part = JsonValue::MakeObject();
				part.set( "inlineData", blob );
				parts.push_back( part );
			}
			// An attachment-only message (no caption) omits the text part
			// entirely rather than sending an empty-string text part --
			// the inlineData part(s) already satisfy Gemini's non-empty-
			// parts requirement.  A text-only call (the common case) is
			// unaffected: text is non-empty by the loop's blank-message
			// no-op contract.
			if( !text.empty() ) {
				JsonValue part = JsonValue::MakeObject();
				part.set( "text", JsonValue::MakeString( text ) );
				parts.push_back( part );
			}
			JsonValue msg = JsonValue::MakeObject();
			msg.set( "role", JsonValue::MakeString( "user" ) );
			msg.set( "parts", parts );
			return JsonSerialize( msg );
		}

		std::string GeminiChatCodec::RewriteElidedUserImages(
			const std::string& userEntryJson, int countToElide ) const
		{
			// Parse + regenerate is LEGAL here: this entry was produced by
			// MakeUserEntry above (loop-generated), never by a provider --
			// only model entries carry the byte-preservation contract.
			// A FunctionResponsePart-style inlineData part carries only
			// media (no text form), so elision replaces the WHOLE part
			// with a plain text part (unlike the tool-result elision,
			// which rewrites the response object in place -- there is no
			// surrounding response object here to carry a note).
			if( countToElide <= 0 ) return userEntryJson;
			JsonValue root;
			std::string perr;
			if( !JsonParse( userEntryJson, root, perr ) || !root.isObject() ) return userEntryJson;
			const JsonValue& parts = root.get( "parts" );
			if( !parts.isArray() ) return userEntryJson;

			bool changed = false;
			int remaining = countToElide;
			JsonValue newParts = JsonValue::MakeArray();
			for( std::size_t i = 0; i < parts.size(); ++i ) {
				const JsonValue& p = parts.at( i );
				if( p.get( "inlineData" ).isObject() && remaining > 0 ) {
					JsonValue textPart = JsonValue::MakeObject();
					textPart.set( "text", JsonValue::MakeString( kUserImageElidedNote ) );
					newParts.push_back( textPart );
					--remaining;
					changed = true;
				}
				else {
					newParts.push_back( p );
				}
			}
			if( !changed ) return userEntryJson;

			JsonValue newRoot = JsonValue::MakeObject();
			const std::vector<std::pair<std::string, JsonValue>>& mem = root.members();
			for( std::size_t i = 0; i < mem.size(); ++i )
				newRoot.set( mem[i].first, mem[i].first == "parts" ? newParts : mem[i].second );
			return JsonSerialize( newRoot );
		}

		std::string GeminiChatCodec::PackToolResults(
			const std::vector<std::pair<ChatToolCall, std::string>>& results ) const
		{
			// ONE user turn: one functionResponse part per call.  When the
			// model's functionCall carried an id it is echoed back as
			// functionResponse.id (the documented matching rule); only
			// synthesized ids are withheld (those calls match by name +
			// order).  read_image results deliver the PNG through
			// functionResponse.parts[].inlineData -- the documented
			// FunctionResponsePart mechanism for multimodal function
			// output -- with the base64 stripped from the JSON half.
			//
			// IMAGE RETENTION, entry-internal half (mirrors the Anthropic
			// codec): within one flush only the LAST image-bearing result
			// keeps a live inlineData part; earlier ones are packed
			// PRE-ELIDED (no parts array; the note already reads elided).
			std::size_t lastImage = results.size();
			for( std::size_t i = 0; i < results.size(); ++i ) {
				if( ChatToolResultCarriesImage( results[i].first, results[i].second ) )
					lastImage = i;
			}

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
						if( i == lastImage ) {
							respObj = StripPngBase64( result, "the PNG is attached as a functionResponse media part" );
						}
						else {
							// Superseded within this very flush: pack it
							// already elided (note reads elided; b64 cleared
							// so no inlineData parts array is emitted below).
							respObj = StripPngBase64( result, kImageElidedNote );
							b64.clear();
						}
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
				if( !call.idSynthesized && !call.id.empty() )
					fr.set( "id", JsonValue::MakeString( call.id ) );
				fr.set( "name", JsonValue::MakeString( call.name ) );
				fr.set( "response", respObj );
				if( !b64.empty() ) {
					// FunctionResponse.parts: [{inlineData:{mimeType,data}}]
					// (FunctionResponsePart / FunctionResponseBlob, per the
					// ai.google.dev/api Content reference, 2026-07-02).
					JsonValue blob = JsonValue::MakeObject();
					blob.set( "mimeType", JsonValue::MakeString( "image/png" ) );
					blob.set( "data", JsonValue::MakeString( b64 ) );
					JsonValue frp = JsonValue::MakeObject();
					frp.set( "inlineData", blob );
					JsonValue frParts = JsonValue::MakeArray();
					frParts.push_back( frp );
					fr.set( "parts", frParts );
				}
				JsonValue frPart = JsonValue::MakeObject();
				frPart.set( "functionResponse", fr );
				parts.push_back( frPart );
			}

			JsonValue msg = JsonValue::MakeObject();
			msg.set( "role", JsonValue::MakeString( "user" ) );
			msg.set( "parts", parts );
			return JsonSerialize( msg );
		}

		std::string GeminiChatCodec::RewriteElidedImages( const std::string& packedEntryJson ) const
		{
			// Parse + regenerate is LEGAL here: this entry was produced by
			// PackToolResults above (loop-generated), not by a provider --
			// only assistant (model) entries carry the byte-preservation
			// contract.  The image travels as functionResponse.parts
			// [{inlineData:...}]; a FunctionResponsePart carries only
			// media (no text form exists), so elision DROPS the parts
			// array and rewrites the response object's note to say the
			// image is gone.
			JsonValue root;
			std::string perr;
			if( !JsonParse( packedEntryJson, root, perr ) || !root.isObject() ) return packedEntryJson;
			const JsonValue& parts = root.get( "parts" );
			if( !parts.isArray() ) return packedEntryJson;

			bool changed = false;
			JsonValue newParts = JsonValue::MakeArray();
			for( std::size_t i = 0; i < parts.size(); ++i ) {
				const JsonValue& p = parts.at( i );
				const JsonValue* fr = p.find( "functionResponse" );
				const bool hasImage = fr && fr->isObject() && fr->get( "parts" ).isArray();
				if( !hasImage ) {
					newParts.push_back( p );
					continue;
				}
				changed = true;
				// Rebuild the functionResponse WITHOUT its parts array and
				// with the response note rewritten to the elision text
				// (PackToolResults always sets a note on an image result;
				// append one defensively if it is ever absent).
				JsonValue newFr = JsonValue::MakeObject();
				const std::vector<std::pair<std::string, JsonValue>>& mem = fr->members();
				for( std::size_t j = 0; j < mem.size(); ++j ) {
					if( mem[j].first == "parts" ) continue;
					if( mem[j].first == "response" && mem[j].second.isObject() ) {
						// Rewrite the ATTACH note StripPngBase64 wrote --
						// preferring "image_note" (the key it falls back to
						// when the RPC result already owned a "note"), so an
						// RPC-owned note field is never clobbered.
						JsonValue newResp = JsonValue::MakeObject();
						bool noted = false;
						const std::vector<std::pair<std::string, JsonValue>>& rm = mem[j].second.members();
						// INVARIANT (mirrors RewriteElidedSummaryText's rule):
						// prefer-image_note-else-note matches StripPngBase64's
						// collision fallback; if an RPC result ever grows
						// image_note WITHOUT note, the rules diverge -- keep
						// the key-choice rules in lockstep.
						bool hasImageNote = false;
						for( std::size_t k = 0; k < rm.size(); ++k )
							if( rm[k].first == "image_note" ) hasImageNote = true;
						const char* noteKey = hasImageNote ? "image_note" : "note";
						for( std::size_t k = 0; k < rm.size(); ++k ) {
							if( rm[k].first == noteKey ) {
								newResp.set( rm[k].first, JsonValue::MakeString( kImageElidedNote ) );
								noted = true;
							}
							else {
								newResp.set( rm[k].first, rm[k].second );
							}
						}
						if( !noted )
							newResp.set( "note", JsonValue::MakeString( kImageElidedNote ) );
						newFr.set( "response", newResp );
					}
					else {
						newFr.set( mem[j].first, mem[j].second );
					}
				}
				JsonValue newPart = JsonValue::MakeObject();
				const std::vector<std::pair<std::string, JsonValue>>& pm = p.members();
				for( std::size_t j = 0; j < pm.size(); ++j )
					newPart.set( pm[j].first, pm[j].first == "functionResponse" ? newFr : pm[j].second );
				newParts.push_back( newPart );
			}
			if( !changed ) return packedEntryJson;

			JsonValue newRoot = JsonValue::MakeObject();
			const std::vector<std::pair<std::string, JsonValue>>& mem = root.members();
			for( std::size_t i = 0; i < mem.size(); ++i )
				newRoot.set( mem[i].first, mem[i].first == "parts" ? newParts : mem[i].second );
			return JsonSerialize( newRoot );
		}

		ChatHttpRequest GeminiChatCodec::BuildRequest(
			const std::string& modelId,
			const std::string& apiKey,
			const std::string& systemPrompt,
			const std::vector<std::string>& rawEntries ) const
		{
			ChatHttpRequest r;
			// The model id is percent-escaped so it cannot alter the URL
			// path or smuggle query parameters.
			r.url = "https://generativelanguage.googleapis.com/v1beta/models/" +
			        SanitizeModelIdForUrl( modelId ) + ":generateContent";
			// The key appears ONLY here, in the auth header (NOT as the
			// ?key= query parameter the docs also allow -- a URL leaks into
			// logs/history far more easily than a header).  Control
			// characters are stripped so a pasted key cannot inject headers.
			r.headers.push_back( std::make_pair( "content-type", "application/json" ) );
			r.headers.push_back( std::make_pair( "x-goog-api-key", SanitizeHeaderValue( apiKey ) ) );

			// Gemini requires ALTERNATING roles on the wire, but the
			// transcript can legally hold ADJACENT user entries (an
			// interrupt-flushed tool-results entry followed by the user's
			// next text message).  Merge every run of adjacent role:"user"
			// entries into ONE content whose parts are the run's parts
			// concatenated with the functionResponse parts FIRST (the tool
			// answers must lead the content that answers a functionCall
			// turn).  User entries are loop-generated (MakeUserEntry /
			// PackToolResults), so parse + re-serialize is legal for them;
			// model entries splice VERBATIM (byte-preservation contract)
			// and are never merged -- ParseResponse guarantees a recorded
			// model entry's role really is "model" (a candidate spoofing
			// any other role is refused outright; role-less content is
			// wrapped under an explicit "model"), so a provider body can
			// never smuggle an assistant turn into this user merge.
			std::vector<std::string> wireEntries;
			std::vector<JsonValue>   userRun;
			std::vector<std::size_t> userRunSrc;   // rawEntries index per userRun element
			wireEntries.reserve( rawEntries.size() );
			const auto flushUserRun = [&]() {
				if( userRun.empty() ) return;
				if( userRun.size() == 1 ) {
					// A lone user entry needs no merge: splice the original
					// string (cheaper, and trivially byte-stable).
					wireEntries.push_back( rawEntries[userRunSrc[0]] );
				}
				else {
					JsonValue parts = JsonValue::MakeArray();
					for( int wantFr = 1; wantFr >= 0; --wantFr ) {
						for( std::size_t i = 0; i < userRun.size(); ++i ) {
							const JsonValue& runParts = userRun[i].get( "parts" );
							for( std::size_t j = 0; j < runParts.size(); ++j ) {
								const bool isFr = runParts.at( j ).has( "functionResponse" );
								if( isFr == ( wantFr == 1 ) ) parts.push_back( runParts.at( j ) );
							}
						}
					}
					JsonValue merged = JsonValue::MakeObject();
					merged.set( "role", JsonValue::MakeString( "user" ) );
					merged.set( "parts", parts );
					wireEntries.push_back( JsonSerialize( merged ) );
				}
				userRun.clear();
				userRunSrc.clear();
			};
			for( std::size_t i = 0; i < rawEntries.size(); ++i ) {
				JsonValue e;
				std::string perr;
				if( JsonParse( rawEntries[i], e, perr ) && e.isObject() &&
				    e.get( "role" ).asString() == "user" && e.get( "parts" ).isArray() ) {
					userRun.push_back( e );
					userRunSrc.push_back( i );
				}
				else {
					flushUserRun();
					wireEntries.push_back( rawEntries[i] );
				}
			}
			flushUserRun();

			std::string body = "{\"systemInstruction\":{\"parts\":[{\"text\":";
			JsonAppendEscapedString( body, systemPrompt );
			body += "}]},\"tools\":[{\"functionDeclarations\":";
			body += GeminiFunctionDeclarationsJson();
			body += "}],\"contents\":[";
			for( std::size_t i = 0; i < wireEntries.size(); ++i ) {
				if( i ) body += ",";
				body += wireEntries[i];
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
				out.step = MakeHttpError( ProviderName(), httpStatus, rawBody );
				return out;
			}

			JsonValue root;
			std::string perr;
			if( !JsonParse( rawBody, root, perr ) || !root.isObject() ) {
				out.step = MakeProviderError( ChatErrorKind::Parse,
					"gemini response did not parse as JSON: " + perr );
				return out;
			}
			const JsonValue& candidates = root.get( "candidates" );
			if( !candidates.isArray() || candidates.size() == 0 ) {
				std::string msg = "gemini response carries no candidates";
				ChatErrorKind kind = ChatErrorKind::Provider;
				const JsonValue& em = root.get( "error" ).get( "message" );
				if( em.isString() ) msg += ": " + em.asString();
				const JsonValue& block = root.get( "promptFeedback" ).get( "blockReason" );
				if( block.isString() ) {
					// A blocked prompt is the provider declining the request.
					msg += " (blockReason " + block.asString() + ")";
					kind = ChatErrorKind::Refusal;
				}
				out.step = MakeProviderError( kind, msg );
				return out;
			}

			const JsonValue& cand = candidates.at( 0 );
			const JsonValue& content = cand.get( "content" );

			// RECORD-OR-REFUSE role gate: the raw-span echo below records
			// candidates[0].content VERBATIM whenever it carries a "role"
			// member, and BuildRequest's adjacent-user merge classifies
			// transcript entries by parsing that role.  A hostile candidate
			// spoofing content.role:"user" would therefore be recorded as
			// the assistant turn and then MERGED into the surrounding user
			// run on the next request -- the conversation collapses into
			// ONE user content with a functionCall part inside it (wire-
			// invalid, permanent poison).  Refuse any candidate whose
			// content.role is present and not "model"; an ABSENT role keeps
			// the wrap-as-model behavior below.
			if( content.isObject() ) {
				const JsonValue* role = content.find( "role" );
				if( role && !( role->isString() && role->asString() == "model" ) ) {
					out.step = MakeProviderError( ChatErrorKind::Provider,
						"gemini candidate content carries role \"" +
						( role->isString() ? role->asString() : std::string( "(non-string)" ) ) +
						"\" instead of \"model\" -- refusing the turn (a spoofed role would corrupt the wire-role alternation on replay)" );
					return out;
				}
			}

			const JsonValue& parts = content.get( "parts" );

			std::string text;
			std::vector<ChatToolCall> calls;
			std::vector<std::string> usedIds;   // every id captured this turn (provider + synthesized)
			int synthCounter = 0;
			const auto idUsed = [&usedIds]( const std::string& id ) {
				for( std::size_t k = 0; k < usedIds.size(); ++k )
					if( usedIds[k] == id ) return true;
				return false;
			};
			for( std::size_t i = 0; i < parts.size(); ++i ) {
				const JsonValue& part = parts.at( i );
				if( const JsonValue* t = part.find( "text" ) ) {
					if( t->isString() ) text += t->asString();
				}
				if( const JsonValue* fc = part.find( "functionCall" ) ) {
					if( !fc->isObject() ) {
						// WIRE-INVARIANT GATE: a functionCall-keyed part whose
						// value is not an object cannot become a pending call,
						// but it WOULD ride in the raw echo -- an un-answered
						// call on every later request.  Refuse the whole turn.
						out.step = MakeProviderError( ChatErrorKind::Provider,
							"gemini response carries a malformed functionCall part (not an object) -- refusing the turn" );
						return out;
					}
					// Gemini 3.x populates functionCall.id and the docs
					// require the response to echo the matching id --
					// capture it.  Synthesize "call_0", "call_1", ...
					// ONLY when absent (those match by name + order and
					// no id field is emitted in the functionResponse).
					ChatToolCall c;
					const JsonValue* idv = fc->find( "id" );
					if( idv && idv->isString() && !idv->asString().empty() ) {
						c.id = idv->asString();
						// Duplicate ids (a repeated provider id, or a provider
						// id colliding with one synthesized earlier this turn)
						// make result matching ambiguous -- refuse the whole
						// turn rather than guess (consistent with the other
						// record-or-refuse gates).
						if( idUsed( c.id ) ) {
							out.step = MakeProviderError( ChatErrorKind::Provider,
								"gemini response repeats functionCall id \"" + c.id +
								"\" -- refusing the turn (results could not be matched unambiguously)" );
							return out;
						}
					}
					else {
						// Skip any candidate that collides with an id already
						// captured this turn (e.g. a provider id literally
						// named "call_0").
						do {
							c.id = "call_" + std::to_string( synthCounter++ );
						} while( idUsed( c.id ) );
						c.idSynthesized = true;
					}
					usedIds.push_back( c.id );
					c.name = fc->get( "name" ).asString();
					const JsonValue& args = fc->get( "args" );
					c.argsJson = args.isObject() ? JsonSerialize( args ) : std::string( "{}" );
					calls.push_back( c );
				}
			}

			const std::string finishReason = cand.get( "finishReason" ).asString();
			// Classify a non-STOP finish for the error kind: token cap and
			// the safety-ish family get their own kinds so a driver can
			// react without string-matching.
			const ChatErrorKind finishKind =
				( finishReason == "MAX_TOKENS" ) ? ChatErrorKind::MaxTokens :
				( finishReason == "SAFETY" || finishReason == "RECITATION" ||
				  finishReason == "BLOCKLIST" || finishReason == "PROHIBITED_CONTENT" ||
				  finishReason == "SPII" || finishReason == "IMAGE_SAFETY" ) ? ChatErrorKind::Refusal :
				ChatErrorKind::Provider;
			if( !calls.empty() ) {
				// Mirror the Anthropic stop_reason gate: a call turn that
				// was cut short (MAX_TOKENS, SAFETY, ...) may be missing
				// calls or carry mangled arguments -- it must NOT execute.
				// POLICY on an ABSENT/empty finishReason: finishReason is
				// nominally optional in the proto, so TEXT-ONLY turns stay
				// lenient (below) -- but a calls-bearing turn without an
				// explicit STOP cannot be distinguished from one cut short
				// mid-call-list, and executing a truncated call set is
				// worse than refusing.  Require STOP before calls execute.
				if( finishReason != "STOP" ) {
					out.step = MakeProviderError( finishKind,
						finishReason.empty()
							? std::string( "gemini returned function calls without a finishReason -- "
							               "refusing (an explicit STOP is required before a call turn executes)" )
							: "gemini returned function calls but stopped with finishReason \"" +
							  finishReason + "\" -- a truncated call turn is not executed" );
					return out;
				}
				out.step.kind = ChatStepResult::Kind::ToolCalls;
				out.step.toolCalls = calls;
			}
			else if( finishReason == "STOP" || finishReason.empty() ) {
				if( !parts.isArray() || parts.size() == 0 ) {
					// A degenerate final turn with missing/empty content:
					// recording it would echo a partless (or parts:null)
					// model turn that poisons every later request.
					out.step = MakeProviderError( ChatErrorKind::Provider,
						"gemini candidate carries no content parts -- refusing the degenerate empty turn" );
					return out;
				}
				out.step.kind = ChatStepResult::Kind::FinalText;
				out.step.finalText = text;
			}
			else {
				// SAFETY / MAX_TOKENS / RECITATION / ... -> error with the
				// finishReason in the message.
				out.step = MakeProviderError( finishKind,
					"gemini stopped with finishReason \"" + finishReason + "\"" );
				return out;
			}
			out.assistantDisplayText = text;
			out.step.assistantDisplayText = text;

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
