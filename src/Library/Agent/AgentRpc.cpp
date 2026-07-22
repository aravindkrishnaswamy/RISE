//////////////////////////////////////////////////////////////////////
//
//  AgentRpc.cpp - the JSON-RPC 2.0 dispatch layer (see AgentRpc.h).
//
//  HandleLine is the single realization of the read-eval-print loop.  It
//  is written to be TOTAL: it parses the request, validates the JSON-RPC
//  envelope, dispatches to the mapped AgentSession call, and serializes a
//  response -- and it wraps the whole body in a try/catch so ANY escaped
//  exception (bad_alloc, a std::exception from a deep call) becomes a
//  -32603 internal-error response rather than crashing the loop.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "AgentRpc.h"

#include "AgentSession.h"
#include "AgentDiagnostic.h"
#include "Base64.h"
#include "Json.h"
#include "SchemaGen.h"

#include <cctype>   // P1-B: std::isspace for the whitespace-split camera-vector shape check
#include <cerrno>   // P1-B: errno for the strtod ERANGE overflow check
#include <cmath>
#include <cstdint>
#include <cstdio>   // preview-render: std::snprintf for the fov degrees-string conversion
#include <cstdlib>  // P1-B: std::strtod for the camera-vector component parse
#include <exception>
#include <string>
#include <vector>
#include "../Utilities/FiniteMath.h"

namespace RISE
{
	namespace Agent
	{
		namespace
		{
			// Standard JSON-RPC 2.0 error codes.
			const int kParseError     = -32700;
			const int kInvalidRequest = -32600;
			const int kMethodNotFound = -32601;
			const int kInvalidParams  = -32602;
			const int kInternalError  = -32603;

			// Secure-MCP slice 2: an app-range (unused-by-the-JSON-RPC-spec)
			// error code for a POLICY refusal under AgentAutonomy::Read --
			// deliberately distinct from every standard code above AND from
			// propose_patch's own "rejected"/"conflict" SUCCESS-result
			// shapes, so an agent cannot confuse "this session's launch
			// posture forbids mutation" with a retriable scene-state outcome.
			const int kAutonomyRefused = -32011;

			//! Secure-MCP slice 6 (limits hardening): a distinct app-range
			//! code for "the attached controller's pending-proposal queue is
			//! full" -- see SceneEditController::kMaxPendingProposals's doc.
			//! Deliberately its OWN code, not a reuse of kAutonomyRefused:
			//! this is a resource/backpressure refusal (the queue needs
			//! draining), not a policy refusal (the launch posture forbids
			//! the verb) -- a caller that wants to distinguish "relaunch
			//! with more authority" from "wait / ask the Owner to resolve
			//! some proposals" needs the two to be tellable apart
			//! programmatically, not just by message text.
			const int kProposalQueueFull = -32012;

			// NOTE: -32013 (kMutatingRateLimitExceeded) is the next code in
			// this app-range family -- reserved for and defined in
			// AgentLoopbackHttpServer.cpp, which enforces the mutating-verb
			// fixed-window rate limit at the HTTP TRANSPORT layer, before a
			// request ever reaches this dispatcher (see that file's doc for
			// why the limiter lives there instead of here). Not redeclared
			// in this file to avoid an unused-constant warning in a
			// translation unit that never triggers it.

			//! Secure-MCP slice 2 hardening: the gate is now DENY-BY-
			//! DEFAULT -- an explicit allowlist of the READ-SAFE verb
			//! names (read_document, read_schema, read_skill, validate,
			//! render, render_status, render_wait, render_cancel,
			//! read_image, read_viewport, list_proposals, query_object_at),
			//! the ONE list the choke point in HandleLine
			//! consults.  Anything NOT on this list -- including the 3
			//! known-mutating verbs (propose_patch, insert_chunk,
			//! remove_chunk) AND any FUTURE verb added to the dispatch
			//! below without also being added here -- is refused under
			//! AgentAutonomy::Read.  This is the deliberate polarity flip
			//! from the pre-hardening `IsMutatingVerb` allow-list-of-
			//! mutators: that shape was FAIL-OPEN (a new mutating verb #13
			//! would be silently PERMITTED under Read until someone
			//! remembered to add it to the mutating list).  Fail-closed
			//! means a new verb is refused-under-read by construction --
			//! the author must consciously classify it read-safe by
			//! adding it here, not merely forget to blacklist it.  See
			//! the AgentRpc.h file header for why `render` in particular
			//! is read-safe (it never mutates the retained Document).
			bool IsReadSafeVerb( const std::string& method )
			{
				return method == "read_document"  ||
				       method == "read_schema"     ||
				       method == "read_skill"      ||
				       method == "validate"        ||
				       method == "render"          ||
				       method == "render_status"   ||
				       method == "render_wait"     ||
				       method == "render_cancel"   ||
				       method == "read_image"      ||
				       // Toolkit slice 1: read_viewport is a PURE READ of the
				       // live interactive viewport pixels -- it never renders
				       // and never mutates the scene, so it belongs in the
				       // read-safe allowlist (available under every autonomy
				       // posture, including Read).
				       method == "read_viewport"   ||
				       // Secure-MCP slice 5b: list_proposals is a READ, not a
				       // mutation or a resolve -- it lists the SAME scene's
				       // staged-proposal queue a caller is already allowed to
				       // read via read_document.  Available under every
				       // autonomy posture, including Read (see AgentRpc.h's
				       // file header for the full read-exposure rationale:
				       // the loopback transport is already token-gated, so the
				       // proposal queue isn't secret to an authenticated
				       // co-editing client).
				       method == "list_proposals"  ||
				       // Toolkit slice 3b: query_object_at is a PURE READ --
				       // it never mutates the retained Document (the
				       // ephemeral objectmap render it reuses composes
				       // camera/dims overrides exactly like render's own,
				       // captured and restored) -- available under every
				       // autonomy posture, including Read, exactly like
				       // render itself.
				       method == "query_object_at" ||
				       // compare_to_reference is a PURE READ -- it renders
				       // (never mutates the retained Document, exactly like
				       // render itself) and grades against a HOST-registered
				       // reference image; available under every autonomy
				       // posture, including Read.
				       method == "compare_to_reference";
			}

			//! Secure-MCP slice 5b: the additional verbs `Propose` autonomy lets
			//! through beyond the read-safe allowlist above -- the 3 known-
			//! mutating verbs.  Dispatching them under Propose does NOT itself
			//! commit anything: it only lets the call REACH AgentSession, whose
			//! own Owner/External authority decides staging-vs-commit (see
			//! AgentRpc.h's file header, "Secure-MCP slice 5b (`Propose`
			//! autonomy)").  Deliberately excludes resolve_proposal -- see that
			//! doc for why it stays Commit-only.
			bool IsProposeSafeVerb( const std::string& method )
			{
				return method == "propose_patch" ||
				       method == "insert_chunk"  ||
				       method == "remove_chunk";
			}

			//! The severity token for a diagnostic.
			const char* SeverityName( AgentDiagnostic::Severity s )
			{
				switch( s ) {
					case AgentDiagnostic::Severity::Error:   return "error";
					case AgentDiagnostic::Severity::Warning: return "warning";
					case AgentDiagnostic::Severity::Info:    return "info";
				}
				return "error";
			}

			//! Build a JSON-RPC success envelope string for `result` under
			//! request id `id` (which is a raw JSON value: number, string, or
			//! null for a request that had no/invalid id).
			std::string MakeSuccess( const JsonValue& id, const JsonValue& result )
			{
				JsonValue env = JsonValue::MakeObject();
				env.set( "jsonrpc", JsonValue::MakeString( "2.0" ) );
				env.set( "id", id );
				env.set( "result", result );
				return JsonSerialize( env );
			}

			//! Build a JSON-RPC error envelope string.
			std::string MakeError( const JsonValue& id, int code, const std::string& message )
			{
				JsonValue err = JsonValue::MakeObject();
				err.set( "code", JsonValue::MakeNumber( static_cast<double>( code ) ) );
				err.set( "message", JsonValue::MakeString( message ) );

				JsonValue env = JsonValue::MakeObject();
				env.set( "jsonrpc", JsonValue::MakeString( "2.0" ) );
				env.set( "id", id );
				env.set( "error", err );
				return JsonSerialize( env );
			}

			//! Secure-MCP slice 2: build the AgentAutonomy::Read policy-
			//! refusal error envelope for mutating verb `verb` -- code
			//! kAutonomyRefused, a message naming the launch posture and how
			//! to escape it, and a structured `data` field {verb,
			//! autonomy:"read"} so a programmatic caller can branch on the
			//! refusal without string-matching the message.
			std::string MakeAutonomyRefusedError( const JsonValue& id, const std::string& verb )
			{
				JsonValue err = JsonValue::MakeObject();
				err.set( "code", JsonValue::MakeNumber( static_cast<double>( kAutonomyRefused ) ) );
				err.set( "message", JsonValue::MakeString(
					"refused: this session runs with --agent-autonomy=read; mutating verbs are "
					"unavailable (relaunch with --agent-autonomy=commit)" ) );
				JsonValue data = JsonValue::MakeObject();
				data.set( "verb", JsonValue::MakeString( verb ) );
				data.set( "autonomy", JsonValue::MakeString( "read" ) );
				err.set( "data", data );

				JsonValue env = JsonValue::MakeObject();
				env.set( "jsonrpc", JsonValue::MakeString( "2.0" ) );
				env.set( "id", id );
				env.set( "error", err );
				return JsonSerialize( env );
			}

			//! Secure-MCP slice 5b: the sibling refusal for AgentAutonomy::Propose
			//! -- used ONLY for resolve_proposal, the one verb deliberately left
			//! off Propose's extended allowlist (see IsProposeSafeVerb's doc and
			//! AgentRpc.h's file header).  Same shape as MakeAutonomyRefusedError
			//! above (kAutonomyRefused, structured {verb,autonomy} data) so a
			//! caller can branch on it identically; `autonomy:"propose"` and a
			//! message naming resolve_proposal specifically (an Owner-only verb,
			//! not something --agent-autonomy=commit alone would fix for an
			//! External session -- so this message does NOT suggest relaunching
			//! at commit, unlike the Read refusal, which would genuinely unblock
			//! the caller).
			std::string MakeProposeAutonomyRefusedError( const JsonValue& id, const std::string& verb )
			{
				JsonValue err = JsonValue::MakeObject();
				err.set( "code", JsonValue::MakeNumber( static_cast<double>( kAutonomyRefused ) ) );
				err.set( "message", JsonValue::MakeString(
					"refused: this session runs with --agent-autonomy=propose; resolve_proposal is "
					"Owner-only (an External/propose session may not resolve ANY proposal, including "
					"its own -- the document owner resolves it from their own Commit-posture session)" ) );
				JsonValue data = JsonValue::MakeObject();
				data.set( "verb", JsonValue::MakeString( verb ) );
				data.set( "autonomy", JsonValue::MakeString( "propose" ) );
				err.set( "data", data );

				JsonValue env = JsonValue::MakeObject();
				env.set( "jsonrpc", JsonValue::MakeString( "2.0" ) );
				env.set( "id", id );
				env.set( "error", err );
				return JsonSerialize( env );
			}

			//! Secure-MCP slice 6: the queue-full refusal error envelope for
			//! propose_patch/insert_chunk/remove_chunk -- built when the
			//! wrapped AgentSession's result carries queueFull==true (see
			//! AgentPatchResult::queueFull / AgentChunkResult::queueFull's
			//! doc). A distinct top-level JSON-RPC error (kProposalQueueFull),
			//! NOT the normal success-envelope result shape those three verbs
			//! otherwise always return -- same posture as
			//! MakeAutonomyRefusedError above: a resource-backpressure
			//! refusal must be tellable apart from a scene-state outcome
			//! (status="rejected"/"conflict"), not folded into it, so a
			//! programmatic caller can retry-after-resolve rather than
			//! mistake this for a permanent per-entity rejection.
			std::string MakeProposalQueueFullError( const JsonValue& id, const std::string& verb )
			{
				JsonValue err = JsonValue::MakeObject();
				err.set( "code", JsonValue::MakeNumber( static_cast<double>( kProposalQueueFull ) ) );
				err.set( "message", JsonValue::MakeString(
					"refused: the pending-proposal queue is full -- the Owner must resolve "
					"(approve/reject) some pending proposals before another can be staged" ) );
				JsonValue data = JsonValue::MakeObject();
				data.set( "verb", JsonValue::MakeString( verb ) );
				err.set( "data", data );

				JsonValue env = JsonValue::MakeObject();
				env.set( "jsonrpc", JsonValue::MakeString( "2.0" ) );
				env.set( "id", id );
				env.set( "error", err );
				return JsonSerialize( env );
			}

