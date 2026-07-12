//////////////////////////////////////////////////////////////////////
//
//  AgentChatLoop.cpp - the provider-agnostic, sans-IO chat-loop core
//    (see AgentChatLoop.h for the contract).
//
//  NO LOGGING anywhere in this file: request/response bodies may embed
//  scene content, and the API key must never reach a log.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "AgentChatLoop.h"

#include "Json.h"

#include <cstdlib>   // getenv -- ONLY for RISE_LOCAL_LLM_BASE_URL (config, not a credential; see MakeCodec)
#include <cstring>   // strlen -- ASCII case-insensitive substring match (multimodal-400 detection)

namespace RISE
{
	namespace Agent
	{
		namespace
		{
			//! The static co-editing system prompt (rule 7): the user and
			//! the agent co-edit ONE live scene through the ten verbs.
			const char* const kSystemPrompt =
				"You are a scene-editing agent embedded in the RISE renderer. You and "
				"the user CO-EDIT one live scene: the user sees the same viewport and "
				"may edit concurrently, so never assume the document is unchanged -- "
				"always work from a fresh read_document.\n"
				"\n"
				"Workflow for every edit:\n"
				"1. read_document to see the scene and its headVersion (read_schema "
				"when unsure about a chunk or parameter; consult read_skill before "
				"scene-authoring tasks -- the skills carry the conventions that make "
				"scenes render correctly on the first try).\n"
				"2. propose_patch (or insert_chunk / remove_chunk) with the "
				"headVersion you just read as baseHeadVersion. On status=conflict, "
				"re-read and re-propose; on "
				"retriable=true, retry the same patch after a moment. "
				"status=diagnosed means the edit WAS applied but the re-derive "
				"produced diagnostics -- read them and fix the reported problem; "
				"do not blindly re-propose the same patch.\n"
				"3. render, then read_image to SEE the result and verify the edit "
				"visually before declaring it done. While modeling/placing things, "
				"use CHEAP LOOKS: render with width/height 128-192 and read_image "
				"maxEdge ~192, and use render's `camera` override to check the "
				"scene from 2-3 different angles WITHOUT editing the actual camera "
				"-- it's ephemeral (restored automatically, never touches the "
				"document) and costs far fewer tokens/seconds than a full render. "
				"For an even cheaper GEOMETRY/PLACEMENT-only check, add "
				"quality:\"draft\" to render -- it uses a fixed studio-preview "
				"shader (capped at 4 samples) that IGNORES the scene's actual "
				"materials and lighting entirely, so it is ONLY useful to confirm "
				"shape/position/camera framing; NEVER judge materials, lighting, "
				"exposure, or colour from a draft render (check the result's "
				"`renderMode` field, not `integrator`, to confirm which pipeline "
				"ran). Only do a full-size, full-sample, quality:\"production\" "
				"render (the default; no width/height/camera override) for the "
				"FINAL check once you're confident the edit is right, and for any "
				"judgement of materials/lighting/exposure/colour. When you just "
				"need to know WHICH object is at one spot (e.g. before moving or "
				"recoloring \"the mug\" or \"that sphere\"), use query_object_at "
				"{x,y} instead of a full render mode:\"objectmap\" -- it is "
				"cheaper to parse than a legend+PNG and returns the object's name "
				"directly (hit:false, not an error, when the pixel is empty); "
				"combine it with `camera` to aim at a spot first.\n"
				"\n"
				// CAPABILITY SCOPE (Model-B F5 slice S2: insert_chunk /
				// remove_chunk shipped -- entity add/remove is real now;
				// round 2: declaration positioning + the full-derivability
				// gate make the rename recipe safe, and the unnamed one-way
				// door is disclosed; round 3: the one-way door is film +
				// rasterizers ONLY -- the SOLE camera IS removable via the
				// kind="camera" positional fallback, so the camera-SWAP
				// recipe (remove FIRST, then insert) is taught, plus the
				// retarget-refused remove+reinsert escape).
				"You can change PARAMETERS of existing entities via propose_patch, "
				"ADD a new entity with insert_chunk (exactly ONE complete "
				"`keyword { ... }` chunk per call, braces on their own lines; "
				"declaration chunks -- painters, materials, geometry, shaders, "
				"media -- are positioned before the objects that consume them "
				"automatically), and DELETE an entity with remove_chunk (refused "
				"while another chunk still references the target -- retarget or "
				"remove the consumers first). Any edit that would leave the "
				"document unable to derive in order is refused cleanly with the "
				"head unchanged. Honest limits: whole-chunk granularity only -- "
				"no rename verb (the safe recipe: insert_chunk the renamed "
				"entity, retarget its consumers via propose_patch, then "
				"remove_chunk the old one), no reordering, and unnamed film and "
				"rasterizer chunks cannot be removed once inserted -- insert "
				"them deliberately. The SOLE camera (even unnamed) IS removable: "
				"remove_chunk with kind=\"camera\" resolves it by position. To "
				"SWAP cameras, REMOVE the old camera FIRST, THEN insert the "
				"replacement -- insert-first creates two cameras and strands "
				"the old unnamed one (the fallback requires exactly one; a "
				"NAMED second camera stays removable by name). If a retarget is "
				"refused because the new entity sits later in the document, "
				"remove_chunk the consumer and re-insert it (it will be "
				"appended after the entity it references). For a BIG addition, "
				"compose the full candidate document and validate it FIRST, then "
				"insert chunk by chunk.\n"
				"\n"
				"Keep responses concise. After acting, report plainly what changed "
				"(entity, parameter, old vs new value when known) and what you "
				"observed in the render.";

