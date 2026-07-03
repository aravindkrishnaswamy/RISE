//////////////////////////////////////////////////////////////////////
//
//  AgentChatLoop.cpp - the provider-agnostic, sans-IO chat-loop core
//    (see AgentChatLoop.h for the contract).
//
//  NO LOGGING anywhere in this file: request/response bodies may embed
//  scene content, and the API key must never reach a log.
//
//////////////////////////////////////////////////////////////////////

#include "AgentChatLoop.h"

#include "Json.h"

namespace RISE
{
	namespace Agent
	{
		namespace
		{
			//! The static co-editing system prompt (rule 7): the user and
			//! the agent co-edit ONE live scene through the nine verbs.
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
				"visually before declaring it done.\n"
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
			mCodec( MakeCodec( ChatProvider::Anthropic ) ),
			mProvider( ChatProvider::Anthropic ),
			mModelId( mCodec->DefaultModelId() ),
			mToolRounds( 0 )
		{
		}

		AgentChatLoop::~AgentChatLoop()
		{
		}

		void AgentChatLoop::Reset()
		{
			mTranscript.clear();
			mPendingCalls.clear();
			mPendingResults.clear();
			mToolRounds = 0;
		}

		void AgentChatLoop::SetProvider( ChatProvider provider, const std::string& modelId )
		{
			// SWITCHING PROVIDER RESETS THE TRANSCRIPT: assistant entries
			// are provider-native raw JSON and cannot cross providers.
			mProvider = provider;
			mCodec = MakeCodec( provider );
			mModelId = modelId.empty() ? std::string( mCodec->DefaultModelId() ) : modelId;
			Reset();
		}

		void AgentChatLoop::AddUserMessage( const std::string& text )
		{
			// EMPTY / whitespace-only user text is a documented NO-OP
			// (see the header): Anthropic hard-400s an empty text block,
			// so recording one would poison every later request.  The
			// caller sent nothing, so nothing is flushed or reset either
			// -- the turn state stays exactly as it was.
			bool blank = true;
			for( std::size_t i = 0; i < text.size(); ++i ) {
				const char c = text[i];
				if( c != ' ' && c != '\t' && c != '\n' && c != '\r' ) { blank = false; break; }
			}
			if( blank ) return;

			// Pending tool calls belong to the PREVIOUS assistant turn --
			// flush them ahead of the new user message so the wire order
			// stays assistant(tool_use) -> user(tool_results) -> user(text).
			// Unanswered calls get synthesized error results in the flush.
			FlushPendingToolResults();

			ChatTranscriptEntry entry;
			entry.role = ChatTranscriptEntry::Role::User;
			entry.displayText = text;
			entry.rawJson = mCodec->MakeUserEntry( text );
			mTranscript.push_back( entry );

			// A new conversation-turn: the tool-round cap starts over.
			mToolRounds = 0;
		}

		ChatHttpRequest AgentChatLoop::BuildRequest( const std::string& apiKey )
		{
			FlushPendingToolResults();

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
			std::string systemPrompt = kSystemPrompt;
			if( !mSkillIndexText.empty() ) {
				systemPrompt += "\n\nAvailable skills:\n";
				systemPrompt += mSkillIndexText;
				systemPrompt += "\nCall read_skill before scene-authoring tasks.";
			}

			// The key goes straight through to the codec's auth header and
			// is retained NOWHERE in this object.
			return mCodec->BuildRequest( mModelId, apiKey, systemPrompt, rawEntries );
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

		std::string AgentChatLoop::ToolCallToJsonRpcLine( const ChatToolCall& call, int rpcId ) const
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
			return JsonSerialize( req );
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
					mTranscript[i].rawJson = mCodec->RewriteElidedImages( mTranscript[i].rawJson );
					mTranscript[i].carriesLiveImage = false;
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
		}

		const ChatTranscriptEntry& AgentChatLoop::TranscriptAt( std::size_t i ) const
		{
			// Bounds-safe like JsonValue::at: a static empty entry when out
			// of range (never throws).
			static const ChatTranscriptEntry kEmpty;
			if( i >= mTranscript.size() ) return kEmpty;
			return mTranscript[i];
		}
	}
}