			//! Facet 5 slice 1a: a head-version as a nested JSON object
			//! {uuid:number, revision:number}.  JSON numbers are doubles, but a
			//! monotonic counter starting at 1 stays well under 2^53, so both
			//! fields are exactly representable -- emitting them as numbers is
			//! lossless (documented in the AgentRpc.h method index).
			JsonValue HeadVersionJson( const RISE::Cst::CstHeadVersion& hv )
			{
				JsonValue o = JsonValue::MakeObject();
				o.set( "uuid",     JsonValue::MakeNumber( static_cast<double>( hv.uuid ) ) );
				o.set( "revision", JsonValue::MakeNumber( static_cast<double>( hv.revision ) ) );
				return o;
			}

			//! Model-B F5 slice S2: parse the OPTIONAL `baseHeadVersion` param
			//! shared by propose_patch / insert_chunk / remove_chunk.  Returns
			//! 1 = present and valid (outBase filled), 0 = absent (or null --
			//! unconditional edit), -1 = malformed (outErr carries the -32602
			//! message).  The validation is the slice-1a contract verbatim:
			//! numeric uuid/revision, finite, non-negative,
			//! integral, <= 2^53 (the largest exactly-representable integer
			//! double; the monotonic-from-1 counters never approach it).
			int ParseBaseHeadVersionParam( const JsonValue& params,
			                               RISE::Cst::CstHeadVersion& outBase,
			                               std::string& outErr )
			{
				const JsonValue* bhv = params.find( "baseHeadVersion" );
				if( !bhv || bhv->isNull() ) return 0;
				if( !bhv->isObject() ) {
					outErr = "Invalid params: 'baseHeadVersion' must be an object {uuid,revision}";
					return -1;
				}
				const JsonValue* u = bhv->find( "uuid" );
				const JsonValue* rv = bhv->find( "revision" );
				if( !u || !u->isNumber() || !rv || !rv->isNumber() ) {
					outErr = "Invalid params: 'baseHeadVersion' needs numeric 'uuid' and 'revision'";
					return -1;
				}
				const double ud = u->asNumber();
				const double rd = rv->asNumber();
				if( !RISE::IsFiniteDouble( ud ) || !RISE::IsFiniteDouble( rd ) ||
					!( ud >= 0.0 && ud <= 9007199254740992.0 && ud == std::floor( ud ) &&
					   rd >= 0.0 && rd <= 9007199254740992.0 && rd == std::floor( rd ) ) ) {
					outErr = "Invalid params: 'baseHeadVersion' uuid/revision must be finite non-negative integers";
					return -1;
				}
				outBase.uuid     = static_cast<std::uint64_t>( ud );
				outBase.revision = static_cast<std::uint64_t>( rd );
				return 1;
			}

			//! Eval-harness hardening (local-model shootout, 2026-07-12): a
			//! REQUIRED-param error that only names the field it wanted gives
			//! a model that mis-named the parameter nothing to work from but
			//! a guess.  Observed root cause: llama3.3:70b sent insert_chunk's
			//! chunk body under the key 'chunk' instead of 'chunkText' and,
			//! after receiving "'chunkText' (string) is required" verbatim,
			//! retried with the SAME wrong key rather than the one the error
			//! named -- the message told it what was MISSING but not what it
			//! had actually SENT, so there was nothing to diff against.
			//! DescribeOtherParamKeys enumerates the top-level keys the
			//! params object actually carries, excluding the ones the caller
			//! says are legitimately expected, so the -32602 message can show
			//! them side by side with the required name.  This only NAMES the
			//! offending keys -- it never guesses which one was "meant" and
			//! the dispatcher never accepts a wrongly-named key in its place;
			//! see AgentChatLoop.cpp's MakeCodec (ChatProvider::Local) for
			//! why tolerant key-aliasing is out of scope here (it would mask
			//! a real model-capability signal the eval measures).
			std::string DescribeOtherParamKeys( const JsonValue& params,
			                                     const std::vector<std::string>& expectedKeys )
			{
				if( !params.isObject() ) return std::string();
				std::string extra;
				for( std::size_t i = 0; i < params.members().size(); ++i ) {
					const std::string& key = params.members()[i].first;
					bool known = false;
					for( std::size_t k = 0; k < expectedKeys.size(); ++k ) {
						if( key == expectedKeys[k] ) { known = true; break; }
					}
					if( known ) continue;
					if( !extra.empty() ) extra += ", ";
					extra += "'" + key + "'";
				}
				return extra;
			}

			//! Secure-MCP slice 6: the per-proposal echo cap list_proposals
			//! enforces on the `value` and `chunkText` fields -- see the
			//! call site's doc for the full rationale (bounds an unbounded-
			//! size caller-supplied payload from ballooning list_proposals'
			//! response, WITHOUT touching the stored proposal itself).
			const std::size_t kProposalFieldEchoCapBytes = 16u * 1024u;

			//! Clips `s` in place to AT MOST kProposalFieldEchoCapBytes bytes
			//! (never over the cap, but a few bytes under it is fine -- see
			//! the boundary walk-back below) and returns whether it was
			//! actually clipped. This IS a correctness issue, not merely a
			//! cosmetic one, for any strict UTF-8 consumer of list_proposals'
			//! JSON: a byte-boundary-only clip landing mid multi-byte
			//! sequence emits an invalid UTF-8 byte (a lone lead byte, or an
			//! orphaned continuation byte) inside a JSON string -- confirmed
			//! to blank the Mac owner-approval panel entirely (strict
			//! JSONSerialization decode failure on ChatViewModel's
			//! refreshProposals), i.e. an untrusted External client could
			//! deny the owner their review UI. After the byte-length resize,
			//! this walks the tail back to the nearest UTF-8 CHARACTER
			//! boundary and drops any trailing incomplete sequence, so the
			//! result is ALWAYS valid UTF-8. `truncated` is still true
			//! whenever ANY clip happened here (the byte-length clip, the
			//! boundary walk-back, or both) -- the caller only needs to know
			//! "this is a prefix, not the whole value"; the STORED proposal
			//! (read back in full on approval) is never touched by this
			//! function.
			bool ClipProposalFieldEcho( std::string& s )
			{
				if( s.size() <= kProposalFieldEchoCapBytes ) return false;
				s.resize( kProposalFieldEchoCapBytes );

				// UTF-8 boundary walk-back. Scan back at most 4 bytes (the
				// longest possible UTF-8 sequence) from the new tail,
				// skipping continuation bytes (10xxxxxx) until a non-
				// continuation byte is found. That byte is either ASCII
				// (0xxxxxxx, always a valid boundary on its own) or a lead
				// byte (110xxxxx/1110xxxx/11110xxx) whose declared sequence
				// length we compare against how many bytes actually remain
				// (`back`) -- if the sequence needs more than `back` bytes,
				// it was truncated mid-character, so drop it (and any
				// continuation bytes already walked past) by resizing back
				// to the lead byte's index. This only ever removes bytes
				// (never adds), so the result stays <= the cap.
				const std::size_t n = s.size();
				const std::size_t scanBack = ( n < 4u ) ? n : 4u;
				for( std::size_t back = 1; back <= scanBack; ++back )
				{
					const std::size_t idx = n - back;
					const unsigned char b = static_cast<unsigned char>( s[idx] );
					if( ( b & 0xC0 ) == 0x80 ) continue;   // continuation byte -- keep walking back

					std::size_t seqLen = 1;
					if( ( b & 0x80 ) == 0x00 )      seqLen = 1;   // ASCII
					else if( ( b & 0xE0 ) == 0xC0 ) seqLen = 2;
					else if( ( b & 0xF0 ) == 0xE0 ) seqLen = 3;
					else if( ( b & 0xF8 ) == 0xF0 ) seqLen = 4;
					else { s.resize( idx ); return true; }   // not a valid lead byte -- drop it and its trailing bytes too

					if( seqLen > back ) s.resize( idx );   // sequence runs off the clipped end -- drop the incomplete tail
					return true;
				}
				// All `scanBack` scanned bytes were continuation bytes --
				// more than the 3 that can ever trail a single lead byte,
				// so the input was already malformed here; drop the whole
				// scanned tail defensively rather than emit it as-is.
				s.resize( n - scanBack );
				return true;
			}

			//! Model-B F5 slice S2: serialize an AgentChunkResult (insert_chunk /
			//! remove_chunk share the shape: the propose_patch result fields plus
			//! the affected chunk's name/kind echo).
			JsonValue ChunkResultJson( const AgentChunkResult& cr )
			{
				JsonValue result = JsonValue::MakeObject();
				result.set( "applied",   JsonValue::MakeBool( cr.applied ) );
				result.set( "rawCode",   JsonValue::MakeNumber( static_cast<double>( cr.rawCode ) ) );
				result.set( "status",    JsonValue::MakeString( cr.status ) );
				result.set( "retriable", JsonValue::MakeBool( cr.retriable ) );
				result.set( "headVersion", HeadVersionJson( cr.headVersion ) );
				result.set( "message",   JsonValue::MakeString( cr.message ) );
				result.set( "name",      JsonValue::MakeString( cr.name ) );
				result.set( "kind",      JsonValue::MakeString( cr.kind ) );
				return result;
			}

			//! Model-B F2 slice S2b: serialize an AgentRenderResult into the
			//! SAME {ok,width,height,meanR,meanG,meanB,integrator,
			//! previewWidth,previewHeight,cameraOverridden,message,
			//! renderJobId} shape the synchronous `render` verb returns
			//! directly (see its handler below) -- shared so `render_wait`'s
			//! post-completion echo (S2b) is byte-for-byte the same field
			//! set a caller would get from a synchronous render, regardless
			//! of which path actually ran it.  `png` bytes are deliberately
			//! excluded (matches the sync handler's existing "render stays
			//! lean; read_image carries the base64 PNG" convention).
			JsonValue RenderResultJson( const AgentRenderResult& rr )
			{
				JsonValue result = JsonValue::MakeObject();
				result.set( "ok",     JsonValue::MakeBool( rr.ok ) );
				result.set( "width",  JsonValue::MakeNumber( static_cast<double>( rr.width ) ) );
				result.set( "height", JsonValue::MakeNumber( static_cast<double>( rr.height ) ) );
				result.set( "meanR",  JsonValue::MakeNumber( rr.meanR ) );
				result.set( "meanG",  JsonValue::MakeNumber( rr.meanG ) );
				result.set( "meanB",  JsonValue::MakeNumber( rr.meanB ) );
				result.set( "integrator", JsonValue::MakeString( rr.integrator ) );
				result.set( "previewWidth",  JsonValue::MakeNumber( static_cast<double>( rr.previewWidth ) ) );
				result.set( "previewHeight", JsonValue::MakeNumber( static_cast<double>( rr.previewHeight ) ) );
				result.set( "cameraOverridden", JsonValue::MakeBool( rr.cameraOverridden ) );
				result.set( "message", JsonValue::MakeString( rr.message ) );
				result.set( "renderJobId", JsonValue::MakeNumber( static_cast<double>( rr.renderJobId ) ) );
				// Model-B F2 slice S3 ADDITIVE wire fields -- see
				// AgentRenderResult::samplesOverridden's doc.
				result.set( "samplesOverridden", JsonValue::MakeBool( rr.samplesOverridden ) );
				result.set( "effectiveSamples", JsonValue::MakeNumber( static_cast<double>( rr.effectiveSamples ) ) );
				// Toolkit slice 2 ADDITIVE wire field -- see
				// AgentRenderResult::renderMode's doc.  "production" or
				// "draft"; distinct from `integrator`, which always names
				// the head's ACTIVE (production) rasterizer regardless of
				// which mode this render actually used.
				result.set( "renderMode", JsonValue::MakeString( rr.renderMode ) );
				// Toolkit slice 3a ADDITIVE wire field: the object-colour
				// `legend` of an OBJECTMAP render.  Emitted ONLY when this
				// render was an objectmap (renderMode=="objectmap") -- a
				// CONDITIONAL key, but sync `render` and `render_wait` both
				// serialize through THIS function, so their key sets stay
				// identical to each other (the S2b contract); the existing
				// render-result tests probe specific keys, never assert an
				// exact key set, so a beauty render simply omitting `legend`
				// keeps them green.  Each entry is {name,colorHex,pixelCount};
				// read the objectmap PNG at NATIVE size (read_image's maxEdge
				// box-downscale blends identity colours and breaks matching).
				if( rr.renderMode == "objectmap" ) {
					JsonValue legend = JsonValue::MakeArray();
					for( std::size_t i = 0; i < rr.legend.size(); ++i ) {
						JsonValue e = JsonValue::MakeObject();
						e.set( "name",       JsonValue::MakeString( rr.legend[i].name ) );
						e.set( "colorHex",   JsonValue::MakeString( rr.legend[i].colorHex ) );
						e.set( "pixelCount", JsonValue::MakeNumber( static_cast<double>( rr.legend[i].pixelCount ) ) );
						legend.push_back( e );
					}
					result.set( "legend", legend );
				}
				return result;
			}