			std::unique_ptr<IChatProviderCodec> MakeCodec( ChatProvider provider )
			{
				if( provider == ChatProvider::Gemini )
					return std::unique_ptr<IChatProviderCodec>( new GeminiChatCodec() );
				if( provider == ChatProvider::OpenAI )
					return std::unique_ptr<IChatProviderCodec>( new OpenAIChatCodec() );
				if( provider == ChatProvider::XAI ) {
					// xAI (Grok): OpenAI-Chat-Completions-compatible endpoint,
					// same Bearer auth (env convention XAI_API_KEY).  Default
					// model id is the literal "grok-4.5"; xAI's usage block
					// carries the OpenAI fields plus extras (e.g.
					// cost_in_usd_ticks) that ParseUsage harmlessly ignores.
					OpenAIChatCodec::Config cfg;
					cfg.providerName   = "xai";
					cfg.baseUrl        = "https://api.x.ai/v1/chat/completions";
					cfg.defaultModelId = "grok-4.5";
					cfg.requiresAuth   = true;
					return std::unique_ptr<IChatProviderCodec>( new OpenAIChatCodec( cfg ) );
				}
				if( provider == ChatProvider::Local ) {
					// A local OpenAI-compatible server (Ollama et al.).  The
					// base URL is read ONCE here, at codec construction, from
					// RISE_LOCAL_LLM_BASE_URL (falling back to Ollama's default
					// loopback endpoint).  CREDENTIAL INVARIANT NOTE: the
					// Library layer deliberately reads NO api key from the
					// environment (keys arrive as parameters) -- but a BASE URL
					// is CONFIG, not a secret, so this one getenv is acceptable.
					// It is deliberately NOT in AgentEvalRunner (which stays
					// wholly env-read-free).  Changing the env var takes effect
					// only on the NEXT provider selection (MakeCodec re-runs on
					// SetProvider), not mid-conversation.  The IP LITERAL
					// 127.0.0.1 (not "localhost") is intentional: macOS ATS
					// exempts IP literals from its cleartext-HTTP block.
					// DESIGN SEAM: this getenv bypasses RunEvalMatrix's
					// envLookup injection (deliberate -- envLookup exists to
					// inject CREDENTIALS; this is config), so a matrix run
					// resolves the local base URL from the REAL process
					// environment.  If a future test needs to mock/override
					// it, thread the URL through AgentEvalRunOptions instead
					// of teaching envLookup about config reads.
					OpenAIChatCodec::Config cfg;
					const char* envUrl = std::getenv( "RISE_LOCAL_LLM_BASE_URL" );
					cfg.providerName   = "local";
					cfg.baseUrl        = ( envUrl && envUrl[0] != '\0' )
						? std::string( envUrl )
						: std::string( "http://127.0.0.1:11434/v1/chat/completions" );
					// Default model "qwen3:32b" (a newer alternative tag is
					// "qwen3.6:27b"); requiresAuth=false -> keyless local
					// servers emit NO Authorization header, while a server
					// started with --api-key still gets a Bearer header when a
					// key is supplied.
					cfg.defaultModelId = "qwen3:32b";
					cfg.requiresAuth   = false;
					return std::unique_ptr<IChatProviderCodec>( new OpenAIChatCodec( cfg ) );
				}
				return std::unique_ptr<IChatProviderCodec>( new AnthropicChatCodec() );
			}
		}

		const char* AgentChatLoop::SystemPrompt()
		{
			return kSystemPrompt;
		}

		void AgentChatLoop::SetSkillIndex( const std::string& indexText )
		{
			// Provider-neutral config (like the provider/model selection):
			// stored verbatim; BuildRequest composes the skills section onto
			// the base prompt when non-empty.  Survives Reset()/SetProvider().
			mSkillIndexText = indexText;
		}

		AgentChatLoop::AgentChatLoop() :
			mCodec( MakeCodec( ChatProvider::OpenAI ) ),
			mProvider( ChatProvider::OpenAI ),
			mModelId( mCodec->DefaultModelId() ),
			mToolRounds( 0 ),
			mSessionEmitted( false ),
			mElideAllImages( false )
		{
		}

		AgentChatLoop::~AgentChatLoop()
		{
		}

		void AgentChatLoop::Reset()
		{
			// Eval-harness E1: a direct Reset closes the trajectory session
			// with a "reset" summary (no-op when SetProvider already closed
			// it as "provider_switch", or when no session is active).
			CloseTrajectorySession( "reset" );
			mTranscript.clear();
			mPendingCalls.clear();
			mPendingResults.clear();
			mToolLineStash.clear();
			mToolRounds = 0;
			// A fresh session may target a different, image-capable model --
			// the text-only proof does not carry across a Reset/SetProvider.
			mElideAllImages = false;
		}

