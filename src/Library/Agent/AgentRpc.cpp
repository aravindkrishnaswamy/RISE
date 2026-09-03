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
			//! names, ENUMERATED IN THE FUNCTION BODY IMMEDIATELY BELOW
			//! (deliberately NOT restated here: a prose copy of a list
			//! three lines away is pure drift surface, and it drifted --
			//! review rounds 13 and 17 both landed on a stale copy of it.
			//! The remote restatements that DO earn their keep, in
			//! AgentRpc.h where the reader cannot see this body, are
			//! machine-checked against these two function bodies by
			//! tests/SourceHygieneTest).  This body is the ONE list the
			//! choke point in HandleLine consults.  Anything NOT on it --
			//! including the mutating verbs enumerated by
			//! IsProposeSafeVerb below, AND any FUTURE verb added to the
			//! dispatch without also being added here -- is refused under
			//! AgentAutonomy::Read.  This is the deliberate polarity flip
			//! from the pre-hardening `IsMutatingVerb` allow-list-of-
			//! mutators: that shape was FAIL-OPEN (a NEW mutating verb
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
				       // Arc 80 (2026-08-12): scene_inventory is a PURE READ by
				       // exactly query_object_at's test -- it runs the same
				       // ephemeral identity render and never touches the
				       // retained Document.
				       method == "scene_inventory" ||
				       // compare_to_reference is a PURE READ -- it renders
				       // (never mutates the retained Document, exactly like
				       // render itself) and grades against a HOST-registered
				       // reference image; available under every autonomy
				       // posture, including Read.
				       method == "compare_to_reference" ||
				       // S1 (2026-08-11): the two staged-build-protocol verbs are
				       // read-safe on exactly the test file_build_plan passes --
				       // both touch per-session state ONLY (which element is
				       // active, which chunks were recorded against it) and the
				       // retained Document not at all: no chunk, no param, no
				       // head bump, no staging, no authority branch.
				       // finish_element additionally RENDERS, which is itself
				       // read-safe (render is on this list two entries up).  And
				       // both must be reachable under every posture for the
				       // load-bearing reason file_build_plan's own entry gives:
				       // reopen_element is the escape the compose-phase refusal
				       // names, so an autonomy layer that refused it would strand
				       // exactly the session that needs it.
				       method == "finish_element" ||
				       method == "reopen_element" ||
				       // G2 (2026-08-10): file_build_plan records a per-session
				       // DECLARATION (the elements, their pieces and their
				       // declared construction) and touches the retained Document not
				       // at all -- no chunk, no param, no head bump -- so it is
				       // read-safe on the same test render/read_viewport pass.
				       // It must ALSO be reachable under every posture for a
				       // second, load-bearing reason: it is the ONLY way to
				       // disarm the build-plan gate, and under Propose the gate
				       // can genuinely fire (insert_chunk reaches the session
				       // there).  A gate whose unblock is refused by the
				       // autonomy layer would be an un-unlockable session.
				       method == "file_build_plan" ||
				       // Arc 77 Phase 2 (2026-08-11): imagine_scene records a
				       // per-session IMAGE TARGET (the model's own description,
				       // rendered by the provider) and touches the retained
				       // Document not at all -- read-safe on the same test
				       // file_build_plan passes.  And it must be reachable under
				       // every posture for the same second, load-bearing
				       // reason: on a capable provider it is one of the TWO
				       // ways to disarm the build-plan gate, and a gate whose
				       // unblock the autonomy layer refuses would be an
				       // un-unlockable session.
				       method == "imagine_scene" ||
				       // Doc 90 slice R1 (2026-08-22): set_render_anchor
				       // re-points a per-session BOOKMARK (which completed
				       // render the next render is shown beside) and touches
				       // the retained Document not at all -- no chunk, no
				       // param, no head bump, no staging, no authority branch.
				       // Read-safe on exactly the test file_build_plan and
				       // imagine_scene pass.
				       //
				       // It has no second, gate-shaped reason to be on this
				       // list -- it unblocks nothing, and a session that never
				       // calls it is never stranded.  It belongs here on the
				       // first test alone, and it must be reachable under Read
				       // for the plain reason that `render` is: a Read-posture
				       // session still renders, so it still accumulates the
				       // renders this verb bookmarks, and refusing the bookmark
				       // while allowing the renders would be an arbitrary
				       // half-mechanism.
				       method == "set_render_anchor";
			}

			//! Secure-MCP slice 5b: the additional verbs `Propose` autonomy lets
			//! through beyond the read-safe allowlist above -- the 6 known-
			//! mutating verbs (the 3 single-item verbs plus the 3 BATCH forms,
			//! insert_chunks, propose_patches and -- R1a (2026-08-09) --
			//! remove_chunks, which carry the identical posture).
			//! Dispatching them under Propose does NOT itself
			//! commit anything: it only lets the call REACH AgentSession, whose
			//! own Owner/External authority decides staging-vs-commit (see
			//! AgentRpc.h's file header, "Secure-MCP slice 5b (`Propose`
			//! autonomy)").  Deliberately excludes resolve_proposal -- see that
			//! doc for why it stays Commit-only.
			bool IsProposeSafeVerb( const std::string& method )
			{
				return method == "propose_patch" ||
				       method == "propose_patches" ||
				       method == "insert_chunk"  ||
				       // insert_chunks is the BATCH form of insert_chunk (see
				       // AgentSession::InsertChunks's doc) -- it mutates via the
				       // exact same AgentSession::InsertChunk delegate per element,
				       // so it carries the SAME authority/autonomy posture as
				       // insert_chunk: letting it reach AgentSession under
				       // Propose autonomy does not itself commit anything, the
				       // session's own Owner/External authority still decides
				       // staging-vs-commit per element.
				       method == "insert_chunks" ||
				       method == "remove_chunk" ||
				       // R1a (2026-08-09): remove_chunks is the BATCH form of
				       // remove_chunk and carries the IDENTICAL posture -- letting
				       // it reach AgentSession under Propose does not commit
				       // anything; the session's own Owner/External authority still
				       // decides staging-vs-commit.  Its one difference from
				       // insert_chunks is WHAT gets staged: ONE proposal for the
				       // whole batch (see AgentSession::RemoveChunks' doc), never N.
				       method == "remove_chunks";
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
			//! -- used for any verb that falls through Propose's extended
			//! allowlist (IsProposeSafeVerb -- see that function's doc and
			//! AgentRpc.h's file header).  Same shape as MakeAutonomyRefusedError
			//! above (kAutonomyRefused, structured {verb,autonomy} data) so a
			//! caller can branch on it identically -- but `autonomy:"propose"`
			//! here, TRUTHFULLY: a caller invoking one of these verbs under
			//! --agent-autonomy=propose is NOT running a read-only session (most
			//! other verbs work fine), so reusing MakeAutonomyRefusedError's
			//! hardcoded `autonomy:"read"` / "this session is read-only" text
			//! would be a FALSE value in the exact field built for programmatic
			//! branching (arc-75 S2.1 fix-round P1 -- caught by review: that
			//! defect is exactly what this function exists to prevent, so the
			//! ONE thing every call site must get right is passing an accurate,
			//! verb-SPECIFIC `message` -- resolve_proposal's reason (Owner-only;
			//! --agent-autonomy=commit alone does not fix it for an External
			//! session) is entirely different from insert_material_scaffold's
			//! (simply not on the allowlist; --agent-autonomy=commit DOES let it
			//! reach AgentSession, same as any IsProposeSafeVerb verb).
			std::string MakeProposeAutonomyRefusedError( const JsonValue& id, const std::string& verb,
			                                             const std::string& message )
			{
				JsonValue err = JsonValue::MakeObject();
				err.set( "code", JsonValue::MakeNumber( static_cast<double>( kAutonomyRefused ) ) );
				err.set( "message", JsonValue::MakeString( message ) );
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
			//! all 6 mutating verbs (propose_patch/propose_patches/
			//! insert_chunk/insert_chunks/remove_chunk/remove_chunks) -- built when the
			//! wrapped AgentSession's result carries queueFull==true (see
			//! AgentPatchResult::queueFull / AgentChunkResult::queueFull's
			//! doc).  The BATCH forms raise it too, from the per-element
			//! result, so a queue that fills mid-batch is reported as
			//! backpressure rather than as N rejections.  A distinct
			//! top-level JSON-RPC error (kProposalQueueFull), NOT the normal
			//! success-envelope result shape those verbs otherwise always
			//! return -- same posture as
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
			//! shared by all 6 mutating verbs (propose_patch/propose_patches/
			//! insert_chunk/insert_chunks/remove_chunk/remove_chunks).  Returns
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

			//! Serialize an AgentChunkIssue list as the wire `issues` array --
			//! shared by ChunkResultJson (insert_chunk/insert_chunks/
			//! remove_chunk) AND the inline result construction of
			//! propose_patch and propose_patches below, so the
			//! {param,value,reason,suggestions} shape can never drift across
			//! the 6 mutating verbs that can emit it.
			JsonValue IssuesJson( const std::vector<AgentChunkIssue>& issues )
			{
				JsonValue arr = JsonValue::MakeArray();
				for( std::size_t i = 0; i < issues.size(); ++i ) {
					const AgentChunkIssue& u = issues[i];
					JsonValue e = JsonValue::MakeObject();
					e.set( "param",  JsonValue::MakeString( u.param ) );
					e.set( "value",  JsonValue::MakeString( u.value ) );
					e.set( "reason", JsonValue::MakeString( u.reason ) );
					JsonValue sugg = JsonValue::MakeArray();
					for( std::size_t j = 0; j < u.suggestions.size(); ++j )
						sugg.push_back( JsonValue::MakeString( u.suggestions[j] ) );
					e.set( "suggestions", sugg );
					arr.push_back( e );
				}
				return arr;
			}

			//! Model-B F5 slice S2: serialize an AgentChunkResult (insert_chunk /
			//! remove_chunk share the shape: the propose_patch result fields plus
			//! the affected chunk's name/kind echo).
			//!
			//! S1 fix-round (2026-08-11): `element` is the build-protocol
			//! attribution -- the element that was active when this chunk was
			//! created.  Passed in by the CREATION verbs only (the caller holds
			//! the session; this helper does not), and OMITTED entirely when
			//! empty, so every existing caller's response shape is unchanged and
			//! no existing field changes meaning.  WHY IT IS ON THE WIRE: design
			//! sec 4 item 2 -- "are the chunks attributed to element X the ones
			//! the wizard window was spent on" -- is the slice's central
			//! measurement, and before this the attribution only reached a
			//! trajectory through `finish_element`'s result.  A run that ends
			//! mid-element (turns exhausted, model stops) never calls
			//! finish_element, so its attributed set was stated nowhere at all.
			//! A PLAIN FACT, never a judgement: it says which window a chunk was
			//! made in, nothing about whether it was a good chunk.
			//! ARC 83 SLICE 6 (2026-08-13): ONE `AgentPatchResult` as the
			//! per-element JSON shape the wire has always emitted for a patch.
			//! It EXISTED IN TWO INLINE COPIES before this slice
			//! (`propose_patches` and `place_element`, the second carrying a
			//! comment noting there was "no shared helper to call"), and this
			//! slice needed a third for `frame_scene`.  Three copies of a wire
			//! shape is exactly the drift surface this file's own
			//! IsReadSafeVerb doc argues against, so the two existing sites now
			//! call this and the new one does too -- the emitted keys are
			//! unchanged, byte for byte, from what both copies produced.
			JsonValue PatchResultJson( const AgentPatchResult& pr )
			{
				JsonValue itemRes = JsonValue::MakeObject();
				itemRes.set( "applied",   JsonValue::MakeBool( pr.applied ) );
				itemRes.set( "rawCode",   JsonValue::MakeNumber( static_cast<double>( pr.rawCode ) ) );
				itemRes.set( "status",    JsonValue::MakeString( pr.status ) );
				itemRes.set( "retriable", JsonValue::MakeBool( pr.retriable ) );
				itemRes.set( "headVersion", HeadVersionJson( pr.headVersion ) );
				itemRes.set( "message",   JsonValue::MakeString( pr.message ) );
				if( !pr.issues.empty() ) itemRes.set( "issues", IssuesJson( pr.issues ) );
				// Doc 90 R3: orphan-pressure report -- CONDITIONAL key, same
				// back-compat posture as `issues` above and the SAME wire
				// shape ("orphans", an array of "keyword/name" strings)
				// replace_geometry_scaffold's own `reportedOrphans` already
				// uses (see this file's ReplaceGeometryScaffold handler).
				if( !pr.reportedOrphans.empty() ) {
					JsonValue orphans = JsonValue::MakeArray();
					for( const std::string& o : pr.reportedOrphans )
						orphans.push_back( JsonValue::MakeString( o ) );
					itemRes.set( "orphans", orphans );
				}
				return itemRes;
			}

			JsonValue ChunkResultJson( const AgentChunkResult& cr,
			                           const std::string& element = std::string() )
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
				// S1 fix-round (2026-08-11): CONDITIONAL key -- absent when the
				// chunk carries no attribution (the protocol is off, the session
				// is not in an element window, or the caller is a verb that does
				// not create).  Absent means "no element recorded", which is
				// exactly the state that makes a chunk freely editable.
				if( !element.empty() ) result.set( "element", JsonValue::MakeString( element ) );
				// Model-B F5 slice S3, extended to remove_chunk by a later slice:
				// actionable insert_chunk/remove_chunk diagnostics -- a non-blocking
				// WARNING on a successful insert (a forward reference), the
				// descriptor-derivable CAUSE of a rejected insert, or the reference-
				// graph-derivable CAUSE of a rejected remove (see AgentChunkIssue's
				// doc). CONDITIONAL key, OMITTED entirely when empty -- same back-
				// compat posture as `legend` above: every existing insert_chunk/
				// remove_chunk caller that doesn't know this key exists sees an
				// unchanged response shape on a clean insert/remove or an unexplained
				// rejection.
				if( !cr.issues.empty() ) result.set( "issues", IssuesJson( cr.issues ) );
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
				// AgentRenderResult::renderMode's doc for the CURRENT value
				// set, deliberately not re-listed here: it has grown three
				// times since this comment was written ("objectmap", the view
				// modes' own wire names, and 2026-08-24's "material"), and a
				// copy of the list is a copy that goes stale.  Distinct from
				// `integrator`, which always names the head's ACTIVE
				// (production) rasterizer regardless of which mode this render
				// actually used.
				result.set( "renderMode", JsonValue::MakeString( rr.renderMode ) );
				result.set( "perceptionAvailable", JsonValue::MakeBool( rr.perceptionAvailable ) );
				result.set( "perceptionPersistentBytes", JsonValue::MakeNumber(
					static_cast<double>( rr.perceptionPersistentBytes ) ) );
				result.set( "perceptionAuxiliaryPeakBytes", JsonValue::MakeNumber(
					static_cast<double>( rr.perceptionAuxiliaryPeakBytes ) ) );
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
				// Creative-richness P2 (73-creative-richness-design.md sec 2 P2
				// / sec 7 re-target): the observed-state design note
				// (AgentSession::ComputeDesignNote, run inside RenderCore_ --
				// see AgentRenderResult::note's doc). CONDITIONAL key, same
				// omit-when-empty convention as `legend` above and as
				// read_skill's index-only `note` -- absent whenever the scan
				// found neither measured deficit (or never ran, e.g. an
				// objectmap/view-mode render).
				if( !rr.note.empty() )
					result.set( "note", JsonValue::MakeString( rr.note ) );
				// GPT slice item 1 (2026-08-24): the coverage-delta advisory --
				// same omit-when-empty convention as `note` above.  See
				// AgentRenderResult::coverageDeltaNote's doc.
				if( !rr.coverageDeltaNote.empty() )
					result.set( "coverageDeltaNote", JsonValue::MakeString( rr.coverageDeltaNote ) );
				// G1 (2026-08-10) `render{isolate:}`: the measured facts about
				// an isolated render, under ONE nested key.  CONDITIONAL --
				// present ONLY when isolation actually applied AND the render
				// itself succeeded (same omit-when-absent convention as
				// `legend`/`note`/`agentRenderCap` above), so every existing
				// render result is byte-identical.  The `rr.ok` check matters:
				// `isolateApplied` is set as soon as object-solo framing
				// happens, but a LATER stage of the same render (e.g. a
				// `light:` resolution/support failure) can still flip
				// `rr.ok` to false -- without this check the result would
				// carry a full isolate block (bbox/framing) describing an
				// image that was never produced, contradicting the
				// `AgentRpc.h` and `AgentMcpAdapter.cpp` docs, which both
				// promise the block only on a SUCCESSFUL isolate render.
				// G1 fix-round (2026-08-10).
				if( rr.ok && rr.isolateApplied ) {
					JsonValue iso = JsonValue::MakeObject();
					iso.set( "object", JsonValue::MakeString( rr.isolateObject ) );
					// FIX 4 (G1 fix-round, 2026-08-10): bboxMin/bboxMax/
					// longestEdge are OMITTED (not sent as measured-looking
					// zeros) when the box is degenerate/unusable -- see
					// AgentRenderResult::isolateBBoxUsable's doc.  Same
					// sentinel-then-omit convention as `bboxCoverage` below.
					if( rr.isolateBBoxUsable ) {
						JsonValue bmin = JsonValue::MakeArray();
						JsonValue bmax = JsonValue::MakeArray();
						for( int a = 0; a < 3; ++a ) {
							bmin.push_back( JsonValue::MakeNumber( rr.isolateBBoxMin[a] ) );
							bmax.push_back( JsonValue::MakeNumber( rr.isolateBBoxMax[a] ) );
						}
						iso.set( "bboxMin", bmin );
						iso.set( "bboxMax", bmax );
						iso.set( "longestEdge", JsonValue::MakeNumber( rr.isolateLongestEdge ) );
					}
					iso.set( "autoFramed", JsonValue::MakeBool( rr.isolateAutoFramed ) );
					if( rr.isolateBBoxCoverage >= 0.0 )
						iso.set( "bboxCoverage", JsonValue::MakeNumber( rr.isolateBBoxCoverage ) );
					result.set( "isolate", iso );
				}
				// G3b (2026-08-10) `render{target:}`: the measured shape match
				// against the part's filed sketch, under ONE nested key.
				// CONDITIONAL, same omit-when-absent convention as `isolate`
				// above -- and gated on `rr.ok` from the START rather than
				// after a review round, because this is EXACTLY the shape of
				// G1's fix-round P1: facts describing an image that never
				// happened.  `targetApplied` is itself only ever set on a
				// successful render whose internal identity pass also
				// succeeded, so the `rr.ok` term is belt-and-braces against a
				// later stage flipping ok after the fact.
				//
				// EVERY NUMBER HERE IS REPORTED, NEVER CHARACTERIZED -- no
				// threshold, no verdict, no advice anywhere in this block or
				// in the message it accompanies (design doc
				// docs/agentic-redesign/77-imagination-target-design.md sec 5.4).
				if( rr.ok && rr.targetApplied ) {
					JsonValue tgt = JsonValue::MakeObject();
					tgt.set( "element", JsonValue::MakeString( rr.targetElement ) );
					tgt.set( "view",    JsonValue::MakeString( rr.targetView ) );
					tgt.set( "vantage", JsonValue::MakeString( rr.targetVantage ) );
					tgt.set( "iou",         JsonValue::MakeNumber( rr.targetIou ) );
					tgt.set( "mirroredIou", JsonValue::MakeNumber( rr.targetMirroredIou ) );
					tgt.set( "sketchAreaFraction",
						JsonValue::MakeNumber( rr.targetSketchAreaFraction ) );
					tgt.set( "silhouetteAreaFraction",
						JsonValue::MakeNumber( rr.targetSilhouetteAreaFraction ) );
					tgt.set( "sketchAspect", JsonValue::MakeNumber( rr.targetSketchAspect ) );
					// Both of these carry the -1.0 "not computed" sentinel and
					// are OMITTED rather than serialized as a measured-looking
					// number -- the same convention `bboxCoverage` and
					// `bboxMin`/`bboxMax` already follow in the isolate block.
					if( rr.targetSilhouetteAspect >= 0.0 )
						tgt.set( "silhouetteAspect", JsonValue::MakeNumber( rr.targetSilhouetteAspect ) );
					if( rr.targetThinnestAxisRatio >= 0.0 )
						tgt.set( "thinnestAxisRatio", JsonValue::MakeNumber( rr.targetThinnestAxisRatio ) );
					tgt.set( "compositeWidth",
						JsonValue::MakeNumber( static_cast<double>( rr.targetCompositeWidth ) ) );
					tgt.set( "compositeHeight",
						JsonValue::MakeNumber( static_cast<double>( rr.targetCompositeHeight ) ) );
					result.set( "target", tgt );
				}
				// Arc 77 Phase 2 (2026-08-11), reshaped by Phase 2b: the
				// WHOLE-SCENE target, under ONE nested key, same
				// omit-when-absent convention and same `rr.ok`
				// belt-and-braces as the two blocks above.
				// `sceneTargetApplied` is only ever set on a qualifying,
				// successful render of a session that imagined a scene (see
				// AgentRenderResult::sceneTargetApplied for the qualification
				// rule), so a session that never imagined one produces a
				// byte-identical render result to before this slice.
				//
				// PHASE 2b (2026-08-11): THERE IS NO SCORE IN THIS BLOCK, AND
				// NONE MAY BE ADDED BACK.  It used to carry `rmse` plus six
				// per-channel means; a live run showed those are a TONE
				// signal, and the only edits they can motivate are exposure /
				// emissive / light-power cranking, which measurably made the
				// picture worse.  The COMPOSITE IMAGE is the comparison.
				// What ships here is only what cannot be chased: that a
				// target exists, its own pixel size, and whether (and at what
				// size) the composite rode back.  See
				// AgentRenderResult::sceneTargetApplied for the full record.
				if( rr.ok && rr.sceneTargetApplied ) {
					JsonValue st = JsonValue::MakeObject();
					st.set( "imagined", JsonValue::MakeBool( true ) );
					st.set( "targetWidth",
						JsonValue::MakeNumber( static_cast<double>( rr.sceneTargetWidth ) ) );
					st.set( "targetHeight",
						JsonValue::MakeNumber( static_cast<double>( rr.sceneTargetHeight ) ) );
					const bool haveComposite = !rr.sceneTargetCompositePng.empty();
					st.set( "composite", JsonValue::MakeBool( haveComposite ) );
					if( haveComposite ) {
						st.set( "compositeWidth",
							JsonValue::MakeNumber( static_cast<double>( rr.sceneTargetCompositeWidth ) ) );
						st.set( "compositeHeight",
							JsonValue::MakeNumber( static_cast<double>( rr.sceneTargetCompositeHeight ) ) );
					}
					result.set( "sceneTarget", st );
				}
				// Doc 90 slice R1 (2026-08-22): THE ITERATION RATCHET, under
				// ONE nested key, same omit-when-absent convention and same
				// `rr.ok` belt-and-braces as the blocks above.  `anchorApplied`
				// is only ever set on a qualifying, successful full-frame
				// production render (see AgentRenderResult::anchorApplied), so
				// a session that never got one produces a byte-identical
				// render result to before this slice.
				//
				// THERE IS NO SCORE IN THIS BLOCK, AND NONE MAY BE ADDED --
				// Phase 2b's law (see the sceneTarget block above), which doc
				// 90 sec 2's history note reaffirmed after the SCORED form of
				// this very slice was falsified.  `anchorRevision` and
				// `currentRevision` are IDENTITIES: which render is the
				// anchor, and which one this is.  Nothing here is a
				// measurement of one image against another, and nothing may
				// become one.
				if( rr.ok && rr.anchorApplied ) {
					JsonValue an = JsonValue::MakeObject();
					an.set( "anchored", JsonValue::MakeBool( true ) );
					an.set( "established", JsonValue::MakeBool( rr.anchorEstablished ) );
					an.set( "anchorRevision",
						JsonValue::MakeNumber( static_cast<double>( rr.anchorRevision ) ) );
					an.set( "currentRevision",
						JsonValue::MakeNumber( static_cast<double>( rr.anchorCurrentRevision ) ) );
					const bool haveAnchorComposite = !rr.anchorCompositePng.empty();
					an.set( "composite", JsonValue::MakeBool( haveAnchorComposite ) );
					if( haveAnchorComposite ) {
						an.set( "compositeWidth",
							JsonValue::MakeNumber( static_cast<double>( rr.anchorCompositeWidth ) ) );
						an.set( "compositeHeight",
							JsonValue::MakeNumber( static_cast<double>( rr.anchorCompositeHeight ) ) );
					}
					result.set( "anchor", an );
				}
				// Arc 80 (2026-08-12): the SCENE INVENTORY -- "where is
				// everything?" -- under ONE nested key, same omit-when-absent
				// convention and same `rr.ok` belt-and-braces as the three
				// blocks above.  `inventoryApplied` is only ever set on a
				// successful, non-isolate, full-scene BEAUTY render of a scene
				// with at least one object (see AgentRenderResult::
				// inventoryApplied), so every other render is byte-identical
				// to before this mechanism existed.
				//
				// IT RIDES THE RENDER RESULT ON PURPOSE.  `scene_inventory`
				// returns the same measurement (through the same code path)
				// for anyone who asks, but every voluntary consultation
				// surface this workstream shipped measured 0/64 uses -- so the
				// answer to "did my objects appear, and where are they" is
				// delivered with the picture, unasked.
				//
				// `text` is the whole inventory as prose; it is what a model
				// actually reads, and the counts beside it are the same facts
				// in machine form.  EVERY CLAUSE IN IT IS A MEASUREMENT --
				// nothing is characterized, no object is called missing, and
				// no fix is suggested (arc 79 sec 8.1: a false clause in a
				// model-facing payload cost an entire session's mechanism).
				if( rr.ok && rr.inventoryApplied ) {
					JsonValue inv = JsonValue::MakeObject();
					inv.set( "objects", JsonValue::MakeNumber( static_cast<double>( rr.inventoryObjectCount ) ) );
					inv.set( "covered", JsonValue::MakeNumber( static_cast<double>( rr.inventoryCoveredCount ) ) );
					inv.set( "passWidth",  JsonValue::MakeNumber( static_cast<double>( rr.inventoryPassWidth ) ) );
					inv.set( "passHeight", JsonValue::MakeNumber( static_cast<double>( rr.inventoryPassHeight ) ) );
					inv.set( "text", JsonValue::MakeString( rr.inventoryText ) );
					// Arc 83 (2026-08-14): emissive objects this render measured
					// as NOT selected by NEE area-light sampling, and why -- see
					// AgentSceneNotAreaSampledEntry.  Same omit-when-empty
					// convention as `issues`/`legend` elsewhere: absent rather
					// than an empty array when every emissive object passed, so
					// an existing caller sees an unchanged response shape.
					if( !rr.inventoryNotAreaSampled.empty() ) {
						JsonValue nas = JsonValue::MakeArray();
						for( std::size_t i = 0; i < rr.inventoryNotAreaSampled.size(); ++i ) {
							const AgentSceneNotAreaSampledEntry& e = rr.inventoryNotAreaSampled[i];
							JsonValue o = JsonValue::MakeObject();
							o.set( "name",   JsonValue::MakeString( e.name ) );
							o.set( "reason", JsonValue::MakeString( e.reason ) );
							nas.push_back( o );
						}
						inv.set( "notAreaSampled", nas );
					}
					result.set( "inventory", inv );
				}
				// Arc 81 (2026-08-12): THE TONAL FACT -- the frame's own luma
				// distribution -- under ONE nested key, same omit-when-absent
				// convention and same `rr.ok` belt-and-braces as the blocks
				// above.  `tonalApplied` is only ever set on a successful,
				// non-isolate, full PRODUCTION beauty render (see
				// AgentRenderResult::tonalApplied for why each exclusion is a
				// truthfulness requirement rather than conservatism), so every
				// other render is byte-identical to before this existed.
				//
				// IT COSTS NO RENDER.  Unlike the inventory beside it, every
				// number here is computed from the pixels this call already
				// produced.
				//
				// WHY IT EXISTS: arc 80 sec 6.1 found the scene that "renders
				// empty" was not empty -- 17 of 19 objects covered pixels, and
				// the picture was one flat blue at about 3% contrast across the
				// frame.  The inventory says where things are; this says
				// whether they are distinguishable.  EVERY CLAUSE IN `text` IS
				// A MEASUREMENT: no threshold, no verdict, no adjective, and
				// none may be added -- the model draws the conclusion.
				if( rr.ok && rr.tonalApplied ) {
					JsonValue tone = JsonValue::MakeObject();
					tone.set( "lumaMean",   JsonValue::MakeNumber( rr.tonalLumaMean ) );
					tone.set( "lumaStdDev", JsonValue::MakeNumber( rr.tonalLumaStdDev ) );
					tone.set( "lumaP1",  JsonValue::MakeNumber( static_cast<double>( rr.tonalLumaP1 ) ) );
					tone.set( "lumaP99", JsonValue::MakeNumber( static_cast<double>( rr.tonalLumaP99 ) ) );
					tone.set( "lumaMode", JsonValue::MakeNumber( static_cast<double>( rr.tonalLumaMode ) ) );
					tone.set( "modeConcentration", JsonValue::MakeNumber( rr.tonalModeConcentration ) );
					tone.set( "modeBand",
						JsonValue::MakeNumber( static_cast<double>( kTonalConcentrationBand ) ) );
					tone.set( "pixels",
						JsonValue::MakeNumber( static_cast<double>( rr.tonalPixelsMeasured ) ) );
					tone.set( "text", JsonValue::MakeString( rr.tonalText ) );
					result.set( "tone", tone );
				}
				return result;
			}

			//! R1b (2026-08-09): the RPC-layer-only half of the
			//! `agentRenderCap` fact (see BuildAgentRenderCapJson below) --
			//! the raw, PRE-clamp width/height/samples the caller actually
			//! sent, for an EXPLICIT value the render `render` handler's own
			//! ParseClampedUInt/samples parse reduced.  Only the RPC layer
			//! (AgentRpc.cpp) ever sees these raw numbers -- by the time a
			//! value reaches AgentSession it is already clamped -- so this is
			//! computed and filled entirely inline in the `render` handler,
			//! never inside AgentSession.  Default-constructed = "nothing was
			//! explicitly clamped", matching every field's meaning.
			struct AgentRenderCapFacts
			{
				bool         explicitWidthClamped = false;
				unsigned int rawWidthRequested = 0;
				bool         explicitHeightClamped = false;
				unsigned int rawHeightRequested = 0;
				bool         explicitSamplesClamped = false;
				double       rawSamplesRequested = 0.0;
			};

			//! R1b (2026-08-09): the honest `agentRenderCap` fact -- see
			//! AgentRenderParams::fromAgentSurface's doc for the full
			//! mechanism this reports on.  Combines what AgentSession itself
			//! resolved (`rr.agentResolutionCapped`/`filmWidth`/`filmHeight`/
			//! `agentSamplesCapped` -- the two ABSENT-value implicit
			//! defaults, live-Job-state-dependent, so only AgentSession can
			//! know them) with what the RPC layer alone knows (`facts` -- an
			//! EXPLICIT value this parser itself clamped, from the caller's
			//! raw pre-clamp request).  Returns false (leaves `out`
			//! untouched) when NEITHER happened -- the caller should omit the
			//! `agentRenderCap` key entirely in that case, matching every
			//! other conditional-key convention in this file (`legend`,
			//! `note`, `issues`): when nothing was clamped, add nothing.
			bool BuildAgentRenderCapJson( const AgentRenderResult& rr,
			                             const AgentRenderCapFacts& facts, JsonValue& out )
			{
				const bool resolutionCapped = rr.agentResolutionCapped ||
					facts.explicitWidthClamped || facts.explicitHeightClamped;
				const bool samplesCapped = rr.agentSamplesCapped || facts.explicitSamplesClamped;
				if( !resolutionCapped && !samplesCapped ) return false;

				out = JsonValue::MakeObject();
				out.set( "maxEdge",    JsonValue::MakeNumber(
					static_cast<double>( kAgentSurfaceMaxRenderEdge ) ) );
				out.set( "maxSamples", JsonValue::MakeNumber(
					static_cast<double>( kAgentSurfaceMaxSamples ) ) );
				out.set( "resolutionCapped", JsonValue::MakeBool( resolutionCapped ) );
				// `filmWidth`/`filmHeight` (the scene's authored Film dims --
				// what an UNCAPPED absent-dims render would have produced)
				// are populated ONLY for the implicit-default case; an
				// explicit over-request needs no such echo -- the caller
				// already knows exactly what it asked for.
				if( rr.agentResolutionCapped ) {
					out.set( "filmWidth",  JsonValue::MakeNumber( static_cast<double>( rr.filmWidth ) ) );
					out.set( "filmHeight", JsonValue::MakeNumber( static_cast<double>( rr.filmHeight ) ) );
				}
				if( facts.explicitWidthClamped || facts.explicitHeightClamped ) {
					out.set( "requestedWidth",  JsonValue::MakeNumber( static_cast<double>( facts.rawWidthRequested ) ) );
					out.set( "requestedHeight", JsonValue::MakeNumber( static_cast<double>( facts.rawHeightRequested ) ) );
				}
				out.set( "samplesCapped", JsonValue::MakeBool( samplesCapped ) );
				if( facts.explicitSamplesClamped ) {
					out.set( "requestedSamples", JsonValue::MakeNumber( facts.rawSamplesRequested ) );
				}
				return true;
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
				// the fixed allowlist of read-safe verb names (IsReadSafeVerb
				// in this file -- its BODY is the sole source of truth for
				// membership; a count or a copy of it here would just be one
				// more thing to drift).  A verb that
				// is not on the read-safe list is refused under Read -- this
				// covers the 6 known-mutating verbs, resolve_proposal, AND any
				// future verb that reaches dispatch without being consciously
				// classified read-safe (the fail-closed property this
				// hardening exists for).  Under AgentAutonomy::Read this is a
				// POLICY refusal, never a scene-state outcome -- see
				// MakeAutonomyRefusedError's doc for why it is a distinct
				// JSON-RPC error code/shape rather than a "rejected"/
				// "conflict" success result.
				//
				// Secure-MCP slice 5b: AgentAutonomy::Propose extends the
				// read-safe set with the 6 mutating verbs (IsProposeSafeVerb)
				// -- letting them REACH AgentSession, whose own Owner/External
				// authority decides staging-vs-commit (see AgentRpc.h's file
				// header).  resolve_proposal is deliberately excluded from
				// Propose's extension -- it is refused here with a Propose-
				// specific message (MakeProposeAutonomyRefusedError) rather
				// than falling through to the generic Read-flavoured one,
				// since "relaunch with --agent-autonomy=commit" is not this
				// verb's actual escape hatch for an External session (only the
				// document owner, at their own Commit-posture session, can
				// resolve a proposal -- see AgentSession::ResolveProposal's
				// doc for the session-layer gate this mirrors).
				//
				// Arc-75 S2.1 fix-round P1: insert_material_scaffold is
				// ALSO excluded from Propose's extension (deliberately not
				// added to IsProposeSafeVerb -- see AgentSession.h's
				// InsertMaterialScaffold doc), so it falls through the SAME
				// `!IsReadSafeVerb && !IsProposeSafeVerb` gate resolve_proposal
				// does.  It MUST take the SAME Propose-specific branch, not
				// the generic MakeAutonomyRefusedError fallback below: that
				// fallback hardcodes `autonomy:"read"` and "this session is
				// read-only" -- a FALSE value/message under an actually-
				// Propose session (most other verbs work fine here; only
				// this one tool is unreachable).  Unlike resolve_proposal,
				// though, --agent-autonomy=commit REALLY IS this verb's
				// escape hatch (it reaches AgentSession exactly like any
				// IsProposeSafeVerb verb once dispatched under Commit), so
				// it gets its OWN message saying so, not resolve_proposal's
				// Owner-only wording.
				if( mAutonomy == AgentAutonomy::Read && !IsReadSafeVerb( m ) ) {
					return MakeAutonomyRefusedError( idValue, m );
				}
				if( mAutonomy == AgentAutonomy::Propose &&
				    !IsReadSafeVerb( m ) && !IsProposeSafeVerb( m ) ) {
					if( m == "resolve_proposal" ) {
						return MakeProposeAutonomyRefusedError( idValue, m,
							"refused: this session runs with --agent-autonomy=propose; resolve_proposal is "
							"Owner-only (an External/propose session may not resolve ANY proposal, including "
							"its own -- the document owner resolves it from their own Commit-posture session)" );
					}
					if( m == "insert_material_scaffold" ) {
						return MakeProposeAutonomyRefusedError( idValue, m,
							"refused: this session runs with --agent-autonomy=propose; insert_material_scaffold "
							"is not on the Propose-autonomy allowlist and is unavailable at this posture "
							"(relaunch at --agent-autonomy=commit to reach it) -- insert_chunk/insert_chunks/"
							"propose_patch/propose_patches/remove_chunk/remove_chunks remain available under Propose "
							"and STAGE proposals as usual" );
					}
					// Arc-75 S3b: insert_geometry_scaffold is the geometry
					// sibling of insert_material_scaffold above -- SAME
					// deliberate exclusion from IsProposeSafeVerb, SAME
					// Propose-specific message shape (truthful
					// data.autonomy="propose", not the generic Read-posture
					// fallback's hardcoded "read").
					if( m == "insert_geometry_scaffold" ) {
						return MakeProposeAutonomyRefusedError( idValue, m,
							"refused: this session runs with --agent-autonomy=propose; insert_geometry_scaffold "
							"is not on the Propose-autonomy allowlist and is unavailable at this posture "
							"(relaunch at --agent-autonomy=commit to reach it) -- insert_chunk/insert_chunks/"
							"propose_patch/propose_patches/remove_chunk/remove_chunks remain available under Propose "
							"and STAGE proposals as usual" );
					}
					// R2 (2026-08-10): replace_geometry_scaffold is the third
					// scaffold verb, excluded from IsProposeSafeVerb for the SAME
					// reason -- and, for this one, additionally because a
					// composite whole-document swap has no AgentProposalKind an
					// Owner could approve card-by-card (see
					// AgentSession::ReplaceGeometryScaffold's authority note).
					if( m == "replace_geometry_scaffold" ) {
						return MakeProposeAutonomyRefusedError( idValue, m,
							"refused: this session runs with --agent-autonomy=propose; replace_geometry_scaffold "
							"is not on the Propose-autonomy allowlist and is unavailable at this posture "
							"(relaunch at --agent-autonomy=commit to reach it) -- insert_chunk/insert_chunks/"
							"propose_patch/propose_patches/remove_chunk/remove_chunks remain available under Propose "
							"and STAGE proposals as usual" );
					}
					// 88 step 2 (2026-08-19): collapse_to_instances is the SECOND
					// verb whose commit is one composite whole-document swap, so it
					// is excluded from IsProposeSafeVerb for the identical reason --
					// there is no AgentProposalKind an Owner could approve
					// card-by-card, and inventing one would replay the collapse
					// against a DIFFERENT head than the one it was fitted to.
					if( m == "collapse_to_instances" ) {
						return MakeProposeAutonomyRefusedError( idValue, m,
							"refused: this session runs with --agent-autonomy=propose; collapse_to_instances "
							"is not on the Propose-autonomy allowlist and is unavailable at this posture "
							"(relaunch at --agent-autonomy=commit to reach it) -- insert_chunk/insert_chunks/"
							"propose_patch/propose_patches/remove_chunk/remove_chunks remain available under Propose "
							"and STAGE proposals as usual" );
					}
					// 88 S5 (2026-08-20): vary_material is the THIRD verb whose
					// commit is one composite whole-document swap, so it is
					// excluded from IsProposeSafeVerb for exactly the reason
					// collapse_to_instances above is, with the same message shape.
					if( m == "vary_material" ) {
						return MakeProposeAutonomyRefusedError( idValue, m,
							"refused: this session runs with --agent-autonomy=propose; vary_material "
							"is not on the Propose-autonomy allowlist and is unavailable at this posture "
							"(relaunch at --agent-autonomy=commit to reach it) -- insert_chunk/insert_chunks/"
							"propose_patch/propose_patches/remove_chunk/remove_chunks remain available under Propose "
							"and STAGE proposals as usual" );
					}
					// Doc 90 slice R2 (2026-08-23): revert_to_revision is the
					// FOURTH verb whose commit is one composite whole-document
					// swap, so it is excluded from IsProposeSafeVerb for exactly
					// the reason the three above are, with the same message
					// shape.  There is no AgentProposalKind for "put the whole
					// document back to what it was", and inventing one would
					// replay the restore against a DIFFERENT head than the one
					// it was checked against.
					if( m == "revert_to_revision" ) {
						return MakeProposeAutonomyRefusedError( idValue, m,
							"refused: this session runs with --agent-autonomy=propose; revert_to_revision "
							"is not on the Propose-autonomy allowlist and is unavailable at this posture "
							"(relaunch at --agent-autonomy=commit to reach it) -- insert_chunk/insert_chunks/"
							"propose_patch/propose_patches/remove_chunk/remove_chunks remain available under Propose "
							"and STAGE proposals as usual" );
					}
					// Cat plan item 1 (2026-08-25): fix_blend_scale is the FIFTH
					// verb whose commit is one composite whole-document swap
					// (potentially across multiple sdf_geometry chunks' `part`
					// occurrences in a single call), so it is excluded from
					// IsProposeSafeVerb for exactly the reason vary_material
					// above is, with the same message shape.
					if( m == "fix_blend_scale" ) {
						return MakeProposeAutonomyRefusedError( idValue, m,
							"refused: this session runs with --agent-autonomy=propose; fix_blend_scale "
							"is not on the Propose-autonomy allowlist and is unavailable at this posture "
							"(relaunch at --agent-autonomy=commit to reach it) -- insert_chunk/insert_chunks/"
							"propose_patch/propose_patches/remove_chunk/remove_chunks remain available under Propose "
							"and STAGE proposals as usual" );
					}
					// GEOMETRY_SHADING_SIGNALS sec 11 (2026-08-30): add_wear is the
					// SIXTH verb whose commit is one composite whole-document swap
					// (a painter or two spliced in plus the material's slots
					// repointed), so it is excluded from IsProposeSafeVerb for
					// exactly the reason vary_material above is, with the same
					// message shape.
					if( m == "add_wear" ) {
						return MakeProposeAutonomyRefusedError( idValue, m,
							"refused: this session runs with --agent-autonomy=propose; add_wear "
							"is not on the Propose-autonomy allowlist and is unavailable at this posture "
							"(relaunch at --agent-autonomy=commit to reach it) -- insert_chunk/insert_chunks/"
							"propose_patch/propose_patches/remove_chunk/remove_chunks remain available under Propose "
							"and STAGE proposals as usual" );
					}
					// WETNESS_COAT_DESIGN sec 6/13 (2026-08-31, item 8 2026-09-01):
					// add_wetness is the SEVENTH verb whose commit is one composite
					// whole-document swap (a Lambertian base is WRAPPED in a new
					// `coated_material` chunk with every bound object rebound to it;
					// a GGX/PBR base gets one or more field chunks spliced plus its
					// slots repointed), so it is excluded from IsProposeSafeVerb for
					// exactly the reason add_wear above is, with the same message
					// shape and the same "each verb refuses what the other has
					// already rewritten" collision with add_wear.
					if( m == "add_wetness" ) {
						return MakeProposeAutonomyRefusedError( idValue, m,
							"refused: this session runs with --agent-autonomy=propose; add_wetness "
							"is not on the Propose-autonomy allowlist and is unavailable at this posture "
							"(relaunch at --agent-autonomy=commit to reach it) -- insert_chunk/insert_chunks/"
							"propose_patch/propose_patches/remove_chunk/remove_chunks remain available under Propose "
							"and STAGE proposals as usual" );
					}
					// CLOTH_FABRIC_DESIGN 9.7 (2026-09-02): make_fabric's
					// commit is one composite whole-document swap too -- up to
					// four minted chunks (a dielectric-F0 painter, a substrate
					// of the preset's class, a weave painter, the
					// `fabric_material`) plus every bound object's `material`
					// reference moved onto the wrapper -- so it is excluded
					// from IsProposeSafeVerb for exactly the reason add_wetness
					// above is, with the same message shape.
					if( m == "make_fabric" ) {
						return MakeProposeAutonomyRefusedError( idValue, m,
							"refused: this session runs with --agent-autonomy=propose; make_fabric "
							"is not on the Propose-autonomy allowlist and is unavailable at this posture "
							"(relaunch at --agent-autonomy=commit to reach it) -- insert_chunk/insert_chunks/"
							"propose_patch/propose_patches/remove_chunk/remove_chunks remain available under Propose "
							"and STAGE proposals as usual" );
					}
					// CLOTH_FABRIC_DESIGN Phase 3 (2026-09-03): add_fuzz's
					// commit is one composite whole-document swap too -- a
					// hair_geometry/hair_material/standard_object triad per
					// bound object -- so it is excluded from
					// IsProposeSafeVerb for exactly the reason make_fabric
					// above is, with the same message shape.
					if( m == "add_fuzz" ) {
						return MakeProposeAutonomyRefusedError( idValue, m,
							"refused: this session runs with --agent-autonomy=propose; add_fuzz "
							"is not on the Propose-autonomy allowlist and is unavailable at this posture "
							"(relaunch at --agent-autonomy=commit to reach it) -- insert_chunk/insert_chunks/"
							"propose_patch/propose_patches/remove_chunk/remove_chunks remain available under Propose "
							"and STAGE proposals as usual" );
					}
					// S2 (2026-08-11): build_element and place_element are the
					// two clean-room verbs.  BOTH mutate (build_element inserts
					// through InsertChunks, place_element patches through
					// ProposePatches), so neither is read-safe -- and both are
					// excluded from IsProposeSafeVerb for the SAME reason the
					// three scaffold verbs above are, with the SAME
					// Propose-specific message shape (truthful
					// data.autonomy="propose", not the generic Read-posture
					// fallback's hardcoded "read").
					// Arc 81 (2026-08-12): light_scene is the clean-room LIGHTING
					// verb and mutates (it inserts the chunks its pass
					// returned, through the ordinary InsertChunks path), so it
					// is excluded from IsProposeSafeVerb for exactly the reason
					// build_element is, with the same message shape.
					if( m == "light_scene" ) {
						return MakeProposeAutonomyRefusedError( idValue, m,
							"refused: this session runs with --agent-autonomy=propose; light_scene "
							"is not on the Propose-autonomy allowlist and is unavailable at this posture "
							"(relaunch at --agent-autonomy=commit to reach it) -- insert_chunk/insert_chunks/"
							"propose_patch/propose_patches/remove_chunk/remove_chunks remain available under Propose "
							"and STAGE proposals as usual" );
					}
					// Arc 82 (2026-08-12): populate_scene is the clean-room
					// POPULATION verb and mutates (it inserts the
					// standard_object chunks its pass returned, through the
					// ordinary InsertChunks path), so it is excluded from
					// IsProposeSafeVerb for exactly the reason light_scene is,
					// with the same message shape.
					if( m == "populate_scene" ) {
						return MakeProposeAutonomyRefusedError( idValue, m,
							"refused: this session runs with --agent-autonomy=propose; populate_scene "
							"is not on the Propose-autonomy allowlist and is unavailable at this posture "
							"(relaunch at --agent-autonomy=commit to reach it) -- insert_chunk/insert_chunks/"
							"propose_patch/propose_patches/remove_chunk/remove_chunks remain available under Propose "
							"and STAGE proposals as usual" );
					}
					// Arc 83 slices 5 and 6 (2026-08-13): environment_scene and
					// frame_scene are the clean-room ENVIRONMENT and FRAMING
					// verbs.  BOTH mutate -- environment_scene inserts through
					// InsertChunks and appends the rasterizer chunk carrying its
					// dome binding, frame_scene patches or replaces the camera
					// chunk -- so both are excluded from IsProposeSafeVerb for
					// exactly the reason light_scene is, with the same message
					// shape.
					if( m == "environment_scene" || m == "frame_scene" ) {
						return MakeProposeAutonomyRefusedError( idValue, m,
							"refused: this session runs with --agent-autonomy=propose; " + m +
							" is not on the Propose-autonomy allowlist and is unavailable at this posture "
							"(relaunch at --agent-autonomy=commit to reach it) -- insert_chunk/insert_chunks/"
							"propose_patch/propose_patches/remove_chunk/remove_chunks remain available under Propose "
							"and STAGE proposals as usual" );
					}
					if( m == "build_element" || m == "place_element" ) {
						return MakeProposeAutonomyRefusedError( idValue, m,
							"refused: this session runs with --agent-autonomy=propose; " + m +
							" is not on the Propose-autonomy allowlist and is unavailable at this posture "
							"(relaunch at --agent-autonomy=commit to reach it) -- insert_chunk/insert_chunks/"
							"propose_patch/propose_patches/remove_chunk/remove_chunks remain available under Propose "
							"and STAGE proposals as usual" );
					}
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
					// ONE snapshot: the bytes and the version that describes
					// them must come from a single locked capture (see
					// AgentSession::ReadDocumentSnapshot) -- an agent passes
					// this headVersion straight back as a baseHeadVersion, so
					// a version that does not match the bytes turns the
					// optimistic-concurrency gate into a rubber stamp.
					const AgentSession::AgentDocumentSnapshot snap = s->ReadDocumentSnapshot();
					result.set( "document", JsonValue::MakeString( snap.document ) );
					result.set( "hasDocument", JsonValue::MakeBool( snap.hasDocument ) );
					result.set( "headVersion", HeadVersionJson( snap.headVersion ) );
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// read_schema {keyword?, keywords?, category?} -> the schema JSON
				//   STATELESS: the schema is a pure descriptor-registry walk
				//   (SchemaGenAll / SchemaGenForChunk touch NO Job), so it needs
				//   NO loaded head -- an agent CONSTRUCTING a scene from scratch
				//   reads the grammar first.  We call SchemaGen directly rather
				//   than through the session so the no-head path works.
				//
				//   FOUR forms, resolved in this order:
				//     `keywords` (array of strings) -> BATCH: `schema` is an
				//       ARRAY that is POSITIONALLY ALIGNED with the request --
				//       `schema[i]` is `keywords[i]`, always.  That is the whole
				//       contract, and it is why this path does NOT dedupe and
				//       does NOT reorder: a model that asks for [a,b,c] and
				//       indexes the reply by position is doing the obvious
				//       thing, and a silently shorter or reordered array is a
				//       trap.  A repeated keyword therefore comes back twice
				//       (the cap bounds the cost).  An unknown keyword occupies
				//       ITS OWN slot as {keyword, error}, so one typo neither
				//       fails the batch nor shifts its neighbours.  `keyword`,
				//       when supplied alongside, is APPENDED after the array --
				//       never prepended, which would shift every index by one --
				//       so it lands at `schema[keywords.length]`.  Every entry
				//       carries its own `keyword` field regardless, so an
				//       element is attributable without counting at all.
				//       Measured motivation: a recorded 48-turn GUI scene build
				//       spent 21 of its 47 tool calls on read_schema -- 16 of
				//       those one keyword at a time.
				//     `keyword` (string) -> ONE chunk's full schema (object).
				//     `category` (string) -> the CHEAP LISTING (discovery-cost
				//       fix): just that category's keyword list + one-line
				//       descriptions, NOT the ~286 KB whole-grammar dump.
				//       Deliberately still names-only, and deliberately NOT
				//       batchable here: this verb's batching covers the
				//       per-keyword fetches (16 of the 21 calls above), not the
				//       5 category listings.  A `categories` array would close
				//       that remainder and is a known, unimplemented gap -- the
				//       guidance now steers a model that already knows what it
				//       wants straight to `keywords`, which is where the bulk of
				//       the waste was.
				//     neither -> the whole grammar.
				//--------------------------------------------------------------
				if( m == "read_schema" ) {
					std::string keyword;
					if( const JsonValue* kw = params.find( "keyword" ) ) {
						if( kw->isString() ) keyword = kw->asString();
						else if( !kw->isNull() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'keyword' must be a string" );
					}
					// Type-checked HERE, above the batch branch, even though the
					// batch ignores its value: otherwise {"keywords":["x"],
					// "category":5} would succeed silently while
					// {"keyword":"x","category":5} is a clean -32602 -- the same
					// malformed value diagnosed on one form and not the other.
					std::string category;
					if( const JsonValue* cat = params.find( "category" ) ) {
						if( cat->isString() ) category = cat->asString();
						else if( !cat->isNull() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'category' must be a string" );
					}
					// BATCH form.  PRESENCE of the array (even empty) selects it,
					// so `keywords:[]` honestly returns an empty array rather than
					// silently falling through to the whole-grammar dump.
					const JsonValue* kws = params.find( "keywords" );
					if( kws && kws->isNull() ) kws = nullptr;
					if( kws && !kws->isArray() )
						return MakeError( idValue, kInvalidParams, "Invalid params: 'keywords' must be an array of strings" );
					if( kws ) {
						// The cap bounds ONE result's cost; an unbounded array is
						// a token bomb.  24 is set from the largest observed real
						// need with headroom: the recorded build above wanted 16
						// distinct keywords, which total ~21 KB of schema against
						// the whole-grammar dump's ~286 KB.  (A previous version
						// of this comment claimed the bare dump becomes the
						// cheaper call "well past two dozen".  It does not, at
						// ANY size: the dump measures 286,275 B while the sum of
						// all 157 per-chunk schemas is 282,636 B, so the dump is
						// never cheaper -- not at 24, not at 157.  The cost bound
						// above is the whole rationale.)
						//
						// It counts BOTH parameters, because both produce entries:
						// with `keyword` appended, {keyword, keywords:[24]} would
						// otherwise return 25 while every model-facing surface says
						// 24.  The advertised number has to be the number the code
						// enforces.  (The five surfaces that restate it are tied to
						// this constant by SourceHygieneTest's read_schema batch-cap
						// parity check -- update it here and they go red.)
						//
						// REJECTED, not truncated: a silent truncation would leave
						// the caller believing it had every schema it asked for.
						const std::size_t kMaxBatch = 24;
						const std::size_t requested = kws->size() + ( keyword.empty() ? 0u : 1u );
						if( requested > kMaxBatch )
							return MakeError( idValue, kInvalidParams,
								"Invalid params: 'keyword' and 'keywords' together accept at most " +
								std::to_string( kMaxBatch ) + " chunk keywords (got " +
								std::to_string( requested ) +
								"); split the batch, or omit both for the whole grammar" );
						// POSITIONAL ALIGNMENT (see this verb's doc): one entry per
						// requested keyword, `keywords` first and in order, then
						// `keyword`.  No dedupe, no reordering -- `schema[i]` is
						// `keywords[i]`.
						JsonValue arr = JsonValue::MakeArray();
						for( std::size_t i = 0; i < kws->size(); ++i ) {
							const JsonValue& e = kws->at( i );
							if( !e.isString() )
								return MakeError( idValue, kInvalidParams,
									"Invalid params: 'keywords[" + std::to_string( i ) + "]' must be a string" );
							arr.push_back( SchemaAsJson( RISE::Agent::SchemaGenForChunk( e.asString() ) ) );
						}
						if( !keyword.empty() )
							arr.push_back( SchemaAsJson( RISE::Agent::SchemaGenForChunk( keyword ) ) );
						JsonValue result = JsonValue::MakeObject();
						result.set( "schema", arr );
						return MakeSuccess( idValue, result );
					}
					// `keyword` takes precedence over `category` if BOTH are
					// supplied (a single chunk is more specific than its category).
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
						// Index-only advisory, set whenever the index is EMPTY:
						// says plainly that no skills are available and names the
						// root tried, so a miswired install is never a silent
						// degradation (and a MISSING root stays distinguishable
						// from a present-but-empty one).
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
				// validate {text?} -> {diagnostics:[...]}  (+ headVersion in the
				//   no-argument form)
				//
				//   TWO FORMS, both read-only and both side-effect-free:
				//
				//   * WITH `text` -- the STATELESS candidate form.  Validation
				//     parses `text` to a CST and derives it into a THROWAWAY Job
				//     (never a session's head), so it needs NO loaded head -- an
				//     agent REPAIRING or composing a scene from scratch validates
				//     a candidate BEFORE any head exists.  We call the static
				//     ValidateText directly so the no-head path works.
				//
				//   * WITHOUT `text` -- validate the CURRENTLY RETAINED document.
				//     Added because requiring `text` made the model re-emit the
				//     WHOLE scene just to check its own three-parameter patch.
				//     MEASURED (trajectory 20260727T063526Z-a7ee472c): calls
				//     17-19 were three propose_patch calls of 183/184/184
				//     request bytes; call 22 was a `validate` whose `text`
				//     argument was 19,828 bytes -- the ENTIRE head, which the
				//     engine already had in memory -- and the turn that
				//     produced it spent 6,369 output tokens and 27.8 s.
				//     HONEST SCOPING of that evidence: a second session
				//     (20260727T064005Z-d815b0c7) also spent 4,689 output
				//     tokens / 26.5 s on a validate, but that one carried a
				//     3,355-byte CANDIDATE against a ~1.9 KB head -- the LEGITIMATE
				//     `text` form during a from-scratch build, not a re-echo.
				//     It is not evidence for this change, and is recorded here
				//     so the claim is not quietly doubled.  The head is validated
				//     through the SAME static ValidateText on the SAME canonical
				//     text ReadDocument() serves, so a diagnosed document reports
				//     identical diagnostics either way -- this form is an
				//     argument shortcut, not a second validator.  (It is also
				//     literally the idiom the eval harness already used
				//     internally for its "diagnostics" checkpoint --
				//     AgentEvalRunner.cpp's CheckDiagnosticsKind -- which had
				//     no way to express it over the wire until now.)  The result also
				//     carries the `headVersion` that was validated (the text form
				//     does NOT: a candidate is unrelated to the head, and stamping
				//     one there would be a lie).
				//
				//   NO HEAD + no `text`: an ERROR, deliberately.  Returning an
				//   empty diagnostics array would tell the model "your document
				//   is clean" about a document that does not exist -- the exact
				//   dishonesty this surface refuses elsewhere.  The message names
				//   the `text` form, which DOES work with no head.
				//
				//   A PRESENT-BUT-NULL `text` reads as ABSENT (the same
				//   convention read_schema's `keyword` and read_skill's `name`
				//   already use); any other non-string value is rejected.  An
				//   EMPTY STRING does NOT read as absent -- presence of a
				//   string selects the text form, so `{"text":""}` validates
				//   the empty candidate and gets EMPTY_DOCUMENT back.  That
				//   keeps the form selection a pure function of the argument's
				//   presence, and puts the honesty where it belongs (in
				//   ValidateText, which refuses to call a content-free
				//   document clean) rather than in a routing special case.
				//
				//   STAGED EDITS.  This validates the HEAD as the engine holds
				//   it.  Under AgentAutonomy::Propose with an External-authority
				//   session (the loopback-HTTP topology) a mutating verb answers
				//   status="staged" and leaves the head UNTOUCHED -- so a clean
				//   verdict here says nothing about a staged edit, which is not
				//   in the head yet.  Both model-facing tool descriptions say so;
				//   the `headVersion` in the result is the check (it will not
				//   have moved).
				//
				//   BOTH forms stamp `validated`: "head" or "text".  It is not
				//   redundant with the presence of `headVersion` -- a call whose
				//   arguments were MALFORMED degrades to empty params
				//   (ToolCallToJsonRpcLine's documented behaviour), so a model
				//   that MEANT to check a candidate can silently land in the
				//   head form.  Without an explicit discriminator it would read
				//   an empty diagnostics array as "my candidate is clean"; with
				//   one, the answer says plainly what was checked.
				//--------------------------------------------------------------
				if( m == "validate" ) {
					const JsonValue* text = params.find( "text" );
					// TYPE FIRST, then the absent/present decision -- so a
					// non-string is refused rather than silently reinterpreted
					// as the head form.
					if( text && !text->isString() && !text->isNull() ) {
						return MakeError( idValue, kInvalidParams,
							"Invalid params: 'text' must be a string when supplied "
							"(omit it entirely to validate the current scene)" );
					}
					// PRESENCE of a NON-EMPTY string selects the text form;
					// OMISSION, an explicit null, OR an explicit empty string
					// selects the head.
					//
					// This reverses an earlier decision (see git history) that
					// deliberately made `{"text":""}` take the text form and
					// report EMPTY_DOCUMENT.  That reasoning was sound for a
					// caller that TYPED an empty string on purpose -- but the
					// GPT wire transport's tool-calling SDK convention fills
					// EVERY optional string parameter it isn't using with ""
					// rather than omitting it, so a GPT-backed session cannot
					// express "no text" at all: every `validate` call arrives
					// as `{"text":""}`, taking the text form, and reporting
					// EMPTY_DOCUMENT no matter what the live document holds --
					// observed retrying the IDENTICAL call three times
					// verbatim, apparently unable to tell "I asked for text
					// validation and my empty string was rejected" from "I
					// asked to validate the live head and it says EMPTY_
					// DOCUMENT", because from that transport's side those two
					// requests are byte-identical.
					//
					// There is no genuine caller-visible loss: an ACTUAL empty
					// candidate has exactly one possible verdict either way
					// (EMPTY_DOCUMENT), so a caller who really means "validate
					// nothing" learns nothing this reroute would have told
					// them that the head form does not already say just as
					// well (EMPTY_DOCUMENT again, if the head itself is
					// empty).  Whitespace-only and comments-only candidates
					// are UNCHANGED -- they are non-empty STRINGS, so they
					// still take the text form and still report EMPTY_
					// DOCUMENT; only the literal `""` is reinterpreted, which
					// is exactly the one shape a "fill unset optionals with
					// empty string" SDK convention can produce.
					//
					// "" reads as absent ONLY when there is a LIVE DOCUMENT for
					// it to be absent FROM -- a session with no scene loaded
					// (or none at all) has nothing to fall back to, and the
					// earlier decision's own worked example still applies
					// there verbatim: routing an empty string to the no-head
					// error path turns a valid, stateless `{"text":""}` call
					// into a hard failure with no document in play to justify
					// it.  So a headless/docless session keeps the pre-
					// existing behaviour -- "" still takes the text form and
					// reports EMPTY_DOCUMENT -- while a session WITH a live
					// head gets the GPT-tolerance fix.
					const bool emptyText  = ( text != nullptr && text->isString() && text->asString().empty() );
					const bool hasHeadDoc = ( s != nullptr ) && s->ReadDocumentSnapshot().hasDocument;
					const bool haveText   = ( text != nullptr && text->isString() && !( emptyText && hasHeadDoc ) );
					// ONE renderer for both forms, so the two result shapes
					// cannot drift apart field by field.
					auto diagnosticsArray = []( const std::vector<AgentDiagnostic>& diags ) {
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
						return arr;
					};
					if( !haveText ) {
						// The CURRENT-HEAD form.  ONE snapshot binds the
						// diagnostics' byte offsets to the headVersion this
						// result stamps -- reading hasDocument / the bytes /
						// the version separately let a commit on another
						// thread (the hosted MCP server runs on its own,
						// over the same Job) land between them, so the
						// offsets described bytes that were not the reported
						// head, with no document in the response to
						// disambiguate.  See AgentSession::ReadDocumentSnapshot.
						if( !s ) {
							return MakeError( idValue, kInvalidParams,
								"Invalid params: no scene is loaded, so there is nothing to "
								"validate; supply 'text' to validate a candidate document" );
						}
						const AgentSession::AgentDocumentSnapshot snap = s->ReadDocumentSnapshot();
						if( !snap.hasDocument ) {
							return MakeError( idValue, kInvalidParams,
								"Invalid params: no scene is loaded, so there is nothing to "
								"validate; supply 'text' to validate a candidate document" );
						}
						JsonValue headResult = JsonValue::MakeObject();
						// (Doc 91) condition G's phase gate: `s` is non-null here
						// (checked just above), so pass its real build phase
						// through rather than ValidateText's conservative
						// no-session default.
						headResult.set( "diagnostics", diagnosticsArray(
							AgentSession::ValidateText( snap.document,
								s->BuildProtocolActive() && s->BuildPhase() == AgentSession::AgentBuildPhase::Pieces,
								&s->LightSoloMeasurements() ) ) );
						headResult.set( "validated", JsonValue::MakeString( "head" ) );
						headResult.set( "headVersion", HeadVersionJson( snap.headVersion ) );
						// Creative-richness P2.b (73-creative-richness-design.md
						// sec 9's closing recommendation): validate no longer
						// attaches a `note` field -- the SAME two design-note
						// conditions now ride the `diagnostics` array above as
						// Info-severity DESIGN_SCALAR_PIPE_UNUSED /
						// DESIGN_NO_ADVANCED_GEOMETRY entries (see
						// AgentSession::ValidateText's AppendDesignDiagnostics_
						// call).  ONE mechanism per carrier: the render-result
						// carrier (AgentSession::RenderCore_) still attaches
						// `note` -- untouched by this slice.
						return MakeSuccess( idValue, headResult );
					}
					JsonValue result = JsonValue::MakeObject();
					// (Doc 91) `s` may be null here (the text form works with no
					// head/session loaded -- the no-head bootstrap case
					// ValidateText's default is conservative for); when it
					// exists, pass its real build phase through.
					result.set( "diagnostics", diagnosticsArray(
						AgentSession::ValidateText( text->asString(),
							s && s->BuildProtocolActive() && s->BuildPhase() == AgentSession::AgentBuildPhase::Pieces,
							s ? &s->LightSoloMeasurements() : nullptr ) ) );
					result.set( "validated", JsonValue::MakeString( "text" ) );
					// Creative-richness P2.b: no `note` field here either -- see
					// the head-form branch's comment above.
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// propose_patch {target,kind?,param,value,baseHeadVersion?}
				//   -> {applied,rawCode,status,retriable,headVersion,message,issues?}
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
				//   + re-propose, so it does NOT set the flag.  A REJECTED patch
				//   (status="rejected", rawCode 0) may carry `issues`: the SAME
				//   {param,value,reason,suggestions} shape insert_chunk/
				//   remove_chunk return (see AgentChunkIssue's doc) -- reason one
				//   of "unknown_target" (target doesn't resolve), "unknown_param",
				//   "numeric_in_reference_slot", "unresolved_reference", or
				//   "invalid_value" (an ill-typed Enum/numeric value; `message`
				//   lists the allowed values for an Enum).  OMITTED entirely when
				//   empty -- an empty `issues` on a rejection does not mean the
				//   patch was fine, only that this pass could not statically pin
				//   the cause.
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
					// 1b: OPTIONAL `occurrence` -- which occurrence of a
					// REPEATABLE `param` (skeleton_geometry's `joint`,
					// sdf_geometry's `part`) to edit, 0-based, document
					// order.  Absent -> occurrence 0 with the legacy
					// (non-occurrence-addressed) resolution, byte-identical
					// to every propose_patch call before this field existed.
					if( const JsonValue* occ = params.find( "occurrence" ) ) {
						if( !occ->isNumber() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'occurrence' must be a number" );
						const double ov = occ->asNumber();
						// Bounded well under INT_MAX (no realistic chunk has
						// anywhere near this many repeated-param lines) so
						// the static_cast below can never overflow `int`.
						if( !RISE::IsFiniteDouble( ov ) ||
						    !( ov >= 0.0 && ov <= 1000000.0 && ov == std::floor( ov ) ) )
							return MakeError( idValue, kInvalidParams,
								"Invalid params: 'occurrence' must be a non-negative integer" );
						sp.occurrence    = static_cast<int>( ov );
						sp.hasOccurrence = true;
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
					// Actionable rejection diagnostics -- the propose_patch sibling
					// of insert_chunk/remove_chunk's `issues` (see AgentChunkIssue's
					// doc). CONDITIONAL key, OMITTED entirely when empty -- same
					// back-compat posture as ChunkResultJson's `issues`.
					if( !pr.issues.empty() ) result.set( "issues", IssuesJson( pr.issues ) );
					// Doc 90 R3: orphan-pressure report -- CONDITIONAL key, same
					// shape PatchResultJson's own copy uses (the propose_patches
					// batch path) and replace_geometry_scaffold's `orphans`.
					if( !pr.reportedOrphans.empty() ) {
						JsonValue orphans = JsonValue::MakeArray();
						for( const std::string& o : pr.reportedOrphans )
							orphans.push_back( JsonValue::MakeString( o ) );
						result.set( "orphans", orphans );
					}
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// propose_patches {patches:[{target,param,value,kind?},...], baseHeadVersion?}
				//   -> {applied:number, total:number, results:[PatchResultJson,...]}
				//   BATCH form of propose_patch (see AgentSession::ProposePatches's
				//   doc): apply MULTIPLE parameter patches in ONE call instead of
				//   one round-trip per patch, collapsing what would otherwise be
				//   N propose_patch calls.  SEQUENTIAL, BEST-EFFORT -- every element
				//   of `patches` is attempted IN ORDER, and a REJECTED element does
				//   not stop the batch.  `baseHeadVersion`, when given, is the
				//   optimistic-concurrency precondition for the BATCH AS A WHOLE
				//   (checked against the FIRST element only).  ONE EXCEPTION to
				//   best-effort: a STALE-BASE CONFLICT on element 0 is BATCH-FATAL
				//   -- no further element is attempted and the tail comes back
				//   status="conflict"/applied=false, so a batch racing a concurrent
				//   editor cannot blind-clobber it (see AgentSession::ProposePatches's
				//   doc for why this differs from insert_chunks).  `applied` is the
				//   count of `results` entries with applied==true; `total` is
				//   `patches.size()`, and `results` always has exactly `total`
				//   entries so results[i] corresponds to patches[i].
				//--------------------------------------------------------------
				if( m == "propose_patches" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					const JsonValue* patchesVal = params.find( "patches" );
					if( !patchesVal || !patchesVal->isArray() ) {
						std::string msg = "Invalid params: 'patches' (array of objects) is required";
						const std::string other = DescribeOtherParamKeys(
							params, { "patches", "baseHeadVersion" } );
						if( !other.empty() )
							msg += " (got " + other + " instead -- rename to 'patches')";
						return MakeError( idValue, kInvalidParams, msg );
					}
					if( patchesVal->size() == 0 ) {
						return MakeError( idValue, kInvalidParams,
							"Invalid params: 'patches' must be a non-empty array of objects" );
					}
					std::vector<AgentSetPatch> patches;
					patches.reserve( patchesVal->size() );
					for( std::size_t i = 0; i < patchesVal->size(); ++i ) {
						const JsonValue& item = patchesVal->at( i );
						if( !item.isObject() ) {
							char buf[128];
							std::snprintf( buf, sizeof( buf ),
								"Invalid params: 'patches[%zu]' must be an object", i );
							return MakeError( idValue, kInvalidParams, buf );
						}
						const JsonValue* target = item.find( "target" );
						const JsonValue* param  = item.find( "param" );
						const JsonValue* value  = item.find( "value" );
						if( !target || !target->isString() ||
						    !param  || !param->isString()  ||
						    !value  || !value->isString() ) {
							char buf[256];
							std::snprintf( buf, sizeof( buf ),
								"Invalid params: 'patches[%zu]' requires string 'target', 'param', and 'value'", i );
							return MakeError( idValue, kInvalidParams, buf );
						}
						AgentSetPatch sp;
						sp.target = target->asString();
						sp.param  = param->asString();
						sp.value  = value->asString();
						if( const JsonValue* kind = item.find( "kind" ) ) {
							if( kind->isString() ) sp.kind = kind->asString();
							else if( !kind->isNull() ) {
								char buf[128];
								std::snprintf( buf, sizeof( buf ),
									"Invalid params: 'patches[%zu].kind' must be a string", i );
								return MakeError( idValue, kInvalidParams, buf );
							}
						}
						// 1b: per-element `occurrence`, same validation and
						// meaning as propose_patch's own -- see that parse
						// site's comment.
						if( const JsonValue* occ = item.find( "occurrence" ) ) {
							if( !occ->isNumber() ) {
								char buf[128];
								std::snprintf( buf, sizeof( buf ),
									"Invalid params: 'patches[%zu].occurrence' must be a number", i );
								return MakeError( idValue, kInvalidParams, buf );
							}
							const double ov = occ->asNumber();
							if( !RISE::IsFiniteDouble( ov ) ||
							    !( ov >= 0.0 && ov <= 1000000.0 && ov == std::floor( ov ) ) ) {
								char buf[160];
								std::snprintf( buf, sizeof( buf ),
									"Invalid params: 'patches[%zu].occurrence' must be a non-negative integer", i );
								return MakeError( idValue, kInvalidParams, buf );
							}
							sp.occurrence    = static_cast<int>( ov );
							sp.hasOccurrence = true;
						}
						patches.push_back( sp );
					}
					RISE::Cst::CstHeadVersion base;
					std::string bErr;
					const int b = ParseBaseHeadVersionParam( params, base, bErr );
					if( b < 0 ) return MakeError( idValue, kInvalidParams, bErr );
					const std::vector<AgentPatchResult> results =
						s->ProposePatches( patches, ( b == 1 ) ? &base : nullptr );
					for( const AgentPatchResult& pr : results ) {
						if( pr.queueFull ) return MakeProposalQueueFullError( idValue, "propose_patches" );
					}
					std::size_t appliedCount = 0;
					JsonValue resultsArr = JsonValue::MakeArray();
					for( const AgentPatchResult& pr : results ) {
						if( pr.applied ) ++appliedCount;
						resultsArr.push_back( PatchResultJson( pr ) );
					}
					JsonValue outRes = JsonValue::MakeObject();
					outRes.set( "applied", JsonValue::MakeNumber( static_cast<double>( appliedCount ) ) );
					outRes.set( "total",   JsonValue::MakeNumber( static_cast<double>( results.size() ) ) );
					outRes.set( "results", resultsArr );
					return MakeSuccess( idValue, outRes );
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
					// S1 fix-round (2026-08-11): carry the attribution -- see
					// ChunkResultJson's `element` doc.
					return MakeSuccess( idValue, ChunkResultJson( cr, s->ChunkElement( cr.name ) ) );
				}

				//--------------------------------------------------------------
				// insert_chunks {chunks:[string,...], baseHeadVersion?}
				//   -> {applied:number, total:number, results:[ChunkResultJson,...]}
				//   BATCH form of insert_chunk (see AgentSession::InsertChunks's
				//   doc): apply MULTIPLE complete chunks in ONE call instead of
				//   one round-trip per chunk, collapsing what would otherwise be
				//   N insert_chunk calls for an N-chunk scene.  SEQUENTIAL,
				//   BEST-EFFORT -- every element of `chunks` is attempted IN
				//   ORDER even if an earlier one is rejected, so a later chunk
				//   that depends on an earlier one resolves cleanly (and a later
				//   chunk that depended on a REJECTED earlier one simply also
				//   fails, with its own actionable `issues`).  `baseHeadVersion`,
				//   when given, is the optimistic-concurrency precondition for
				//   the BATCH AS A WHOLE (checked against the FIRST element
				//   only -- see AgentSession::InsertChunks's doc).  `applied` is
				//   the count of `results` entries with applied==true; `total`
				//   is `chunks.size()`.  Each `results` element is the EXACT
				//   same shape ChunkResultJson gives insert_chunk (status, name,
				//   kind, issues warnings/causes per chunk).  REQUIRES a head:
				//   no session -> error.
				//--------------------------------------------------------------
				if( m == "insert_chunks" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					const JsonValue* chunks = params.find( "chunks" );
					if( !chunks || !chunks->isArray() ) {
						std::string msg = "Invalid params: 'chunks' (array of strings) is required";
						// See DescribeOtherParamKeys's doc (mirrors insert_chunk's
						// identical guidance): name whatever the caller sent
						// INSTEAD so a wrong-key mistake is visible in the SAME
						// error round-trip.
						const std::string other = DescribeOtherParamKeys(
							params, { "chunks", "baseHeadVersion" } );
						if( !other.empty() )
							msg += " (got " + other + " instead -- rename to 'chunks')";
						return MakeError( idValue, kInvalidParams, msg );
					}
					if( chunks->size() == 0 ) {
						return MakeError( idValue, kInvalidParams,
							"Invalid params: 'chunks' must be a non-empty array of strings" );
					}
					std::vector<std::string> chunkTexts;
					chunkTexts.reserve( chunks->size() );
					for( std::size_t i = 0; i < chunks->size(); ++i ) {
						const JsonValue& item = chunks->at( i );
						if( !item.isString() ) {
							char buf[128];
							std::snprintf( buf, sizeof( buf ),
								"Invalid params: 'chunks[%zu]' must be a string", i );
							return MakeError( idValue, kInvalidParams, buf );
						}
						chunkTexts.push_back( item.asString() );
					}
					RISE::Cst::CstHeadVersion base;
					std::string bErr;
					const int b = ParseBaseHeadVersionParam( params, base, bErr );
					if( b < 0 ) return MakeError( idValue, kInvalidParams, bErr );
					const std::vector<AgentChunkResult> results =
						s->InsertChunks( chunkTexts, ( b == 1 ) ? &base : nullptr );
					// Secure-MCP slice 6: if ANY element hit the pending-proposal
					// queue-full condition (External authority, live controller,
					// queue already at capacity), report it with the SAME queue-
					// full error shape insert_chunk uses -- it's the identical
					// External-authority staging posture, just discovered on one
					// of several elements instead of the sole one.
					for( const AgentChunkResult& cr : results ) {
						if( cr.queueFull ) return MakeProposalQueueFullError( idValue, "insert_chunks" );
					}
					std::size_t appliedCount = 0;
					JsonValue resultsArr = JsonValue::MakeArray();
					for( const AgentChunkResult& cr : results ) {
						if( cr.applied ) ++appliedCount;
						// S1 fix-round (2026-08-11): per ELEMENT of the batch --
						// a batch spanning a finish_element cannot happen (one
						// call, one window), but asking per chunk costs nothing
						// and cannot go stale.
						resultsArr.push_back( ChunkResultJson( cr, s->ChunkElement( cr.name ) ) );
					}
					JsonValue result = JsonValue::MakeObject();
					result.set( "applied", JsonValue::MakeNumber( static_cast<double>( appliedCount ) ) );
					result.set( "total",   JsonValue::MakeNumber( static_cast<double>( results.size() ) ) );
					result.set( "results", resultsArr );
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// insert_material_scaffold {family, name, tone, wear, scale, baseHeadVersion?}
				//   -> {applied:number, total:number, results:[ChunkResultJson,...],
				//       family, name, material:{name,kind}, boundSlots:[{param,painter},...]}
				//   Arc-75 slice S2.1: expand one of FIVE material-family
				//   templates ("weathered_wood", "rough_stone",
				//   "brushed_metal", "aged_bronze", "glazed_ceramic") into a
				//   small wired painter graph (2-4 painters + 1 material,
				//   every chunk named tmpl_<name>_<role>) with a
				//   microsurface slot painter-BOUND -- one tool call instead
				//   of hand-typing a scalar/painter graph.  ALL FIVE params
				//   are REQUIRED (no defaults): a missing param is a
				//   BLOCKING error naming it, deliberately (the surface is
				//   new and the creative micro-decision is the point).
				//   `tone` is "r g b" (each 0..1); `wear` is 0..1 (variation
				//   intensity); `scale` is >0 (spatial frequency).  Chunk
				//   generation, family/tone/wear/scale validation, and the
				//   name-collision precheck (refuses the WHOLE expansion,
				//   document unchanged, before any chunk is generated) all
				//   happen in AgentSession::InsertMaterialScaffold; this
				//   handler only extracts params and serializes the result.
				//   The actual insert is submitted through the SAME
				//   InsertChunks path insert_chunks uses, so authority/
				//   autonomy staging-vs-commit, conflict detection, and
				//   per-chunk `issues` are all inherited unchanged.
				//--------------------------------------------------------------
				//--------------------------------------------------------------
				// file_build_plan {elements:[{element,pieces,construction,outline,view?,note?},...]}
				//   -> {filed:true, replacedPreviousPlan:bool, elementCount:number,
				//       elements:[{element,pieces,construction,note,outline,view,
				//                  pointCount,areaFraction,aspect},...],
				//       phase, activeElement,
				//       png_base64, byteLength, compositeWidth, compositeHeight,
				//       sketchCanvas, message}
				//   G2 (2026-08-10): the ONLY way to disarm the build-plan gate
				//   (AgentSession.h's block above FileBuildPlan).  READ-SAFE --
				//   it records a per-session declaration and touches the
				//   Document not at all, so there is no headVersion, no
				//   baseHeadVersion, no conflict, no staging, and no authority
				//   branch: an External-authority session files a plan
				//   directly, exactly like it renders directly.
				//   Every param error below is a clean kInvalidParams naming
				//   the accepted enum; the plan CONTENT is never judged (a
				//   plan of all-`primitive` is as valid as any other).
				//   G3a (2026-08-10) SCHEMA v2: `outline` is REQUIRED per
				//   element and `view` is optional.  Both are validated HERE,
				//   before FileBuildPlan is called, so a defect is a clean
				//   -32602 naming the element INDEX and the defect -- and, being
				//   a schema error rather than a gate interception, it never
				//   touches the gate's refusal counter (G2h pins that; the
				//   G3a cases extend it).  The result gains ONE composite PNG
				//   under the SAME `png_base64` field name read_image /
				//   render{imageMaxEdge} / compare_to_reference use, so every
				//   image-retention and image-surfacing policy built on that
				//   field covers it with no second code path.  The wire still
				//   carries only numbers and names INBOUND -- the mask bytes
				//   are computed host-side from the model's own coordinates,
				//   so no caller-supplied image data enters the process.
				//   S1 (2026-08-11) SCHEMA v3: `parts` became `elements`, each
				//   entry's `part` became `element`, and `pieces` (a REQUIRED
				//   list of at least one name) joined them.  Filing also enters
				//   the PIECES phase with the first element active, reported in
				//   `phase`/`activeElement`.  Same validate-here contract: an
				//   empty or over-long `pieces` is a -32602 that never touches
				//   the gate's counter.
				//--------------------------------------------------------------
				if( m == "file_build_plan" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					const std::string enumList = AgentSession::BuildPlanConstructionList();
					const std::string viewList = AgentSession::BuildPlanViewList();
					const JsonValue* elementsVal = params.find( "elements" );
					if( !elementsVal || !elementsVal->isArray() || elementsVal->size() == 0 ) {
						return MakeError( idValue, kInvalidParams,
							"Invalid params: 'elements' (non-empty array of {element, pieces, construction, "
							"outline[, view][, note]}) is required; each element's 'construction' must be one "
							"of: " + enumList );
					}
					// G3a: bound the plan SIZE before doing per-entry work.
					// Unlike G2's fields, the plan's cost now grows with the
					// caller's array (one 64 KB mask per element), so an
					// unbounded list is an unbounded allocation reachable from
					// the wire.  See AgentSession's kBuildPlanMaxElements.
					if( elementsVal->size() > AgentSession::kBuildPlanMaxElements ) {
						return MakeError( idValue, kInvalidParams,
							"Invalid params: 'elements' has " + std::to_string( elementsVal->size() ) +
							" entries -- at most " + std::to_string( AgentSession::kBuildPlanMaxElements ) +
							" are accepted" );
					}
					std::vector<AgentSession::AgentBuildPlanEntry> entries;
					entries.reserve( elementsVal->size() );
					for( std::size_t i = 0; i < elementsVal->size(); ++i ) {
						const JsonValue& e = elementsVal->at( i );
						char idx[40];
						std::snprintf( idx, sizeof( idx ), "elements[%d]", static_cast<int>( i ) );
						if( !e.isObject() ) {
							return MakeError( idValue, kInvalidParams,
								std::string( "Invalid params: " ) + idx + " must be an object "
								"{element, pieces, construction, outline[, view][, note]}" );
						}
						const JsonValue* elemVal = e.find( "element" );
						if( !elemVal || !elemVal->isString() || elemVal->asString().empty() ) {
							return MakeError( idValue, kInvalidParams,
								std::string( "Invalid params: " ) + idx + ".element (non-empty string) is required" );
						}
						// S1: `pieces` is REQUIRED with at least one non-empty
						// entry -- the decomposition IS the artifact this
						// mechanism measures, so there is no default and no
						// opt-out.  ANY names are accepted; nothing checks what
						// they say (the same anti-Goodhart freedom `outline`
						// has on the shape dimension).
						const JsonValue* piecesVal = e.find( "pieces" );
						if( !piecesVal || !piecesVal->isArray() || piecesVal->size() == 0 ) {
							return MakeError( idValue, kInvalidParams,
								std::string( "Invalid params: " ) + idx + ".pieces (non-empty array of "
								"strings) is required -- the pieces this element breaks down into, in your "
								"own words" );
						}
						if( piecesVal->size() > AgentSession::kBuildPlanMaxPiecesPerElement ) {
							return MakeError( idValue, kInvalidParams,
								std::string( "Invalid params: " ) + idx + ".pieces has " +
								std::to_string( piecesVal->size() ) + " entries -- at most " +
								std::to_string( AgentSession::kBuildPlanMaxPiecesPerElement ) +
								" are accepted" );
						}
						std::vector<std::string> pieces;
						pieces.reserve( piecesVal->size() );
						for( std::size_t p = 0; p < piecesVal->size(); ++p ) {
							const JsonValue& pv = piecesVal->at( p );
							if( !pv.isString() || pv.asString().empty() ) {
								char pidx[24];
								std::snprintf( pidx, sizeof( pidx ), "[%d]", static_cast<int>( p ) );
								return MakeError( idValue, kInvalidParams,
									std::string( "Invalid params: " ) + idx + ".pieces" + pidx +
									" must be a non-empty string" );
							}
							pieces.push_back( pv.asString() );
						}
						// C4 (2026-08-19): `construction` is a LIST of at most
						// AgentSession::kBuildPlanMaxConstructionMethods
						// methods -- a copper still is a `lathe` pot AND a
						// `sweep` coil, and the single-value field made the
						// second verb's gated schema unreachable for the whole
						// element.
						//
						// A BARE STRING IS ALSO ACCEPTED and normalized to a
						// one-entry list.  The schema says array, but models
						// routinely send a scalar where a schema says array,
						// and refusing that costs a turn and teaches nothing:
						// the intent of "construction": "lathe" is not
						// ambiguous.  Both tool-description surfaces state
						// that both forms work, so this is a documented
						// contract rather than a silent leniency.
						//
						// Everything past the shape check is
						// AgentSession::ValidateConstructionList -- the SAME
						// predicate FileBuildPlan re-checks with, so the wire
						// error and the session's own precondition cannot
						// disagree (the pattern `outline` already follows).
						const JsonValue* consVal = e.find( "construction" );
						std::vector<std::string> construction;
						if( !consVal || ( !consVal->isString() && !consVal->isArray() ) ) {
							return MakeError( idValue, kInvalidParams,
								std::string( "Invalid params: " ) + idx + ".construction is required "
								"-- an array of at most " +
								std::to_string( AgentSession::kBuildPlanMaxConstructionMethods ) +
								" of: " + enumList + " (a single method may also be given as a plain "
								"string)" );
						}
						if( consVal->isString() ) {
							construction.push_back( consVal->asString() );
						}
						else {
							for( std::size_t c = 0; c < consVal->size(); ++c ) {
								const JsonValue& cv = consVal->at( c );
								if( !cv.isString() ) {
									char cidx[24];
									std::snprintf( cidx, sizeof( cidx ), "[%d]", static_cast<int>( c ) );
									return MakeError( idValue, kInvalidParams,
										std::string( "Invalid params: " ) + idx + ".construction" + cidx +
										" must be a string -- one of: " + enumList );
								}
								construction.push_back( cv.asString() );
							}
						}
						{
							std::string cerr;
							if( !AgentSession::ValidateConstructionList( construction, cerr ) ) {
								return MakeError( idValue, kInvalidParams,
									std::string( "Invalid params: " ) + idx + ".construction " + cerr );
							}
						}
						const JsonValue* noteVal = e.find( "note" );
						if( noteVal && !noteVal->isString() ) {
							return MakeError( idValue, kInvalidParams,
								std::string( "Invalid params: " ) + idx + ".note must be a string when present" );
						}
						// G3a: `outline` is REQUIRED with NO opt-out value --
						// the force level the design resolves to (every element
						// gets a sketch; a blob is legal, an absence is not).
						// The grammar check is AgentSession::ValidateElementOutline,
						// the SAME predicate FileBuildPlan itself uses, so the
						// wire error and the session's own precondition can
						// never disagree about what a valid outline is.
						const JsonValue* outlineVal = e.find( "outline" );
						if( !outlineVal || !outlineVal->isString() ) {
							return MakeError( idValue, kInvalidParams,
								std::string( "Invalid params: " ) + idx + ".outline (string) is required "
								"-- a closed 2D polygon of at least 3 \"x y\" points separated by "
								"semicolons, e.g. \"0 0; 1 0; 1 2; 0 2\"" );
						}
						{
							std::string oerr;
							if( !AgentSession::ValidateElementOutline( outlineVal->asString(), oerr ) ) {
								return MakeError( idValue, kInvalidParams,
									std::string( "Invalid params: " ) + idx + ".outline " + oerr );
							}
						}
						// G3a: `view` is OPTIONAL and defaults to
						// AgentSession::kBuildPlanDefaultView.  Present-but-
						// wrong is an error (a typo'd view would otherwise be
						// silently recorded as `front` and mislead G3b's
						// comparison vantage); absent is not.
						const JsonValue* viewVal = e.find( "view" );
						if( viewVal && ( !viewVal->isString() ||
						                 !AgentSession::IsValidElementView( viewVal->asString() ) ) ) {
							return MakeError( idValue, kInvalidParams,
								std::string( "Invalid params: " ) + idx + ".view is `" +
								( viewVal->isString() ? viewVal->asString() : JsonSerialize( *viewVal ) ) +
								"` -- when present it must be one of: " + viewList );
						}
						AgentSession::AgentBuildPlanEntry entry;
						entry.element      = elemVal->asString();
						entry.pieces       = pieces;
						entry.construction = construction;
						entry.outline      = outlineVal->asString();
						if( viewVal ) entry.view = viewVal->asString();
						if( noteVal ) entry.note = noteVal->asString();
						entries.push_back( entry );
					}

					const AgentSession::AgentBuildPlanResult pr = s->FileBuildPlan( entries );
					// G3a: every outline was validated above with the SAME
					// predicate FileBuildPlan re-checks, so !ok is unreachable
					// from the wire.  Surface it as kInvalidParams anyway
					// rather than shipping filed:false with an ok-shaped
					// envelope -- a silent disagreement between the two layers
					// would otherwise look like a successful filing.
					if( !pr.ok ) return MakeError( idValue, kInvalidParams, pr.message );
					JsonValue elementsArr = JsonValue::MakeArray();
					for( std::size_t i = 0; i < pr.elements.size(); ++i ) {
						const AgentSession::AgentBuildPlanEntry& e = pr.elements[i];
						JsonValue o = JsonValue::MakeObject();
						o.set( "element",      JsonValue::MakeString( e.element ) );
						JsonValue piecesArr = JsonValue::MakeArray();
						for( std::size_t p = 0; p < e.pieces.size(); ++p )
							piecesArr.push_back( JsonValue::MakeString( e.pieces[p] ) );
						o.set( "pieces",       piecesArr );
						// C4 (2026-08-19): the echo is an ARRAY, always -- even
						// for a caller that sent a bare string.  One shape
						// back means a reader never has to branch on what was
						// sent, and it is the honest report of what the
						// session actually recorded.  Consumers of this field
						// (AgentChatLoop's transcript one-liner) read
						// array-or-string defensively so an older recorded
						// payload still renders.
						JsonValue consArr = JsonValue::MakeArray();
						for( std::size_t c = 0; c < e.construction.size(); ++c )
							consArr.push_back( JsonValue::MakeString( e.construction[c] ) );
						o.set( "construction", consArr );
						o.set( "note",         JsonValue::MakeString( e.note ) );
						// The per-element sketch FACTS, index-parallel to
						// pr.elements by construction (FileBuildPlan builds one
						// sketch per entry, in order).
						if( i < pr.sketches.size() ) {
							const AgentSession::AgentElementSketch& sk = pr.sketches[i];
							o.set( "outline",      JsonValue::MakeString( sk.outline ) );
							o.set( "view",         JsonValue::MakeString( sk.view ) );
							o.set( "pointCount",   JsonValue::MakeNumber( static_cast<double>( sk.pointCount ) ) );
							o.set( "areaFraction", JsonValue::MakeNumber( sk.areaFraction ) );
							o.set( "aspect",       JsonValue::MakeNumber( sk.aspect ) );
						}
						elementsArr.push_back( o );
					}
					JsonValue result = JsonValue::MakeObject();
					result.set( "filed",                JsonValue::MakeBool( pr.ok ) );
					result.set( "replacedPreviousPlan", JsonValue::MakeBool( pr.replacedPreviousPlan ) );
					result.set( "elementCount",         JsonValue::MakeNumber( static_cast<double>( pr.elements.size() ) ) );
					result.set( "elements",             elementsArr );
					// S1: the phase facts, so a driver (and a census) can read
					// the state transition without parsing prose.
					result.set( "phase",                JsonValue::MakeString(
						AgentSession::BuildPhaseName( s->BuildPhase() ) ) );
					// ARC 83 sec 4.1: the SESSION MODE travels with the phase
					// wherever session state is surfaced -- "building" while
					// the gates can fire, "refining" once they cannot.  A
					// census reads the transition here rather than inferring
					// it from the absence of refusals.
					result.set( "mode",                 JsonValue::MakeString(
						AgentSession::SessionModeName( s->SessionMode() ) ) );
					if( !s->ActiveElement().empty() )
						result.set( "activeElement",    JsonValue::MakeString( s->ActiveElement() ) );
					// G3a: the ONE composite sketch PNG, under the SAME
					// `png_base64` field name every other image-bearing verb
					// uses (see this handler's header comment).
					if( !pr.compositePng.empty() ) {
						result.set( "png_base64",      JsonValue::MakeString( Base64Encode( pr.compositePng ) ) );
						result.set( "byteLength",      JsonValue::MakeNumber( static_cast<double>( pr.compositePng.size() ) ) );
						result.set( "compositeWidth",  JsonValue::MakeNumber( static_cast<double>( pr.compositeWidth ) ) );
						result.set( "compositeHeight", JsonValue::MakeNumber( static_cast<double>( pr.compositeHeight ) ) );
						result.set( "sketchCanvas",    JsonValue::MakeNumber( static_cast<double>( AgentSession::kElementSketchCanvas ) ) );
					}
					result.set( "message",              JsonValue::MakeString( pr.message ) );
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// finish_element {}
				//   -> {ok, element, phase, nextElement?, chunks:[...],
				//       piecesNamed:[...], piecesNotNamed:[...], isolate?,
				//       isolateCandidates?, rendered, materialLook,
				//       png_base64?, byteLength?, imageWidth?, imageHeight?,
				//       message}
				//   S1 (2026-08-11).  Takes NO params: it closes whichever
				//   element is active, which is session state, not something a
				//   caller names -- naming it would invite a mismatch the model
				//   would then have to resolve.  READ-SAFE: it changes the
				//   session's phase and nothing in the Document.  It RENDERS
				//   (the isolate look of the element just closed), which is
				//   read-safe for exactly the reason the `render` verb is.
				//   ok:false is not an error envelope -- it means the call did
				//   nothing (no plan filed, already in the compose phase, or the
				//   protocol is off) and `message` says which.
				//--------------------------------------------------------------
				if( m == "finish_element" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					const AgentSession::AgentFinishElementResult fr = s->FinishElement();
					JsonValue result = JsonValue::MakeObject();
					result.set( "ok",    JsonValue::MakeBool( fr.ok ) );
					result.set( "phase", JsonValue::MakeString( fr.phase ) );
					result.set( "mode",  JsonValue::MakeString(
						AgentSession::SessionModeName( s->SessionMode() ) ) );
					if( !fr.element.empty() )     result.set( "element",     JsonValue::MakeString( fr.element ) );
					if( !fr.nextElement.empty() ) result.set( "nextElement", JsonValue::MakeString( fr.nextElement ) );
					if( fr.ok ) {
						JsonValue chunksArr = JsonValue::MakeArray();
						for( std::size_t i = 0; i < fr.chunks.size(); ++i )
							chunksArr.push_back( JsonValue::MakeString( fr.chunks[i] ) );
						result.set( "chunks", chunksArr );
						JsonValue namedArr = JsonValue::MakeArray();
						for( std::size_t i = 0; i < fr.piecesNamed.size(); ++i )
							namedArr.push_back( JsonValue::MakeString( fr.piecesNamed[i] ) );
						result.set( "piecesNamed", namedArr );
						JsonValue notNamedArr = JsonValue::MakeArray();
						for( std::size_t i = 0; i < fr.piecesNotNamed.size(); ++i )
							notNamedArr.push_back( JsonValue::MakeString( fr.piecesNotNamed[i] ) );
						result.set( "piecesNotNamed", notNamedArr );
						result.set( "rendered", JsonValue::MakeBool( fr.rendered ) );
						if( !fr.isolateObject.empty() ) {
							result.set( "isolate", JsonValue::MakeString( fr.isolateObject ) );
							result.set( "isolateCandidates",
								JsonValue::MakeNumber( static_cast<double>( fr.isolateCandidates ) ) );
						}
						// 2026-08-24 (the lit material look): whether the SECOND,
						// studio-lit panel made it into the image below.  A
						// tail-appended boolean beside the existing `rendered`,
						// never a second image field -- see
						// AgentFinishElementResult::png's doc for why a second
						// `png_base64` would reach no model on any transport.
						result.set( "materialLook", JsonValue::MakeBool( fr.materialLookRendered ) );
						// The isolate look rides under the SAME `png_base64`
						// field name every image-bearing verb uses, so every
						// image-retention and surfacing policy covers it with no
						// second code path.  Since 2026-08-24 those bytes are
						// normally a TWO-PANEL composite (draft form left,
						// studio-lit materials right) and `imageWidth`/
						// `imageHeight` describe the composite, not a panel.
						if( !fr.png.empty() ) {
							result.set( "png_base64",  JsonValue::MakeString( Base64Encode( fr.png ) ) );
							result.set( "byteLength",  JsonValue::MakeNumber( static_cast<double>( fr.png.size() ) ) );
							result.set( "imageWidth",  JsonValue::MakeNumber( static_cast<double>( fr.width ) ) );
							result.set( "imageHeight", JsonValue::MakeNumber( static_cast<double>( fr.height ) ) );
						}
					}
					result.set( "message", JsonValue::MakeString( fr.message ) );
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// reopen_element {element}
				//   -> {ok, element, phase, previousPhase, chunks:[...],
				//       unfinished:[...], message}
				//   S1 (2026-08-11).  READ-SAFE and NEVER GATED: it is the
				//   escape the compose-phase creation refusal names, so refusing
				//   it -- at the autonomy layer, by a phase rule, or by any cap
				//   -- would strand exactly the session that needs it.  The only
				//   -32602 is a missing/empty/non-string `element`; an element
				//   that is not in the filed plan comes back ok:false listing the
				//   filed names, because that is a STATE mismatch (which names
				//   exist depends on the session), not a param-SHAPE defect.
				//--------------------------------------------------------------
				if( m == "reopen_element" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					const JsonValue* elemVal = params.find( "element" );
					if( !elemVal || !elemVal->isString() || elemVal->asString().empty() ) {
						return MakeError( idValue, kInvalidParams,
							"Invalid params: 'element' (non-empty string) is required -- the name of an "
							"element in the filed build plan" );
					}
					const AgentSession::AgentReopenElementResult rr2 =
						s->ReopenElement( elemVal->asString() );
					JsonValue result = JsonValue::MakeObject();
					result.set( "ok",            JsonValue::MakeBool( rr2.ok ) );
					result.set( "phase",         JsonValue::MakeString( rr2.phase ) );
					result.set( "mode",          JsonValue::MakeString(
						AgentSession::SessionModeName( s->SessionMode() ) ) );
					result.set( "previousPhase", JsonValue::MakeString( rr2.previousPhase ) );
					if( !rr2.element.empty() ) result.set( "element", JsonValue::MakeString( rr2.element ) );
					if( rr2.ok ) {
						JsonValue chunksArr = JsonValue::MakeArray();
						for( std::size_t i = 0; i < rr2.chunks.size(); ++i )
							chunksArr.push_back( JsonValue::MakeString( rr2.chunks[i] ) );
						result.set( "chunks", chunksArr );
						JsonValue unfinishedArr = JsonValue::MakeArray();
						for( std::size_t i = 0; i < rr2.unfinished.size(); ++i )
							unfinishedArr.push_back( JsonValue::MakeString( rr2.unfinished[i] ) );
						result.set( "unfinished", unfinishedArr );
					}
					result.set( "message", JsonValue::MakeString( rr2.message ) );
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// build_element {element, height, notes?}
				//   -> {ok, element, provider, model, chunksExtracted,
				//       landed:[...], rejected:[{name?,kind?,reason}],
				//       chunkResults:[...], retryRan, retrySucceeded,
				//       sdfPartCount, bbox?:{min:[3],max:[3],height},
				//       capabilityRefusal?, message}
				//   S2 (2026-08-11).  MUTATING -- it inserts the chunks the
				//   builder returned through the ordinary InsertChunks path, so
				//   it is NOT on IsReadSafeVerb.  It is also deliberately NOT on
				//   IsProposeSafeVerb, for the same reason and with the same
				//   Propose-specific message shape the three scaffold verbs have
				//   (see the autonomy block above): adding a verb there ripples
				//   through every "N mutating verbs" prose restatement that
				//   SourceHygieneTest's verb-parity scan pins, and this slice
				//   does not need Propose reachability to be measured.
				//   `element` and `height` are validated HERE so a defect is a
				//   clean -32602 -- a schema error, which (like file_build_plan's)
				//   never touches any refusal counter.  Every STATE mismatch (not
				//   the active element, wrong phase, no completer installed) is an
				//   ok:false success envelope instead, because which element is
				//   active depends on the session, not on the request's shape.
				//--------------------------------------------------------------
				if( m == "build_element" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					const JsonValue* elemVal = params.find( "element" );
					if( !elemVal || !elemVal->isString() || elemVal->asString().empty() ) {
						return MakeError( idValue, kInvalidParams,
							"Invalid params: 'element' (non-empty string) is required -- the name of the "
							"ACTIVE element in the filed build plan" );
					}
					const JsonValue* hVal = params.find( "height" );
					if( !hVal || !hVal->isNumber() || !( hVal->asNumber() > 0.0 ) ) {
						return MakeError( idValue, kInvalidParams,
							"Invalid params: 'height' (a number greater than 0) is required -- the "
							"element's target extent in Y, in world units" );
					}
					std::string notes;
					{
						const JsonValue* nVal = params.find( "notes" );
						if( nVal ) {
							if( !nVal->isString() ) {
								return MakeError( idValue, kInvalidParams,
									"Invalid params: 'notes', when present, must be a string" );
							}
							notes = nVal->asString();
						}
					}

					const AgentSession::AgentBuildElementResult br =
						s->BuildElement( elemVal->asString(), hVal->asNumber(), notes );

					JsonValue result = JsonValue::MakeObject();
					result.set( "ok", JsonValue::MakeBool( br.ok ) );
					if( br.capabilityRefusal )
						result.set( "capabilityRefusal", JsonValue::MakeBool( true ) );
					if( !br.element.empty() )      result.set( "element",  JsonValue::MakeString( br.element ) );
					if( !br.providerName.empty() ) result.set( "provider", JsonValue::MakeString( br.providerName ) );
					if( !br.modelId.empty() )      result.set( "model",    JsonValue::MakeString( br.modelId ) );
					if( br.ok ) {
						result.set( "chunksExtracted",
							JsonValue::MakeNumber( static_cast<double>( br.chunksExtracted ) ) );
						JsonValue landedArr = JsonValue::MakeArray();
						for( std::size_t i = 0; i < br.landed.size(); ++i )
							landedArr.push_back( JsonValue::MakeString( br.landed[i] ) );
						result.set( "landed", landedArr );
						JsonValue rejArr = JsonValue::MakeArray();
						for( std::size_t i = 0; i < br.rejected.size(); ++i ) {
							JsonValue o = JsonValue::MakeObject();
							if( !br.rejected[i].name.empty() )
								o.set( "name", JsonValue::MakeString( br.rejected[i].name ) );
							if( !br.rejected[i].kind.empty() )
								o.set( "kind", JsonValue::MakeString( br.rejected[i].kind ) );
							o.set( "reason", JsonValue::MakeString( br.rejected[i].reason ) );
							rejArr.push_back( o );
						}
						result.set( "rejected", rejArr );
						JsonValue crArr = JsonValue::MakeArray();
						for( std::size_t i = 0; i < br.chunkResults.size(); ++i ) {
							// S1 fix-round's `element` attribution rides along here
							// too -- see ChunkResultJson's doc.
							crArr.push_back( ChunkResultJson( br.chunkResults[i],
								s->ChunkElement( br.chunkResults[i].name ) ) );
						}
						result.set( "chunkResults", crArr );
						result.set( "retryRan",       JsonValue::MakeBool( br.retryRan ) );
						result.set( "retrySucceeded", JsonValue::MakeBool( br.retrySucceeded ) );
						result.set( "sdfPartCount",
							JsonValue::MakeNumber( static_cast<double>( br.sdfPartCount ) ) );
						if( br.bboxValid ) {
							JsonValue bbox = JsonValue::MakeObject();
							JsonValue mn = JsonValue::MakeArray(), mx = JsonValue::MakeArray();
							for( int k = 0; k < 3; ++k ) {
								mn.push_back( JsonValue::MakeNumber( br.bboxMin[k] ) );
								mx.push_back( JsonValue::MakeNumber( br.bboxMax[k] ) );
							}
							bbox.set( "min", mn );
							bbox.set( "max", mx );
							bbox.set( "height", JsonValue::MakeNumber( br.bboxMax[1] - br.bboxMin[1] ) );
							result.set( "bbox", bbox );
						}
					}
					result.set( "message", JsonValue::MakeString( br.message ) );
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// place_element {element, position, scale?, orientation?}
				//   -> {ok, element, root, objects:[...], skipped:[{object,reason}],
				//       patchResults:[...], patchesApplied, patchesRejected,
				//       bbox?:{min:[3],max:[3]}, message}
				//   S2 (2026-08-11).  MUTATING (it submits one ProposePatches
				//   batch), so NOT read-safe and -- like build_element above --
				//   deliberately not on IsProposeSafeVerb either.  The only
				//   -32602s are param SHAPE defects; an element that is not in
				//   the plan, or has no placeable object, is an ok:false success
				//   envelope, for reopen_element's reason (a STATE mismatch, not
				//   a shape defect).
				//--------------------------------------------------------------
				if( m == "place_element" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					const JsonValue* elemVal = params.find( "element" );
					if( !elemVal || !elemVal->isString() || elemVal->asString().empty() ) {
						return MakeError( idValue, kInvalidParams,
							"Invalid params: 'element' (non-empty string) is required -- the name of an "
							"element in the filed build plan" );
					}
					const JsonValue* posVal = params.find( "position" );
					if( !posVal || !posVal->isString() || posVal->asString().empty() ) {
						return MakeError( idValue, kInvalidParams,
							"Invalid params: 'position' (a \"x y z\" string) is required -- where the "
							"element's base-centre goes in world space" );
					}
					std::string scaleStr, orientStr;
					{
						const JsonValue* sc = params.find( "scale" );
						if( sc ) {
							// Accepted as a NUMBER or as a STRING; the session
							// layer parses one canonical form.  87 step 5 made
							// the string form carry three per-axis factors as
							// well as one uniform one, which is why the schema
							// now advertises `string` -- a bare number is still
							// accepted, and still means a uniform factor.
							if( sc->isNumber() ) {
								char b[48];
								std::snprintf( b, sizeof( b ), "%.10g", sc->asNumber() );
								scaleStr = b;
							}
							else if( sc->isString() ) scaleStr = sc->asString();
							else {
								return MakeError( idValue, kInvalidParams,
									"Invalid params: 'scale', when present, must be a number (a uniform "
									"factor greater than 0) or a string -- one factor \"2\" or three "
									"\"sx sy sz\"" );
							}
						}
						const JsonValue* orv = params.find( "orientation" );
						if( orv ) {
							if( !orv->isString() ) {
								return MakeError( idValue, kInvalidParams,
									"Invalid params: 'orientation', when present, must be a \"ex ey ez\" "
									"string in degrees" );
							}
							orientStr = orv->asString();
						}
					}

					const AgentSession::AgentPlaceElementResult prr =
						s->PlaceElement( elemVal->asString(), posVal->asString(), scaleStr, orientStr );

					JsonValue result = JsonValue::MakeObject();
					result.set( "ok", JsonValue::MakeBool( prr.ok ) );
					if( !prr.element.empty() ) result.set( "element", JsonValue::MakeString( prr.element ) );
					// 87 step 5: the ROOT NODE the transform landed on.  Named
					// rather than left implicit, because it is a real, editable
					// chunk in the document -- a caller that wants to nudge the
					// element by hand patches this one name instead of N.
					if( !prr.root.empty() ) result.set( "root", JsonValue::MakeString( prr.root ) );
					JsonValue objArr = JsonValue::MakeArray();
					for( std::size_t i = 0; i < prr.objects.size(); ++i )
						objArr.push_back( JsonValue::MakeString( prr.objects[i] ) );
					result.set( "objects", objArr );
					JsonValue skipArr = JsonValue::MakeArray();
					for( std::size_t i = 0; i < prr.skipped.size(); ++i ) {
						JsonValue o = JsonValue::MakeObject();
						o.set( "object", JsonValue::MakeString( prr.skipped[i].object ) );
						o.set( "reason", JsonValue::MakeString( prr.skipped[i].reason ) );
						skipArr.push_back( o );
					}
					result.set( "skipped", skipArr );
					// The SAME per-element shape propose_patches emits -- arc 83
					// slice 6 made PatchResultJson the one definition of it, and
					// the keys are unchanged from the inline copy that stood here.
					JsonValue prArr = JsonValue::MakeArray();
					for( std::size_t i = 0; i < prr.patchResults.size(); ++i )
						prArr.push_back( PatchResultJson( prr.patchResults[i] ) );
					result.set( "patchResults", prArr );
					result.set( "patchesApplied",
						JsonValue::MakeNumber( static_cast<double>( prr.patchesApplied ) ) );
					result.set( "patchesRejected",
						JsonValue::MakeNumber( static_cast<double>( prr.patchesRejected ) ) );
					if( prr.bboxValid ) {
						JsonValue bbox = JsonValue::MakeObject();
						JsonValue mn = JsonValue::MakeArray(), mx = JsonValue::MakeArray();
						for( int k = 0; k < 3; ++k ) {
							mn.push_back( JsonValue::MakeNumber( prr.bboxMin[k] ) );
							mx.push_back( JsonValue::MakeNumber( prr.bboxMax[k] ) );
						}
						bbox.set( "min", mn );
						bbox.set( "max", mx );
						result.set( "bbox", bbox );
					}
					result.set( "message", JsonValue::MakeString( prr.message ) );
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// light_scene {notes?}
				//   -> {ok, provider, model, chunksExtracted, landed,
				//       rejected:[{name,kind,reason}], chunkResults, retryRan,
				//       retrySucceeded, contributions:[{name,kind,soloed,
				//       meanLuma?,share?,reason?}], soloableLights, soloed,
				//       allLightsMeanLuma?, sourcesPlanned, sourcesReturned,
				//       sourcesTruncated, completions,
				//       areaLights, zeroAreaLights, skyLights?, otherLights?,
				//       sources:[string], message}
				//   Arc 81 (2026-08-12), the clean-room LIGHTING pass.
				//   ARC 83 SLICE 2 (2026-08-12), the owner's direct redesign:
				//   ONE call is exactly TWO completions -- one enumeration
				//   completion answering WHAT IN THIS WORLD PHYSICALLY EMITS
				//   LIGHT, bounded at kLightSourceBudget sources, then one
				//   build completion authoring the chunks for ALL of them in
				//   one answer (plus at most one repair retry on the build).
				//   The verb, its params and every field above the arc-83
				//   block are unchanged, so the model's turn count does not
				//   grow; the new fields report the enumeration and the
				//   completions actually spent.
				//   MUTATING -- it inserts the chunks its pass returned through
				//   the ordinary InsertChunks path, so it is NOT on
				//   IsReadSafeVerb; it is also deliberately NOT on
				//   IsProposeSafeVerb, for build_element's reason and with the
				//   same Propose-specific message (see the autonomy block
				//   above).
				//   THE ONLY -32602 is a non-string `notes`.  There are no
				//   required params at all: which scene gets lit is a property
				//   of the session, not of the request, and every state outcome
				//   (no head, no completer, the per-session cap) is an ok:false
				//   success envelope rather than a schema error.
				//--------------------------------------------------------------
				if( m == "light_scene" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					std::string notes;
					{
						const JsonValue* nVal = params.find( "notes" );
						if( nVal ) {
							if( !nVal->isString() ) {
								return MakeError( idValue, kInvalidParams,
									"Invalid params: 'notes', when present, must be a string" );
							}
							notes = nVal->asString();
						}
					}

					const AgentSession::AgentLightSceneResult lr = s->LightScene( notes );

					JsonValue result = JsonValue::MakeObject();
					result.set( "ok", JsonValue::MakeBool( lr.ok ) );
					if( lr.capabilityRefusal )
						result.set( "capabilityRefusal", JsonValue::MakeBool( true ) );
					if( !lr.providerName.empty() ) result.set( "provider", JsonValue::MakeString( lr.providerName ) );
					if( !lr.modelId.empty() )      result.set( "model",    JsonValue::MakeString( lr.modelId ) );
					if( lr.ok ) {
						result.set( "chunksExtracted",
							JsonValue::MakeNumber( static_cast<double>( lr.chunksExtracted ) ) );
						JsonValue landedArr = JsonValue::MakeArray();
						for( std::size_t i = 0; i < lr.landed.size(); ++i )
							landedArr.push_back( JsonValue::MakeString( lr.landed[i] ) );
						result.set( "landed", landedArr );
						JsonValue rejArr = JsonValue::MakeArray();
						for( std::size_t i = 0; i < lr.rejected.size(); ++i ) {
							JsonValue o = JsonValue::MakeObject();
							if( !lr.rejected[i].name.empty() )
								o.set( "name", JsonValue::MakeString( lr.rejected[i].name ) );
							if( !lr.rejected[i].kind.empty() )
								o.set( "kind", JsonValue::MakeString( lr.rejected[i].kind ) );
							o.set( "reason", JsonValue::MakeString( lr.rejected[i].reason ) );
							rejArr.push_back( o );
						}
						result.set( "rejected", rejArr );
						JsonValue crArr = JsonValue::MakeArray();
						for( std::size_t i = 0; i < lr.chunkResults.size(); ++i ) {
							crArr.push_back( ChunkResultJson( lr.chunkResults[i],
								s->ChunkElement( lr.chunkResults[i].name ) ) );
						}
						result.set( "chunkResults", crArr );
						result.set( "retryRan",       JsonValue::MakeBool( lr.retryRan ) );
						result.set( "retrySucceeded", JsonValue::MakeBool( lr.retrySucceeded ) );
						result.set( "soloableLights",
							JsonValue::MakeNumber( static_cast<double>( lr.soloableLightCount ) ) );
						result.set( "soloed",
							JsonValue::MakeNumber( static_cast<double>( lr.soloedCount ) ) );
						// OMITTED when nothing was soloed: with no solo there
						// is no frame to have been the reference, and a 0.0
						// here would read as a black scene rather than as an
						// absent measurement (the inventory's own
						// omit-rather-than-fabricate rule).
						if( lr.soloedCount > 0 )
							result.set( "allLightsMeanLuma", JsonValue::MakeNumber( lr.allLightsMeanLuma ) );
						JsonValue conArr = JsonValue::MakeArray();
						for( std::size_t i = 0; i < lr.contributions.size(); ++i ) {
							const AgentSession::AgentLightContribution& c = lr.contributions[i];
							JsonValue o = JsonValue::MakeObject();
							o.set( "name", JsonValue::MakeString( c.name ) );
							o.set( "kind", JsonValue::MakeString( c.kind ) );
							o.set( "soloed", JsonValue::MakeBool( c.soloed ) );
							if( c.soloed ) {
								o.set( "meanLuma", JsonValue::MakeNumber( c.meanLuma ) );
								o.set( "share",    JsonValue::MakeNumber( c.shareOfSoloedTotal ) );
							}
							else if( !c.reason.empty() ) {
								o.set( "reason", JsonValue::MakeString( c.reason ) );
							}
							conArr.push_back( o );
						}
						result.set( "contributions", conArr );

						// ---- ARC 83 SLICE 2: the enumeration step, as STRUCTURE
						// and not only as prose.  A census that has to parse
						// the message to learn how many completions a call
						// spent is a census that will drift.
						result.set( "sourcesPlanned",
							JsonValue::MakeNumber( static_cast<double>( lr.sources.size() ) ) );
						result.set( "sourcesReturned",
							JsonValue::MakeNumber( static_cast<double>( lr.sourcesReturned ) ) );
						result.set( "sourcesTruncated", JsonValue::MakeBool( lr.sourcesTruncated ) );
						result.set( "completions",
							JsonValue::MakeNumber( static_cast<double>( lr.completionsSpent ) ) );
						result.set( "areaLights",
							JsonValue::MakeNumber( static_cast<double>( lr.areaLightsBuilt ) ) );
						result.set( "zeroAreaLights",
							JsonValue::MakeNumber( static_cast<double>( lr.zeroAreaLightsBuilt ) ) );
						// OMITTED when zero, for the same reason the inventory
						// omits what it did not measure: a kind that was never
						// built is not a fact worth a line.
						if( lr.skyLightsBuilt > 0 )
							result.set( "skyLights",
								JsonValue::MakeNumber( static_cast<double>( lr.skyLightsBuilt ) ) );
						if( lr.otherLightsBuilt > 0 )
							result.set( "otherLights",
								JsonValue::MakeNumber( static_cast<double>( lr.otherLightsBuilt ) ) );
						JsonValue srcArr = JsonValue::MakeArray();
						for( std::size_t i = 0; i < lr.sources.size(); ++i )
							srcArr.push_back( JsonValue::MakeString( lr.sources[i] ) );
						result.set( "sources", srcArr );
					}
					result.set( "message", JsonValue::MakeString( lr.message ) );
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// populate_scene {notes?}
				//   -> {ok, provider, model, chunksExtracted,
				//       created:[{name,geometry,material}],
				//       rejected:[{name,kind,reason}], chunkResults, retryRan,
				//       retrySucceeded, objectsBefore, objectsAfter, message}
				//   Arc 82 (2026-08-12), the clean-room POPULATION pass.
				//   MUTATING -- it inserts the standard_object chunks its pass
				//   returned through the ordinary InsertChunks path, so it is
				//   NOT on IsReadSafeVerb; it is also deliberately NOT on
				//   IsProposeSafeVerb, for light_scene's reason and with the
				//   same Propose-specific message (see the autonomy block
				//   above).
				//   THE ONLY -32602 is a non-string `notes`.  There are no
				//   required params at all: which scene gets populated is a
				//   property of the session, not of the request, and every
				//   state outcome (no head, no completer, no geometry to
				//   repeat, the per-session cap) is an ok:false success
				//   envelope rather than a schema error.
				//--------------------------------------------------------------
				if( m == "populate_scene" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					std::string notes;
					{
						const JsonValue* nVal = params.find( "notes" );
						if( nVal ) {
							if( !nVal->isString() ) {
								return MakeError( idValue, kInvalidParams,
									"Invalid params: 'notes', when present, must be a string" );
							}
							notes = nVal->asString();
						}
					}

					const AgentSession::AgentPopulateSceneResult pr = s->PopulateScene( notes );

					JsonValue result = JsonValue::MakeObject();
					result.set( "ok", JsonValue::MakeBool( pr.ok ) );
					if( pr.capabilityRefusal )
						result.set( "capabilityRefusal", JsonValue::MakeBool( true ) );
					if( !pr.providerName.empty() ) result.set( "provider", JsonValue::MakeString( pr.providerName ) );
					if( !pr.modelId.empty() )      result.set( "model",    JsonValue::MakeString( pr.modelId ) );
					if( pr.ok ) {
						result.set( "chunksExtracted",
							JsonValue::MakeNumber( static_cast<double>( pr.chunksExtracted ) ) );
						JsonValue createdArr = JsonValue::MakeArray();
						for( std::size_t i = 0; i < pr.created.size(); ++i ) {
							JsonValue o = JsonValue::MakeObject();
							o.set( "name", JsonValue::MakeString( pr.created[i].name ) );
							// OMITTED rather than sent empty: a repeat always
							// names both, but a chunk result whose identity
							// echo came back without them must not report a
							// blank as if it were a reference.
							if( !pr.created[i].geometry.empty() )
								o.set( "geometry", JsonValue::MakeString( pr.created[i].geometry ) );
							if( !pr.created[i].material.empty() )
								o.set( "material", JsonValue::MakeString( pr.created[i].material ) );
							createdArr.push_back( o );
						}
						result.set( "created", createdArr );
						JsonValue rejArr = JsonValue::MakeArray();
						for( std::size_t i = 0; i < pr.rejected.size(); ++i ) {
							JsonValue o = JsonValue::MakeObject();
							if( !pr.rejected[i].name.empty() )
								o.set( "name", JsonValue::MakeString( pr.rejected[i].name ) );
							if( !pr.rejected[i].kind.empty() )
								o.set( "kind", JsonValue::MakeString( pr.rejected[i].kind ) );
							o.set( "reason", JsonValue::MakeString( pr.rejected[i].reason ) );
							rejArr.push_back( o );
						}
						result.set( "rejected", rejArr );
						JsonValue crArr = JsonValue::MakeArray();
						for( std::size_t i = 0; i < pr.chunkResults.size(); ++i ) {
							crArr.push_back( ChunkResultJson( pr.chunkResults[i],
								s->ChunkElement( pr.chunkResults[i].name ) ) );
						}
						result.set( "chunkResults", crArr );
						result.set( "retryRan",       JsonValue::MakeBool( pr.retryRan ) );
						result.set( "retrySucceeded", JsonValue::MakeBool( pr.retrySucceeded ) );
						result.set( "objectsBefore",
							JsonValue::MakeNumber( static_cast<double>( pr.objectCountBefore ) ) );
						result.set( "objectsAfter",
							JsonValue::MakeNumber( static_cast<double>( pr.objectCountAfter ) ) );
					}
					result.set( "message", JsonValue::MakeString( pr.message ) );
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// environment_scene {notes?}
				//   -> {ok, capabilityRefusal?, provider, model, chunksExtracted,
				//       landed:[string], rejected:[{name?,kind?,reason}],
				//       chunkResults, patchResults, retryRan, retrySucceeded, completions,
				//       painters, media, boundPainter?, bindingApplied,
				//       bindingReason, rasterizer?, radianceMapBefore,
				//       radianceMapAfter, globalMediumBefore, globalMediumAfter,
				//       toneBefore?:{lumaMean,lumaStdDev,lumaP1,lumaP99},
				//       toneAfter?:{...}, message}
				//   Arc 83 slice 5 (2026-08-13), the clean-room ENVIRONMENT pass
				//   -- ONE completion (plus at most one repair retry) authoring
				//   the scene's SURROUND: the dome that fills the frame where no
				//   object is, and the medium light travels through.  It inserts
				//   painter / function / medium chunks through the ordinary
				//   InsertChunks path and then MAKES THE DOME BINDING itself by
				//   appending a rasterizer chunk that carries `radiance_map` --
				//   see AgentSession::EnvironmentScene for why an append rather
				//   than a patch (an appended painter referenced from an EARLIER
				//   rasterizer chunk would derive to a silent no-dome).
				//   MUTATING: not read-safe, and deliberately not on the Propose
				//   allowlist either, exactly like light_scene.
				//   THE ONLY -32602 is a non-string `notes`.
				//--------------------------------------------------------------
				if( m == "environment_scene" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					std::string notes;
					{
						const JsonValue* nVal = params.find( "notes" );
						if( nVal ) {
							if( !nVal->isString() ) {
								return MakeError( idValue, kInvalidParams,
									"Invalid params: 'notes', when present, must be a string" );
							}
							notes = nVal->asString();
						}
					}

					const AgentSession::AgentEnvironmentSceneResult er = s->EnvironmentScene( notes );

					JsonValue result = JsonValue::MakeObject();
					result.set( "ok", JsonValue::MakeBool( er.ok ) );
					if( er.capabilityRefusal )
						result.set( "capabilityRefusal", JsonValue::MakeBool( true ) );
					if( !er.providerName.empty() ) result.set( "provider", JsonValue::MakeString( er.providerName ) );
					if( !er.modelId.empty() )      result.set( "model",    JsonValue::MakeString( er.modelId ) );
					if( er.ok ) {
						result.set( "chunksExtracted",
							JsonValue::MakeNumber( static_cast<double>( er.chunksExtracted ) ) );
						JsonValue landedArr = JsonValue::MakeArray();
						for( std::size_t i = 0; i < er.landed.size(); ++i )
							landedArr.push_back( JsonValue::MakeString( er.landed[i] ) );
						result.set( "landed", landedArr );
						JsonValue rejArr = JsonValue::MakeArray();
						for( std::size_t i = 0; i < er.rejected.size(); ++i ) {
							JsonValue o = JsonValue::MakeObject();
							if( !er.rejected[i].name.empty() )
								o.set( "name", JsonValue::MakeString( er.rejected[i].name ) );
							if( !er.rejected[i].kind.empty() )
								o.set( "kind", JsonValue::MakeString( er.rejected[i].kind ) );
							o.set( "reason", JsonValue::MakeString( er.rejected[i].reason ) );
							rejArr.push_back( o );
						}
						result.set( "rejected", rejArr );
						JsonValue crArr = JsonValue::MakeArray();
						for( std::size_t i = 0; i < er.chunkResults.size(); ++i ) {
							crArr.push_back( ChunkResultJson( er.chunkResults[i],
								s->ChunkElement( er.chunkResults[i].name ) ) );
						}
						result.set( "chunkResults", crArr );
						// The TWO parameter edits the dome binding is made of,
						// in the same per-patch shape every other patching verb
						// emits -- an empty array when no binding was made, in
						// which case `bindingReason` says why.
						JsonValue prArr = JsonValue::MakeArray();
						for( std::size_t i = 0; i < er.patchResults.size(); ++i )
							prArr.push_back( PatchResultJson( er.patchResults[i] ) );
						result.set( "patchResults", prArr );
						result.set( "retryRan",       JsonValue::MakeBool( er.retryRan ) );
						result.set( "retrySucceeded", JsonValue::MakeBool( er.retrySucceeded ) );
						result.set( "completions",
							JsonValue::MakeNumber( static_cast<double>( er.completionsSpent ) ) );
						result.set( "painters",
							JsonValue::MakeNumber( static_cast<double>( er.paintersBuilt ) ) );
						result.set( "media",
							JsonValue::MakeNumber( static_cast<double>( er.mediaBuilt ) ) );
						// OMITTED rather than sent empty, the inventory's
						// omit-rather-than-fabricate rule: no painter was bound,
						// so there is no name to report and `bindingReason` says
						// why.
						if( !er.boundPainter.empty() )
							result.set( "boundPainter", JsonValue::MakeString( er.boundPainter ) );
						result.set( "bindingApplied", JsonValue::MakeBool( er.bindingApplied ) );
						if( !er.bindingReason.empty() )
							result.set( "bindingReason", JsonValue::MakeString( er.bindingReason ) );
						if( !er.rasterizerKind.empty() )
							result.set( "rasterizer", JsonValue::MakeString( er.rasterizerKind ) );
						result.set( "radianceMapBefore",  JsonValue::MakeBool( er.radianceMapBefore ) );
						result.set( "radianceMapAfter",   JsonValue::MakeBool( er.radianceMapAfter ) );
						result.set( "globalMediumBefore", JsonValue::MakeBool( er.globalMediumBefore ) );
						result.set( "globalMediumAfter",  JsonValue::MakeBool( er.globalMediumAfter ) );
						// THE HEADLINE, as STRUCTURE and not only as prose -- a
						// census that has to parse the message to read a tonal
						// spread is a census that will drift.  Each reading is
						// OMITTED ENTIRELY when it was not measured: zeros here
						// would read as a black frame rather than as an absent
						// measurement.
						if( er.toneBefore.measured ) {
							JsonValue t = JsonValue::MakeObject();
							t.set( "lumaMean",   JsonValue::MakeNumber( er.toneBefore.lumaMean ) );
							t.set( "lumaStdDev", JsonValue::MakeNumber( er.toneBefore.lumaStdDev ) );
							t.set( "lumaP1",     JsonValue::MakeNumber( er.toneBefore.lumaP1 ) );
							t.set( "lumaP99",    JsonValue::MakeNumber( er.toneBefore.lumaP99 ) );
							result.set( "toneBefore", t );
						}
						if( er.toneAfter.measured ) {
							JsonValue t = JsonValue::MakeObject();
							t.set( "lumaMean",   JsonValue::MakeNumber( er.toneAfter.lumaMean ) );
							t.set( "lumaStdDev", JsonValue::MakeNumber( er.toneAfter.lumaStdDev ) );
							t.set( "lumaP1",     JsonValue::MakeNumber( er.toneAfter.lumaP1 ) );
							t.set( "lumaP99",    JsonValue::MakeNumber( er.toneAfter.lumaP99 ) );
							result.set( "toneAfter", t );
						}
					}
					result.set( "message", JsonValue::MakeString( er.message ) );
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// frame_scene {notes?}
				//   -> {ok, capabilityRefusal?, provider, model, chunksExtracted,
				//       action, cameraKindBefore?, cameraNameBefore?,
				//       cameraKindAfter?, cameraNameAfter?, paramsApplied:[string],
				//       rejected:[{name?,kind?,reason}], patchResults, chunkResults,
				//       retryRan, retrySucceeded, completions,
				//       objectsBefore?, coveredBefore?, objectsAfter?, coveredAfter?,
				//       broughtIntoFrame:[string], pushedOutOfFrame:[string],
				//       message}
				//   Arc 83 slice 6 (2026-08-13), the clean-room FRAMING pass --
				//   ONE completion (plus at most one repair retry) returning ONE
				//   camera chunk, applied as a PATCH of the scene's existing
				//   camera when the kind matches and as an insert-then-remove
				//   REPLACEMENT when it does not.  The arc-80 inventory is run on
				//   BOTH sides of the edit, so the coverage figures are measured
				//   rather than asserted, and a reframe that covers FEWER objects
				//   is reported plainly and never auto-reverted.
				//   MUTATING: not read-safe, and deliberately not on the Propose
				//   allowlist either, exactly like light_scene.
				//   THE ONLY -32602 is a non-string `notes`.
				//--------------------------------------------------------------
				if( m == "frame_scene" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					std::string notes;
					{
						const JsonValue* nVal = params.find( "notes" );
						if( nVal ) {
							if( !nVal->isString() ) {
								return MakeError( idValue, kInvalidParams,
									"Invalid params: 'notes', when present, must be a string" );
							}
							notes = nVal->asString();
						}
					}

					const AgentSession::AgentFrameSceneResult fr = s->FrameScene( notes );

					JsonValue result = JsonValue::MakeObject();
					result.set( "ok", JsonValue::MakeBool( fr.ok ) );
					if( fr.capabilityRefusal )
						result.set( "capabilityRefusal", JsonValue::MakeBool( true ) );
					if( !fr.providerName.empty() ) result.set( "provider", JsonValue::MakeString( fr.providerName ) );
					if( !fr.modelId.empty() )      result.set( "model",    JsonValue::MakeString( fr.modelId ) );
					if( fr.ok ) {
						result.set( "chunksExtracted",
							JsonValue::MakeNumber( static_cast<double>( fr.chunksExtracted ) ) );
						result.set( "action", JsonValue::MakeString( fr.action ) );
						if( !fr.cameraKindBefore.empty() )
							result.set( "cameraKindBefore", JsonValue::MakeString( fr.cameraKindBefore ) );
						if( !fr.cameraNameBefore.empty() )
							result.set( "cameraNameBefore", JsonValue::MakeString( fr.cameraNameBefore ) );
						if( !fr.cameraKindAfter.empty() )
							result.set( "cameraKindAfter", JsonValue::MakeString( fr.cameraKindAfter ) );
						if( !fr.cameraNameAfter.empty() )
							result.set( "cameraNameAfter", JsonValue::MakeString( fr.cameraNameAfter ) );
						JsonValue paramArr = JsonValue::MakeArray();
						for( std::size_t i = 0; i < fr.paramsApplied.size(); ++i )
							paramArr.push_back( JsonValue::MakeString( fr.paramsApplied[i] ) );
						result.set( "paramsApplied", paramArr );
						JsonValue rejArr = JsonValue::MakeArray();
						for( std::size_t i = 0; i < fr.rejected.size(); ++i ) {
							JsonValue o = JsonValue::MakeObject();
							if( !fr.rejected[i].name.empty() )
								o.set( "name", JsonValue::MakeString( fr.rejected[i].name ) );
							if( !fr.rejected[i].kind.empty() )
								o.set( "kind", JsonValue::MakeString( fr.rejected[i].kind ) );
							o.set( "reason", JsonValue::MakeString( fr.rejected[i].reason ) );
							rejArr.push_back( o );
						}
						result.set( "rejected", rejArr );
						JsonValue prArr = JsonValue::MakeArray();
						for( std::size_t i = 0; i < fr.patchResults.size(); ++i )
							prArr.push_back( PatchResultJson( fr.patchResults[i] ) );
						result.set( "patchResults", prArr );
						JsonValue crArr = JsonValue::MakeArray();
						for( std::size_t i = 0; i < fr.chunkResults.size(); ++i ) {
							crArr.push_back( ChunkResultJson( fr.chunkResults[i],
								s->ChunkElement( fr.chunkResults[i].name ) ) );
						}
						result.set( "chunkResults", crArr );
						result.set( "retryRan",       JsonValue::MakeBool( fr.retryRan ) );
						result.set( "retrySucceeded", JsonValue::MakeBool( fr.retrySucceeded ) );
						result.set( "completions",
							JsonValue::MakeNumber( static_cast<double>( fr.completionsSpent ) ) );
						// OMITTED when the identity pass did not succeed: a 0
						// coverage count would read as "nothing is in frame"
						// rather than as "this was not measured".
						if( fr.measuredBefore ) {
							result.set( "objectsBefore",
								JsonValue::MakeNumber( static_cast<double>( fr.objectsBefore ) ) );
							result.set( "coveredBefore",
								JsonValue::MakeNumber( static_cast<double>( fr.coveredBefore ) ) );
						}
						if( fr.measuredAfter ) {
							result.set( "objectsAfter",
								JsonValue::MakeNumber( static_cast<double>( fr.objectsAfter ) ) );
							result.set( "coveredAfter",
								JsonValue::MakeNumber( static_cast<double>( fr.coveredAfter ) ) );
						}
						JsonValue inArr = JsonValue::MakeArray();
						for( std::size_t i = 0; i < fr.broughtIntoFrame.size(); ++i )
							inArr.push_back( JsonValue::MakeString( fr.broughtIntoFrame[i] ) );
						result.set( "broughtIntoFrame", inArr );
						JsonValue outArr = JsonValue::MakeArray();
						for( std::size_t i = 0; i < fr.pushedOutOfFrame.size(); ++i )
							outArr.push_back( JsonValue::MakeString( fr.pushedOutOfFrame[i] ) );
						result.set( "pushedOutOfFrame", outArr );
					}
					result.set( "message", JsonValue::MakeString( fr.message ) );
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// imagine_scene {description}
				//   -> {ok, imagined:bool, replacedPreviousTarget, provider,
				//       model, width, height, png_base64, byteLength,
				//       imageWidth, imageHeight, message}
				//   Arc 77 Phase 2 (2026-08-11).  READ-SAFE -- it records a
				//   per-session IMAGE TARGET and touches the Document not at
				//   all, so there is no headVersion, no conflict, no staging
				//   and no authority branch, exactly like file_build_plan.
				//
				//   THE ONLY SCHEMA ERROR is a missing/empty/non-string
				//   `description`, and it is a clean -32602.  That distinction
				//   is load-bearing, not cosmetic: a -32602 is a SCHEMA defect
				//   and DISARMS NOTHING (it never reaches AgentSession, so it
				//   cannot touch the gate's refusal counter and cannot trip the
				//   provider-failure disarm), whereas a PROVIDER failure --
				//   which arrives as ok:false from the session -- drops the
				//   imagine requirement for the session.  A model that
				//   mis-shapes the call must not be able to switch the
				//   mechanism off by doing so.
				//
				//   A provider with no image capability answers ok:false with
				//   an honest statement; the tool is still DECLARED there (one
				//   shared tool table for every provider -- see
				//   AgentChatCodecs.cpp's kToolDefs), so the wire shape is
				//   uniform and only the answer differs.
				//
				//   The generated image rides under the SAME `png_base64`
				//   field name every other image-bearing verb uses, so
				//   IsImageResult and every retention/elision policy built on
				//   it cover this with no second code path.
				//--------------------------------------------------------------
				if( m == "imagine_scene" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					const JsonValue* descVal = params.find( "description" );
					if( !descVal || !descVal->isString() || descVal->asString().empty() ) {
						return MakeError( idValue, kInvalidParams,
							"Invalid params: 'description' (non-empty string) is required -- your own "
							"words for what the finished scene should look like" );
					}
					const AgentSession::AgentImagineResult ir = s->ImagineScene( descVal->asString() );
					JsonValue result = JsonValue::MakeObject();
					result.set( "ok",      JsonValue::MakeBool( ir.ok ) );
					// `imagined` is the same bool under the name a reader of
					// the verb expects, mirroring file_build_plan's `filed`.
					result.set( "imagined", JsonValue::MakeBool( ir.ok ) );
					result.set( "replacedPreviousTarget", JsonValue::MakeBool( ir.replacedPreviousTarget ) );
					if( !ir.providerName.empty() )
						result.set( "provider", JsonValue::MakeString( ir.providerName ) );
					if( !ir.modelId.empty() )
						result.set( "model", JsonValue::MakeString( ir.modelId ) );
					// Two structured facts about WHY a refusal happened, so a
					// census (and a driver) can tell the three outcomes apart
					// without matching on prose: capability refusal, provider
					// failure that disarmed the requirement, or success.
					if( ir.capabilityRefusal )
						result.set( "capabilityAvailable", JsonValue::MakeBool( false ) );
					if( ir.requirementDisarmed )
						result.set( "requirementDisarmed", JsonValue::MakeBool( true ) );
					if( ir.ok ) {
						result.set( "width",  JsonValue::MakeNumber( static_cast<double>( ir.width ) ) );
						result.set( "height", JsonValue::MakeNumber( static_cast<double>( ir.height ) ) );
						if( !ir.png.empty() ) {
							result.set( "png_base64",  JsonValue::MakeString( Base64Encode( ir.png ) ) );
							result.set( "byteLength",  JsonValue::MakeNumber( static_cast<double>( ir.png.size() ) ) );
							result.set( "imageWidth",  JsonValue::MakeNumber( static_cast<double>( ir.width ) ) );
							result.set( "imageHeight", JsonValue::MakeNumber( static_cast<double>( ir.height ) ) );
						}
					}
					result.set( "message", JsonValue::MakeString( ir.message ) );
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// set_render_anchor {}  ->  {ok, pinned, anchorRevision,
				//                            previousAnchorRevision?, message}
				//   Doc 90 slice R1 (2026-08-22).  READ-SAFE -- it re-points a
				//   per-session bookmark and touches the Document not at all,
				//   so there is no headVersion, no conflict, no staging and no
				//   authority branch, exactly like file_build_plan.
				//
				//   IT TAKES NO PARAMETERS, so there is no schema error it can
				//   produce: any `params` object is accepted and ignored.  See
				//   AgentSession::SetRenderAnchor for why the no-argument
				//   shape was chosen over a revision argument (two frames held
				//   instead of every frame of the session, and "keep this one"
				//   is the move the measured failure needed -- time travel on
				//   the DOCUMENT is slice R2's verb).
				//
				//   `ok:false` means only "nothing to pin": no full-frame
				//   production render has completed in this session yet.  It
				//   is a plain answer, not an error, and nothing changed.
				//--------------------------------------------------------------
				if( m == "set_render_anchor" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					const AgentSession::AgentSetRenderAnchorResult ar = s->SetRenderAnchor();
					JsonValue result = JsonValue::MakeObject();
					result.set( "ok",     JsonValue::MakeBool( ar.ok ) );
					// `pinned` is the same bool under the name a reader of the
					// verb expects, mirroring file_build_plan's `filed` and
					// imagine_scene's `imagined`.
					result.set( "pinned", JsonValue::MakeBool( ar.pinned ) );
					if( ar.ok ) {
						result.set( "anchorRevision",
							JsonValue::MakeNumber( static_cast<double>( ar.anchorRevision ) ) );
						if( ar.hadPreviousAnchor )
							result.set( "previousAnchorRevision",
								JsonValue::MakeNumber( static_cast<double>( ar.previousAnchorRevision ) ) );
					}
					result.set( "message", JsonValue::MakeString( ar.message ) );
					return MakeSuccess( idValue, result );
				}

				if( m == "insert_material_scaffold" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					const JsonValue* familyVal = params.find( "family" );
					if( !familyVal || !familyVal->isString() ) {
						return MakeError( idValue, kInvalidParams,
							"Invalid params: 'family' (string) is required -- one of weathered_wood, "
							"rough_stone, brushed_metal, aged_bronze, glazed_ceramic" );
					}
					const JsonValue* nameVal = params.find( "name" );
					if( !nameVal || !nameVal->isString() ) {
						return MakeError( idValue, kInvalidParams, "Invalid params: 'name' (string) is required" );
					}
					const JsonValue* toneVal = params.find( "tone" );
					if( !toneVal || !toneVal->isString() ) {
						return MakeError( idValue, kInvalidParams,
							"Invalid params: 'tone' (string, \"r g b\" each 0..1) is required" );
					}
					const JsonValue* wearVal = params.find( "wear" );
					if( !wearVal || !wearVal->isNumber() ) {
						return MakeError( idValue, kInvalidParams, "Invalid params: 'wear' (number, 0..1) is required" );
					}
					const JsonValue* scaleVal = params.find( "scale" );
					if( !scaleVal || !scaleVal->isNumber() ) {
						return MakeError( idValue, kInvalidParams, "Invalid params: 'scale' (number, > 0) is required" );
					}
					RISE::Cst::CstHeadVersion base;
					std::string bErr;
					const int b = ParseBaseHeadVersionParam( params, base, bErr );
					if( b < 0 ) return MakeError( idValue, kInvalidParams, bErr );

					const AgentSession::AgentScaffoldResult sr = s->InsertMaterialScaffold(
						familyVal->asString(), nameVal->asString(), toneVal->asString(),
						wearVal->asNumber(), scaleVal->asNumber(), ( b == 1 ) ? &base : nullptr );

					if( !sr.ok ) return MakeError( idValue, kInvalidParams, sr.message );

					for( const AgentChunkResult& cr : sr.chunkResults ) {
						if( cr.queueFull ) return MakeProposalQueueFullError( idValue, "insert_material_scaffold" );
					}
					std::size_t appliedCount = 0;
					JsonValue resultsArr = JsonValue::MakeArray();
					for( const AgentChunkResult& cr : sr.chunkResults ) {
						if( cr.applied ) ++appliedCount;
						// S1 fix-round (2026-08-11): see ChunkResultJson's
						// `element` doc.
						resultsArr.push_back( ChunkResultJson( cr, s->ChunkElement( cr.name ) ) );
					}
					JsonValue material = JsonValue::MakeObject();
					material.set( "name", JsonValue::MakeString( sr.materialName ) );
					material.set( "kind", JsonValue::MakeString( sr.materialKind ) );
					JsonValue boundSlots = JsonValue::MakeArray();
					for( const auto& kv : sr.boundSlots ) {
						JsonValue e = JsonValue::MakeObject();
						e.set( "param",   JsonValue::MakeString( kv.first ) );
						e.set( "painter", JsonValue::MakeString( kv.second ) );
						boundSlots.push_back( e );
					}
					JsonValue result = JsonValue::MakeObject();
					result.set( "applied",    JsonValue::MakeNumber( static_cast<double>( appliedCount ) ) );
					result.set( "total",      JsonValue::MakeNumber( static_cast<double>( sr.chunkResults.size() ) ) );
					result.set( "results",    resultsArr );
					result.set( "family",     JsonValue::MakeString( sr.family ) );
					result.set( "name",       JsonValue::MakeString( nameVal->asString() ) );
					result.set( "material",   material );
					result.set( "boundSlots", boundSlots );
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// insert_geometry_scaffold {family, name, ..., baseHeadVersion?}
				//   -> {applied:number, total:number, results:[ChunkResultJson,...],
				//       family, name, geometry:{name,kind}}
				//   Arc-75 slice S3b (original four families) + slice E3
				//   (blended_chain, volume_bank): expand one of SIX
				//   geometry-family templates into a small chunk graph
				//   (every chunk named tmpl_<name>_<role>) -- one tool call
				//   instead of hand-composing the underlying chunk(s)
				//   yourself.  REQUIRED params differ BY FAMILY (E3 widened
				//   this beyond the original uniform five -- see
				//   AgentSession::InsertGeometryScaffold's header doc for
				//   the full per-family table):
				//     - displaced_slab / sweep_rail / blended_vessel /
				//       sdf_column: family, name, size (>0), detail (0..1),
				//       aspect (>0).
				//     - blended_chain: family, name, points (2-6
				//       semicolon-separated "x y z" triplets), size (>0),
				//       taper (0..1), detail (0..1) -- NO aspect.
				//     - volume_bank: family, name, size (>0), aspect (>0),
				//       detail (0..1), tone ("r g b" each 0..1) -- NO
				//       points/taper.
				//   A missing param for the RESOLVED family is a BLOCKING
				//   error naming it; `family` itself must resolve to one of
				//   the six names before any other param is checked (so an
				//   unrecognized family is refused before a family-specific
				//   "missing param" message would be misleading).  Chunk
				//   generation, full param validation, and the name-
				//   collision precheck (refuses the WHOLE expansion,
				//   document unchanged, before any chunk is generated) all
				//   happen in AgentSession::InsertGeometryScaffold; this
				//   handler only extracts params and serializes the result.
				//   The actual insert is submitted through the SAME
				//   InsertChunks path insert_chunks/insert_material_scaffold
				//   use, so authority/autonomy staging-vs-commit, conflict
				//   detection, and per-chunk `issues` are all inherited
				//   unchanged.  Unlike insert_material_scaffold, no
				//   material/standard_object is generated for FIVE of the
				//   six families -- the model wires those itself;
				//   `geometry` names the ONE geometry chunk to bind into a
				//   standard_object.geometry slot.  volume_bank is the sole
				//   exception (see InsertGeometryScaffold's doc for why) --
				//   its result additionally carries non-empty `material`/
				//   `medium`/`object` fields.
				//--------------------------------------------------------------
				if( m == "insert_geometry_scaffold" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					const JsonValue* familyVal = params.find( "family" );
					if( !familyVal || !familyVal->isString() ) {
						return MakeError( idValue, kInvalidParams,
							"Invalid params: 'family' (string) is required -- one of displaced_slab, "
							"sweep_rail, blended_vessel, sdf_column, blended_chain, volume_bank, quadruped "
							"(alias: creature)" );
					}
					const std::string familyStr = familyVal->asString();
					// P3 fix-round: validate `family` against the known list
					// BEFORE any per-family required-param check runs -- an
					// unrecognized family must always come back as "unknown
					// family", never as a misleading "'aspect' is required"
					// (or similar) just because the unrecognized name
					// happened to fall through to the wrong per-family
					// branch below.
					static const char* const kKnownGeometryScaffoldFamilies[] = {
						"displaced_slab", "sweep_rail", "blended_vessel", "sdf_column",
						"blended_chain", "volume_bank", "quadruped", "creature",
					};
					bool familyKnown = false;
					for( const char* f : kKnownGeometryScaffoldFamilies ) {
						if( familyStr == f ) { familyKnown = true; break; }
					}
					if( !familyKnown ) {
						return MakeError( idValue, kInvalidParams,
							"Invalid params: unknown family `" + familyStr + "` -- valid families are: "
							"displaced_slab, sweep_rail, blended_vessel, sdf_column, blended_chain, volume_bank, "
							"quadruped (alias: creature)" );
					}
					const bool isChain     = ( familyStr == "blended_chain" );
					const bool isBank      = ( familyStr == "volume_bank" );
					// Creature scaffold slice: quadruped needs neither
					// `detail` nor `aspect` (see AgentSession::
					// InsertGeometryScaffold's header doc for why) -- takes
					// `build` instead, parsed below.
					const bool isQuadruped = ( familyStr == "quadruped" || familyStr == "creature" );

					const JsonValue* nameVal = params.find( "name" );
					if( !nameVal || !nameVal->isString() ) {
						return MakeError( idValue, kInvalidParams, "Invalid params: 'name' (string) is required" );
					}
					const JsonValue* sizeVal = params.find( "size" );
					if( !sizeVal || !sizeVal->isNumber() ) {
						return MakeError( idValue, kInvalidParams, "Invalid params: 'size' (number, > 0) is required" );
					}
					double detailNum = 0.0;
					if( !isQuadruped ) {
						const JsonValue* detailVal = params.find( "detail" );
						if( !detailVal || !detailVal->isNumber() ) {
							return MakeError( idValue, kInvalidParams, "Invalid params: 'detail' (number, 0..1) is required" );
						}
						detailNum = detailVal->asNumber();
					}

					double aspectNum = 1.0;   // placeholder for blended_chain/quadruped, which ignore it
					std::string pointsStr;
					double taperNum = 0.0;
					std::string toneStr;
					std::string buildStr;

					if( isChain ) {
						const JsonValue* pointsVal = params.find( "points" );
						if( !pointsVal || !pointsVal->isString() ) {
							return MakeError( idValue, kInvalidParams,
								"Invalid params: 'points' (string, 2-6 semicolon-separated \"x y z\" triplets) "
								"is required for family blended_chain" );
						}
						pointsStr = pointsVal->asString();
						const JsonValue* taperVal = params.find( "taper" );
						if( !taperVal || !taperVal->isNumber() ) {
							return MakeError( idValue, kInvalidParams,
								"Invalid params: 'taper' (number, 0..1) is required for family blended_chain" );
						}
						taperNum = taperVal->asNumber();
					} else if( isQuadruped ) {
						const JsonValue* buildVal = params.find( "build" );
						if( buildVal ) {
							if( !buildVal->isString() ) {
								return MakeError( idValue, kInvalidParams, "Invalid params: 'build' must be a string" );
							}
							buildStr = buildVal->asString();
						}
					} else {
						const JsonValue* aspectVal = params.find( "aspect" );
						if( !aspectVal || !aspectVal->isNumber() ) {
							return MakeError( idValue, kInvalidParams, "Invalid params: 'aspect' (number, > 0) is required" );
						}
						aspectNum = aspectVal->asNumber();
						if( isBank ) {
							const JsonValue* toneVal = params.find( "tone" );
							if( !toneVal || !toneVal->isString() ) {
								return MakeError( idValue, kInvalidParams,
									"Invalid params: 'tone' (string, \"r g b\" each 0..1) is required for family volume_bank" );
							}
							toneStr = toneVal->asString();
						}
					}

					RISE::Cst::CstHeadVersion base;
					std::string bErr;
					const int b = ParseBaseHeadVersionParam( params, base, bErr );
					if( b < 0 ) return MakeError( idValue, kInvalidParams, bErr );

					const AgentSession::AgentGeometryScaffoldResult sr = s->InsertGeometryScaffold(
						familyStr, nameVal->asString(),
						sizeVal->asNumber(), detailNum, aspectNum,
						pointsStr, taperNum, toneStr, buildStr,
						( b == 1 ) ? &base : nullptr );

					if( !sr.ok ) return MakeError( idValue, kInvalidParams, sr.message );

					for( const AgentChunkResult& cr : sr.chunkResults ) {
						if( cr.queueFull ) return MakeProposalQueueFullError( idValue, "insert_geometry_scaffold" );
					}
					std::size_t appliedCount = 0;
					JsonValue resultsArr = JsonValue::MakeArray();
					for( const AgentChunkResult& cr : sr.chunkResults ) {
						if( cr.applied ) ++appliedCount;
						// S1 fix-round (2026-08-11): see ChunkResultJson's
						// `element` doc.
						resultsArr.push_back( ChunkResultJson( cr, s->ChunkElement( cr.name ) ) );
					}
					JsonValue geometry = JsonValue::MakeObject();
					geometry.set( "name", JsonValue::MakeString( sr.geometryName ) );
					geometry.set( "kind", JsonValue::MakeString( sr.geometryKind ) );
					JsonValue result = JsonValue::MakeObject();
					result.set( "applied",  JsonValue::MakeNumber( static_cast<double>( appliedCount ) ) );
					result.set( "total",    JsonValue::MakeNumber( static_cast<double>( sr.chunkResults.size() ) ) );
					result.set( "results",  resultsArr );
					result.set( "family",   JsonValue::MakeString( sr.family ) );
					result.set( "name",     JsonValue::MakeString( nameVal->asString() ) );
					result.set( "geometry", geometry );
					// E3: volume_bank is the sole family with non-empty
					// material/medium/object -- see BuildVolumeBank's doc.
					// Every other family keeps these three objects with
					// empty name/kind, matching `geometry` on the original
					// four families' shape.
					if( !sr.materialName.empty() || !sr.materialKind.empty() ) {
						JsonValue material = JsonValue::MakeObject();
						material.set( "name", JsonValue::MakeString( sr.materialName ) );
						material.set( "kind", JsonValue::MakeString( sr.materialKind ) );
						result.set( "material", material );
					}
					if( !sr.mediumName.empty() || !sr.mediumKind.empty() ) {
						JsonValue medium = JsonValue::MakeObject();
						medium.set( "name", JsonValue::MakeString( sr.mediumName ) );
						medium.set( "kind", JsonValue::MakeString( sr.mediumKind ) );
						result.set( "medium", medium );
					}
					if( !sr.objectName.empty() || !sr.objectKind.empty() ) {
						JsonValue object = JsonValue::MakeObject();
						object.set( "name", JsonValue::MakeString( sr.objectName ) );
						object.set( "kind", JsonValue::MakeString( sr.objectKind ) );
						result.set( "object", object );
					}
					if( !sr.message.empty() ) result.set( "message", JsonValue::MakeString( sr.message ) );
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// replace_geometry_scaffold {target, family, name, ..., baseHeadVersion?}
				//   -> {applied:number, total:number, results:[ChunkResultJson,...],
				//       status, retriable, headVersion,
				//       family, name, target, geometry:{name,kind},
				//       previousGeometry:{name,kind,removed,referrers[]},
				//       orphans:[...], message}
				//   R2 (2026-08-10): the SAME six-family expansion
				//   insert_geometry_scaffold performs (identical params, identical
				//   validation, identical generators -- one shared code path),
				//   but instead of leaving the model to wire the result it
				//   REBINDS an EXISTING standard_object's `geometry` slot to the
				//   new chunk, preserves every other param on that object
				//   (position/orientation/scale/material -- placement tuned by
				//   LOOKING survives a form change), and removes the old geometry
				//   chunk when nothing else references it.  ONE call = ONE head
				//   bump = ONE undo step; all-or-nothing on any refusal.
				//   `target` (string) is REQUIRED and names the object, NOT the
				//   geometry; `family` volume_bank is refused (it emits its own
				//   object -- see AgentSession::ReplaceGeometryScaffold's doc).
				//   Target resolution, the geometry-chunk-instead-of-object
				//   diagnosis, orphan policy, the E1/R1c candidate gates and the
				//   External-authority refusal all live in
				//   AgentSession::ReplaceGeometryScaffold; this handler only
				//   extracts params and serializes the result.
				//   R2 fix-round (2026-08-10, P1): top-level `status`/`retriable`/
				//   `headVersion` carry the composite commit's ACTUAL disposition --
				//   "applied" / "rejected" / "conflict" / "diagnosed" -- mirroring
				//   remove_chunks' identically-named fields.  EVERY outcome that
				//   reached a commit attempt returns MakeSuccess with these fields
				//   set (including a conflict, a transient-retriable reject, and a
				//   diagnosed-but-mutated code-3, none of which are "the request was
				//   invalid"); MakeError is reserved for genuine pre-commit request
				//   failures (bad/missing params, unknown family, the volume_bank
				//   refusal, target-resolution failure, name collision, no retained
				//   Document, the External-authority hard refusal).
				//--------------------------------------------------------------
				if( m == "replace_geometry_scaffold" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					const JsonValue* targetVal = params.find( "target" );
					if( !targetVal || !targetVal->isString() ) {
						return MakeError( idValue, kInvalidParams,
							"Invalid params: 'target' (string) is required -- the name of the standard_object "
							"whose geometry slot to rebind" );
					}
					const JsonValue* familyVal = params.find( "family" );
					if( !familyVal || !familyVal->isString() ) {
						return MakeError( idValue, kInvalidParams,
							"Invalid params: 'family' (string) is required -- one of displaced_slab, "
							"sweep_rail, blended_vessel, sdf_column, blended_chain, quadruped (alias: creature)" );
					}
					const std::string familyStr = familyVal->asString();
					// Same rule as insert_geometry_scaffold's own P3 fix: resolve
					// `family` against the known list BEFORE any per-family
					// required-param check, so an unrecognized family never comes
					// back as a misleading "'aspect' is required".  volume_bank IS
					// in the known list here (so it gets the specific "that family
					// emits its own object" refusal from the session, not a
					// generic "unknown family" that would send the model hunting
					// for a typo it did not make).
					static const char* const kKnownGeometryScaffoldFamiliesR2[] = {
						"displaced_slab", "sweep_rail", "blended_vessel", "sdf_column",
						"blended_chain", "volume_bank", "quadruped", "creature",
					};
					bool familyKnownR2 = false;
					for( const char* f : kKnownGeometryScaffoldFamiliesR2 ) {
						if( familyStr == f ) { familyKnownR2 = true; break; }
					}
					if( !familyKnownR2 ) {
						return MakeError( idValue, kInvalidParams,
							"Invalid params: unknown family `" + familyStr + "` -- valid families are: "
							"displaced_slab, sweep_rail, blended_vessel, sdf_column, blended_chain, quadruped "
							"(alias: creature) (volume_bank emits its own standard_object and cannot rebind an "
							"existing one -- use insert_geometry_scaffold for it)" );
					}
					const bool isChainR2     = ( familyStr == "blended_chain" );
					const bool isBankR2      = ( familyStr == "volume_bank" );
					const bool isQuadrupedR2 = ( familyStr == "quadruped" || familyStr == "creature" );

					const JsonValue* nameVal = params.find( "name" );
					if( !nameVal || !nameVal->isString() ) {
						return MakeError( idValue, kInvalidParams, "Invalid params: 'name' (string) is required" );
					}
					const JsonValue* sizeVal = params.find( "size" );
					if( !sizeVal || !sizeVal->isNumber() ) {
						return MakeError( idValue, kInvalidParams, "Invalid params: 'size' (number, > 0) is required" );
					}
					double detailNumR2 = 0.0;
					if( !isQuadrupedR2 ) {
						const JsonValue* detailVal = params.find( "detail" );
						if( !detailVal || !detailVal->isNumber() ) {
							return MakeError( idValue, kInvalidParams, "Invalid params: 'detail' (number, 0..1) is required" );
						}
						detailNumR2 = detailVal->asNumber();
					}

					double aspectNumR2 = 1.0;   // placeholder for blended_chain/quadruped, which ignore it
					std::string pointsStrR2;
					double taperNumR2 = 0.0;
					std::string toneStrR2;
					std::string buildStrR2;

					if( isChainR2 ) {
						const JsonValue* pointsVal = params.find( "points" );
						if( !pointsVal || !pointsVal->isString() ) {
							return MakeError( idValue, kInvalidParams,
								"Invalid params: 'points' (string, 2-6 semicolon-separated \"x y z\" triplets) "
								"is required for family blended_chain" );
						}
						pointsStrR2 = pointsVal->asString();
						const JsonValue* taperVal = params.find( "taper" );
						if( !taperVal || !taperVal->isNumber() ) {
							return MakeError( idValue, kInvalidParams,
								"Invalid params: 'taper' (number, 0..1) is required for family blended_chain" );
						}
						taperNumR2 = taperVal->asNumber();
					} else if( isQuadrupedR2 ) {
						const JsonValue* buildVal = params.find( "build" );
						if( buildVal ) {
							if( !buildVal->isString() ) {
								return MakeError( idValue, kInvalidParams, "Invalid params: 'build' must be a string" );
							}
							buildStrR2 = buildVal->asString();
						}
					} else {
						const JsonValue* aspectVal = params.find( "aspect" );
						if( !aspectVal || !aspectVal->isNumber() ) {
							return MakeError( idValue, kInvalidParams, "Invalid params: 'aspect' (number, > 0) is required" );
						}
						aspectNumR2 = aspectVal->asNumber();
						if( isBankR2 ) {
							const JsonValue* toneVal = params.find( "tone" );
							if( toneVal && toneVal->isString() ) toneStrR2 = toneVal->asString();
							// A missing `tone` is NOT diagnosed here: volume_bank is refused outright by the
							// session below, and demanding a param for a family that cannot be used would
							// bury the real reason under a param error.
						}
					}

					RISE::Cst::CstHeadVersion base;
					std::string bErr;
					const int b = ParseBaseHeadVersionParam( params, base, bErr );
					if( b < 0 ) return MakeError( idValue, kInvalidParams, bErr );

					const AgentSession::AgentGeometryScaffoldResult sr = s->ReplaceGeometryScaffold(
						targetVal->asString(), familyStr, nameVal->asString(),
						sizeVal->asNumber(), detailNumR2, aspectNumR2,
						pointsStrR2, taperNumR2, toneStrR2, buildStrR2,
						( b == 1 ) ? &base : nullptr );

					// R2 fix-round (P1): `sr.ok` now means "reached a commit-stage disposition" (see
					// AgentGeometryScaffoldResult::ok's doc) -- false ONLY for a genuine pre-commit refusal
					// (bad/missing params, unknown family, the volume_bank refusal, target-resolution
					// failure, name collision, no retained Document, the External-authority hard refusal).
					// EVERY commit-stage outcome -- applied, rejected, conflict, diagnosed, or a transient
					// retriable reject -- is `ok==true` and returns MakeSuccess below, carrying `status`/
					// `retriable`/`headVersion` so a caller can tell them apart without string-matching
					// `message`.  Before this fix, ALL of those non-applied commit outcomes were folded into
					// this one `MakeError( kInvalidParams, ... )` -- a code that is factually wrong (the
					// params were fine) and gave a caller no field to branch on; see `remove_chunks` above for
					// the identical envelope shape this now matches.
					if( !sr.ok ) return MakeError( idValue, kInvalidParams, sr.message );

					// P3 fix-round: NO queueFull check here (unlike insert_chunk(s)/remove_chunk(s)) --
					// this verb has no staging path at all (see ReplaceGeometryScaffold's AUTHORITY doc):
					// an External-authority session is refused with kInvalidParams above, BEFORE any commit
					// is built, so `AgentChunkResult::queueFull` can never be set on a result that reaches
					// this line.  (A prior revision of this handler looped `sr.chunkResults` checking it --
					// dead code, removed.)
					std::size_t appliedCountR2 = 0;
					JsonValue resultsArrR2 = JsonValue::MakeArray();
					for( const AgentChunkResult& cr : sr.chunkResults ) {
						if( cr.applied ) ++appliedCountR2;
						// S1 fix-round (2026-08-11): see ChunkResultJson's
						// `element` doc.  The chunk this verb ERASED (the
						// previous geometry) is reported under `previousGeometry`
						// and carries no attribution any more -- by the same fix
						// round that dropped it (AgentSession.cpp).
						resultsArrR2.push_back( ChunkResultJson( cr, s->ChunkElement( cr.name ) ) );
					}
					JsonValue geometryR2 = JsonValue::MakeObject();
					geometryR2.set( "name", JsonValue::MakeString( sr.geometryName ) );
					geometryR2.set( "kind", JsonValue::MakeString( sr.geometryKind ) );
					JsonValue prevGeom = JsonValue::MakeObject();
					prevGeom.set( "name",    JsonValue::MakeString( sr.previousGeometryName ) );
					prevGeom.set( "kind",    JsonValue::MakeString( sr.previousGeometryKind ) );
					prevGeom.set( "removed", JsonValue::MakeBool( sr.previousGeometryRemoved ) );
					if( !sr.previousGeometryReferrers.empty() ) {
						JsonValue refs = JsonValue::MakeArray();
						for( const std::string& rname : sr.previousGeometryReferrers )
							refs.push_back( JsonValue::MakeString( rname ) );
						prevGeom.set( "referrers", refs );
					}
					JsonValue result = JsonValue::MakeObject();
					result.set( "applied",   JsonValue::MakeNumber( static_cast<double>( appliedCountR2 ) ) );
					result.set( "total",     JsonValue::MakeNumber( static_cast<double>( sr.chunkResults.size() ) ) );
					result.set( "results",   resultsArrR2 );
					// R2 fix-round (P1): the composite-commit disposition, mirroring remove_chunks' own
					// top-level status/retriable/headVersion fields (see AgentGeometryScaffoldResult's doc).
					result.set( "status",     JsonValue::MakeString( sr.status ) );
					result.set( "retriable",  JsonValue::MakeBool( sr.retriable ) );
					result.set( "headVersion", HeadVersionJson( sr.headVersion ) );
					result.set( "family",   JsonValue::MakeString( sr.family ) );
					result.set( "name",     JsonValue::MakeString( nameVal->asString() ) );
					result.set( "target",   JsonValue::MakeString( sr.replacedObject ) );
					result.set( "geometry", geometryR2 );
					result.set( "previousGeometry", prevGeom );
					if( !sr.reportedOrphans.empty() ) {
						JsonValue orphans = JsonValue::MakeArray();
						for( const std::string& o : sr.reportedOrphans )
							orphans.push_back( JsonValue::MakeString( o ) );
						result.set( "orphans", orphans );
					}
					if( !sr.message.empty() ) result.set( "message", JsonValue::MakeString( sr.message ) );
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// collapse_to_instances {target?, name?, baseHeadVersion?}
				//   -> {ok,applied,rawCode,status,retriable,headVersion,message,
				//       source,geometry,collapsed,countU,countV,
				//       instanceChunks:[string,...],removedObjects:[string,...]}
				//   88 step 2 (2026-08-19): the VERB half of design-note
				//   condition C -- rewrite a run of hand-authored copies of one
				//   geometry as ONE `source` + `count_u` instancing chunk (two
				//   for a grid), keeping the first copy as the source.  The
				//   contract is that the RENDERED SCENE is unchanged, so every
				//   case it cannot prove that for is a REFUSAL with the document
				//   byte-identical (see AgentSession::CollapseToInstances).  A
				//   pre-commit refusal comes back as ok=false with the reason in
				//   `message` -- a SUCCESSFUL response, not a JSON-RPC error,
				//   because "these copies are not on a regular grid" is an answer
				//   rather than a malformed call.
				//--------------------------------------------------------------
				if( m == "collapse_to_instances" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					std::string targetStr, nameStr;
					if( const JsonValue* tv = params.find( "target" ) ) {
						if( tv->isString() ) targetStr = tv->asString();
						else if( !tv->isNull() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'target' must be a string" );
					}
					if( const JsonValue* nv = params.find( "name" ) ) {
						if( nv->isString() ) nameStr = nv->asString();
						else if( !nv->isNull() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'name' must be a string" );
					}
					RISE::Cst::CstHeadVersion base;
					std::string bErr;
					const int b = ParseBaseHeadVersionParam( params, base, bErr );
					if( b < 0 ) return MakeError( idValue, kInvalidParams, bErr );

					const AgentSession::AgentCollapseResult cir =
						s->CollapseToInstances( targetStr, nameStr, ( b == 1 ) ? &base : nullptr );

					JsonValue result = JsonValue::MakeObject();
					result.set( "ok",          JsonValue::MakeBool( cir.ok ) );
					result.set( "applied",     JsonValue::MakeBool( cir.applied ) );
					result.set( "rawCode",     JsonValue::MakeNumber( static_cast<double>( cir.rawCode ) ) );
					result.set( "status",      JsonValue::MakeString( cir.status ) );
					result.set( "retriable",   JsonValue::MakeBool( cir.retriable ) );
					result.set( "headVersion", HeadVersionJson( cir.headVersion ) );
					if( !cir.message.empty() )      result.set( "message",  JsonValue::MakeString( cir.message ) );
					if( !cir.sourceObject.empty() ) result.set( "source",   JsonValue::MakeString( cir.sourceObject ) );
					if( !cir.geometry.empty() )     result.set( "geometry", JsonValue::MakeString( cir.geometry ) );
					if( cir.collapsedCount > 0 )
						result.set( "collapsed", JsonValue::MakeNumber( static_cast<double>( cir.collapsedCount ) ) );
					if( cir.countU > 0 ) {
						result.set( "countU", JsonValue::MakeNumber( static_cast<double>( cir.countU ) ) );
						result.set( "countV", JsonValue::MakeNumber( static_cast<double>( cir.countV ) ) );
					}
					if( !cir.instanceChunks.empty() ) {
						JsonValue arr = JsonValue::MakeArray();
						for( const std::string& nm : cir.instanceChunks ) arr.push_back( JsonValue::MakeString( nm ) );
						result.set( "instanceChunks", arr );
					}
					if( !cir.removedObjects.empty() ) {
						JsonValue arr = JsonValue::MakeArray();
						for( const std::string& nm : cir.removedObjects ) arr.push_back( JsonValue::MakeString( nm ) );
						result.set( "removedObjects", arr );
					}
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// revert_to_revision {revision, baseHeadVersion?}
				//   -> {ok,applied,rawCode,status,retriable,headVersion,message,
				//       requestedRevision,previousRevision,
				//       oldestAvailableRevision,droppedAttributions?,
				//       restoredAttributions?}
				//   Doc 90 slice R2 (2026-08-23): restore the DOCUMENT to its
				//   text as of an earlier head revision of this session, as ONE
				//   NEW commit -- append-only history, one undo step (see
				//   AgentSession::RevertToRevision).  Every refusal leaves the
				//   document byte-identical, and comes back as ok=false with the
				//   reason in `message` -- a SUCCESSFUL response, not a JSON-RPC
				//   error, for collapse_to_instances' reason: "that revision has
				//   aged out, the oldest still here is 40" is an ANSWER rather
				//   than a malformed call, and the model's next move is in it.
				//   A missing/ill-typed `revision` IS malformed, so that one is
				//   an error.
				//--------------------------------------------------------------
				if( m == "revert_to_revision" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					const JsonValue* rv = params.find( "revision" );
					if( !rv || !rv->isNumber() ) {
						return MakeError( idValue, kInvalidParams,
							"Invalid params: 'revision' (number, a head revision this session has already "
							"reported) is required" );
					}
					const double rvNum = rv->asNumber();
					// The SAME bound ParseBaseHeadVersionParam applies to a head
					// version on this wire: 2^53, past which a JSON number
					// cannot represent consecutive integers at all.
					if( !( rvNum >= 0.0 && rvNum <= 9007199254740992.0 &&
					       rvNum == std::floor( rvNum ) ) ) {
						return MakeError( idValue, kInvalidParams,
							"Invalid params: 'revision' must be a non-negative whole number" );
					}
					RISE::Cst::CstHeadVersion base;
					std::string bErr;
					const int b = ParseBaseHeadVersionParam( params, base, bErr );
					if( b < 0 ) return MakeError( idValue, kInvalidParams, bErr );

					const AgentSession::AgentRevertResult rr2 =
						s->RevertToRevision( static_cast<std::uint64_t>( rvNum ),
						                     ( b == 1 ) ? &base : nullptr );

					JsonValue result = JsonValue::MakeObject();
					result.set( "ok",          JsonValue::MakeBool( rr2.ok ) );
					result.set( "applied",     JsonValue::MakeBool( rr2.applied ) );
					result.set( "rawCode",     JsonValue::MakeNumber( static_cast<double>( rr2.rawCode ) ) );
					result.set( "status",      JsonValue::MakeString( rr2.status ) );
					result.set( "retriable",   JsonValue::MakeBool( rr2.retriable ) );
					result.set( "headVersion", HeadVersionJson( rr2.headVersion ) );
					result.set( "requestedRevision",
						JsonValue::MakeNumber( static_cast<double>( rr2.requestedRevision ) ) );
					result.set( "previousRevision",
						JsonValue::MakeNumber( static_cast<double>( rr2.previousRevision ) ) );
					result.set( "oldestAvailableRevision",
						JsonValue::MakeNumber( static_cast<double>( rr2.oldestAvailableRevision ) ) );
					if( rr2.droppedAttributions > 0 ) {
						result.set( "droppedAttributions",
							JsonValue::MakeNumber( static_cast<double>( rr2.droppedAttributions ) ) );
					}
					// The SYMMETRIC half (doc 90 R2 fix round): a restore can
					// bring a chunk -- and its build-element attribution --
					// BACK, which a client tracking the ledger needs to see
					// for the same reason it needs the drops.  Same
					// present-only-when-nonzero shape.
					if( rr2.restoredAttributions > 0 ) {
						result.set( "restoredAttributions",
							JsonValue::MakeNumber( static_cast<double>( rr2.restoredAttributions ) ) );
					}
					if( !rr2.message.empty() ) result.set( "message", JsonValue::MakeString( rr2.message ) );
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// vary_material {material?, all?, baseHeadVersion?}
				//   -> {ok,applied,rawCode,status,retriable,headVersion,message,
				//       material,materialKind,painter,slots:[string,...],
				//       previousRoughness,qualifying,objects}
				//   88 S5 (2026-08-20): the VERB half of design-note condition D --
				//   bind ONE material's roughness slot(s) to a
				//   `scalar_painter { expression ... }` fbm wear field banded
				//   around the constant that is already there.  A pre-commit
				//   refusal comes back as ok=false with the reason in `message` --
				//   a SUCCESSFUL response, not a JSON-RPC error, because "no
				//   material here has a readable constant microsurface" is an
				//   answer rather than a malformed call (the same shape
				//   collapse_to_instances uses).
				//
				//   Materials-realism item 2 (2026-08-24): `all:true` routes to
				//   AgentSession::VaryMaterialAll instead -- ONE call fixes EVERY
				//   currently-flagged material (capped, `remaining` names the
				//   rest) rather than requiring one call per material, which
				//   measurement showed models rarely do on their own (coverage-
				//   per-call observed at 1 against 17-20 flagged materials).
				//   `all` and `material` are mutually exclusive: `all:true` with
				//   a `material` string is a malformed call, same posture as any
				//   other param combination this dispatcher already refuses
				//   rather than silently prioritizes one over the other.
				//--------------------------------------------------------------
				if( m == "vary_material" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					std::string materialStr;
					if( const JsonValue* mv = params.find( "material" ) ) {
						if( mv->isString() ) materialStr = mv->asString();
						else if( !mv->isNull() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'material' must be a string" );
					}
					bool allFlag = false;
					if( const JsonValue* av = params.find( "all" ) ) {
						if( av->isBool() ) allFlag = av->asBool();
						else if( !av->isNull() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'all' must be a boolean" );
					}
					if( allFlag && !materialStr.empty() )
						return MakeError( idValue, kInvalidParams,
							"Invalid params: 'all' and 'material' are mutually exclusive -- pass 'all':true to "
							"fix every flagged material, or 'material' to name one" );
					RISE::Cst::CstHeadVersion base;
					std::string bErr;
					const int b = ParseBaseHeadVersionParam( params, base, bErr );
					if( b < 0 ) return MakeError( idValue, kInvalidParams, bErr );

					if( allFlag ) {
						const AgentSession::AgentVaryMaterialBatchResult br =
							s->VaryMaterialAll( ( b == 1 ) ? &base : nullptr );
						JsonValue result = JsonValue::MakeObject();
						result.set( "ok",         JsonValue::MakeBool( br.ok ) );
						// Review-round P2-2: matches the single-material form
						// (and AgentCollapseResult) -- always set, "" for a
						// normal (non-conflict/non-rejected) outcome, so the
						// wire's generic status=="conflict"/"rejected"
						// interception can catch a batch refusal.
						result.set( "status",     JsonValue::MakeString( br.status ) );
						result.set( "qualifying", JsonValue::MakeNumber( static_cast<double>( br.qualifyingMaterials ) ) );
						result.set( "applied",    JsonValue::MakeNumber( static_cast<double>( br.appliedCount ) ) );
						result.set( "refused",    JsonValue::MakeNumber( static_cast<double>( br.refusedCount ) ) );
						result.set( "remaining",  JsonValue::MakeNumber( static_cast<double>( br.remainingCount ) ) );
						if( !br.message.empty() ) result.set( "message", JsonValue::MakeString( br.message ) );
						JsonValue arr = JsonValue::MakeArray();
						for( const AgentSession::AgentVaryMaterialResult& vr : br.perMaterial ) {
							JsonValue entry = JsonValue::MakeObject();
							entry.set( "applied", JsonValue::MakeBool( vr.applied ) );
							if( !vr.material.empty() )     entry.set( "material",     JsonValue::MakeString( vr.material ) );
							if( !vr.materialKind.empty() ) entry.set( "materialKind", JsonValue::MakeString( vr.materialKind ) );
							if( !vr.painterChunk.empty() ) entry.set( "painter",      JsonValue::MakeString( vr.painterChunk ) );
							if( !vr.message.empty() )      entry.set( "message",      JsonValue::MakeString( vr.message ) );
							arr.push_back( entry );
						}
						result.set( "perMaterial", arr );
						return MakeSuccess( idValue, result );
					}

					const AgentSession::AgentVaryMaterialResult vr =
						s->VaryMaterial( materialStr, ( b == 1 ) ? &base : nullptr );

					JsonValue result = JsonValue::MakeObject();
					result.set( "ok",          JsonValue::MakeBool( vr.ok ) );
					result.set( "applied",     JsonValue::MakeBool( vr.applied ) );
					result.set( "rawCode",     JsonValue::MakeNumber( static_cast<double>( vr.rawCode ) ) );
					result.set( "status",      JsonValue::MakeString( vr.status ) );
					result.set( "retriable",   JsonValue::MakeBool( vr.retriable ) );
					result.set( "headVersion", HeadVersionJson( vr.headVersion ) );
					if( !vr.message.empty() )      result.set( "message",      JsonValue::MakeString( vr.message ) );
					if( !vr.material.empty() )     result.set( "material",     JsonValue::MakeString( vr.material ) );
					if( !vr.materialKind.empty() ) result.set( "materialKind", JsonValue::MakeString( vr.materialKind ) );
					if( !vr.painterChunk.empty() ) result.set( "painter",      JsonValue::MakeString( vr.painterChunk ) );
					if( !vr.reboundSlots.empty() ) {
						JsonValue arr = JsonValue::MakeArray();
						for( const std::string& nm : vr.reboundSlots ) arr.push_back( JsonValue::MakeString( nm ) );
						result.set( "slots", arr );
					}
					if( !vr.material.empty() )
						result.set( "previousRoughness", JsonValue::MakeNumber( vr.previousRoughness ) );
					result.set( "qualifying", JsonValue::MakeNumber( static_cast<double>( vr.qualifyingMaterials ) ) );
					result.set( "objects",    JsonValue::MakeNumber( static_cast<double>( vr.boundObjects ) ) );
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// fix_blend_scale {target?, baseHeadVersion?}
				//   -> {ok,applied,rawCode,status,retriable,headVersion,message,
				//       offendersFound,fixed,remaining,perJoint:[string,...]}
				//   Cat plan item 1 (2026-08-25): the CALLABLE VERB half of
				//   design-note condition J (the blend-scale law) -- clamps
				//   every offending smin joint's k down to its safe bound in
				//   ONE call, using the SAME ScanSdfGeometryBlendScaleOffenders_
				//   scan the note and diagnostic read.  `target` (optional)
				//   scopes the scan-and-fix to one named sdf_geometry chunk;
				//   omitted, it covers the whole document.  A pre-commit
				//   refusal (nothing to fix, an unresolvable target) comes
				//   back as ok=false with the reason in `message` -- a
				//   SUCCESSFUL response, not a JSON-RPC error, the same shape
				//   vary_material uses for its own "nothing qualifies" case.
				//--------------------------------------------------------------
				if( m == "fix_blend_scale" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					std::string targetStr;
					if( const JsonValue* tv = params.find( "target" ) ) {
						if( tv->isString() ) targetStr = tv->asString();
						else if( !tv->isNull() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'target' must be a string" );
					}
					RISE::Cst::CstHeadVersion base;
					std::string bErr;
					const int b = ParseBaseHeadVersionParam( params, base, bErr );
					if( b < 0 ) return MakeError( idValue, kInvalidParams, bErr );

					const AgentSession::AgentFixBlendScaleResult fr =
						s->FixBlendScale( targetStr, ( b == 1 ) ? &base : nullptr );

					JsonValue result = JsonValue::MakeObject();
					result.set( "ok",             JsonValue::MakeBool( fr.ok ) );
					result.set( "applied",        JsonValue::MakeBool( fr.applied ) );
					result.set( "rawCode",        JsonValue::MakeNumber( static_cast<double>( fr.rawCode ) ) );
					result.set( "status",         JsonValue::MakeString( fr.status ) );
					result.set( "retriable",      JsonValue::MakeBool( fr.retriable ) );
					result.set( "headVersion",    HeadVersionJson( fr.headVersion ) );
					if( !fr.message.empty() ) result.set( "message", JsonValue::MakeString( fr.message ) );
					result.set( "offendersFound", JsonValue::MakeNumber( static_cast<double>( fr.offendersFound ) ) );
					result.set( "fixed",          JsonValue::MakeNumber( static_cast<double>( fr.fixedCount ) ) );
					result.set( "remaining",      JsonValue::MakeNumber( static_cast<double>( fr.remainingCount ) ) );
					if( !fr.perJointSummary.empty() ) {
						JsonValue arr = JsonValue::MakeArray();
						for( const std::string& line : fr.perJointSummary ) arr.push_back( JsonValue::MakeString( line ) );
						result.set( "perJoint", arr );
					}
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// add_wear {material?, baseHeadVersion?}
				//   -> {ok,applied,rawCode,status,retriable,headVersion,message,
				//       material,materialKind,colorSlot,painter,roughPainter,
				//       roughSlots:[string,...],previousRoughness,baseColor:[r,g,b],
				//       geometry,qualifying,objects}
				//   GEOMETRY_SHADING_SIGNALS sec 11 / sec 13 Phase 4 (2026-08-30):
				//   the VERB half of design-note condition L -- rewrite ONE
				//   material's colour (and, where it has one, its roughness) into
				//   the curvature wear composition the 2026-08-29 census proved
				//   models will not author unprompted.  A pre-commit refusal comes
				//   back as ok=false with the reason in `message` -- a SUCCESSFUL
				//   response, not a JSON-RPC error, the same shape vary_material
				//   uses for its own "nothing qualifies" case.
				//--------------------------------------------------------------
				if( m == "add_wear" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					std::string materialStr;
					if( const JsonValue* mv = params.find( "material" ) ) {
						if( mv->isString() ) materialStr = mv->asString();
						else if( !mv->isNull() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'material' must be a string" );
					}
					RISE::Cst::CstHeadVersion base;
					std::string bErr;
					const int b = ParseBaseHeadVersionParam( params, base, bErr );
					if( b < 0 ) return MakeError( idValue, kInvalidParams, bErr );

					const AgentSession::AgentAddWearResult wr =
						s->AddWear( materialStr, ( b == 1 ) ? &base : nullptr );

					JsonValue result = JsonValue::MakeObject();
					result.set( "ok",          JsonValue::MakeBool( wr.ok ) );
					result.set( "applied",     JsonValue::MakeBool( wr.applied ) );
					result.set( "rawCode",     JsonValue::MakeNumber( static_cast<double>( wr.rawCode ) ) );
					result.set( "status",      JsonValue::MakeString( wr.status ) );
					result.set( "retriable",   JsonValue::MakeBool( wr.retriable ) );
					result.set( "headVersion", HeadVersionJson( wr.headVersion ) );
					if( !wr.message.empty() )          result.set( "message",      JsonValue::MakeString( wr.message ) );
					if( !wr.material.empty() )         result.set( "material",     JsonValue::MakeString( wr.material ) );
					if( !wr.materialKind.empty() )     result.set( "materialKind", JsonValue::MakeString( wr.materialKind ) );
					if( !wr.colorSlot.empty() )        result.set( "colorSlot",    JsonValue::MakeString( wr.colorSlot ) );
					if( !wr.colorPainter.empty() )     result.set( "painter",      JsonValue::MakeString( wr.colorPainter ) );
					if( !wr.roughnessPainter.empty() ) result.set( "roughPainter", JsonValue::MakeString( wr.roughnessPainter ) );
					if( !wr.roughnessSlots.empty() ) {
						JsonValue arr = JsonValue::MakeArray();
						for( const std::string& nm : wr.roughnessSlots ) arr.push_back( JsonValue::MakeString( nm ) );
						result.set( "roughSlots", arr );
					}
					if( !wr.geometryKind.empty() )     result.set( "geometry",     JsonValue::MakeString( wr.geometryKind ) );
					if( !wr.material.empty() ) {
						result.set( "previousRoughness", JsonValue::MakeNumber( wr.previousRoughness ) );
						JsonValue rgb = JsonValue::MakeArray();
						rgb.push_back( JsonValue::MakeNumber( wr.baseR ) );
						rgb.push_back( JsonValue::MakeNumber( wr.baseG ) );
						rgb.push_back( JsonValue::MakeNumber( wr.baseB ) );
						result.set( "baseColor", rgb );
					}
					result.set( "qualifying", JsonValue::MakeNumber( static_cast<double>( wr.qualifyingMaterials ) ) );
					result.set( "objects",    JsonValue::MakeNumber( static_cast<double>( wr.boundObjects ) ) );
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// add_wetness {material?, baseHeadVersion?}
				//   -> {ok,applied,rawCode,status,retriable,headVersion,message,
				//       material,materialKind,wrappedInCoat,coatedMaterial,
				//       coatWeightPainter,coatRoughnessPainter,rebindObjectCount,
				//       reflectanceSlot,reflectancePainter,
				//       scatteringSlots:[string,...],scatteringPainters:[string,...],
				//       baseColor:[r,g,b],geometry,geometryUniform,isMetallic,
				//       isOrenNayar,qualifying,objects}
				//   docs/WETNESS_COAT_DESIGN.md Phase 1 + Phase 2 item 8
				//   (2026-08-31): apply the two-mask (damp/wet) wetness
				//   composition to ONE material -- a Lambertian base is WRAPPED
				//   in a new `coated_material` chunk (the original left
				//   untouched, bound objects rebound to the wrapper), a GGX/PBR
				//   base in place.  A pre-commit refusal comes back as ok=false with the
				//   reason in `message`, the same shape add_wear/vary_material use.
				//--------------------------------------------------------------
				if( m == "add_wetness" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					std::string materialStr;
					if( const JsonValue* mv = params.find( "material" ) ) {
						if( mv->isString() ) materialStr = mv->asString();
						else if( !mv->isNull() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'material' must be a string" );
					}
					RISE::Cst::CstHeadVersion base;
					std::string bErr;
					const int b = ParseBaseHeadVersionParam( params, base, bErr );
					if( b < 0 ) return MakeError( idValue, kInvalidParams, bErr );

					const AgentSession::AgentAddWetnessResult wr =
						s->AddWetness( materialStr, ( b == 1 ) ? &base : nullptr );

					JsonValue result = JsonValue::MakeObject();
					result.set( "ok",          JsonValue::MakeBool( wr.ok ) );
					result.set( "applied",     JsonValue::MakeBool( wr.applied ) );
					result.set( "rawCode",     JsonValue::MakeNumber( static_cast<double>( wr.rawCode ) ) );
					result.set( "status",      JsonValue::MakeString( wr.status ) );
					result.set( "retriable",   JsonValue::MakeBool( wr.retriable ) );
					result.set( "headVersion", HeadVersionJson( wr.headVersion ) );
					if( !wr.message.empty() )      result.set( "message",      JsonValue::MakeString( wr.message ) );
					if( !wr.material.empty() )     result.set( "material",     JsonValue::MakeString( wr.material ) );
					if( !wr.materialKind.empty() ) result.set( "materialKind", JsonValue::MakeString( wr.materialKind ) );
					result.set( "wrappedInCoat", JsonValue::MakeBool( wr.wrappedInCoat ) );
					if( !wr.coatedMaterial.empty() )       result.set( "coatedMaterial",       JsonValue::MakeString( wr.coatedMaterial ) );
					if( !wr.coatWeightPainter.empty() )    result.set( "coatWeightPainter",    JsonValue::MakeString( wr.coatWeightPainter ) );
					if( !wr.coatRoughnessPainter.empty() ) result.set( "coatRoughnessPainter", JsonValue::MakeString( wr.coatRoughnessPainter ) );
					if( wr.rebindObjectCount > 0 )          result.set( "rebindObjectCount",   JsonValue::MakeNumber( static_cast<double>( wr.rebindObjectCount ) ) );
					if( !wr.reflectanceSlot.empty() )    result.set( "reflectanceSlot",    JsonValue::MakeString( wr.reflectanceSlot ) );
					if( !wr.reflectancePainter.empty() ) result.set( "reflectancePainter", JsonValue::MakeString( wr.reflectancePainter ) );
					if( !wr.scatteringSlots.empty() ) {
						JsonValue arr = JsonValue::MakeArray();
						for( const std::string& nm : wr.scatteringSlots ) arr.push_back( JsonValue::MakeString( nm ) );
						result.set( "scatteringSlots", arr );
					}
					if( !wr.scatteringPainters.empty() ) {
						JsonValue arr = JsonValue::MakeArray();
						for( const std::string& nm : wr.scatteringPainters ) arr.push_back( JsonValue::MakeString( nm ) );
						result.set( "scatteringPainters", arr );
					}
					if( !wr.geometryKind.empty() ) result.set( "geometry", JsonValue::MakeString( wr.geometryKind ) );
					if( !wr.material.empty() ) {
						result.set( "geometryUniform", JsonValue::MakeBool( wr.geometryUniform ) );
						result.set( "isMetallic",      JsonValue::MakeBool( wr.isMetallic ) );
						result.set( "isOrenNayar",     JsonValue::MakeBool( wr.isOrenNayar ) );
						JsonValue rgb = JsonValue::MakeArray();
						rgb.push_back( JsonValue::MakeNumber( wr.baseR ) );
						rgb.push_back( JsonValue::MakeNumber( wr.baseG ) );
						rgb.push_back( JsonValue::MakeNumber( wr.baseB ) );
						result.set( "baseColor", rgb );
					}
					result.set( "qualifying", JsonValue::MakeNumber( static_cast<double>( wr.qualifyingMaterials ) ) );
					result.set( "objects",    JsonValue::MakeNumber( static_cast<double>( wr.boundObjects ) ) );
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// make_fabric {material?, fabric?, baseHeadVersion?}
				//   -> {ok,applied,rawCode,status,retriable,headVersion,message,
				//       material,materialKind,fabricPreset,fabricMaterial,
				//       baseMaterial,mintedSubstrate,mintedSubstrateKind,
				//       substrateWasReused,originalNowUnreferenced,weavePainter,
				//       rotationPainter,rebindObjectCount,geometry,geometryUniform,
				//       qualifying,objects}
				//   docs/CLOTH_FABRIC_DESIGN.md 9.7 (2026-09-02): convert ONE
				//   material into a `fabric_material` over a substrate of the
				//   preset's class, MINTING that substrate when the bound base
				//   is not already one (9.3: a preset cannot configure a
				//   substrate it merely references).  `fabric` is the first
				//   ENUM-typed argument on any of these verbs; an unknown
				//   spelling is a refusal, not a silent fall back to `custom`.
				//   A pre-commit refusal comes back as ok=false with the reason
				//   in `message` -- a SUCCESSFUL response, not a JSON-RPC
				//   error, the same shape add_wetness/add_wear use.
				//--------------------------------------------------------------
				if( m == "make_fabric" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					std::string materialStr, fabricStr;
					if( const JsonValue* mv = params.find( "material" ) ) {
						if( mv->isString() ) materialStr = mv->asString();
						else if( !mv->isNull() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'material' must be a string" );
					}
					if( const JsonValue* fv = params.find( "fabric" ) ) {
						if( fv->isString() ) fabricStr = fv->asString();
						else if( !fv->isNull() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'fabric' must be a string" );
					}
					RISE::Cst::CstHeadVersion base;
					std::string bErr;
					const int b = ParseBaseHeadVersionParam( params, base, bErr );
					if( b < 0 ) return MakeError( idValue, kInvalidParams, bErr );

					const AgentSession::AgentMakeFabricResult fr =
						s->MakeFabric( materialStr, fabricStr, ( b == 1 ) ? &base : nullptr );

					JsonValue result = JsonValue::MakeObject();
					result.set( "ok",          JsonValue::MakeBool( fr.ok ) );
					result.set( "applied",     JsonValue::MakeBool( fr.applied ) );
					result.set( "rawCode",     JsonValue::MakeNumber( static_cast<double>( fr.rawCode ) ) );
					result.set( "status",      JsonValue::MakeString( fr.status ) );
					result.set( "retriable",   JsonValue::MakeBool( fr.retriable ) );
					result.set( "headVersion", HeadVersionJson( fr.headVersion ) );
					if( !fr.message.empty() )             result.set( "message",             JsonValue::MakeString( fr.message ) );
					if( !fr.material.empty() )            result.set( "material",            JsonValue::MakeString( fr.material ) );
					if( !fr.materialKind.empty() )        result.set( "materialKind",        JsonValue::MakeString( fr.materialKind ) );
					if( !fr.fabricPreset.empty() )        result.set( "fabricPreset",        JsonValue::MakeString( fr.fabricPreset ) );
					if( !fr.fabricMaterial.empty() )      result.set( "fabricMaterial",      JsonValue::MakeString( fr.fabricMaterial ) );
					if( !fr.baseMaterial.empty() )        result.set( "baseMaterial",        JsonValue::MakeString( fr.baseMaterial ) );
					if( !fr.mintedSubstrate.empty() )     result.set( "mintedSubstrate",     JsonValue::MakeString( fr.mintedSubstrate ) );
					if( !fr.mintedSubstrateKind.empty() ) result.set( "mintedSubstrateKind", JsonValue::MakeString( fr.mintedSubstrateKind ) );
					if( !fr.weavePainter.empty() )        result.set( "weavePainter",        JsonValue::MakeString( fr.weavePainter ) );
					if( !fr.rotationPainter.empty() )     result.set( "rotationPainter",     JsonValue::MakeString( fr.rotationPainter ) );
					if( fr.rebindObjectCount > 0 )        result.set( "rebindObjectCount",   JsonValue::MakeNumber( static_cast<double>( fr.rebindObjectCount ) ) );
					if( !fr.geometryKind.empty() )        result.set( "geometry",            JsonValue::MakeString( fr.geometryKind ) );
					if( !fr.material.empty() ) {
						// The three substrate-decision bools and the geometry
						// read are facts about the DOCUMENT, so they ship
						// whenever a material was actually chosen -- including
						// on a refusal that got far enough to read them.
						result.set( "substrateWasReused",      JsonValue::MakeBool( fr.substrateWasReused ) );
						result.set( "originalNowUnreferenced", JsonValue::MakeBool( fr.originalNowUnreferenced ) );
						result.set( "geometryUniform",         JsonValue::MakeBool( fr.geometryUniform ) );
					}
					result.set( "qualifying", JsonValue::MakeNumber( static_cast<double>( fr.qualifyingMaterials ) ) );
					result.set( "objects",    JsonValue::MakeNumber( static_cast<double>( fr.boundObjects ) ) );
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// add_fuzz {material?, amount?, baseHeadVersion?}
				//   -> {ok,applied,rawCode,status,retriable,headVersion,message,
				//       material,materialKind,amount,fuzzGeometry,fuzzMaterial,
				//       fuzzObject,strandCount,mintedObjectCount,boundObjects,
				//       qualifying}
				//   docs/CLOTH_FABRIC_DESIGN.md Phase 3 (2026-09-03): grow a
				//   sparse hair_geometry/hair_material fuzz shell over every
				//   object bound to a fabric-like material.  NEVER edits or
				//   rebinds the target material or its bound objects -- it
				//   only ADDS new sibling chunks.  A pre-commit refusal comes
				//   back as ok=false with the reason in `message` -- a
				//   SUCCESSFUL response, not a JSON-RPC error, the same shape
				//   make_fabric/add_wetness/add_wear use.
				//--------------------------------------------------------------
				if( m == "add_fuzz" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					std::string materialStr, amountStr;
					if( const JsonValue* mv = params.find( "material" ) ) {
						if( mv->isString() ) materialStr = mv->asString();
						else if( !mv->isNull() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'material' must be a string" );
					}
					if( const JsonValue* av = params.find( "amount" ) ) {
						if( av->isString() ) amountStr = av->asString();
						else if( !av->isNull() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'amount' must be a string" );
					}
					RISE::Cst::CstHeadVersion base;
					std::string bErr;
					const int b = ParseBaseHeadVersionParam( params, base, bErr );
					if( b < 0 ) return MakeError( idValue, kInvalidParams, bErr );

					const AgentSession::AgentAddFuzzResult zr =
						s->AddFuzz( materialStr, amountStr, ( b == 1 ) ? &base : nullptr );

					JsonValue result = JsonValue::MakeObject();
					result.set( "ok",          JsonValue::MakeBool( zr.ok ) );
					result.set( "applied",     JsonValue::MakeBool( zr.applied ) );
					result.set( "rawCode",     JsonValue::MakeNumber( static_cast<double>( zr.rawCode ) ) );
					result.set( "status",      JsonValue::MakeString( zr.status ) );
					result.set( "retriable",   JsonValue::MakeBool( zr.retriable ) );
					result.set( "headVersion", HeadVersionJson( zr.headVersion ) );
					if( !zr.message.empty() )      result.set( "message",      JsonValue::MakeString( zr.message ) );
					if( !zr.material.empty() )     result.set( "material",     JsonValue::MakeString( zr.material ) );
					if( !zr.materialKind.empty() ) result.set( "materialKind", JsonValue::MakeString( zr.materialKind ) );
					if( !zr.amount.empty() )       result.set( "amount",       JsonValue::MakeString( zr.amount ) );
					if( !zr.fuzzGeometry.empty() ) result.set( "fuzzGeometry", JsonValue::MakeString( zr.fuzzGeometry ) );
					if( !zr.fuzzMaterial.empty() ) result.set( "fuzzMaterial", JsonValue::MakeString( zr.fuzzMaterial ) );
					if( !zr.fuzzObject.empty() )   result.set( "fuzzObject",   JsonValue::MakeString( zr.fuzzObject ) );
					if( zr.strandCount > 0 )       result.set( "strandCount",       JsonValue::MakeNumber( static_cast<double>( zr.strandCount ) ) );
					if( zr.mintedObjectCount > 0 ) result.set( "mintedObjectCount", JsonValue::MakeNumber( static_cast<double>( zr.mintedObjectCount ) ) );
					result.set( "boundObjects", JsonValue::MakeNumber( static_cast<double>( zr.boundObjects ) ) );
					result.set( "qualifying",   JsonValue::MakeNumber( static_cast<double>( zr.qualifyingMaterials ) ) );
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// remove_chunk {target, kind?, baseHeadVersion?}
				//   -> {applied,rawCode,status,retriable,headVersion,message,name,kind,issues?}
				//   Model-B F5 slice S2: REMOVE the chunk resolved by bare name
				//   (+ optional kind narrowing, same resolution as propose_patch)
				//   via the trivia-preserving erase; a still-referenced target is
				//   rejected with the dry-run diagnostic, head byte-identical. A
				//   REJECTED remove may carry `issues` (same shape as
				//   insert_chunk/propose_patch): reason "still_referenced" NAMES
				//   the blocking referrer chunk(s) in `suggestions` when the
				//   reference graph can identify them; omitted when it cannot
				//   (see AgentChunkIssue's doc -- an empty/absent `issues` on a
				//   rejection is never proof the target was unreferenced).
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
				// remove_chunks {targets:[string,...], baseHeadVersion?}
				//   -> {applied,rawCode,status,retriable,headVersion,message,
				//       removed:number,total:number,note?,results:[ChunkResultJson,...]}
				//   R1a (2026-08-09): the ATOMIC BATCH form of remove_chunk (see
				//   AgentSession::RemoveChunks' doc) -- delete N chunks in ONE
				//   call, ONE head-version bump, ONE undo step, instead of N
				//   round-trips.  DELIBERATELY UNLIKE insert_chunks, which is
				//   sequential/best-effort: this is ALL-OR-NOTHING.  Every
				//   target resolves and the remainder derives, or NOTHING is
				//   removed and `headVersion` is the head the call started from
				//   (a half-torn-down subassembly is a state the model never
				//   reasoned about; a half-built one is at least coherent).
				//   Intra-batch references need no ordering: a chunk referenced
				//   ONLY by other chunks in the same batch is removable in any
				//   listed order, and `still_referenced` fires only for
				//   referrers OUTSIDE the batch (which it names).  Duplicate
				//   names are deduped, not refused -- reported in `note`.
				//   NO per-target `kind`: bare names only, so the sole-unnamed-
				//   camera case (`remove_chunk kind:"camera"`) stays exclusive
				//   to the singular verb, and an ambiguous name refuses the
				//   batch naming itself.
				//   `results` is one ChunkResultJson per UNIQUE target in
				//   first-occurrence order (NOT one per input element -- match
				//   by `name`); each carries the batch verdict plus, on a
				//   refusal, the `issues` that localize the cause
				//   ("unknown_target" / "still_referenced").  `removed` is the
				//   number of chunks actually removed (0 on any refusal, else
				//   `total`); `total` is the deduped target count.  `note` is a
				//   CONDITIONAL key, omitted when there is nothing to report.
				//   REQUIRES a head: no session -> error.
				//--------------------------------------------------------------
				if( m == "remove_chunks" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					const JsonValue* tv = params.find( "targets" );
					if( !tv || !tv->isArray() ) {
						std::string msg = "Invalid params: 'targets' (array of strings) is required";
						// See DescribeOtherParamKeys's doc (mirrors insert_chunks'
						// identical guidance): name whatever the caller sent
						// INSTEAD -- notably a singular 'target', the most likely
						// slip for someone reaching for this verb -- so a
						// wrong-key mistake is visible in the SAME round-trip.
						const std::string other = DescribeOtherParamKeys(
							params, { "targets", "baseHeadVersion" } );
						if( !other.empty() )
							msg += " (got " + other + " instead -- rename to 'targets'; "
							       "remove_chunks takes an ARRAY, remove_chunk takes a single 'target')";
						return MakeError( idValue, kInvalidParams, msg );
					}
					if( tv->size() == 0 ) {
						return MakeError( idValue, kInvalidParams,
							"Invalid params: 'targets' must be a non-empty array of chunk names" );
					}
					std::vector<std::string> targets;
					targets.reserve( tv->size() );
					for( std::size_t i = 0; i < tv->size(); ++i ) {
						const JsonValue& item = tv->at( i );
						if( !item.isString() ) {
							char buf[128];
							std::snprintf( buf, sizeof( buf ),
								"Invalid params: 'targets[%zu]' must be a string", i );
							return MakeError( idValue, kInvalidParams, buf );
						}
						targets.push_back( item.asString() );
					}
					RISE::Cst::CstHeadVersion base;
					std::string bErr;
					const int b = ParseBaseHeadVersionParam( params, base, bErr );
					if( b < 0 ) return MakeError( idValue, kInvalidParams, bErr );
					const AgentSession::AgentRemoveBatchResult br =
						s->RemoveChunks( targets, ( b == 1 ) ? &base : nullptr );
					// Secure-MCP slice 6: see propose_patch's identical queue-full
					// check.  ONE batch stages ONE proposal, so there is one flag
					// to check, not a per-element scan like insert_chunks does.
					if( br.queueFull ) return MakeProposalQueueFullError( idValue, "remove_chunks" );
					JsonValue resultsArr = JsonValue::MakeArray();
					for( const AgentChunkResult& tr : br.targetResults )
						resultsArr.push_back( ChunkResultJson( tr ) );
					JsonValue result = JsonValue::MakeObject();
					result.set( "applied",     JsonValue::MakeBool( br.applied ) );
					result.set( "rawCode",     JsonValue::MakeNumber( static_cast<double>( br.rawCode ) ) );
					result.set( "status",      JsonValue::MakeString( br.status ) );
					result.set( "retriable",   JsonValue::MakeBool( br.retriable ) );
					result.set( "headVersion", HeadVersionJson( br.headVersion ) );
					result.set( "message",     JsonValue::MakeString( br.message ) );
					// ALL-OR-NOTHING, restated numerically so a caller never has to
					// infer it from the per-target array.
					result.set( "removed", JsonValue::MakeNumber(
						br.applied ? static_cast<double>( br.targetResults.size() ) : 0.0 ) );
					result.set( "total",   JsonValue::MakeNumber( static_cast<double>( br.targetResults.size() ) ) );
					// CONDITIONAL key (same back-compat posture as `issues`): a
					// batch with nothing to remark on carries no `note` at all.
					if( !br.note.empty() ) result.set( "note", JsonValue::MakeString( br.note ) );
					result.set( "results", resultsArr );
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// render {samples?,width?,height?,camera?,imageMaxEdge?} ->
				//   {ok,width,height,meanR,meanG,meanB,integrator,
				//    previewWidth,previewHeight,cameraOverridden,message,
				//    renderJobId}
				//   (Image bytes only when `imageMaxEdge` is given: then the
				//    result also carries png_base64/byteLength/imageWidth/
				//    imageHeight, downscaled to that long-edge bound by the
				//    SAME AgentSession::ReadImage(maxEdge,...) call read_image
				//    makes -- so the bytes equal what a following
				//    read_image{maxEdge:N} would have returned, in one round
				//    trip instead of two.  Refused with mode:"objectmap" (must
				//    be read at native size) and with async (no pixels exist at
				//    submit time).  Omitted -> the result is unchanged.
				//    `integrator` = the ACTIVE
				//    rasterizer's chunk keyword, empty when none is active.
				//    width/height/camera are the OPTIONAL preview-render
				//    overrides -- `camera` absent = today's exact behaviour;
				//    width/height absent = the R1b agent-surface resolution
				//    default below, NOT "the scene's authored dims" (see the
				//    R1b block further down this comment).
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
				//
				//   R1b slice (2026-08-09, docs/agentic-redesign/
				//   75-expressive-surface-arc.md): the agent RPC surface is
				//   now STRUCTURALLY unable to trigger an expensive
				//   production render.  Motivating trajectory: a live GUI
				//   session called `render {imageMaxEdge:512}` with no
				//   width/height -- `imageMaxEdge` only downscales the
				//   RETURNED png, it never bounds render cost -- and the
				//   call ran at the scene's FULL authored Film resolution
				//   and FULL authored sample count, a very long render for
				//   no reason the model could see coming.  Two caps, BOTH
				//   production-beauty-only (draft/objectmap/view-mode/
				//   BeautyVariant all keep their existing FIXED configs,
				//   completely untouched -- see AgentSession::
				//   kAgentSurfaceMaxRenderEdge / ::kAgentSurfaceMaxSamples's
				//   doc and AgentRenderParams::fromAgentSurface's doc for
				//   the full mechanism):
				//     * RESOLUTION: explicit width/height are now clamped to
				//       [16,kAgentSurfaceMaxRenderEdge] (was [16,512]); an
				//       ABSENT pair no longer falls through to the scene's
				//       full authored Film size -- it renders at the
				//       scene's own aspect ratio, scaled so the long edge is
				//       kAgentSurfaceMaxRenderEdge, resolved by AgentSession::
				//       RenderCore_'s applyFilmOverride() (aspect-ratio
				//       math needs the live Film, only safely readable
				//       under the coordinator's park -- see that function's
				//       own doc).
				//     * SAMPLES: explicit `samples` is now clamped to
				//       [1,kAgentSurfaceMaxSamples] (was [1,65536]); an
				//       ABSENT override on a rasterizer whose scene-authored
				//       sample count exceeds the cap is force-capped inside
				//       RenderCore_'s doRenderWork, reusing the EXISTING
				//       samplesOverridden/effectiveSamples honesty fields
				//       (Model-B F2 slice S3) -- a caller reads the SAME two
				//       fields regardless of whether an override was
				//       REQUESTED or silently FORCED.  On a rasterizer that
				//       does not support IRasterizer::SetSampleCountOverride
				//       (MLT, photon-map-only, AutoRasterizer's outer
				//       wrapper) the cap attempt is reported as honestly NOT
				//       applied in `message`, never silently ignored, and
				//       the render still runs (never refused).
				//   Both caps report an ADDITIVE `agentRenderCap` result
				//   key -- see BuildAgentRenderCapJson's doc above -- ONLY
				//   when something actually got reduced below what an
				//   uncapped call would have produced; omitted entirely
				//   otherwise (the "when nothing was clamped, add nothing"
				//   convention this file uses throughout).  `render_wait`'s
				//   post-completion echo (RenderResultJson, shared with the
				//   sync path here) carries the SAME fact for an async or
				//   pinned submission, so the async/pinned/sync paths are
				//   capped and REPORTED identically.
				//   Gated on AgentRenderParams::fromAgentSurface, set true
				//   ONLY by this handler (and nowhere else) -- CheckRenderKind's
				//   eval-scoring grading renders (AgentEvalRunner.cpp) build
				//   AgentRenderParams directly in C++ and never route
				//   through this dispatcher, so they see FULL requested
				//   fidelity, completely unaffected by either cap.
				//--------------------------------------------------------------
				if( m == "render" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					int samples = -1;
					// R1b (2026-08-09): honest fact-reporting bookkeeping -- see
					// AgentRenderCapFacts' doc below.  `explicitSamplesClamped`
					// is true iff the raw request exceeded the cap BEFORE the
					// clamp below reduced it; `rawSamplesRequested` keeps that
					// raw value for the result's `agentRenderCap.requestedSamples`.
					bool explicitSamplesClamped = false;
					double rawSamplesRequested = 0.0;
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
							// [1,kAgentSurfaceMaxSamples] rather than rejected (a
							// caller's out-of-range guess still renders, just at
							// the clamped count -- matches the width/height
							// ParseClampedUInt convention just below).
							//
							// R1b (2026-08-09): TIGHTENED from a generous
							// [1,65536] to [1,kAgentSurfaceMaxSamples] -- the
							// agent RPC surface must be STRUCTURALLY unable to
							// trigger an expensive production render (a live
							// GUI trajectory once requested a full-authored-spp
							// render this way); see AgentSession::
							// kAgentSurfaceMaxSamples's doc.  A caller that
							// genuinely needs a higher-fidelity measurement
							// renders from the GUI directly -- that path is
							// untouched (this clamp lives ONLY on the agent RPC
							// surface, never in AgentSession itself).
							if( samples != -1 ) {
								if( samples < 1 ) samples = 1;
								else if( samples > kAgentSurfaceMaxSamples ) {
									explicitSamplesClamped = true;
									rawSamplesRequested = sv;
									samples = kAgentSurfaceMaxSamples;
								}
							}
						}
						else if( !sm->isNull() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'samples' must be a number" );
					}

					// Preview-render dims: width/height are CLAMPED to
					// [16,kAgentSurfaceMaxRenderEdge] (never rejected -- an
					// out-of-range guess still renders, just at the clamped
					// size) and must be supplied TOGETHER (one without the
					// other is ambiguous: keep today's exact behaviour -- no
					// override -- rather than guess an aspect ratio).
					//
					// R1b (2026-08-09): TIGHTENED from [16,512] -- same
					// rationale as the `samples` cap just above; see
					// kAgentSurfaceMaxRenderEdge's doc.  ABSENT
					// width/height on a production beauty render used to fall
					// through to the scene's full authored Film resolution --
					// closed below (`rparams.fromAgentSurface = true`, which
					// gates RenderCore_'s implicit aspect-preserving default;
					// see AgentRenderParams::fromAgentSurface's doc).
					AgentRenderParams rparams;
					rparams.samples = samples;
					rparams.fromAgentSurface = true;
					// Agent transports roll perception out by default; direct C++
					// callers retain AgentRenderParams' opt-in false default.
					rparams.perception = true;
					if( const JsonValue* pv = params.find( "perception" ) ) {
						if( pv->isBool() ) rparams.perception = pv->asBool();
						else if( !pv->isNull() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'perception' must be a boolean" );
					}
					unsigned int width = 0, height = 0;
					std::string dimErr;
					const int wPresent = ParseClampedUInt( params, "width",  16,
						kAgentSurfaceMaxRenderEdge, width,  dimErr );
					if( wPresent < 0 ) return MakeError( idValue, kInvalidParams, dimErr );
					const int hPresent = ParseClampedUInt( params, "height", 16,
						kAgentSurfaceMaxRenderEdge, height, dimErr );
					if( hPresent < 0 ) return MakeError( idValue, kInvalidParams, dimErr );
					// R1b: raw pre-clamp values, for the honest
					// `agentRenderCap.requestedWidth/Height` fact below --
					// ParseClampedUInt already validated these are finite and
					// in double-range, so re-reading them here is safe.
					bool explicitWidthClamped = false, explicitHeightClamped = false;
					unsigned int rawWidthRequested = 0, rawHeightRequested = 0;
					if( wPresent == 1 && hPresent == 1 ) {
						rparams.width  = width;
						rparams.height = height;
						const double rawW = params.find( "width"  )->asNumber();
						const double rawH = params.find( "height" )->asNumber();
						if( rawW > static_cast<double>( kAgentSurfaceMaxRenderEdge ) ) {
							explicitWidthClamped = true;
							rawWidthRequested = static_cast<unsigned int>( rawW );
						}
						if( rawH > static_cast<double>( kAgentSurfaceMaxRenderEdge ) ) {
							explicitHeightClamped = true;
							rawHeightRequested = static_cast<unsigned int>( rawH );
						}
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

					// G1 (2026-08-10) `render{isolate:}` ADDITIVE param:
					// {"isolate":"<standard_object name>"} ->
					// AgentRenderParams::isolate.  Valid with EVERY mode and
					// with quality:"draft" -- see AgentRenderParams::isolate's
					// doc.  Any non-string, non-null value is a clean -32602;
					// an unresolvable/ambiguous name is NOT rejected here
					// (that needs a live Job to check against) --
					// AgentSession::RenderCore_ fails the render itself with
					// the available-name list, same contract as `view`/`light`.
					if( const JsonValue* iv = params.find( "isolate" ) ) {
						if( iv->isString() ) {
							rparams.isolate = iv->asString();
						}
						else if( !iv->isNull() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'isolate' must be a string" );
					}

					// G3b (2026-08-10) `render{target:}` ADDITIVE param:
					// {"target":"<part name from the filed build plan>"} ->
					// AgentRenderParams::target.  Two DIFFERENT rejection
					// classes, deliberately:
					//   * a non-string value, or `target` WITHOUT `isolate`,
					//     is a param-SHAPE defect -- decidable with no live
					//     scene and no filed plan -- so it is a clean -32602
					//     here, and the missing-pairing sentence comes from
					//     AgentSession::TargetRequiresIsolateMessage so the
					//     wire and the session state the requirement
					//     identically (the G3a shared-validator discipline).
					//   * a name that is not in the filed plan, or no plan
					//     filed at all, needs LIVE session state, so it is NOT
					//     rejected here: AgentSession::RenderCore_ fails the
					//     render itself (ok:false) with the filed part names
					//     in `message` -- the same contract an unresolvable
					//     `view`/`light`/`isolate` already has.
					if( const JsonValue* tv = params.find( "target" ) ) {
						if( tv->isString() ) {
							rparams.target = tv->asString();
						}
						else if( !tv->isNull() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'target' must be a string" );
					}
					if( !rparams.target.empty() && rparams.isolate.empty() ) {
						return MakeError( idValue, kInvalidParams,
							"Invalid params: " + AgentSession::TargetRequiresIsolateMessage( rparams.target ) );
					}

					// GUI render modes P1 (docs/gui/RENDER_MODES.md "X-ray axis")
					// ADDITIVE param: {"xray":false} -> AgentRenderParams::xray.
					// DEFAULT FALSE -- absent means the transmissive surface is
					// shown normally, matching the viewport's own default; pass
					// {"xray":true} to see opaque geometry inside/behind it.
					// Meaningful ONLY under a view-mode `mode`
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

					// ADDITIVE param {"imageMaxEdge":N}: return the rendered PNG
					// INLINE, so an ordinary look costs ONE call instead of
					// render + read_image.  PRESENCE is the opt-in -- absent
					// leaves the result exactly what it was.  N is a long-edge
					// bound clamped [16,1024], read_image's own maxEdge contract,
					// because the bytes come from the SAME
					// AgentSession::ReadImage(maxEdge,...) call read_image makes.
					unsigned int imageMaxEdge = 0;
					std::string imeErr;
					const int imePresent = ParseClampedUInt( params, "imageMaxEdge", 16, 1024, imageMaxEdge, imeErr );
					if( imePresent < 0 ) return MakeError( idValue, kInvalidParams, imeErr );
					if( imePresent == 1 ) {
						// An objectmap must be read at NATIVE size: the box
						// downscale blends the flat identity colours and breaks
						// the exact-byte legend match.  Refused here -- before the
						// render runs, so nothing is wasted -- rather than
						// silently downscaled (a corrupt map) or silently sent at
						// native size (an unasked-for token bill).
						if( rparams.renderTarget == AgentRenderTarget::ObjectMap ) {
							return MakeError( idValue, kInvalidParams,
								"Invalid params: 'imageMaxEdge' is not supported with mode:\"objectmap\" "
								"-- a downscale blends the identity colours and breaks the legend match. "
								"Render without it, then read_image with NO maxEdge." );
						}
						// An async submit returns before any pixels exist.
						if( wantAsync ) {
							return MakeError( idValue, kInvalidParams,
								"Invalid params: 'imageMaxEdge' is not supported with 'async' -- the "
								"submit returns before the render produces pixels. Use render_wait, "
								"then read_image." );
						}
					}

					// Arc 77 Phase 2b (2026-08-11): relay the bound into the
					// session so the scene-target composite's RENDER TILE is
					// sized by the SAME number the plain frame below is -- see
					// AgentRenderParams::imageMaxEdge.  0 when absent, which is
					// also what the async path always sees (imageMaxEdge is
					// refused with `async` above).
					rparams.imageMaxEdge = ( imePresent == 1 ) ? imageMaxEdge : 0u;

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
					JsonValue renderResult = RenderResultJson( rr );
					// R1b (2026-08-09): the honest `agentRenderCap` fact --
					// see BuildAgentRenderCapJson's doc.  Combines what THIS
					// handler alone knows (the caller's raw pre-clamp width/
					// height/samples, captured above) with what AgentSession
					// resolved inside `rr` (the two absent-value implicit
					// defaults).  Only the SYNC path can report the explicit-
					// clamp half -- the raw pre-clamp request never survives
					// into an async submission's later render_wait echo (see
					// that handler's own call to this same helper, with an
					// all-false `facts`).  Omitted entirely when nothing was
					// clamped.
					{
						AgentRenderCapFacts facts;
						facts.explicitWidthClamped   = explicitWidthClamped;
						facts.rawWidthRequested      = rawWidthRequested;
						facts.explicitHeightClamped  = explicitHeightClamped;
						facts.rawHeightRequested     = rawHeightRequested;
						facts.explicitSamplesClamped = explicitSamplesClamped;
						facts.rawSamplesRequested    = rawSamplesRequested;
						JsonValue cap;
						if( BuildAgentRenderCapJson( rr, facts, cap ) ) renderResult.set( "agentRenderCap", cap );
					}
					// The inline image rides under the SAME "png_base64" field
					// name read_image and compare_to_reference use, so
					// AgentChatCodecs' IsImageResult -- and every retention /
					// elision policy built on it -- covers this without a second
					// code path.  Attached only on a SUCCESSFUL render: on a
					// failure the cache still holds the PREVIOUS render, and
					// returning those pixels as this call's image would be a lie.
					// Arc 77 Phase 2 (2026-08-11): `sceneTargetImageRides`
					// joins `!rr.targetApplied` on this branch for the
					// IDENTICAL reason -- the scene-target composite below
					// REPLACES the frame, and exactly one `png_base64` may be
					// written (JsonValue::set APPENDS, so a second write would
					// serialize a duplicate key, not overwrite).  The two
					// comparison blocks are themselves mutually exclusive (see
					// ApplySceneTargetComparison_'s qualification rule), so at
					// most one of the two branches below can fire.
					//
					// Phase 2b (2026-08-11): the test is now the composite's
					// PRESENCE, not `sceneTargetApplied`, because the two can
					// now differ -- a qualifying render whose composite failed
					// to encode still reports the scene-target facts, and it
					// must fall back to the plain frame rather than returning
					// no image at all.  (The other way they differ, a render
					// that asked for no image, cannot reach this branch:
					// imePresent is 0 there.)
					const bool sceneTargetImageRides =
						rr.ok && rr.sceneTargetApplied && !rr.sceneTargetCompositePng.empty();
					// Doc 90 slice R1 (2026-08-22): the ANCHOR composite joins
					// the exactly-one-`png_base64` discipline as the OUTERMOST
					// of the three, because it CONTAINS whichever of the other
					// two would otherwise have ridden: when a scene-target
					// composite was built, AgentSession::
					// ApplyRenderAnchorComparison_ used it as the anchor
					// composite's lower pane, and otherwise the lower pane is
					// the very frame the plain branch would have returned, at
					// the very dimensions that branch would have used.  So
					// this is a SUPERSET, never a substitution -- nothing the
					// model would have seen is lost by this branch winning.
					// (The part-`target` strip is disjoint by construction: it
					// requires `isolate`, which the anchor's qualification rule
					// refuses.)
					const bool anchorImageRides =
						rr.ok && rr.anchorApplied && !rr.anchorCompositePng.empty();
					if( imePresent == 1 && rr.ok && !rr.targetApplied &&
						!sceneTargetImageRides && !anchorImageRides ) {
						unsigned int imgW = 0, imgH = 0;
						const std::vector<unsigned char> png = s->ReadImage( imageMaxEdge, imgW, imgH );
						if( !png.empty() ) {
							renderResult.set( "png_base64", JsonValue::MakeString( Base64Encode( png ) ) );
							renderResult.set( "byteLength", JsonValue::MakeNumber( static_cast<double>( png.size() ) ) );
							// NOT "width"/"height" -- those are the RENDER's dims.
							renderResult.set( "imageWidth",  JsonValue::MakeNumber( static_cast<double>( imgW ) ) );
							renderResult.set( "imageHeight", JsonValue::MakeNumber( static_cast<double>( imgH ) ) );
						}
					}
					// G3b (2026-08-10): on a `target` comparison the inline
					// image is the [sketch | silhouette | overlay] COMPOSITE,
					// and it rides UNCONDITIONALLY -- `imageMaxEdge` is not
					// required, and when it IS supplied the composite REPLACES
					// the rendered frame (hence the `!rr.targetApplied` term on
					// the branch above: exactly one image, never two
					// `png_base64` writes).
					//
					// WHY the composite wins.  The composite IS the call: the
					// transport keeps only the most recent tool-result image
					// live (one global slot), so this strip is the ONE moment
					// the filed sketch re-enters the model's context, which is
					// the mechanism the design doc's sec 4.3 "Retention
					// interaction" decision rests on -- if the beauty frame
					// won, the sketch would never be seen again and the whole
					// comparison would degrade to numbers.  A caller that wants
					// the rendered frame of the same isolate render can re-issue
					// without `target`, or call read_image (the comparison's own
					// internal identity pass is cache-guarded, so read_image
					// still serves THIS render's beauty pixels).  Both tool
					// surfaces state this outright.
					//
					// Same "png_base64" field name every image-bearing verb
					// uses, so AgentChatCodecs' IsImageResult -- which already
					// lists `render` -- covers it with no second code path.
					if( rr.ok && rr.targetApplied && !rr.targetCompositePng.empty() ) {
						renderResult.set( "png_base64",
							JsonValue::MakeString( Base64Encode( rr.targetCompositePng ) ) );
						renderResult.set( "byteLength",
							JsonValue::MakeNumber( static_cast<double>( rr.targetCompositePng.size() ) ) );
						renderResult.set( "imageWidth",
							JsonValue::MakeNumber( static_cast<double>( rr.targetCompositeWidth ) ) );
						renderResult.set( "imageHeight",
							JsonValue::MakeNumber( static_cast<double>( rr.targetCompositeHeight ) ) );
					}
					// Arc 77 Phase 2 (2026-08-11), reshaped by Phase 2b: on a
					// qualifying full-frame render of a session that has
					// imagined a scene AND asked for an inline image, that
					// image is the composite -- the imagined target ABOVE this
					// render, on a canvas exactly as wide as the frame would
					// have been.
					//
					// WHY IT IS NOW CONDITIONAL ON `imageMaxEdge`.  Phase 2
					// shipped the strip unconditionally, and it REPLACED the
					// frame with a half-width panel of it: the model saw its
					// own scene smaller than it would have with no target at
					// all -- less resolution to spot its own defects -- and it
					// saw it permanently juxtaposed with an unreachable ideal.
					// The composite now carries the render at exactly the size
					// the plain frame would have been (see
					// AgentSession::ApplySceneTargetComparison_), which means
					// it is sized by `imageMaxEdge` and therefore exists only
					// when one was asked for.  A render that wanted no picture
					// gets none, exactly as before this mechanism existed.
					// The retention argument that made the composite a
					// requirement is unchanged: the transport keeps only the
					// most recent tool-result image live, so this is the ONE
					// moment the imagined target re-enters the model's context.
					// A caller that wants the plain frame can call read_image,
					// which still serves THIS render's own beauty pixels
					// (nothing here touches the image cache).
					if( sceneTargetImageRides && !anchorImageRides ) {
						renderResult.set( "png_base64",
							JsonValue::MakeString( Base64Encode( rr.sceneTargetCompositePng ) ) );
						renderResult.set( "byteLength",
							JsonValue::MakeNumber( static_cast<double>( rr.sceneTargetCompositePng.size() ) ) );
						renderResult.set( "imageWidth",
							JsonValue::MakeNumber( static_cast<double>( rr.sceneTargetCompositeWidth ) ) );
						renderResult.set( "imageHeight",
							JsonValue::MakeNumber( static_cast<double>( rr.sceneTargetCompositeHeight ) ) );
					}
					// Doc 90 slice R1 (2026-08-22): the ANCHOR composite --
					// this render set beneath the render the model chose as
					// its reference point, both panes labelled with the head
					// revision they were rendered at.  It wins over both
					// branches above because it CONTAINS what they would have
					// shown (see anchorImageRides' comment); it never fires
					// without `imageMaxEdge`, because the lower pane is sized
					// by exactly the rule that flag drives, and it never fires
					// on the render that BECAME the anchor, because there is
					// then nothing to set it beside.
					if( anchorImageRides ) {
						renderResult.set( "png_base64",
							JsonValue::MakeString( Base64Encode( rr.anchorCompositePng ) ) );
						renderResult.set( "byteLength",
							JsonValue::MakeNumber( static_cast<double>( rr.anchorCompositePng.size() ) ) );
						renderResult.set( "imageWidth",
							JsonValue::MakeNumber( static_cast<double>( rr.anchorCompositeWidth ) ) );
						renderResult.set( "imageHeight",
							JsonValue::MakeNumber( static_cast<double>( rr.anchorCompositeHeight ) ) );
					}
					return MakeSuccess( idValue, renderResult );
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
						if( ar.found ) {
							JsonValue nested = RenderResultJson( ar.result );
							// R1b (2026-08-09): the ABSENT-value half of the
							// `agentRenderCap` fact -- see BuildAgentRenderCapJson's
							// doc.  An async submission's raw pre-clamp explicit
							// width/height/samples request (if any) was already
							// clamped and does not survive to this later echo
							// (the render's own `render{async:true}` submit
							// call is the only place that saw it) -- an
							// all-default AgentRenderCapFacts here means this
							// reports ONLY what AgentSession itself resolved
							// (`ar.result.agentResolutionCapped`/
							// `agentSamplesCapped`), which is exactly the same
							// information the SYNC path's `rr.*` half carries.
							JsonValue cap;
							if( BuildAgentRenderCapJson( ar.result, AgentRenderCapFacts(), cap ) )
								nested.set( "agentRenderCap", cap );
							result.set( "result", nested );
						}
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
				// read_image {maxEdge?,representation?} -> {png_base64:string,byteLength:number,
				//                           width:number,height:number}
				//   Reads the LAST successful render's cached PNG bytes and
				//   base64-encodes them so the binary travels in JSON.
				//   maxEdge (OPTIONAL, clamped [16,1024]) downscales the
				//   returned image to that long-edge bound (box filter,
				//   aspect-preserving, never upscales) -- re-encoded from the
				//   cached full-resolution pixels, no re-render. Omission keeps
				//   beauty native but bounds the larger perception atlas to 1024.
				//--------------------------------------------------------------
				if( m == "read_image" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					unsigned int maxEdge = 0;
					std::string meErr;
					const int mePresent = ParseClampedUInt( params, "maxEdge", 16, 1024, maxEdge, meErr );
					if( mePresent < 0 ) return MakeError( idValue, kInvalidParams, meErr );
					std::string representation = "beauty";
					if( const JsonValue* rv = params.find( "representation" ) ) {
						if( rv->isString() ) representation = rv->asString();
						else if( !rv->isNull() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'representation' must be a string" );
					}
					if( representation != "beauty" && representation != "perception" ) {
						return MakeError( idValue, kInvalidParams,
							"Invalid params: 'representation' must be \"beauty\" or \"perception\"" );
					}
					unsigned int imgW = 0, imgH = 0;
					AgentPerceptionInfo perceptionInfo;
					const unsigned int effectiveMaxEdge = mePresent == 1
						? maxEdge : ( representation == "perception" ? 1024u : 0u );
					const std::vector<unsigned char> png = representation == "perception"
						? s->ReadPerception( effectiveMaxEdge, imgW, imgH, perceptionInfo )
						: ( ( mePresent == 1 ) ? s->ReadImage( maxEdge, imgW, imgH )
						                           : s->ReadImage( 0, imgW, imgH ) );
					JsonValue result = JsonValue::MakeObject();
					result.set( "png_base64", JsonValue::MakeString( Base64Encode( png ) ) );
					result.set( "byteLength", JsonValue::MakeNumber( static_cast<double>( png.size() ) ) );
					result.set( "width",  JsonValue::MakeNumber( static_cast<double>( imgW ) ) );
					result.set( "height", JsonValue::MakeNumber( static_cast<double>( imgH ) ) );
					result.set( "representation", JsonValue::MakeString( representation ) );
					if( representation == "perception" ) {
						result.set( "available", JsonValue::MakeBool( perceptionInfo.available && !png.empty() ) );
						JsonValue panels = JsonValue::MakeArray();
						panels.push_back( JsonValue::MakeString( "beauty" ) );
						panels.push_back( JsonValue::MakeString( "albedo" ) );
						panels.push_back( JsonValue::MakeString( "world_normal" ) );
						panels.push_back( JsonValue::MakeString( "log_depth" ) );
						result.set( "panels", panels );
						result.set( "sourceWidth", JsonValue::MakeNumber( perceptionInfo.sourceWidth ) );
						result.set( "sourceHeight", JsonValue::MakeNumber( perceptionInfo.sourceHeight ) );
						result.set( "validDepthPixels", JsonValue::MakeNumber( perceptionInfo.validDepthPixels ) );
						result.set( "depthMin", JsonValue::MakeNumber( perceptionInfo.depthMin ) );
						result.set( "depthMax", JsonValue::MakeNumber( perceptionInfo.depthMax ) );
						result.set( "guidePrefilter", JsonValue::MakeString( perceptionInfo.guidePrefilter ) );
						result.set( "persistentBytes", JsonValue::MakeNumber(
							static_cast<double>( perceptionInfo.persistentBytes ) ) );
						result.set( "auxiliaryPeakBytes", JsonValue::MakeNumber(
							static_cast<double>( perceptionInfo.auxiliaryPeakBytes ) ) );
						result.set( "encoderRowBytes", JsonValue::MakeNumber(
							static_cast<double>( perceptionInfo.encoderRowBytes ) ) );
					}
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
				//   observe).  `available` is false with one of SEVEN reasons:
				//   "no_controller" (headless session -- no viewport at all;
				//   PERMANENT), "no_frame_yet" (controller attached but no
				//   interactive frame produced yet -- it resolves once the
				//   viewport draws, but NOT because the caller retried), or,
				//   when the parked frame copy is refused,
				//   "editor_transaction_in_progress" / "render_in_progress" /
				//   "editor_interaction_finalize_failed" (all three RETRIABLE) /
				//   "editor_shutting_down" / "editor_interaction_unrecoverable"
				//   (both PERMANENT -- retrying can never succeed; round-10).
				//   See AgentSession::ReadViewport's doc for the authoritative
				//   list, the retriability of each, and when a `render`
				//   fallback actually helps.  Round-14: for
				//   "render_in_progress" an OVERRIDDEN render is refused
				//   outright, and a PLAIN one is NOT simply "accepted" either
				//   -- it waits up to 30 s on the render slot, then succeeds
				//   only if the occupant (often the user's own production
				//   render, which shares that slot) finished inside the
				//   window, and it is refused with no wait at all when a
				//   DIRECT PARKED render holds the gate.  Retrying the free
				//   read_viewport is the cheap poll; see the authority block
				//   for the full three-way outcome and the recommendation.
				//   In every case png_base64 is "" and
				//   byteLength/width/height are 0.  available:false is a STRUCTURED SUCCESS result, NOT
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
							pv.set( "contentSource", JsonValue::MakeNumber(
								panesInfo.panes[i].contentSource ) );
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
				// scene_inventory {} ->
				//   {ok,objects,covered,passWidth,passHeight,
				//    framePositionComputed,framePositionNote?,
				//    entries:[{name,pixelCount,frameFraction,onScreen,
				//              frameX?,frameY?,worldCentre?,placement,
				//              offFrameDirection?}],
				//    text,message}
				//   Arc 80 (2026-08-12): "where is everything?", asked
				//   directly.  TAKES NO PARAMS -- it inventories the ACTIVE
				//   camera's view of the live scene.  Shares ONE code path
				//   (AgentSession::ComputeSceneInventory_) with the `inventory`
				//   block every full-scene beauty render already carries, so
				//   the two surfaces cannot describe the same scene
				//   differently; the PAYLOAD path is the one expected to
				//   matter (every voluntary consultation surface this
				//   workstream shipped measured 0/64 uses), and no behaviour
				//   depends on this verb being called.
				//   A scene with no objects, or a failed identity pass,
				//   returns ok:false with the reason in `message` -- never a
				//   fabricated empty inventory.
				//--------------------------------------------------------------
				if( m == "scene_inventory" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );

					const AgentSession::AgentSceneInventoryResult si = s->SceneInventory();
					JsonValue result = JsonValue::MakeObject();
					result.set( "ok",         JsonValue::MakeBool( si.ok ) );
					result.set( "objects",    JsonValue::MakeNumber( static_cast<double>( si.objectCount ) ) );
					result.set( "covered",    JsonValue::MakeNumber( static_cast<double>( si.coveredCount ) ) );
					result.set( "passWidth",  JsonValue::MakeNumber( static_cast<double>( si.passWidth ) ) );
					result.set( "passHeight", JsonValue::MakeNumber( static_cast<double>( si.passHeight ) ) );
					result.set( "framePositionComputed",
						JsonValue::MakeBool( si.framePositionComputed ) );
					// CONDITIONAL, the same omit-when-empty convention as
					// `note`/`legend`/`issues`: present only when there IS a
					// reason, i.e. when the analytic classification was
					// suppressed rather than run.
					if( !si.framePositionSuppressedReason.empty() )
						result.set( "framePositionNote",
							JsonValue::MakeString( si.framePositionSuppressedReason ) );
					JsonValue entries = JsonValue::MakeArray();
					for( std::size_t i = 0; i < si.entries.size(); ++i ) {
						const AgentSession::AgentSceneInventoryEntry& e = si.entries[i];
						JsonValue o = JsonValue::MakeObject();
						o.set( "name",          JsonValue::MakeString( e.name ) );
						o.set( "pixelCount",    JsonValue::MakeNumber( static_cast<double>( e.pixelCount ) ) );
						o.set( "frameFraction", JsonValue::MakeNumber( e.frameFraction ) );
						o.set( "onScreen",      JsonValue::MakeBool( e.onScreen ) );
						o.set( "placement",     JsonValue::MakeString( e.placement ) );
						// Every remaining key is OMITTED rather than sent as a
						// measured-looking zero when it was not measured --
						// the sentinel-then-omit convention `bboxCoverage` and
						// `bboxMin`/`bboxMax` already follow above.
						// frameX/frameY ride ONLY when a position was really
						// measured, and `framePositionSource` says WHICH
						// measurement it is: "pixels" (the object's pixel
						// bounding box in the identity pass) or "bbox" (its
						// projected world bounding box, the fallback when no
						// pixel carried its identity colour).  A fabricated
						// 0,0 would read as "top-left corner".
						if( e.framePositionKnown ) {
							o.set( "frameX", JsonValue::MakeNumber( e.frameX ) );
							o.set( "frameY", JsonValue::MakeNumber( e.frameY ) );
							o.set( "framePositionSource", JsonValue::MakeString(
								e.framePositionFromPixels ? "pixels" : "bbox" ) );
						}
						if( e.worldCentreKnown ) {
							JsonValue c = JsonValue::MakeArray();
							for( int a = 0; a < 3; ++a )
								c.push_back( JsonValue::MakeNumber( e.worldCentre[a] ) );
							o.set( "worldCentre", c );
						}
						if( !e.offFrameDirection.empty() )
							o.set( "offFrameDirection", JsonValue::MakeString( e.offFrameDirection ) );
						// 87 STEP 5 (2026-08-18): the two STRUCTURE keys, under
						// the same omit-when-absent convention as every key
						// above -- a root, authored object sends neither, so a
						// flat scene's payload is byte-identical to what it was
						// before hierarchy existed.  `parent` is the authored
						// tree (which rows are one assembly); `instancedFrom`
						// is the chunk an author can actually edit for a
						// synthesized repetition, and is the ONLY sanctioned
						// way to get it -- a client must not derive it by
						// splitting the name.
						if( !e.parent.empty() )
							o.set( "parent", JsonValue::MakeString( e.parent ) );
						if( !e.instancedFrom.empty() )
							o.set( "instancedFrom", JsonValue::MakeString( e.instancedFrom ) );
						entries.push_back( o );
					}
					result.set( "entries", entries );
					// F9(b) fix round (2026-08-14): AgentSession.h documents
					// `AgentRenderResult::inventoryNotAreaSampled` as carrying
					// "the same fact in machine form" as `inventoryText` -- that
					// claim was only true on the render payload's `inventory`
					// block (see above), not here.  `si` is the SAME
					// AgentSceneInventoryResult struct (ComputeSceneInventory_
					// populates both), so si.notAreaSampled is already there for
					// free; mirror the render path's serialization so the two
					// surfaces cannot drift.  Same omit-when-empty convention.
					if( !si.notAreaSampled.empty() ) {
						JsonValue nas = JsonValue::MakeArray();
						for( std::size_t i = 0; i < si.notAreaSampled.size(); ++i ) {
							const AgentSceneNotAreaSampledEntry& e = si.notAreaSampled[i];
							JsonValue o = JsonValue::MakeObject();
							o.set( "name",   JsonValue::MakeString( e.name ) );
							o.set( "reason", JsonValue::MakeString( e.reason ) );
							nas.push_back( o );
						}
						result.set( "notAreaSampled", nas );
					}
					result.set( "text",    JsonValue::MakeString( si.text ) );
					result.set( "message", JsonValue::MakeString( si.message ) );
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

					// R1b (2026-08-09): TIGHTENED from a generous [1,65536] to
					// [1,kAgentSurfaceMaxSamples] -- supplying `samples` here
					// switches this comparison render to
					// AgentRenderQuality::Production (see
					// AgentCompareToReferenceParams' doc), the SAME production-
					// beauty cost concern the `render` verb's own samples cap
					// addresses; see kAgentSurfaceMaxSamples's doc.
					// Does NOT affect AgentEvalRunner's own grading renders --
					// those call AgentSession::Render/CheckRenderKind directly
					// in C++, never through this RPC handler.
					bool compareSamplesClamped = false;
					double compareRawSamplesRequested = 0.0;
					if( const JsonValue* sv = params.find( "samples" ) ) {
						if( sv->isNumber() ) {
							// Explicit finite-range guard.  NOTE: this is NOT
							// equivalent to a finiteness check -- the range
							// idiom is NaN-BLIND by construction (it catches
							// only +/-Inf).  It was historically chosen because
							// std::isfinite was dead code under
							// -ffinite-math-only; that is no longer true
							// (macOS pairs -fno-finite-math-only since
							// 2026-07-29).  See the 'samples' parse in
							// the render dispatch above, which uses the
							// canonical RISE::IsFiniteDouble.
							const double sd = sv->asNumber();
							if( !( sd >= -2147483648.0 && sd <= 2147483647.0 ) )
								return MakeError( idValue, kInvalidParams, "Invalid params: 'samples' must be a finite, in-range number" );
							int samples = static_cast<int>( sd );
							if( samples < 1 ) samples = 1;
							else if( samples > kAgentSurfaceMaxSamples ) {
								compareSamplesClamped = true;
								compareRawSamplesRequested = sd;
								samples = kAgentSurfaceMaxSamples;
							}
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
					// R1b (2026-08-09): honest `agentRenderCap` fact -- see
					// BuildAgentRenderCapJson's doc.  compare_to_reference has
					// no width/height override at all (always the reference's
					// own dims -- see AgentCompareToReferenceParams' doc), so
					// only the samples half of the fact is ever meaningful
					// here; omitted entirely when the request's `samples` (if
					// any) was already within the cap.
					if( compareSamplesClamped ) {
						JsonValue cap = JsonValue::MakeObject();
						cap.set( "maxSamples", JsonValue::MakeNumber(
							static_cast<double>( kAgentSurfaceMaxSamples ) ) );
						cap.set( "samplesCapped", JsonValue::MakeBool( true ) );
						cap.set( "requestedSamples", JsonValue::MakeNumber( compareRawSamplesRequested ) );
						result.set( "agentRenderCap", cap );
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
				//   {resolved:bool, retriable:bool, status:string, headVersion,
				//    message}
				//   Secure-MCP slice 5b: approve or reject a staged proposal.
				//   OWNER-ONLY -- routes to AgentSession::ResolveProposal, which
				//   refuses (resolved=false) for a non-Owner-authority session,
				//   including one resolving its own proposal.  NOT on the
				//   read-safe allowlist (refused under Read) and deliberately
				//   NOT on Propose's extension (refused under Propose too, with
				//   a Propose-specific message -- see the autonomy choke point
				//   above); reachable only under Commit, the posture the
				//   GUI's in-process ADMINISTRATIVE dispatcher (the one
				//   behind agentHandleLine) permanently runs at.
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
					// resolved:false has two flavours and a caller must be able
					// to tell them apart: a TRANSIENT refusal (a render or an
					// open editor gesture held the gate) leaves the proposal
					// PENDING and resolving again later works, while every
					// other refusal is permanent.  Always present so a client
					// never has to treat a missing key as either.
					result.set( "retriable", JsonValue::MakeBool( rr.retriable ) );
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