			//! Parse a schema JSON STRING (from SchemaGen) into a JsonValue so
			//! it embeds as a nested object in the response, not a stringified
			//! blob.  On the (defensive) chance SchemaGen ever emits something
			//! that does not re-parse, fall back to a string value so the
			//! caller still gets the schema.
			JsonValue SchemaAsJson( const std::string& schemaText )
			{
				JsonValue parsed;
				std::string err;
				if( JsonParse( schemaText, parsed, err ) ) return parsed;
				return JsonValue::MakeString( schemaText );
			}

			//! Preview-render: parse an OPTIONAL numeric field into a
			//! CLAMPED unsigned int in [loClamp,hiClamp].  Returns 1 = present
			//! and valid (out filled, CLAMPED into range -- a caller-supplied
			//! out-of-range value is silently clamped rather than rejected, so
			//! an agent that guesses "1024" for a 512-max field still gets a
			//! usable preview instead of an error), 0 = absent/null, -1 =
			//! present but not a finite number (outErr carries the -32602
			//! message).
			int ParseClampedUInt( const JsonValue& params, const char* field,
			                      unsigned int loClamp, unsigned int hiClamp,
			                      unsigned int& out, std::string& outErr )
			{
				const JsonValue* v = params.find( field );
				if( !v || v->isNull() ) return 0;
				if( !v->isNumber() ) {
					outErr = std::string( "Invalid params: '" ) + field + "' must be a number";
					return -1;
				}
				const double d = v->asNumber();
				if( !RISE::IsFiniteDouble( d ) || !( d >= -2147483648.0 && d <= 2147483647.0 ) ) {
					outErr = std::string( "Invalid params: '" ) + field + "' must be a finite, in-range number";
					return -1;
				}
				double clamped = d;
				if( clamped < static_cast<double>( loClamp ) ) clamped = static_cast<double>( loClamp );
				if( clamped > static_cast<double>( hiClamp ) ) clamped = static_cast<double>( hiClamp );
				out = static_cast<unsigned int>( clamped );
				return 1;
			}

			//! P1-B: split `s` on ASCII whitespace and return the tokens.  No
			//! regex / no deps -- a plain hand-rolled scan matching the
			//! whitespace-separated numeric-triple convention used
			//! throughout the scene-file grammar (CameraIntrospection's own
			//! ParseVec3 uses sscanf's equivalent whitespace-skipping).
			std::vector<std::string> SplitWhitespace( const std::string& s )
			{
				std::vector<std::string> out;
				std::size_t i = 0;
				const std::size_t n = s.size();
				while( i < n ) {
					while( i < n && std::isspace( static_cast<unsigned char>( s[i] ) ) ) ++i;
					if( i >= n ) break;
					std::size_t j = i;
					while( j < n && !std::isspace( static_cast<unsigned char>( s[j] ) ) ) ++j;
					out.push_back( s.substr( i, j - i ) );
					i = j;
				}
				return out;
			}

			//! P1-B: validate that `token` parses as a single finite number
			//! (no trailing garbage).  Reject at the string layer before
			//! converting: no valid decimal or hexadecimal token contains n/i,
			//! and strtod reports ERANGE for an overflow or underflow.  Rejects
			//! empty tokens and tokens with unconsumed trailing characters (so
			//! "5abc" does not silently parse as 5).
			bool IsFiniteNumberToken( const std::string& token )
			{
				if( token.empty() ) return false;
				if( token.find_first_of( "nNiI" ) != std::string::npos ) return false;
				const char* start = token.c_str();
				char* end = nullptr;
				errno = 0;
				(void)std::strtod( start, &end );
				if( end != start + token.size() ) return false;   // trailing garbage
				if( errno == ERANGE ) return false;
				return true;
			}

			//! P1-B: validate a camera vector-field STRING SHAPE -- exactly
			//! 3 whitespace-separated finite numbers, matching what
			//! CameraIntrospection::SetProperty's ParseVec3 (sscanf "%lf %lf
			//! %lf") actually accepts.  Catches the false-observation bug at
			//! the wire boundary: "5 5" (2 tokens) or "abc def ghi"
			//! (non-numeric) used to sail through as a no-op override while
			//! the render result still reported cameraOverridden==true.
			//! Returns true + leaves outErr untouched on a valid shape;
			//! false + outErr set to a clean, field-naming message otherwise.
			bool ValidateVec3Shape( const std::string& value, const char* fieldName, std::string& outErr )
			{
				const std::vector<std::string> toks = SplitWhitespace( value );
				if( toks.size() != 3 ) {
					outErr = std::string( "Invalid params: '" ) + fieldName +
						"' must be a string of 3 numbers \"x y z\" (got " +
						std::to_string( toks.size() ) + " token(s))";
					return false;
				}
				for( std::size_t i = 0; i < toks.size(); ++i ) {
					if( !IsFiniteNumberToken( toks[i] ) ) {
						outErr = std::string( "Invalid params: '" ) + fieldName +
							"' must be a string of 3 numbers \"x y z\" (component " +
							std::to_string( i ) + " = \"" + toks[i] + "\" is not a finite number)";
						return false;
					}
				}
				return true;
			}

			//! Preview-render: parse the OPTIONAL `camera` override object
			//! `{location,lookat,up?,fov?}` (all string fields; location/lookat
			//! required together with the object, up/fov independently
			//! optional).  Returns 1 = present and valid (outOverride filled),
			//! 0 = absent/null (no override requested), -1 = malformed
			//! (outErr carries the -32602 message).
			//!
			//! P1-B: EVERY vector field's SHAPE is validated here (exactly 3
			//! finite numbers) before it ever reaches AgentSession -- a
			//! malformed vector (wrong token count, non-numeric component,
			//! trailing garbage) is a clean -32602 naming the field, not a
			//! silent no-op that still reports cameraOverridden==true.  `fov`
			//! is validated as a single finite number strictly inside the
			//! open interval (0, 180) degrees (0/180/negative/non-finite are
			//! all physically nonsensical field-of-view values).
			int ParseCameraOverrideParam( const JsonValue& params,
			                              AgentCameraOverride& outOverride,
			                              std::string& outErr )
			{
				const JsonValue* cam = params.find( "camera" );
				if( !cam || cam->isNull() ) return 0;
				if( !cam->isObject() ) {
					outErr = "Invalid params: 'camera' must be an object {location,lookat,up?,fov?}";
					return -1;
				}
				const JsonValue* loc = cam->find( "location" );
				const JsonValue* la  = cam->find( "lookat" );
				if( !loc || !loc->isString() || !la || !la->isString() ) {
					outErr = "Invalid params: 'camera' needs 'location' and 'lookat', each a string of 3 numbers \"x y z\" (e.g. \"0 5 10\")";
					return -1;
				}
				if( !ValidateVec3Shape( loc->asString(), "camera.location", outErr ) ) return -1;
				if( !ValidateVec3Shape( la->asString(),  "camera.lookat",   outErr ) ) return -1;
				outOverride.hasLocation = true;
				outOverride.location    = loc->asString();
				outOverride.hasLookAt   = true;
				outOverride.lookAt      = la->asString();
				if( const JsonValue* up = cam->find( "up" ) ) {
					if( up->isString() ) {
						if( !ValidateVec3Shape( up->asString(), "camera.up", outErr ) ) return -1;
						outOverride.hasUp = true;
						outOverride.up    = up->asString();
					}
					else if( !up->isNull() ) {
						outErr = "Invalid params: 'camera.up' must be a string of 3 numbers \"x y z\"";
						return -1;
					}
				}
				if( const JsonValue* fov = cam->find( "fov" ) ) {
					if( fov->isNumber() ) {
						const double fv = fov->asNumber();
						if( !RISE::IsFiniteDouble( fv ) || !( fv > 0.0 && fv < 180.0 ) ) {
							outErr = "Invalid params: 'camera.fov' must be in (0, 180) degrees";
							return -1;
						}
						char buf[64];
						std::snprintf( buf, sizeof( buf ), "%.6g", fv );
						outOverride.hasFov = true;
						outOverride.fov    = buf;
					} else if( !fov->isNull() ) {
						outErr = "Invalid params: 'camera.fov' must be a number (degrees)";
						return -1;
					}
				}
				return 1;
			}
		}

		AgentRpcDispatcher::AgentRpcDispatcher( std::unique_ptr<AgentSession> session,
		                                        AgentAutonomy autonomy )
			: mSession( std::move( session ) )
			, mAutonomy( autonomy )
		{
		}

		AgentRpcDispatcher::~AgentRpcDispatcher()
		{
		}