		void AgentChatLoop::SetProvider( ChatProvider provider, const std::string& modelId )
		{
			// Eval-harness E1: close the current trajectory session BEFORE
			// the provider swap so the summary belongs to the old provider and
			// the next session record reflects the new one.  Reset() below then
			// finds the session already closed (a no-op "reset").
			CloseTrajectorySession( "provider_switch" );
			// SWITCHING PROVIDER RESETS THE TRANSCRIPT: assistant entries
			// are provider-native raw JSON and cannot cross providers.
			mProvider = provider;
			mCodec = MakeCodec( provider );
			mModelId = modelId.empty() ? std::string( mCodec->DefaultModelId() ) : modelId;
			Reset();
		}

		namespace
		{
			bool IsBlank( const std::string& text )
			{
				for( std::size_t i = 0; i < text.size(); ++i ) {
					const char c = text[i];
					if( c != ' ' && c != '\t' && c != '\n' && c != '\r' ) return false;
				}
				return true;
			}

			//! Case-insensitive substring test (ASCII).
			bool ContainsCI( const std::string& hay, const char* needle )
			{
				const std::size_t nlen = std::strlen( needle );
				if( nlen == 0 ) return true;
				if( hay.size() < nlen ) return false;
				for( std::size_t i = 0; i + nlen <= hay.size(); ++i ) {
					std::size_t j = 0;
					for( ; j < nlen; ++j ) {
						char a = hay[i + j];
						char b = needle[j];
						if( a >= 'A' && a <= 'Z' ) a = static_cast<char>( a - 'A' + 'a' );
						if( b >= 'A' && b <= 'Z' ) b = static_cast<char>( b - 'A' + 'a' );
						if( a != b ) break;
					}
					if( j == nlen ) return true;
				}
				return false;
			}

			//! TEXT-ONLY-MODEL IMAGE-REJECTION detection.  A text-only
			//! backend (observed: Ollama's qwen3:32b via the OpenAI-
			//! compatible Local provider) answers the NEXT request after an
			//! image was packed into the conversation with HTTP 400 whose
			//! body reads e.g. "Multimodal data provided, but model does not
			//! support multimodal requests".  The match is deliberately
			//! NARROW -- status must be exactly 400 AND the raw body must
			//! contain BOTH "multimodal" and "not support" (case-
			//! insensitive) -- so an ordinary bad-request 400 (malformed
			//! JSON, unknown model, rate-limit-shaped 400, ...) does NOT
			//! trigger the image-elide retry.  Both tokens are provider-
			//! stable substrings of the observed message; requiring the
			//! pair avoids matching a 400 that merely mentions one word.
			bool IsMultimodalUnsupported400( long httpStatus, const std::string& rawBody )
			{
				if( httpStatus != 400 ) return false;
				return ContainsCI( rawBody, "multimodal" ) &&
				       ContainsCI( rawBody, "not support" );
			}
		}

		namespace
		{
			// Eval-harness E1: the auth header names stripped from every
			// recorded request (case-insensitive) -- the key must never reach
			// a trajectory line via the header path.
			bool IsAuthHeaderName( const std::string& name )
			{
				std::string lower;
				lower.reserve( name.size() );
				for( std::size_t i = 0; i < name.size(); ++i ) {
					char c = name[i];
					if( c >= 'A' && c <= 'Z' ) c = static_cast<char>( c - 'A' + 'a' );
					lower += c;
				}
				return lower == "authorization" || lower == "x-api-key" ||
				       lower == "x-goog-api-key";
			}

			//! A copy of `req` with every auth header removed by name.
			ChatHttpRequest StripAuthHeaders( const ChatHttpRequest& req )
			{
				ChatHttpRequest out;
				out.url = req.url;
				out.body = req.body;
				for( std::size_t i = 0; i < req.headers.size(); ++i ) {
					if( IsAuthHeaderName( req.headers[i].first ) ) continue;
					out.headers.push_back( req.headers[i] );
				}
				return out;
			}
		}

		void AgentChatLoop::AddUserMessage( const std::string& text )
		{
			AddUserMessage( text, std::vector<ChatAttachment>() );
		}

		void AgentChatLoop::AddUserMessage( const std::string& text,
		                                    const std::vector<ChatAttachment>& attachments )
		{
			// EMPTY / whitespace-only text with NO attachments is a
			// documented NO-OP (see the header): Anthropic hard-400s an
			// empty text block, so recording one would poison every later
			// request.  The caller sent nothing, so nothing is flushed or
			// reset either -- the turn state stays exactly as it was. An
			// attachment-only message (blank text, non-empty attachments)
			// is legitimate -- MakeUserEntry omits the empty text block
			// for that case (see AgentChatCodecs.cpp) -- so the no-op only
			// fires when BOTH are empty.
			if( IsBlank( text ) && attachments.empty() ) return;

			// Eval-harness E1: ensure the session record leads the trajectory.
			EnsureSessionRecordEmitted();

			// Pending tool calls belong to the PREVIOUS assistant turn --
			// flush them ahead of the new user message so the wire order
			// stays assistant(tool_use) -> user(tool_results) -> user(text).
			// Unanswered calls get synthesized error results in the flush.
			FlushPendingToolResults();

			ChatTranscriptEntry entry;
			entry.role = ChatTranscriptEntry::Role::User;
			entry.displayText = text;
			entry.rawJson = mCodec->MakeUserEntry( text, attachments );
			entry.liveUserImageCount = static_cast<int>( attachments.size() );
			mTranscript.push_back( entry );

			// Eval-harness E1: record the user turn (text + attachment count).
			if( mRecorder ) {
				TrajectoryUserRecord u;
				u.text = text;
				u.attachments = static_cast<int>( attachments.size() );
				mRecorder->EmitUser( u );
			}

			// USER IMAGE RETENTION (see the file header): elide the oldest
			// live user images across the WHOLE transcript, just enough
			// that the running total never exceeds kMaxLiveUserImages.
			// Walking oldest-first and only ever touching User entries
			// (Assistant/ToolResults entries always carry
			// liveUserImageCount == 0, so this loop never looks at them).
			if( !attachments.empty() ) {
				int totalLive = 0;
				for( std::size_t i = 0; i < mTranscript.size(); ++i )
					totalLive += mTranscript[i].liveUserImageCount;

				int toElide = totalLive - kMaxLiveUserImages;
				for( std::size_t i = 0; toElide > 0 && i < mTranscript.size(); ++i ) {
					ChatTranscriptEntry& e = mTranscript[i];
					if( e.role != ChatTranscriptEntry::Role::User || e.liveUserImageCount <= 0 )
						continue;
					const int elideHere = ( toElide < e.liveUserImageCount ) ? toElide : e.liveUserImageCount;
					const std::size_t beforeBytes = e.rawJson.size();
					e.rawJson = mCodec->RewriteElidedUserImages( e.rawJson, elideHere );
					e.liveUserImageCount -= elideHere;
					toElide -= elideHere;
					// Eval-harness E1: record the transcript rewrite.
					if( mRecorder ) {
						TrajectoryHistoryEditRecord h;
						h.entryIndex = static_cast<int>( i );
						h.beforeBytes = static_cast<long long>( beforeBytes );
						h.afterBytes = static_cast<long long>( e.rawJson.size() );
						h.reason = "user_image_elision";
						mRecorder->EmitHistoryEdit( h );
					}
				}
			}

			// A new conversation-turn: the tool-round cap starts over.
			mToolRounds = 0;
		}

		ChatHttpRequest AgentChatLoop::BuildRequest( const std::string& apiKey )
		{
			FlushPendingToolResults();

			// TEXT-ONLY-MODEL IMAGE-REJECTION RECOVERY: once a model has
			// proven text-only, every request stays image-free -- strip any
			// image the flush above may have just packed (a new read_image
			// result) before it reaches the wire.  Idempotent no-op in the
			// common case (nothing live), so the pre-fix byte shape is
			// preserved for every image-capable session.
			if( mElideAllImages ) ElideAllLiveImages();

			// Nothing to send: refuse with an EMPTY request (url == "").
			// Documented in the header; the caller must not perform it.
			if( mTranscript.empty() )
				return ChatHttpRequest();

			std::vector<std::string> rawEntries;
			rawEntries.reserve( mTranscript.size() );
			for( std::size_t i = 0; i < mTranscript.size(); ++i )
				rawEntries.push_back( mTranscript[i].rawJson );

			// Facet 5 slice S1: append the skills section (when set) to the
			// base prompt.  An empty index text sends the base prompt
			// UNCHANGED -- byte-identical to the pre-S1 behaviour.
			const std::string systemPrompt = ComposeSystemPrompt();

			// The key goes straight through to the codec's auth header and
			// is retained NOWHERE in this object.
			ChatHttpRequest req = mCodec->BuildRequest( mModelId, apiKey, systemPrompt, rawEntries );

			// Eval-harness E1: cache the request for RecordHttpRound's
			// convenience overload -- but with EVERY auth header STRIPPED so
			// the key is still held NOWHERE in this object (the same rule the
			// class contract states).
			if( mRecorder ) mLastRequest = StripAuthHeaders( req );

			return req;
		}