		std::string AgentRpcDispatcher::HandleLine( const std::string& jsonRpcRequest )
		{
			// The id defaults to null: a request that fails to parse (or whose
			// envelope is invalid before we can read an id) responds with
			// id=null, which is the JSON-RPC contract for un-attributable
			// errors.  Once we have read a valid id we echo it back.
			JsonValue idValue = JsonValue::MakeNull();

			try {
				// (1) Parse the line -> -32700 on malformation.
				JsonValue req;
				std::string parseErr;
				if( !JsonParse( jsonRpcRequest, req, parseErr ) ) {
					return MakeError( idValue, kParseError, "Parse error: " + parseErr );
				}

				// (2) Envelope must be an object -> else -32600.
				if( !req.isObject() ) {
					return MakeError( idValue, kInvalidRequest, "Invalid Request: not a JSON object" );
				}

				// (3) Echo the id if present (number / string / null are the
				// valid id types; anything else we treat as absent -> null).
				if( const JsonValue* id = req.find( "id" ) ) {
					if( id->isNumber() || id->isString() || id->isNull() ) idValue = *id;
				}

				// (4) `method` must be a string -> else -32600.
				const JsonValue* method = req.find( "method" );
				if( !method || !method->isString() ) {
					return MakeError( idValue, kInvalidRequest, "Invalid Request: missing or non-string 'method'" );
				}
				const std::string& m = method->asString();

				// (5) `params` is optional; when present it must be an object
				// (this set uses named params only).  Absent -> empty object.
				JsonValue params = JsonValue::MakeObject();
				if( const JsonValue* p = req.find( "params" ) ) {
					if( p->isObject() ) params = *p;
					else if( !p->isNull() )
						return MakeError( idValue, kInvalidParams, "Invalid params: 'params' must be an object" );
				}

				// (5b) Secure-MCP slice 2 hardening (extended by slice 5b): the
				// ONE choke point for the launch-time autonomy policy --
				// checked BEFORE any per-verb block, DENY-BY-DEFAULT against
				// the fixed allowlist of read-safe verb names (IsReadSafeVerb,
				// now 10 -- the original 9 plus list_proposals).  A verb that
				// is not on the read-safe list is refused under Read -- this
				// covers the 3 known-mutating verbs, resolve_proposal, AND any
				// future verb that reaches dispatch without being consciously
				// classified read-safe (the fail-closed property this
				// hardening exists for).  Under AgentAutonomy::Read this is a
				// POLICY refusal, never a scene-state outcome -- see
				// MakeAutonomyRefusedError's doc for why it is a distinct
				// JSON-RPC error code/shape rather than a "rejected"/
				// "conflict" success result.
				//
				// Secure-MCP slice 5b: AgentAutonomy::Propose extends the
				// read-safe set with the 3 mutating verbs (IsProposeSafeVerb)
				// -- letting them REACH AgentSession, whose own Owner/External
				// authority decides staging-vs-commit (see AgentRpc.h's file
				// header).  resolve_proposal is deliberately excluded from
				// Propose's extension -- it is refused here with the Propose-
				// specific message (MakeProposeAutonomyRefusedError) rather
				// than falling through to the generic Read-flavoured one,
				// since "relaunch with --agent-autonomy=commit" is not this
				// verb's actual escape hatch for an External session (only the
				// document owner, at their own Commit-posture session, can
				// resolve a proposal -- see AgentSession::ResolveProposal's
				// doc for the session-layer gate this mirrors).
				if( mAutonomy == AgentAutonomy::Read && !IsReadSafeVerb( m ) ) {
					return MakeAutonomyRefusedError( idValue, m );
				}
				if( mAutonomy == AgentAutonomy::Propose &&
				    !IsReadSafeVerb( m ) && !IsProposeSafeVerb( m ) ) {
					if( m == "resolve_proposal" ) return MakeProposeAutonomyRefusedError( idValue, m );
					return MakeAutonomyRefusedError( idValue, m );
				}

				AgentSession* s = mSession.get();

				//--------------------------------------------------------------
				// read_document -> {document:string, hasDocument:bool, headVersion:{uuid,revision}}
				//   No session (no-head bootstrap): an agent starting FRESH
				//   calls read_document first -- an error there is hostile.
				//   Return the honest, useful signal: an empty document with
				//   hasDocument=false + headVersion {0,0} (there is genuinely no
				//   head yet).  Facet 5 slice 1a: an agent reads headVersion here
				//   and passes it back as a patch's baseHeadVersion.
				//--------------------------------------------------------------
				if( m == "read_document" ) {
					JsonValue result = JsonValue::MakeObject();
					if( !s ) {
						result.set( "document", JsonValue::MakeString( "" ) );
						result.set( "hasDocument", JsonValue::MakeBool( false ) );
						result.set( "headVersion", HeadVersionJson( RISE::Cst::CstHeadVersion{} ) );
						return MakeSuccess( idValue, result );
					}
					result.set( "document", JsonValue::MakeString( s->ReadDocument() ) );
					result.set( "hasDocument", JsonValue::MakeBool( s->HasDocument() ) );
					result.set( "headVersion", HeadVersionJson( s->HeadVersion() ) );
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// read_schema {keyword?} -> the schema JSON (nested object)
				//   STATELESS: the schema is a pure descriptor-registry walk
				//   (SchemaGenAll / SchemaGenForChunk touch NO Job), so it needs
				//   NO loaded head -- an agent CONSTRUCTING a scene from scratch
				//   reads the grammar first.  We call SchemaGen directly rather
				//   than through the session so the no-head path works.
				//--------------------------------------------------------------
				if( m == "read_schema" ) {
					std::string keyword;
					if( const JsonValue* kw = params.find( "keyword" ) ) {
						if( kw->isString() ) keyword = kw->asString();
						else if( !kw->isNull() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'keyword' must be a string" );
					}
					// CHEAP LISTING mode (discovery-cost fix): {category:"<name>"}
					// with NO keyword returns just the keyword list (+ one-line
					// descriptions) of that category, NOT the ~300KB full dump.
					// `keyword` takes precedence if BOTH are supplied (a single
					// chunk is more specific than its category).
					std::string category;
					if( const JsonValue* cat = params.find( "category" ) ) {
						if( cat->isString() ) category = cat->asString();
						else if( !cat->isNull() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'category' must be a string" );
					}
					std::string schemaText;
					if( !keyword.empty() )       schemaText = RISE::Agent::SchemaGenForChunk( keyword );
					else if( !category.empty() ) schemaText = RISE::Agent::SchemaGenCategory( category );
					else                         schemaText = RISE::Agent::SchemaGenAll();
					JsonValue result = JsonValue::MakeObject();
					result.set( "schema", SchemaAsJson( schemaText ) );
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// read_skill {name?} -> the skills index / one skill's markdown
				//   STATELESS (like read_schema): skills are on-disk markdown,
				//   no Job involved -- so it needs NO loaded head and works in
				//   the no-head bootstrap.  Progressive disclosure: no `name`
				//   -> {skills:[{name,title,hook},...]} (the INDEX); a `name`
				//   -> {name, markdown}.  Path safety lives in
				//   AgentSession::ReadSkill (bare-name-only; '/', '\\', ".."
				//   rejected; only .md files inside the skills root are
				//   served); a rejected or unknown name maps to -32602 with
				//   the session's message.
				//--------------------------------------------------------------
				if( m == "read_skill" ) {
					std::string name;
					if( const JsonValue* nv = params.find( "name" ) ) {
						if( nv->isString() ) name = nv->asString();
						else if( !nv->isNull() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'name' must be a string" );
					}
					const AgentSkillResult sr = AgentSession::ReadSkill( name );
					if( !sr.ok ) {
						return MakeError( idValue, kInvalidParams, "Invalid params: " + sr.error );
					}
					JsonValue result = JsonValue::MakeObject();
					if( name.empty() ) {
						JsonValue arr = JsonValue::MakeArray();
						for( std::size_t i = 0; i < sr.index.size(); ++i ) {
							JsonValue e = JsonValue::MakeObject();
							e.set( "name",  JsonValue::MakeString( sr.index[i].name ) );
							e.set( "title", JsonValue::MakeString( sr.index[i].title ) );
							e.set( "hook",  JsonValue::MakeString( sr.index[i].hook ) );
							arr.push_back( e );
						}
						result.set( "skills", arr );
						// Index-only advisory: a missing skills ROOT (vs a
						// present-but-empty one) -- surfaced so an agent can tell
						// a miswired install from "no skills shipped".
						if( !sr.note.empty() )
							result.set( "note", JsonValue::MakeString( sr.note ) );
					}
					else {
						result.set( "name",     JsonValue::MakeString( sr.name ) );
						result.set( "markdown", JsonValue::MakeString( sr.markdown ) );
					}
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// validate {text} -> {diagnostics:[...]}
				//   STATELESS: validation parses `text` to a CST and derives it
				//   into a THROWAWAY Job (never a session's head), so it needs
				//   NO loaded head -- an agent REPAIRING a scene from scratch
				//   validates a candidate BEFORE any head exists.  We call the
				//   static ValidateText directly so the no-head path works.
				//--------------------------------------------------------------
				if( m == "validate" ) {
					const JsonValue* text = params.find( "text" );
					if( !text || !text->isString() ) {
						return MakeError( idValue, kInvalidParams, "Invalid params: 'text' (string) is required" );
					}
					const std::vector<AgentDiagnostic> diags = AgentSession::ValidateText( text->asString() );
					JsonValue arr = JsonValue::MakeArray();
					for( const AgentDiagnostic& d : diags ) {
						JsonValue dj = JsonValue::MakeObject();
						dj.set( "severity", JsonValue::MakeString( SeverityName( d.severity ) ) );
						dj.set( "code",     JsonValue::MakeString( d.code ) );
						dj.set( "message",  JsonValue::MakeString( d.message ) );
						dj.set( "offset",   JsonValue::MakeNumber( static_cast<double>( d.offset ) ) );
						dj.set( "length",   JsonValue::MakeNumber( static_cast<double>( d.length ) ) );
						arr.push_back( dj );
					}
					JsonValue result = JsonValue::MakeObject();
					result.set( "diagnostics", arr );
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// propose_patch {target,kind?,param,value,baseHeadVersion?}
				//   -> {applied,rawCode,status,retriable,headVersion,message}
				//   `applied` is CLEAN success only; `status` is the four-state
				//   gate {"applied","rejected","diagnosed","conflict"} (a rawCode-3
				//   re-derive is applied=false/status="diagnosed": mutated but
				//   the re-derive diagnosed -- NOT a clean apply; a stale
				//   baseHeadVersion is applied=false/status="conflict", head
				//   untouched).  REQUIRES a head (it edits the retained Document):
				//   no session -> error.  Facet 5 slice 1a: the OPTIONAL
				//   `baseHeadVersion` object {uuid:number,revision:number} is the
				//   optimistic-concurrency precondition -- when present, a mismatch
				//   with the current head returns status="conflict" WITHOUT
				//   mutating; absent -> unconditional (back-compat).  The result's
				//   `headVersion` is the head AFTER the call (post-commit on a
				//   clean apply; current head otherwise).  `retriable` splits the
				//   "rejected" bucket: true = TRANSIENT refusal (today: an open
				//   editor transaction in LIVE mode) -- resubmit the SAME patch
				//   later; false = permanent (retrying verbatim can never
				//   succeed).  A "conflict" is retriable-by-protocol via re-read
				//   + re-propose, so it does NOT set the flag.
				//--------------------------------------------------------------
				if( m == "propose_patch" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					const JsonValue* target = params.find( "target" );
					const JsonValue* param  = params.find( "param" );
					const JsonValue* value  = params.find( "value" );
					if( !target || !target->isString() ||
					    !param  || !param->isString()  ||
					    !value  || !value->isString() ) {
						return MakeError( idValue, kInvalidParams,
							"Invalid params: 'target', 'param', and 'value' (strings) are required" );
					}
					AgentSetPatch sp;
					sp.target = target->asString();
					sp.param  = param->asString();
					sp.value  = value->asString();
					if( const JsonValue* kind = params.find( "kind" ) ) {
						if( kind->isString() ) sp.kind = kind->asString();
						else if( !kind->isNull() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'kind' must be a string" );
					}
					// OPTIONAL baseHeadVersion: when present it MUST be an object
					// with numeric, finite, non-negative INTEGRAL uuid/revision
					// (else -32602); null is treated as absent (unconditional
					// edit).  Validation factored into ParseBaseHeadVersionParam
					// (Model-B F5 S2: insert_chunk / remove_chunk share it).
					{
						std::string bErr;
						const int b = ParseBaseHeadVersionParam( params, sp.baseVersion, bErr );
						if( b < 0 ) return MakeError( idValue, kInvalidParams, bErr );
						sp.hasBaseVersion = ( b == 1 );
					}
					const AgentPatchResult pr = s->ProposePatch( sp );
					// Secure-MCP slice 6: a queue-full refusal is a distinct
					// top-level JSON-RPC error, not the normal success-
					// envelope result shape -- see MakeProposalQueueFullError's
					// doc.
					if( pr.queueFull ) return MakeProposalQueueFullError( idValue, "propose_patch" );
					JsonValue result = JsonValue::MakeObject();
					result.set( "applied", JsonValue::MakeBool( pr.applied ) );
					result.set( "rawCode", JsonValue::MakeNumber( static_cast<double>( pr.rawCode ) ) );
					result.set( "status",  JsonValue::MakeString( pr.status ) );
					result.set( "retriable", JsonValue::MakeBool( pr.retriable ) );
					result.set( "headVersion", HeadVersionJson( pr.headVersion ) );
					result.set( "message", JsonValue::MakeString( pr.message ) );
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// insert_chunk {chunkText, baseHeadVersion?}
				//   -> {applied,rawCode,status,retriable,headVersion,message,name,kind}
				//   Model-B F5 slice S2: ADD one complete chunk to the head and
				//   REALIZE it via a dry-run-guarded full re-derive.  The result
				//   gates exactly like propose_patch ({"applied","rejected",
				//   "diagnosed","conflict"} + retriable) and echoes the parsed
				//   chunk's kind/name.  REQUIRES a head: no session -> error.
				//--------------------------------------------------------------
				if( m == "insert_chunk" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					const JsonValue* chunkText = params.find( "chunkText" );
					if( !chunkText || !chunkText->isString() ) {
						std::string msg = "Invalid params: 'chunkText' (string) is required";
						// See DescribeOtherParamKeys's doc: name whatever the
						// caller sent INSTEAD so a wrong-key mistake (e.g.
						// 'chunk') is visible in the SAME error round-trip,
						// not just what was missing.
						const std::string other = DescribeOtherParamKeys(
							params, { "chunkText", "baseHeadVersion" } );
						if( !other.empty() )
							msg += " (got " + other + " instead -- rename to 'chunkText')";
						return MakeError( idValue, kInvalidParams, msg );
					}
					RISE::Cst::CstHeadVersion base;
					std::string bErr;
					const int b = ParseBaseHeadVersionParam( params, base, bErr );
					if( b < 0 ) return MakeError( idValue, kInvalidParams, bErr );
					const AgentChunkResult cr =
						s->InsertChunk( chunkText->asString(), ( b == 1 ) ? &base : nullptr );
					// Secure-MCP slice 6: see propose_patch's identical
					// queue-full check above.
					if( cr.queueFull ) return MakeProposalQueueFullError( idValue, "insert_chunk" );
					return MakeSuccess( idValue, ChunkResultJson( cr ) );
				}

				//--------------------------------------------------------------
				// remove_chunk {target, kind?, baseHeadVersion?}
				//   -> {applied,rawCode,status,retriable,headVersion,message,name,kind}
				//   Model-B F5 slice S2: REMOVE the chunk resolved by bare name
				//   (+ optional kind narrowing, same resolution as propose_patch)
				//   via the trivia-preserving erase; a still-referenced target is
				//   rejected with the dry-run diagnostic, head byte-identical.
				//--------------------------------------------------------------
				if( m == "remove_chunk" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					const JsonValue* target = params.find( "target" );
					if( !target || !target->isString() ) {
						return MakeError( idValue, kInvalidParams,
							"Invalid params: 'target' (string) is required" );
					}
					std::string kind;
					if( const JsonValue* kv = params.find( "kind" ) ) {
						if( kv->isString() ) kind = kv->asString();
						else if( !kv->isNull() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'kind' must be a string" );
					}
					RISE::Cst::CstHeadVersion base;
					std::string bErr;
					const int b = ParseBaseHeadVersionParam( params, base, bErr );
					if( b < 0 ) return MakeError( idValue, kInvalidParams, bErr );
					const AgentChunkResult cr =
						s->RemoveChunk( target->asString(), kind, ( b == 1 ) ? &base : nullptr );
					// Secure-MCP slice 6: see propose_patch's identical
					// queue-full check above.
					if( cr.queueFull ) return MakeProposalQueueFullError( idValue, "remove_chunk" );
					return MakeSuccess( idValue, ChunkResultJson( cr ) );
				}

				//--------------------------------------------------------------
				// render {samples?,width?,height?,camera?} ->
				//   {ok,width,height,meanR,meanG,meanB,integrator,
				//    previewWidth,previewHeight,cameraOverridden,message,
				//    renderJobId}
				//   (NOT the image bytes -- render stays lean; read_image
				//    carries the base64 PNG.  `integrator` = the ACTIVE
				//    rasterizer's chunk keyword, empty when none is active.
				//    width/height/camera are the OPTIONAL preview-render
				//    overrides -- absent = today's exact behaviour.
				//    `renderJobId` (Model-B F2 slice S1, ADDITIVE) is a
				//    monotonically increasing id for this render -- see
				//    AgentRenderResult::renderJobId's doc for the LIVE
				//    (controller-tracked) vs headless (session-local)
				//    semantics.  Pre-S2 hardening: the two id spaces are
				//    disjoint BY PARITY -- coordinator-minted ids are always
				//    EVEN, session-local ids are always ODD -- so a future
				//    Status(jobId)/Wait(jobId) verb can reject a
				//    session-local id outright rather than aliasing it onto
				//    a coordinator job.  A FAILED render (ok:false) still
				//    carries a real renderJobId when the render actually
				//    ran -- this field names "a call that ran", not "a call
				//    that succeeded".  Fix-round-1 P3-d: {"async":true}
				//    REQUIRES a LIVE in-app controller (a running GUI
				//    session) -- `rise --agent-stdio` is headless (no
				//    SceneEditController exists in that process), so async
				//    is unreachable there by design; it refuses cleanly via
				//    AgentSession::RenderAsync's own "no controller
				//    attached" message rather than silently downgrading to
				//    a synchronous render.  Use the non-async `render` verb
				//    from `--agent-stdio`.)
				//--------------------------------------------------------------
				if( m == "render" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					int samples = -1;
					if( const JsonValue* sm = params.find( "samples" ) ) {
						if( sm->isNumber() ) {
							// Guard the cast: static_cast<int>(inf/nan) is UB.  A
							// hostile {"samples":1e999} parses to +inf.  The finite
							// test must survive the production -ffast-math build; the
							// bounds are exactly representable as double (2^31-1 and
							// -2^31).
							const double sv = sm->asNumber();
							if( !RISE::IsFiniteDouble( sv ) || !( sv >= -2147483648.0 && sv <= 2147483647.0 ) )
								return MakeError( idValue, kInvalidParams, "Invalid params: 'samples' must be a finite, in-range number" );
							samples = static_cast<int>( sv );
							// Model-B F2 slice S3 (EffectiveRenderConfig): -1
							// stays the "no override" sentinel (AgentRenderParams::
							// samples doc); anything else is CLAMPED into
							// [1,65536] rather than rejected (a caller's
							// out-of-range guess still renders, just at the
							// clamped count -- matches the width/height
							// ParseClampedUInt convention just below).  65536 is
							// a generous cap: no production RISE scene
							// authors anywhere near that SPP, but the clamp
							// exists to keep a hostile/typo'd huge value from
							// ballooning a single render's cost unboundedly.
							if( samples != -1 ) {
								if( samples < 1 ) samples = 1;
								else if( samples > 65536 ) samples = 65536;
							}
						}
						else if( !sm->isNull() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'samples' must be a number" );
					}

					// Preview-render dims: width/height are CLAMPED to [16,512]
					// (never rejected -- an out-of-range guess still renders,
					// just at the clamped size) and must be supplied TOGETHER
					// (one without the other is ambiguous: keep today's exact
					// behaviour -- no override -- rather than guess an aspect
					// ratio).
					AgentRenderParams rparams;
					rparams.samples = samples;
					unsigned int width = 0, height = 0;
					std::string dimErr;
					const int wPresent = ParseClampedUInt( params, "width",  16, 512, width,  dimErr );
					if( wPresent < 0 ) return MakeError( idValue, kInvalidParams, dimErr );
					const int hPresent = ParseClampedUInt( params, "height", 16, 512, height, dimErr );
					if( hPresent < 0 ) return MakeError( idValue, kInvalidParams, dimErr );
					if( wPresent == 1 && hPresent == 1 ) {
						rparams.width  = width;
						rparams.height = height;
					}

					AgentCameraOverride camOverride;
					std::string camErr;
					const int camPresent = ParseCameraOverrideParam( params, camOverride, camErr );
					if( camPresent < 0 ) return MakeError( idValue, kInvalidParams, camErr );
					if( camPresent == 1 ) rparams.camera = camOverride;

					// Model-B F2 slice S2a ADDITIVE param: {"async":true} ->
					// submit to the controller's dedicated agent-render worker
					// and return IMMEDIATELY with {renderJobId,status:"submitted"}
					// instead of blocking for the render's duration.  Absent or
					// false -> today's exact synchronous behaviour (unchanged).
					bool wantAsync = false;
					if( const JsonValue* av = params.find( "async" ) ) {
						if( av->isBool() ) wantAsync = av->asBool();
						else if( !av->isNull() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'async' must be a boolean" );
					}

					// Model-B F2 slice S3 ADDITIVE param: {"pinned":true} ->
					// this render (async or sync) is PINNED -- see
					// AgentRenderParams::pinned's doc for the single-slot
					// supersession-refusal policy this enables.  Absent or
					// false -> today's PREVIEW semantics, unchanged.
					bool wantPinned = false;
					if( const JsonValue* pv = params.find( "pinned" ) ) {
						if( pv->isBool() ) wantPinned = pv->asBool();
						else if( !pv->isNull() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'pinned' must be a boolean" );
					}
					rparams.pinned = wantPinned;

					// Toolkit slice 2 ADDITIVE param: {"quality":"draft"|
					// "production"} -> AgentRenderParams::quality.  Absent
					// or "production" is today's EXACT behaviour
					// (strictly additive); "draft" routes this ONE render
					// through the ephemeral studio-preview pipeline -- see
					// AgentRenderQuality's doc.  Any other string (or a
					// non-string, non-null value) is a clean -32602, not a
					// silent fall-through to production.
					if( const JsonValue* qv = params.find( "quality" ) ) {
						if( qv->isString() ) {
							const std::string qs = qv->asString();
							if( qs == "draft" ) {
								rparams.quality = AgentRenderQuality::Draft;
							} else if( qs == "production" ) {
								rparams.quality = AgentRenderQuality::Production;
							} else {
								return MakeError( idValue, kInvalidParams,
									"Invalid params: 'quality' must be \"draft\" or \"production\"" );
							}
						}
						else if( !qv->isNull() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'quality' must be a string" );
					}

					// Toolkit slice 3a / GUI render modes P1+P2a (docs/gui/RENDER_MODES.md
					// §8) ADDITIVE param: {"mode":"beauty"|"objectmap"|<a
					// casterFactory OR BeautyVariant registry mode --
					// "normals"|"depth"|"facets"|"wireframe"|"deep_reflect"|"direct"}
					// -> AgentRenderParams::renderTarget (+ ::viewMode for the
					// registry modes).  Absent or "beauty" is today's EXACT
					// behaviour (strictly additive); "objectmap" routes this ONE
					// render through the ephemeral identity pipeline and returns a
					// per-object `legend` in the result -- see AgentRenderTarget's
					// doc.  A casterFactory data-mode name routes through the
					// diagnostic ephemeral pipeline (CreateInteractiveViewModePipeline,
					// no legend); a BeautyVariant name (P2a) routes through the REAL
					// production-class ephemeral pipeline (CreateBeautyVariantPipeline,
					// also no legend) -- both still set renderTarget=ViewMode, since
					// from this parser's perspective they're both "one of the
					// registry's other agent-visible modes"; AgentSession::RenderCore_
					// branches internally on IsBeautyVariantMode.  `quality`/`samples`
					// are ignored under any of these (noted in the result message).
					// The accepted-name set is built FROM
					// Implementation::GetViewportRenderModes -- NOT a hardcoded list --
					// filtered to `casterFactory` OR BeautyVariant entries, so a
					// future mode is agent-visible by construction the moment it's
					// added to the registry (docs/gui/RENDER_MODES.md §4's parity
					// promise).  Any other string (or a non-string, non-null value)
					// is a clean -32602; the error message enumerates every
					// accepted name dynamically so it never drifts from the
					// registry.
					if( const JsonValue* mv = params.find( "mode" ) ) {
						if( mv->isString() ) {
							const std::string ms = mv->asString();
							if( ms == "objectmap" ) {
								rparams.renderTarget = AgentRenderTarget::ObjectMap;
							} else if( ms == "beauty" ) {
								rparams.renderTarget = AgentRenderTarget::Beauty;
							} else {
								unsigned int modeCount = 0;
								const Implementation::ViewportRenderModeInfo* modes =
									Implementation::GetViewportRenderModes( modeCount );
								const Implementation::ViewportRenderModeInfo* found = nullptr;
								for( unsigned int i = 0; i < modeCount; ++i ) {
									const bool agentVisible = modes[i].casterFactory
										|| Implementation::IsBeautyVariantMode( modes[i].mode );
									if( agentVisible && ms == modes[i].name ) {
										found = &modes[i];
										break;
									}
								}
								if( found ) {
									rparams.renderTarget = AgentRenderTarget::ViewMode;
									rparams.viewMode     = found->mode;
								} else {
									std::string accepted = "\"beauty\", \"objectmap\"";
									for( unsigned int i = 0; i < modeCount; ++i ) {
										if( modes[i].casterFactory || Implementation::IsBeautyVariantMode( modes[i].mode ) ) {
											accepted += ", \"";
											accepted += modes[i].name;
											accepted += "\"";
										}
									}
									return MakeError( idValue, kInvalidParams,
										"Invalid params: 'mode' must be one of " + accepted );
								}
							}
						}
						else if( !mv->isNull() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'mode' must be a string" );
					}

					// GUI render modes P2a `render{view:}` surface (docs/gui/
					// RENDER_MODES.md §8, deferred from P1) ADDITIVE param:
					// {"view":"<named view or scene camera name>"} ->
					// AgentRenderParams::view.  Valid with EVERY mode -- see
					// AgentRenderParams::view's doc.  Any non-string, non-null
					// value is a clean -32602; an unresolvable name is NOT
					// rejected here (that needs a live Job to check against) --
					// AgentSession::RenderCore_ fails the render itself with the
					// available-name list.
					if( const JsonValue* vv = params.find( "view" ) ) {
						if( vv->isString() ) {
							rparams.view = vv->asString();
						}
						else if( !vv->isNull() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'view' must be a string" );
					}

					// GUI render modes P2b `render{light:}` surface (docs/gui/
					// RENDER_MODES.md §3 "light solo", §9) ADDITIVE param:
					// {"light":"<light or emissive-object name>"} ->
					// AgentRenderParams::light.  Valid with "beauty" and the
					// four BeautyVariant mode names (deep_reflect/direct/
					// indirect/clay_lights) -- see AgentRenderParams::light's
					// doc.  Any non-string, non-null value is a clean -32602;
					// an unresolvable name is NOT rejected here (needs a live
					// Job to check against) -- AgentSession::RenderCore_ fails
					// the render itself with the available-name list, same
					// contract as an unresolvable `view`.
					if( const JsonValue* lv = params.find( "light" ) ) {
						if( lv->isString() ) {
							rparams.light = lv->asString();
						}
						else if( !lv->isNull() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'light' must be a string" );
					}

					// GUI render modes P1 (docs/gui/RENDER_MODES.md "X-ray axis")
					// ADDITIVE param: {"xray":false} -> AgentRenderParams::xray.
					// DEFAULT TRUE (2026-07-17 user decision) -- absent means
					// see-through, matching the viewport's own default; pass
					// {"xray":false} to inspect the transmissive surface
					// itself.  Meaningful ONLY under a view-mode `mode`
					// (normals/depth/facets/wireframe) -- supplying it under
					// "beauty"/"objectmap" is ACCEPTED and silently ignored
					// (same precedent as quality/samples under those targets),
					// honestly noted in the result message by AgentSession::
					// Render.  A non-bool, non-null value is a clean -32602.
					if( const JsonValue* xv = params.find( "xray" ) ) {
						if( xv->isBool() ) {
							rparams.xray = xv->asBool();
						}
						else if( !xv->isNull() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'xray' must be a boolean" );
					}

					if( wantAsync ) {
						const AgentSession::AgentRenderAsyncResult ar = s->RenderAsync( rparams );
						JsonValue result = JsonValue::MakeObject();
						result.set( "renderJobId", JsonValue::MakeNumber( static_cast<double>( ar.renderJobId ) ) );
						result.set( "status", JsonValue::MakeString( ar.accepted ? "submitted" : "refused" ) );
						result.set( "message", JsonValue::MakeString( ar.message ) );
						result.set( "pinned", JsonValue::MakeBool( ar.pinned ) );
						return MakeSuccess( idValue, result );
					}

					const AgentRenderResult rr = s->Render( rparams );
					// Model-B F2 slice S1 ADDITIVE wire field 'renderJobId':
					// see AgentRenderResult::renderJobId's doc for the LIVE
					// vs headless id semantics.  RenderResultJson (slice S2b)
					// is the SAME field-by-field shape this handler used to
					// build inline -- factored out so render_wait's
					// post-completion echo can return an IDENTICAL shape.
					return MakeSuccess( idValue, RenderResultJson( rr ) );
				}

				//--------------------------------------------------------------
				// render_status {renderJobId} -> {found,active}
				//   Model-B F2 slice S2a: poll the status of a render job id
				//   returned by `render` (async or sync).  `found`=false for an
				//   unrecognized id (headless session, or a session-local/ODD
				//   id -- see SceneEditController::WaitForRenderJob's parity
				//   contract); `active` is meaningful only when `found`=true.
				//--------------------------------------------------------------
				if( m == "render_status" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					const JsonValue* rj = params.find( "renderJobId" );
					if( !rj || !rj->isNumber() ) {
						return MakeError( idValue, kInvalidParams,
							"Invalid params: 'renderJobId' (number) is required" );
					}
					const double rv = rj->asNumber();
					// Same exact-double-integer bound as ParseBaseHeadVersionParam
					// above (2^53, the largest integer a double represents exactly) --
					// renderJobId is a small monotonic counter in practice, but the
					// guard must reject NaN/+inf/huge values BEFORE the narrowing
					// static_cast below (UB otherwise).
					if( !RISE::IsFiniteDouble( rv ) || !( rv >= 0.0 && rv <= 9007199254740992.0 ) )
						return MakeError( idValue, kInvalidParams, "Invalid params: 'renderJobId' must be a finite, non-negative number" );
					const std::uint64_t jobId = static_cast<std::uint64_t>( rv );
					const AgentSession::AgentRenderJobStatus st = s->RenderStatus( jobId );
					JsonValue result = JsonValue::MakeObject();
					result.set( "found",  JsonValue::MakeBool( st.found ) );
					result.set( "active", JsonValue::MakeBool( st.active ) );
					// Model-B F2 slice S3 ADDITIVE wire field -- see
					// AgentSession::AgentRenderJobStatus::pinned's doc.
					result.set( "pinned", JsonValue::MakeBool( st.pinned ) );
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// render_wait {renderJobId, timeoutMs?} ->
				//   {completed,found,active,result?}
				//   Model-B F2 slice S2a: block up to timeoutMs (default 5000,
				//   clamped [0,60000]) for the render job to complete.
				//   `completed`=true iff it was observed to finish (or was
				//   already finished) within the timeout; false on timeout or
				//   an unrecognized id.
				//   Model-B F2 slice S2b ADDITIVE field: `result`, present
				//   iff `completed` AND this session cached that job's full
				//   render stats (i.e. THIS renderJobId was the one most
				//   recently submitted via render{"async":true} on this
				//   session -- see AgentSession::LastAsyncRenderResult's
				//   strict-identity doc).  Same shape as the synchronous
				//   `render` verb's success result (RenderResultJson) --
				//   lets an async-driven caller retrieve an IDENTICAL
				//   contract to a synchronous render without a second call.
				//   Absent when the id belongs to a DIFFERENT session, a
				//   synchronous render (which already returned its result
				//   directly), or hasn't completed -- a caller that needs
				//   the stats and doesn't get `result` here has nothing
				//   further to poll for on this verb; this is additive and
				//   silently omitted rather than erroring, matching
				//   render_status/render_wait's existing "found=false is not
				//   an error" honesty convention.
				//--------------------------------------------------------------
				if( m == "render_wait" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					const JsonValue* rj = params.find( "renderJobId" );
					if( !rj || !rj->isNumber() ) {
						return MakeError( idValue, kInvalidParams,
							"Invalid params: 'renderJobId' (number) is required" );
					}
					const double rv = rj->asNumber();
					// Same exact-double-integer bound as ParseBaseHeadVersionParam
					// above (2^53, the largest integer a double represents exactly) --
					// renderJobId is a small monotonic counter in practice, but the
					// guard must reject NaN/+inf/huge values BEFORE the narrowing
					// static_cast below (UB otherwise).
					if( !RISE::IsFiniteDouble( rv ) || !( rv >= 0.0 && rv <= 9007199254740992.0 ) )
						return MakeError( idValue, kInvalidParams, "Invalid params: 'renderJobId' must be a finite, non-negative number" );
					const std::uint64_t jobId = static_cast<std::uint64_t>( rv );

					unsigned int timeoutMs = 5000;
					std::string toErr;
					const int toPresent = ParseClampedUInt( params, "timeoutMs", 0, 60000, timeoutMs, toErr );
					if( toPresent < 0 ) return MakeError( idValue, kInvalidParams, toErr );

					const bool completed = s->RenderWait( jobId, timeoutMs );
					const AgentSession::AgentRenderJobStatus st = s->RenderStatus( jobId );
					JsonValue result = JsonValue::MakeObject();
					result.set( "completed", JsonValue::MakeBool( completed ) );
					result.set( "found",  JsonValue::MakeBool( st.found ) );
					result.set( "active", JsonValue::MakeBool( st.active ) );
					// Model-B F2 slice S3 ADDITIVE wire field -- see
					// AgentSession::AgentRenderJobStatus::pinned's doc.
					result.set( "pinned", JsonValue::MakeBool( st.pinned ) );
					if( completed ) {
						const AgentSession::AgentLastAsyncRenderResult ar = s->LastAsyncRenderResult( jobId );
						if( ar.found ) result.set( "result", RenderResultJson( ar.result ) );
					}
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// render_cancel {renderJobId?} -> {cancelled,found,active}
				//   Model-B F2 slice S2b: trip the cancel signal for the
				//   OUTSTANDING async render, WITHOUT blocking for it to
				//   actually stop (the Swift chat driver's cancelTurn path
				//   uses this so a production render doesn't have to wait
				//   behind an agent render -- the single-slot worker would
				//   otherwise serialize them).  `renderJobId` is OPTIONAL and
				//   advisory only -- the controller's agent-render worker is
				//   single-slot, so there is at most one outstanding async
				//   render to cancel regardless of which id is named (see
				//   AgentSession::CancelAsyncRender's doc).  `cancelled` is
				//   true iff a live controller was attached to route the
				//   cancel through (mirrors AgentSession::CancelAsyncRender's
				//   no-op-when-headless contract -- it is NOT an error to
				//   call this with nothing outstanding, or from a headless
				//   `--agent-stdio` session; both report cancelled=false
				//   rather than an RPC error, matching render_status/
				//   render_wait's "found=false" honesty for an unrecognized
				//   or absent job).  `found`/`active` echo the CURRENT
				//   status of `renderJobId` immediately after the cancel
				//   request (found=false when renderJobId is absent/0 or
				//   unrecognized) so a caller can observe the pre-completion
				//   state in the same round-trip without a second call.
				//--------------------------------------------------------------
				if( m == "render_cancel" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					std::uint64_t jobId = 0;
					if( const JsonValue* rj = params.find( "renderJobId" ) ) {
						if( rj->isNumber() ) {
							const double rv = rj->asNumber();
							// Same exact-double-integer bound as render_status/
							// render_wait above.
							if( !RISE::IsFiniteDouble( rv ) || !( rv >= 0.0 && rv <= 9007199254740992.0 ) )
								return MakeError( idValue, kInvalidParams, "Invalid params: 'renderJobId' must be a finite, non-negative number" );
							jobId = static_cast<std::uint64_t>( rv );
						}
						else if( !rj->isNull() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'renderJobId' must be a number" );
					}

					const bool hadController = s->HasController();
					s->CancelAsyncRender( jobId );
					const AgentSession::AgentRenderJobStatus st =
						( jobId != 0 ) ? s->RenderStatus( jobId ) : AgentSession::AgentRenderJobStatus();
					JsonValue result = JsonValue::MakeObject();
					result.set( "cancelled", JsonValue::MakeBool( hadController ) );
					result.set( "found",  JsonValue::MakeBool( st.found ) );
					result.set( "active", JsonValue::MakeBool( st.active ) );
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// read_image {maxEdge?} -> {png_base64:string,byteLength:number,
				//                           width:number,height:number}
				//   Reads the LAST successful render's cached PNG bytes and
				//   base64-encodes them so the binary travels in JSON.
				//   maxEdge (OPTIONAL, clamped [16,1024]) downscales the
				//   returned image to that long-edge bound (box filter,
				//   aspect-preserving, never upscales) -- re-encoded from the
				//   cached full-resolution pixels, no re-render.
				//--------------------------------------------------------------
				if( m == "read_image" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					unsigned int maxEdge = 0;
					std::string meErr;
					const int mePresent = ParseClampedUInt( params, "maxEdge", 16, 1024, maxEdge, meErr );
					if( mePresent < 0 ) return MakeError( idValue, kInvalidParams, meErr );
					unsigned int imgW = 0, imgH = 0;
					const std::vector<unsigned char> png =
						( mePresent == 1 ) ? s->ReadImage( maxEdge, imgW, imgH )
						                   : s->ReadImage( 0, imgW, imgH );
					JsonValue result = JsonValue::MakeObject();
					result.set( "png_base64", JsonValue::MakeString( Base64Encode( png ) ) );
					result.set( "byteLength", JsonValue::MakeNumber( static_cast<double>( png.size() ) ) );
					result.set( "width",  JsonValue::MakeNumber( static_cast<double>( imgW ) ) );
					result.set( "height", JsonValue::MakeNumber( static_cast<double>( imgH ) ) );
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// read_viewport {maxEdge?} -> {available:bool,reason:string,
				//                              png_base64:string,byteLength:number,
				//                              width:number,height:number}
				//   Toolkit slice 1: the LIVE interactive GUI viewport's CURRENT
				//   pixels -- the exact frame the user is looking at right now,
				//   NOT the agent's own last render (contrast read_image, which
				//   returns THIS session's last headless render).  NEVER
				//   triggers a render -- it copies whatever the interactive
				//   render loop has most recently produced (the cheapest
				//   observe).  `available` is false with reason "no_controller"
				//   (headless session -- no viewport at all) or "no_frame_yet"
				//   (controller attached but no interactive frame produced yet);
				//   in both cases png_base64 is "" and byteLength/width/height
				//   are 0.  available:false is a STRUCTURED SUCCESS result, NOT
				//   a JSON-RPC error (the list_proposals precedent) -- only "no
				//   session loaded" is the usual MakeError gate.  maxEdge
				//   (OPTIONAL, clamped [16,1024]) downscales exactly as
				//   read_image's maxEdge does (box filter, aspect-preserving,
				//   never upscales), no re-render.
				//
				//   DELIBERATE CONTRAST with read_image: read_image returns a
				//   silent EMPTY image (png_base64 "", byteLength 0) with no
				//   availability flag when nothing has rendered; read_viewport
				//   instead carries an explicit available/reason pair because
				//   "no live viewport" and "an all-black frame" are genuinely
				//   different states a caller must distinguish.  Do NOT
				//   "harmonize" the two shapes -- the asymmetry is intentional.
				//--------------------------------------------------------------
				if( m == "read_viewport" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					unsigned int maxEdge = 0;
					std::string meErr;
					const int mePresent = ParseClampedUInt( params, "maxEdge", 16, 1024, maxEdge, meErr );
					if( mePresent < 0 ) return MakeError( idValue, kInvalidParams, meErr );
					unsigned int imgW = 0, imgH = 0;
					bool available = false;
					std::string reason;
					unsigned int srcPane = 0;   // user-review P1#3: captured atomically with the frame
					// user-review P1-3 (round 2): the WHOLE pane set is now
					// snapshotted ATOMICALLY with the frame inside ReadViewport's
					// parked window -- layout/primary/visibility/mode/vantage all
					// describe the SAME frame as the PNG.  The earlier code read
					// them back via DescribeViewportPanes AFTER the render resumed,
					// so they could describe a LATER state than the pixels.
					AgentSession::ViewportPanesInfo panesInfo;
					bool havePaneSet = false;
					const std::vector<unsigned char> png =
						s->ReadViewport( ( mePresent == 1 ) ? maxEdge : 0, imgW, imgH, available, reason, srcPane,
						                 panesInfo, havePaneSet );
					JsonValue result = JsonValue::MakeObject();
					result.set( "available",  JsonValue::MakeBool( available ) );
					result.set( "reason",     JsonValue::MakeString( reason ) );
					result.set( "png_base64", JsonValue::MakeString( Base64Encode( png ) ) );
					result.set( "byteLength", JsonValue::MakeNumber( static_cast<double>( png.size() ) ) );
					result.set( "width",  JsonValue::MakeNumber( static_cast<double>( imgW ) ) );
					result.set( "height", JsonValue::MakeNumber( static_cast<double>( imgH ) ) );
					// P3c (§7.8 ratified decision 3): pane-set introspection,
					// read-only.  `sourcePane` states WHICH pane the PNG came
					// from -- in a multi-pane layout the viewport read is the
					// last-rendered pane, and without this field the agent
					// cannot attribute the image (the review-r2-B honesty gap,
					// now closed structurally rather than by a note).
					if( havePaneSet )
					{
						JsonValue panesObj = JsonValue::MakeObject();
						panesObj.set( "layout",     JsonValue::MakeNumber( panesInfo.layout ) );
						panesObj.set( "primary",    JsonValue::MakeNumber( static_cast<double>( panesInfo.primary ) ) );
						panesObj.set( "sourcePane", JsonValue::MakeNumber( static_cast<double>( panesInfo.sourcePane ) ) );
						JsonValue arr = JsonValue::MakeArray();
						for( unsigned int i = 0; i < 4; ++i )
						{
							JsonValue pv = JsonValue::MakeObject();
							pv.set( "visible",     JsonValue::MakeBool( panesInfo.panes[i].visible ) );
							pv.set( "mode",        JsonValue::MakeString( panesInfo.panes[i].mode ) );
							pv.set( "vantageKind", JsonValue::MakeNumber( panesInfo.panes[i].vantageKind ) );
							pv.set( "namedView",   JsonValue::MakeString( panesInfo.panes[i].namedView ) );
							arr.push_back( pv );
						}
						panesObj.set( "panes", arr );
						result.set( "paneSet", panesObj );
					}
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// query_object_at {x,y,camera?,width?,height?} ->
				//   {hit,name,kind,pixelX,pixelY,width,height,message}
				//   Toolkit slice 3b: the cheap single-pixel companion to
				//   render mode:"objectmap" -- see AgentSession::
				//   QueryObjectAt's doc for the full contract (reuses the
				//   objectmap ephemeral pipeline wholesale; camera/width/
				//   height compose exactly like render's own overrides).
				//   `x`/`y` are REQUIRED integer pixel coordinates; an
				//   out-of-range (x,y) for the EFFECTIVE film dims is a
				//   clean -32602 (checked BEFORE the render runs), NOT a
				//   structured hit:false.
				//--------------------------------------------------------------
				if( m == "query_object_at" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );

					const JsonValue* xv = params.find( "x" );
					const JsonValue* yv = params.find( "y" );
					if( !xv || !xv->isNumber() || !yv || !yv->isNumber() ) {
						return MakeError( idValue, kInvalidParams,
							"Invalid params: 'x' and 'y' (numbers) are required" );
					}
					const double xd = xv->asNumber();
					const double yd = yv->asNumber();
					// Guard the narrowing casts below against a hostile/typo'd
					// 1e999 or NaN.
					if( !RISE::IsFiniteDouble( xd ) || !RISE::IsFiniteDouble( yd ) ||
						!( xd >= -2147483648.0 && xd <= 2147483647.0 ) ||
					    !( yd >= -2147483648.0 && yd <= 2147483647.0 ) )
					{
						return MakeError( idValue, kInvalidParams,
							"Invalid params: 'x' and 'y' must be finite, in-range numbers" );
					}
					const int qx = static_cast<int>( xd );
					const int qy = static_cast<int>( yd );

					// Same width/height/camera override composition as
					// render (ParseClampedUInt/ParseCameraOverrideParam are
					// the SAME helpers render's dispatch uses just above).
					AgentQueryObjectParams qparams;
					unsigned int qw = 0, qh = 0;
					std::string qDimErr;
					const int qwPresent = ParseClampedUInt( params, "width",  16, 512, qw, qDimErr );
					if( qwPresent < 0 ) return MakeError( idValue, kInvalidParams, qDimErr );
					const int qhPresent = ParseClampedUInt( params, "height", 16, 512, qh, qDimErr );
					if( qhPresent < 0 ) return MakeError( idValue, kInvalidParams, qDimErr );
					if( qwPresent == 1 && qhPresent == 1 ) {
						qparams.width  = qw;
						qparams.height = qh;
					}

					AgentCameraOverride qCamOverride;
					std::string qCamErr;
					const int qCamPresent = ParseCameraOverrideParam( params, qCamOverride, qCamErr );
					if( qCamPresent < 0 ) return MakeError( idValue, kInvalidParams, qCamErr );
					if( qCamPresent == 1 ) qparams.camera = qCamOverride;

					const AgentSession::AgentQueryObjectResult qr = s->QueryObjectAt( qx, qy, qparams );
					if( qr.outOfRange ) {
						return MakeError( idValue, kInvalidParams,
							qr.message.empty()
								? "Invalid params: 'x'/'y' out of range for the effective film dims"
								: qr.message );
					}
					JsonValue result = JsonValue::MakeObject();
					result.set( "hit",     JsonValue::MakeBool( qr.hit ) );
					result.set( "name",    JsonValue::MakeString( qr.name ) );
					result.set( "kind",    JsonValue::MakeString( qr.kind ) );
					result.set( "pixelX",  JsonValue::MakeNumber( static_cast<double>( qr.pixelX ) ) );
					result.set( "pixelY",  JsonValue::MakeNumber( static_cast<double>( qr.pixelY ) ) );
					result.set( "width",   JsonValue::MakeNumber( static_cast<double>( qr.width ) ) );
					result.set( "height",  JsonValue::MakeNumber( static_cast<double>( qr.height ) ) );
					result.set( "message", JsonValue::MakeString( qr.message ) );
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// compare_to_reference {reference,camera?,visual?,samples?,split?,splitObjects?} ->
				//   {rmse,channelDelta:{r,g,b},grid:[{rmse,dr,dg,db},x9],
				//    worstCell,width,height,reference,summary,
				//    png_base64?,compositeWidth?,compositeHeight?,
				//    split?:{ok,objectRmse,backgroundRmse,objectPixelFraction,note}}
				//   The reconstruction feedback instrument: see AgentRpc.h's
				//   file-header doc for the full contract.  `reference`
				//   (REQUIRED, non-empty string) names a HOST-registered
				//   image (AgentSession::SetReferenceImages) -- an unknown
				//   name is a clean -32602 naming every registered
				//   reference (AgentSession::AgentCompareToReferenceResult::
				//   badReference distinguishes this from every other
				//   failure, which maps to -32603).  `camera`/`visual`/
				//   `samples` compose exactly as AgentCompareToReferenceParams
				//   documents.  The composite image, when present, is
				//   returned under the SAME "png_base64" field name
				//   read_image uses -- deliberately, so the chat-loop's
				//   image retention/elision policy (which keys off that
				//   literal field name) applies to it identically.  `split`
				//   (OPTIONAL bool, default false) requests the object-vs-
				//   background RMSE breakdown -- see
				//   AgentSession::AgentCompareSplitResult's doc for the
				//   candidate-objectmap mask mechanism and its honesty
				//   caveat.  The "split" result key is OMITTED entirely
				//   when `split` was false or absent (back-compat).
				//   `splitObjects` (OPTIONAL array of strings, default empty,
				//   only meaningful alongside `split:true`) SCOPES the
				//   OBJECT bucket to just the named registered object(s) --
				//   see AgentCompareToReferenceParams::splitObjects' doc for
				//   why: WITHOUT it, a scene's own ground plane / backdrop /
				//   any other staging geometry counts as OBJECT too (they
				//   are registered objects like any other), so to get a
				//   true hero-object-vs-staging reading, scope this to the
				//   hero object's name.  Each element must be a string --
				//   a non-array or a non-string element is a clean -32602.
				//   A requested name absent from the candidate's objectmap
				//   legend is never a hard failure: it is dropped from the
				//   mask and surfaced in `split.note` instead (see
				//   AgentSession::CompareToReference's split block).
				//--------------------------------------------------------------
				if( m == "compare_to_reference" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );

					const JsonValue* refVal = params.find( "reference" );
					if( !refVal || !refVal->isString() || refVal->asString().empty() ) {
						return MakeError( idValue, kInvalidParams,
							"Invalid params: 'reference' (a non-empty string) is required" );
					}

					AgentCompareToReferenceParams cparams;
					cparams.reference = refVal->asString();

					if( const JsonValue* vv = params.find( "visual" ) ) {
						if( vv->isBool() ) cparams.visual = vv->asBool();
						else if( !vv->isNull() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'visual' must be a boolean" );
					}

					if( const JsonValue* sv = params.find( "samples" ) ) {
						if( sv->isNumber() ) {
							// Same explicit finite-range guard idiom as every
							// other numeric parse in this file (NOT
							// std::isfinite -- dead code under
							// -ffinite-math-only; see the 'samples' parse in
							// the render dispatch above).
							const double sd = sv->asNumber();
							if( !( sd >= -2147483648.0 && sd <= 2147483647.0 ) )
								return MakeError( idValue, kInvalidParams, "Invalid params: 'samples' must be a finite, in-range number" );
							int samples = static_cast<int>( sd );
							if( samples < 1 ) samples = 1;
							else if( samples > 65536 ) samples = 65536;
							cparams.samples = samples;
						}
						else if( !sv->isNull() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'samples' must be a number" );
					}

					if( const JsonValue* spv = params.find( "split" ) ) {
						if( spv->isBool() ) cparams.split = spv->asBool();
						else if( !spv->isNull() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'split' must be a boolean" );
					}

					if( const JsonValue* sov = params.find( "splitObjects" ) ) {
						if( sov->isArray() ) {
							for( std::size_t i = 0; i < sov->size(); ++i ) {
								const JsonValue& el = sov->at( i );
								if( !el.isString() )
									return MakeError( idValue, kInvalidParams,
										"Invalid params: 'splitObjects' must be an array of strings" );
								cparams.splitObjects.push_back( el.asString() );
							}
						}
						else if( !sov->isNull() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'splitObjects' must be an array of strings" );
					}

					AgentCameraOverride crCamOverride;
					std::string crCamErr;
					const int crCamPresent = ParseCameraOverrideParam( params, crCamOverride, crCamErr );
					if( crCamPresent < 0 ) return MakeError( idValue, kInvalidParams, crCamErr );
					if( crCamPresent == 1 ) cparams.camera = crCamOverride;

					const AgentCompareToReferenceResult cr = s->CompareToReference( cparams );
					if( !cr.ok ) {
						return MakeError( idValue, cr.badReference ? kInvalidParams : kInternalError,
							cr.error.empty() ? "compare_to_reference failed" : cr.error );
					}

					JsonValue channelDelta = JsonValue::MakeObject();
					channelDelta.set( "r", JsonValue::MakeNumber( cr.channelDeltaR ) );
					channelDelta.set( "g", JsonValue::MakeNumber( cr.channelDeltaG ) );
					channelDelta.set( "b", JsonValue::MakeNumber( cr.channelDeltaB ) );

					JsonValue grid = JsonValue::MakeArray();
					for( const AgentCompareGridCell& cell : cr.grid ) {
						JsonValue cj = JsonValue::MakeObject();
						cj.set( "rmse", JsonValue::MakeNumber( cell.rmse ) );
						cj.set( "dr",   JsonValue::MakeNumber( cell.dr ) );
						cj.set( "dg",   JsonValue::MakeNumber( cell.dg ) );
						cj.set( "db",   JsonValue::MakeNumber( cell.db ) );
						grid.push_back( cj );
					}

					JsonValue result = JsonValue::MakeObject();
					result.set( "rmse",         JsonValue::MakeNumber( cr.rmse ) );
					result.set( "channelDelta", channelDelta );
					result.set( "grid",         grid );
					result.set( "worstCell",    JsonValue::MakeString( cr.worstCell ) );
					result.set( "width",        JsonValue::MakeNumber( static_cast<double>( cr.width ) ) );
					result.set( "height",       JsonValue::MakeNumber( static_cast<double>( cr.height ) ) );
					result.set( "reference",    JsonValue::MakeString( cr.reference ) );
					result.set( "summary",      JsonValue::MakeString( cr.summary ) );
					if( !cr.compositePng.empty() ) {
						result.set( "png_base64",       JsonValue::MakeString( Base64Encode( cr.compositePng ) ) );
						result.set( "compositeWidth",   JsonValue::MakeNumber( static_cast<double>( cr.compositeWidth ) ) );
						result.set( "compositeHeight",  JsonValue::MakeNumber( static_cast<double>( cr.compositeHeight ) ) );
					}
					// split:true -- the object-vs-background RMSE breakdown
					// (AgentCompareSplitResult).  Omitted entirely when the
					// request's `split` was false (back-compat) -- see
					// AgentSession::AgentCompareToReferenceResult::hasSplit's
					// doc.  Present (with a `note`, possibly `ok:false`) on a
					// failed/degenerate split -- never a crash, never fails
					// the overall compare.
					if( cr.hasSplit ) {
						JsonValue split = JsonValue::MakeObject();
						split.set( "ok",                  JsonValue::MakeBool( cr.split.ok ) );
						split.set( "objectRmse",          JsonValue::MakeNumber( cr.split.objectRmse ) );
						split.set( "backgroundRmse",      JsonValue::MakeNumber( cr.split.backgroundRmse ) );
						split.set( "objectPixelFraction", JsonValue::MakeNumber( cr.split.objectPixelFraction ) );
						split.set( "note",                JsonValue::MakeString( cr.split.note ) );
						result.set( "split", split );
					}
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// list_proposals {} -> {proposals:[{id,kind,target,entityKind,
				//                        param,value,chunkText,truncated,
				//                        baseVersion,sessionLabel,status},...]}
				//   Secure-MCP slice 5b: the queue of every AgentProposal staged
				//   on the ATTACHED controller (pending + resolved -- resolved
				//   entries stay for audit).  READ-SAFE (see IsReadSafeVerb):
				//   available under every autonomy posture.  CONTROLLER-
				//   ATTACHED ONLY: a headless session reports an empty array,
				//   not an error.  REQUIRES a session (the usual "no session
				//   loaded" internal error otherwise, matching every other
				//   session-backed verb in this dispatch).
				//
				//   Secure-MCP slice 6: `value` and `chunkText` are each
				//   clipped to kProposalFieldEchoCapBytes (16 KiB) in THIS
				//   LISTING ONLY -- a caller can stage an arbitrarily large
				//   propose_patch `value` or insert_chunk/remove_chunk
				//   `chunkText` (there is no size cap on those params
				//   themselves), and an unbounded echo of every such payload
				//   on every list_proposals call would let a queue of a few
				//   large proposals balloon this response arbitrarily. The
				//   STORED proposal (what ResolveProposal replays on
				//   approval) is NEVER touched -- s->ListProposals() above
				//   already returned the full, untouched text; the clip
				//   happens here, on a local copy, purely for the wire echo.
				//   `truncated` (additive) is true iff EITHER field was
				//   actually clipped for this entry, so a client can tell "this
				//   is a prefix" apart from "this is the whole value" without
				//   comparing byte lengths itself.
				//--------------------------------------------------------------
				if( m == "list_proposals" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					const std::vector<AgentSession::AgentProposalEntry> proposals = s->ListProposals();
					JsonValue arr = JsonValue::MakeArray();
					for( const AgentSession::AgentProposalEntry& p : proposals ) {
						std::string valueEcho = p.value;
						std::string chunkTextEcho = p.chunkText;
						const bool valueClipped = ClipProposalFieldEcho( valueEcho );
						const bool chunkClipped = ClipProposalFieldEcho( chunkTextEcho );

						JsonValue pj = JsonValue::MakeObject();
						pj.set( "id",           JsonValue::MakeNumber( static_cast<double>( p.id ) ) );
						pj.set( "kind",         JsonValue::MakeString( p.kind ) );
						pj.set( "target",       JsonValue::MakeString( p.target ) );
						pj.set( "entityKind",   JsonValue::MakeString( p.entityKind ) );
						pj.set( "param",        JsonValue::MakeString( p.param ) );
						pj.set( "value",        JsonValue::MakeString( valueEcho ) );
						pj.set( "chunkText",    JsonValue::MakeString( chunkTextEcho ) );
						pj.set( "truncated",    JsonValue::MakeBool( valueClipped || chunkClipped ) );
						pj.set( "baseVersion",  HeadVersionJson( p.baseVersion ) );
						pj.set( "sessionLabel", JsonValue::MakeString( p.sessionLabel ) );
						pj.set( "status",       JsonValue::MakeString( p.status ) );
						arr.push_back( pj );
					}
					JsonValue result = JsonValue::MakeObject();
					result.set( "proposals", arr );
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// resolve_proposal {proposalId, approve:bool} ->
				//   {resolved:bool, status:string, headVersion, message}
				//   Secure-MCP slice 5b: approve or reject a staged proposal.
				//   OWNER-ONLY -- routes to AgentSession::ResolveProposal, which
				//   refuses (resolved=false) for a non-Owner-authority session,
				//   including one resolving its own proposal.  NOT on the
				//   read-safe allowlist (refused under Read) and deliberately
				//   NOT on Propose's extension (refused under Propose too, with
				//   a Propose-specific message -- see the autonomy choke point
				//   above); reachable only under Commit, the posture the
				//   in-process Owner dispatcher runs at.
				//--------------------------------------------------------------
				if( m == "resolve_proposal" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					const JsonValue* pid = params.find( "proposalId" );
					const JsonValue* appr = params.find( "approve" );
					if( !pid || !pid->isNumber() || !appr || !appr->isBool() ) {
						return MakeError( idValue, kInvalidParams,
							"Invalid params: 'proposalId' (number) and 'approve' (boolean) are required" );
					}
					const double pidD = pid->asNumber();
					// Same exact-double-integer bound as every other id/version
					// field in this file (2^53, the largest exactly-representable
					// integer double) -- guards the narrowing cast below against
					// UB on a hostile/huge value.
					if( !RISE::IsFiniteDouble( pidD ) || !( pidD >= 0.0 && pidD <= 9007199254740992.0 ) ) {
						return MakeError( idValue, kInvalidParams,
							"Invalid params: 'proposalId' must be a finite, non-negative number" );
					}
					const std::uint64_t proposalId = static_cast<std::uint64_t>( pidD );
					const bool approve = appr->asBool();

					const AgentSession::AgentResolveResult rr = s->ResolveProposal( proposalId, approve );
					JsonValue result = JsonValue::MakeObject();
					result.set( "resolved", JsonValue::MakeBool( rr.ok ) );
					result.set( "status",   JsonValue::MakeString( rr.status ) );
					// headVersion: exactly one of paramResult/chunkResult is
					// populated on any REAL resolve -- approve, reject, OR
					// conflict (AgentSession::ResolveProposal fills whichever
					// shape matches the proposal's kind for all three
					// outcomes as of the Secure-MCP slice 5b fix round P2-2;
					// see its doc) -- `status` is only set on the populated
					// one (both default to "", never a real status string),
					// so its non-emptiness cleanly selects which struct to
					// read.  A refusal (rr.ok == false -- e.g. an unknown id,
					// or a non-Owner session) leaves BOTH empty, so this
					// falls through to chunkResult's default {0,0} -- the
					// honest "no head to report" case, distinct from a real
					// reject's now-populated, non-zero current head.
					const RISE::Cst::CstHeadVersion hv =
						!rr.paramResult.status.empty() ? rr.paramResult.headVersion
						                                : rr.chunkResult.headVersion;
					result.set( "headVersion", HeadVersionJson( hv ) );
					result.set( "message", JsonValue::MakeString( rr.message ) );
					return MakeSuccess( idValue, result );
				}

				// (6) Anything else -> method not found.
				return MakeError( idValue, kMethodNotFound, "Method not found: " + m );
			}
			catch( const std::exception& e ) {
				return MakeError( idValue, kInternalError, std::string( "internal error: " ) + e.what() );
			}
			catch( ... ) {
				return MakeError( idValue, kInternalError, "internal error: unknown exception" );
			}
		}
	}
}