		ChatStepResult AgentChatLoop::HandleResponse( long httpStatus, const std::string& rawBody )
		{
			// Caller-contract guard: a legitimate response can only follow
			// a BuildRequest, and BuildRequest flushes the pending set --
			// so a HandleResponse while tool calls are still pending is a
			// double-consume for one round.  Refuse it, recording nothing.
			if( !mPendingCalls.empty() ) {
				ChatStepResult refused;
				refused.kind = ChatStepResult::Kind::ProviderError;
				refused.errorKind = ChatErrorKind::Misuse;
				refused.errorMessage =
					"chat-loop misuse: HandleResponse called while " +
					std::to_string( mPendingCalls.size() ) +
					" tool call(s) of the previous turn are still pending -- resolve them "
					"with AddToolResult (or BuildRequest/AddUserMessage, which flush them "
					"with synthesized error results) before handling another response";
				return refused;
			}

			ChatParsedResponse pr = mCodec->ParseResponse( httpStatus, rawBody );

			// TEXT-ONLY-MODEL IMAGE-REJECTION RECOVERY: a text-only backend
			// 400-rejects the request because a prior read_image (or a user
			// attachment) put an image on the wire.  Detect it NARROWLY
			// (status 400 + "multimodal"/"not support" in the body), and
			// only the FIRST time (mElideAllImages gates once-per-session,
			// which is also once-per-round: the retry request carries no
			// image, so a well-behaved server cannot 400-multimodal it
			// again; a misbehaving one that does finds mElideAllImages
			// already set and falls through to the normal provider_error).
			// On detection: elide EVERY live image from the transcript, set
			// the sticky bool so later rounds stay image-free, and flag the
			// step so the driver re-issues the SAME round once.  The step
			// stays a ProviderError (errorKind Http) so that if the retry
			// ALSO fails the existing error UX is unchanged; nothing is
			// recorded either way (the ProviderError contract).
			if( pr.step.kind == ChatStepResult::Kind::ProviderError &&
			    pr.step.errorKind == ChatErrorKind::Http &&
			    !mElideAllImages &&
			    IsMultimodalUnsupported400( httpStatus, rawBody ) ) {
				mElideAllImages = true;
				ElideAllLiveImages();
				pr.step.retryWithoutImages = true;
				return pr.step;
			}

			if( pr.step.kind == ChatStepResult::Kind::ToolCalls ) {
				// Iteration cap: the (kMaxToolRoundsPerTurn+1)-th tool round
				// within one conversation-turn is refused WITHOUT recording
				// the assistant turn -- the transcript stays at its previous
				// consistent state (no unanswered tool_use on the wire) and
				// a new AddUserMessage resets the counter.
				if( mToolRounds >= kMaxToolRoundsPerTurn ) {
					ChatStepResult capped;
					capped.kind = ChatStepResult::Kind::ProviderError;
					capped.errorKind = ChatErrorKind::IterationCap;
					capped.errorMessage =
						"iteration cap: the model requested more than " +
						std::to_string( kMaxToolRoundsPerTurn ) +
						" tool rounds in one turn; stopping so the loop cannot spin";
					return capped;
				}
				++mToolRounds;

				ChatTranscriptEntry entry;
				entry.role = ChatTranscriptEntry::Role::Assistant;
				entry.displayText = pr.assistantDisplayText;
				entry.rawJson = pr.assistantEntryJson;
				mTranscript.push_back( entry );

				mPendingCalls = pr.step.toolCalls;
				mPendingResults.clear();
			}
			else if( pr.step.kind == ChatStepResult::Kind::FinalText ) {
				ChatTranscriptEntry entry;
				entry.role = ChatTranscriptEntry::Role::Assistant;
				entry.displayText = pr.assistantDisplayText;
				entry.rawJson = pr.assistantEntryJson;
				mTranscript.push_back( entry );

				mPendingCalls.clear();
				mPendingResults.clear();
			}
			// ProviderError: record nothing -- the transcript stays at its
			// previous consistent state and the caller may retry.

			return pr.step;
		}

		std::string AgentChatLoop::ToolCallToJsonRpcLine( const ChatToolCall& call, int rpcId )
		{
			JsonValue params;
			std::string perr;
			if( !JsonParse( call.argsJson, params, perr ) || !params.isObject() )
				params = JsonValue::MakeObject();

			JsonValue req = JsonValue::MakeObject();
			req.set( "jsonrpc", JsonValue::MakeString( "2.0" ) );
			req.set( "id", JsonValue::MakeNumber( static_cast<double>( rpcId ) ) );
			req.set( "method", JsonValue::MakeString( call.name ) );
			req.set( "params", params );
			const std::string line = JsonSerialize( req );

			// Eval-harness E1: stamp the dispatch start + line, keyed by call
			// id, so AddToolResult can complete the `tool` record's latency.
			if( mRecorder ) {
				ToolLinePending pend;
				pend.id = call.id;
				pend.line = line;
				pend.startMs = mTrajectoryClock ? mTrajectoryClock() : TrajectoryNowMs();
				mToolLineStash.push_back( pend );
			}
			return line;
		}

		void AgentChatLoop::AddToolResult( const ChatToolCall& call, const std::string& rawJsonRpcResponseLine )
		{
			// Accept a result ONLY for a pending, not-yet-answered call of
			// the current assistant turn (documented in the header): an id
			// that is not pending, or a second result for an id that
			// already has one, is IGNORED -- first result wins.
			bool pending = false;
			for( std::size_t i = 0; i < mPendingCalls.size(); ++i ) {
				if( mPendingCalls[i].id == call.id ) { pending = true; break; }
			}
			if( !pending ) return;
			for( std::size_t i = 0; i < mPendingResults.size(); ++i ) {
				if( mPendingResults[i].first.id == call.id ) return;
			}

			mPendingResults.push_back( std::make_pair( call, rawJsonRpcResponseLine ) );

			// Eval-harness E1: complete the `tool` record for this call --
			// pair the stamped JSON-RPC line/latency with the response envelope.
			if( mRecorder ) {
				EnsureSessionRecordEmitted();
				TrajectoryToolRecord t;
				t.name = call.name;
				t.callId = call.id;
				t.argsJson = call.argsJson.empty() ? std::string( "{}" ) : call.argsJson;
				t.jsonRpcResponse = rawJsonRpcResponseLine;
				const int64_t now = mTrajectoryClock ? mTrajectoryClock() : TrajectoryNowMs();
				for( std::size_t k = 0; k < mToolLineStash.size(); ++k ) {
					if( mToolLineStash[k].id == call.id ) {
						t.jsonRpcRequest = mToolLineStash[k].line;
						t.latencyMs = now - mToolLineStash[k].startMs;
						mToolLineStash.erase( mToolLineStash.begin() + k );
						break;
					}
				}
				JsonValue env;
				std::string perr;
				if( JsonParse( rawJsonRpcResponseLine, env, perr ) && env.isObject() ) {
					const JsonValue* errp = env.find( "error" );
					if( errp && !errp->isNull() ) t.error = true;
					const JsonValue& result = env.get( "result" );
					if( result.isObject() ) {
						const JsonValue& hv = result.get( "headVersion" );
						if( hv.isObject() ) {
							if( hv.get( "revision" ).isNumber() )
								t.headVersionAfter = static_cast<long long>( hv.get( "revision" ).asNumber() );
						}
						else if( hv.isNumber() ) {
							t.headVersionAfter = static_cast<long long>( hv.asNumber() );
						}
					}
				}
				mRecorder->EmitTool( t );
			}

			// Once every pending call of the assistant turn has a result,
			// pack them ALL into one user message (Anthropic requires the
			// parallel tool_use blocks to be answered together).
			if( mPendingResults.size() >= mPendingCalls.size() )
				FlushPendingToolResults();
		}

		void AgentChatLoop::FlushPendingToolResults()
		{
			// No pending turn -> nothing to flush.  (mPendingResults cannot
			// be non-empty here: AddToolResult only accepts answers to
			// pending calls.  Clear defensively all the same.)
			if( mPendingCalls.empty() ) {
				mPendingResults.clear();
				return;
			}

			// WIRE INVARIANT (by construction): every tool call of the
			// recorded assistant turn is answered in this ONE user message.
			// Any pending call without a buffered result -- user interrupt,
			// tool crash, partial round -- gets a SYNTHESIZED error result;
			// otherwise the transcript would replay an unanswered tool call
			// on every later request (Anthropic hard-400s it, Gemini
			// rejects the mismatched functionResponse count) and only
			// Reset() could recover.
			static const char* const kUnexecutedEnvelope =
				"{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32001,"
				"\"message\":\"tool call was not executed (cancelled or interrupted)\"}}";

			std::vector<std::pair<ChatToolCall, std::string>> ordered;
			ordered.reserve( mPendingCalls.size() );
			std::vector<bool> used( mPendingResults.size(), false );
			std::string names;
			for( std::size_t i = 0; i < mPendingCalls.size(); ++i ) {
				std::size_t found = mPendingResults.size();
				for( std::size_t j = 0; j < mPendingResults.size(); ++j ) {
					if( !used[j] && mPendingResults[j].first.id == mPendingCalls[i].id ) {
						found = j;
						break;
					}
				}
				if( i ) names += ", ";
				names += mPendingCalls[i].name;
				if( found < mPendingResults.size() ) {
					used[found] = true;
					ordered.push_back( mPendingResults[found] );
				}
				else {
					names += " (not executed)";
					ordered.push_back( std::make_pair( mPendingCalls[i],
					                                   std::string( kUnexecutedEnvelope ) ) );
				}
			}
			// Defensive: mPendingResults can only hold answers to pending
			// calls (AddToolResult filters), so `ordered` covers everything.

			// IMAGE RETENTION (see the header): when this entry packs a NEW
			// image, elide the image from every older ToolResults entry so
			// only the most recent PNG rides (and is billed) per request.
			// ToolResults entries are loop-generated, so rewriting them is
			// legal; assistant entries are never touched.
			bool carriesImage = false;
			for( std::size_t i = 0; i < ordered.size(); ++i ) {
				if( ChatToolResultCarriesImage( ordered[i].first, ordered[i].second ) ) {
					carriesImage = true;
					break;
				}
			}
			if( carriesImage ) {
				for( std::size_t i = 0; i < mTranscript.size(); ++i ) {
					if( mTranscript[i].role != ChatTranscriptEntry::Role::ToolResults ||
					    !mTranscript[i].carriesLiveImage ) continue;
					const std::size_t beforeBytes = mTranscript[i].rawJson.size();
					mTranscript[i].rawJson = mCodec->RewriteElidedImages( mTranscript[i].rawJson );
					mTranscript[i].carriesLiveImage = false;
					// Eval-harness E1: record the transcript rewrite.
					if( mRecorder ) {
						EnsureSessionRecordEmitted();
						TrajectoryHistoryEditRecord h;
						h.entryIndex = static_cast<int>( i );
						h.beforeBytes = static_cast<long long>( beforeBytes );
						h.afterBytes = static_cast<long long>( mTranscript[i].rawJson.size() );
						h.reason = "tool_image_elision";
						mRecorder->EmitHistoryEdit( h );
					}
				}
			}

			ChatTranscriptEntry entry;
			entry.role = ChatTranscriptEntry::Role::ToolResults;
			entry.displayText = "[tool results: " + names + "]";
			entry.rawJson = mCodec->PackToolResults( ordered );
			entry.carriesLiveImage = carriesImage;
			mTranscript.push_back( entry );

			mPendingResults.clear();
			mPendingCalls.clear();
			// Eval-harness E1: the turn's tool calls are done -- drop any
			// remaining stamped lines (unanswered/synthesized calls).
			mToolLineStash.clear();
		}

		void AgentChatLoop::ElideAllLiveImages()
		{
			// TEXT-ONLY-MODEL IMAGE-REJECTION RECOVERY: rewrite every entry
			// that still carries a live image to its already-elided form.
			// Two independent kinds ride here, each with its own codec
			// rewrite + its own liveness flag (mirroring the two retention
			// policies already in this file):
			//   * ToolResults entries (packed read_image PNGs) via
			//     RewriteElidedImages -- clears carriesLiveImage.
			//   * User entries (reference-image attachments) via
			//     RewriteElidedUserImages(count) -- clears liveUserImageCount.
			// Rewriting is legal ONLY because both entry kinds are
			// loop-generated; assistant entries carry the byte-preservation
			// contract and are never touched (their flags are always 0).
			for( std::size_t i = 0; i < mTranscript.size(); ++i ) {
				ChatTranscriptEntry& e = mTranscript[i];
				if( e.role == ChatTranscriptEntry::Role::ToolResults && e.carriesLiveImage ) {
					const std::size_t beforeBytes = e.rawJson.size();
					e.rawJson = mCodec->RewriteElidedImages( e.rawJson );
					e.carriesLiveImage = false;
					if( mRecorder ) {
						EnsureSessionRecordEmitted();
						TrajectoryHistoryEditRecord h;
						h.entryIndex = static_cast<int>( i );
						h.beforeBytes = static_cast<long long>( beforeBytes );
						h.afterBytes = static_cast<long long>( e.rawJson.size() );
						h.reason = "multimodal_tool_image_elision";
						mRecorder->EmitHistoryEdit( h );
					}
				}
				else if( e.role == ChatTranscriptEntry::Role::User && e.liveUserImageCount > 0 ) {
					const std::size_t beforeBytes = e.rawJson.size();
					e.rawJson = mCodec->RewriteElidedUserImages( e.rawJson, e.liveUserImageCount );
					e.liveUserImageCount = 0;
					if( mRecorder ) {
						EnsureSessionRecordEmitted();
						TrajectoryHistoryEditRecord h;
						h.entryIndex = static_cast<int>( i );
						h.beforeBytes = static_cast<long long>( beforeBytes );
						h.afterBytes = static_cast<long long>( e.rawJson.size() );
						h.reason = "multimodal_user_image_elision";
						mRecorder->EmitHistoryEdit( h );
					}
				}
			}
		}

		const ChatTranscriptEntry& AgentChatLoop::TranscriptAt( std::size_t i ) const
		{
			// Bounds-safe like JsonValue::at: a static empty entry when out
			// of range (never throws).
			static const ChatTranscriptEntry kEmpty;
			if( i >= mTranscript.size() ) return kEmpty;
			return mTranscript[i];
		}

		//==============================================================
		// Eval-harness E1: trajectory helpers + hooks.
		//==============================================================
		namespace
		{
			//! ChatErrorKind -> the OTel-ish error.type token.
			const char* ErrorKindToString( ChatErrorKind kind )
			{
				switch( kind ) {
					case ChatErrorKind::Http:         return "http";
					case ChatErrorKind::Parse:        return "parse";
					case ChatErrorKind::Provider:     return "provider";
					case ChatErrorKind::Refusal:      return "refusal";
					case ChatErrorKind::MaxTokens:    return "max_tokens";
					case ChatErrorKind::IterationCap: return "iteration_cap";
					case ChatErrorKind::Misuse:       return "misuse";
					case ChatErrorKind::None:
					default:                          return "";
				}
			}

			//! Normalized finish reasons derived from a parsed disposition.
			std::vector<std::string> FinishReasonsForStep( const ChatStepResult& step )
			{
				std::vector<std::string> out;
				if( step.kind == ChatStepResult::Kind::ToolCalls ) {
					out.push_back( "tool_calls" );
				}
				else if( step.kind == ChatStepResult::Kind::FinalText ) {
					out.push_back( "stop" );
				}
				else if( step.errorKind == ChatErrorKind::MaxTokens ) {
					out.push_back( "length" );
				}
				else if( step.errorKind == ChatErrorKind::Refusal ) {
					out.push_back( "content_filter" );
				}
				return out;
			}

			//! Best-effort provider-neutral response-model extraction.
			std::string ExtractResponseModel( const std::string& rawBody )
			{
				JsonValue root;
				std::string perr;
				if( !JsonParse( rawBody, root, perr ) || !root.isObject() ) return std::string();
				if( root.get( "model" ).isString() ) return root.get( "model" ).asString();
				if( root.get( "modelVersion" ).isString() ) return root.get( "modelVersion" ).asString();
				return std::string();
			}

			//! Extract just the sampling PARAMS from a request body: every
			//! top-level member except the large conversation/tool arrays.
			//! Returns a JSON object literal string.
			std::string ExtractRequestParams( const std::string& body )
			{
				JsonValue root;
				std::string perr;
				if( !JsonParse( body, root, perr ) || !root.isObject() )
					return "{}";
				JsonValue params = JsonValue::MakeObject();
				const std::vector<std::pair<std::string, JsonValue> >& members = root.members();
				for( std::size_t i = 0; i < members.size(); ++i ) {
					const std::string& key = members[i].first;
					if( key == "messages" || key == "contents" || key == "tools" ||
					    key == "system" || key == "systemInstruction" || key == "toolConfig" )
						continue;
					params.set( key, members[i].second );
				}
				return JsonSerialize( params );
			}

			//! Serialize a header list (already auth-stripped) as a JSON
			//! object literal string.
			std::string HeadersToJson(
				const std::vector<std::pair<std::string, std::string> >& headers )
			{
				JsonValue o = JsonValue::MakeObject();
				for( std::size_t i = 0; i < headers.size(); ++i )
					o.set( headers[i].first, JsonValue::MakeString( headers[i].second ) );
				return JsonSerialize( o );
			}
		}

		std::string AgentChatLoop::ComposeSystemPrompt() const
		{
			std::string systemPrompt = kSystemPrompt;
			if( !mSkillIndexText.empty() ) {
				systemPrompt += "\n\nAvailable skills:\n";
				systemPrompt += mSkillIndexText;
				systemPrompt += "\nCall read_skill before scene-authoring tasks.";
			}
			return systemPrompt;
		}

		void AgentChatLoop::EnsureSessionRecordEmitted()
		{
			if( !mRecorder || mSessionEmitted ) return;
			mSessionEmitted = true;   // set first (the emit path never re-enters)

			TrajectorySessionRecord sess;
			sess.provider = mCodec->ProviderName();
			sess.requestModel = mModelId;
			sess.systemPrompt = ComposeSystemPrompt();
			sess.systemPromptHash = TrajectoryHashHex( sess.systemPrompt );
			sess.toolDefsHash = TrajectoryHashHex( ChatToolDefsFingerprint() );
			sess.scenePath = mTrajectoryConfig.scenePath;
			sess.sceneHeadVersion = mTrajectoryConfig.sceneHeadVersion;
			mRecorder->EmitSession( sess );
		}

		void AgentChatLoop::CloseTrajectorySession( const std::string& status )
		{
			if( !mRecorder || !mSessionEmitted ) return;
			mRecorder->EmitSummary( status );
			mSessionEmitted = false;
			// Roll to a fresh trace id on the SAME sink so the next recorded
			// action starts a distinct session (the GUI additionally rotates
			// to a new FILE on a new chat -- see the driver wiring).
			if( mTrajectorySink ) {
				ChatTrajectoryConfig cfg = mTrajectoryConfig;
				cfg.traceId.clear();
				mRecorder.reset( new ChatTrajectoryRecorder( mTrajectorySink, cfg ) );
			}
		}

		void AgentChatLoop::SetTrajectorySink(
			std::function<void(const std::string&)> sink,
			const ChatTrajectoryConfig& config )
		{
			// Replacing an active session closes it with a "replaced" summary
			// so no rollup is lost.
			if( mRecorder && mSessionEmitted )
				mRecorder->EmitSummary( "replaced" );

			mSessionEmitted = false;
			mTrajectorySink = sink;
			mTrajectoryConfig = config;
			mTrajectoryClock = config.clock;   // "" -> the recorder/loop use a real clock

			if( sink ) {
				mRecorder.reset( new ChatTrajectoryRecorder( sink, config ) );
			}
			else {
				// An empty sink DETACHES recording (every hook becomes a no-op).
				mRecorder.reset();
				mTrajectorySink = std::function<void(const std::string&)>();
			}
		}

		void AgentChatLoop::RecordHttpRound(
			const ChatHttpRequest& request, long httpStatus,
			const std::string& rawBody, int64_t elapsedMs, int attempt, int retryOf )
		{
			if( !mRecorder ) return;
			EnsureSessionRecordEmitted();

			// Parse the disposition for finish reasons + error type (a second,
			// independent parse from HandleResponse's -- keeps the two hooks
			// cleanly separated; response bodies are small).
			ChatParsedResponse pr = mCodec->ParseResponse( httpStatus, rawBody );
			ChatUsage usage = mCodec->ParseUsage( rawBody );

			TrajectoryLlmRecord rec;
			rec.requestModel = mModelId;
			rec.responseModel = ExtractResponseModel( rawBody );
			rec.requestParamsJson = ExtractRequestParams( request.body );
			// BELT: strip auth headers by name before the record is built.
			rec.requestHeadersJson = HeadersToJson( StripAuthHeaders( request ).headers );
			rec.httpStatus = httpStatus;
			rec.latencyMs = elapsedMs;
			rec.finishReasons = FinishReasonsForStep( pr.step );
			rec.inputTokens = usage.inputTokens;
			rec.outputTokens = usage.outputTokens;
			rec.cacheReadInputTokens = usage.cacheReadInputTokens;
			if( pr.step.kind == ChatStepResult::Kind::ProviderError )
				rec.errorType = ErrorKindToString( pr.step.errorKind );
			rec.attempt = attempt;
			rec.retryOf = retryOf;
			rec.responseBody = rawBody;
			mRecorder->EmitLlm( rec );
		}

		void AgentChatLoop::RecordHttpRound(
			long httpStatus, const std::string& rawBody, int64_t elapsedMs,
			int attempt, int retryOf )
		{
			if( !mRecorder ) return;
			RecordHttpRound( mLastRequest, httpStatus, rawBody, elapsedMs, attempt, retryOf );
		}

		void AgentChatLoop::FinishTrajectory( const std::string& status )
		{
			CloseTrajectorySession( status );
		}
	}
}
