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
			//! the agent co-edit ONE live scene through the six verbs.
			const char* const kSystemPrompt =
				"You are a scene-editing agent embedded in the RISE renderer. You and "
				"the user CO-EDIT one live scene: the user sees the same viewport and "
				"may edit concurrently, so never assume the document is unchanged -- "
				"always work from a fresh read_document.\n"
				"\n"
				"Workflow for every edit:\n"
				"1. read_document to see the scene and its headVersion (read_schema "
				"when unsure about a chunk or parameter).\n"
				"2. propose_patch with the headVersion you just read as "
				"baseHeadVersion. On status=conflict, re-read and re-propose; on "
				"retriable=true, retry the same patch after a moment.\n"
				"3. render, then read_image to SEE the result and verify the edit "
				"visually before declaring it done.\n"
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
			// Buffered tool results belong to the PREVIOUS assistant turn --
			// flush them ahead of the new user message so the wire order
			// stays assistant(tool_use) -> user(tool_results) -> user(text).
			FlushPendingToolResults();
			mPendingCalls.clear();

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

			std::vector<std::string> rawEntries;
			rawEntries.reserve( mTranscript.size() );
			for( std::size_t i = 0; i < mTranscript.size(); ++i )
				rawEntries.push_back( mTranscript[i].rawJson );

			// The key goes straight through to the codec's auth header and
			// is retained NOWHERE in this object.
			return mCodec->BuildRequest( mModelId, apiKey, kSystemPrompt, rawEntries );
		}

		ChatStepResult AgentChatLoop::HandleResponse( long httpStatus, const std::string& rawBody )
		{
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
			mPendingResults.push_back( std::make_pair( call, rawJsonRpcResponseLine ) );

			// Once every pending call of the assistant turn has a result,
			// pack them ALL into one user message (Anthropic requires the
			// parallel tool_use blocks to be answered together).
			if( !mPendingCalls.empty() && mPendingResults.size() >= mPendingCalls.size() )
				FlushPendingToolResults();
		}

		void AgentChatLoop::FlushPendingToolResults()
		{
			if( mPendingResults.empty() ) return;

			ChatTranscriptEntry entry;
			entry.role = ChatTranscriptEntry::Role::ToolResults;
			std::string names;
			for( std::size_t i = 0; i < mPendingResults.size(); ++i ) {
				if( i ) names += ", ";
				names += mPendingResults[i].first.name;
			}
			entry.displayText = "[tool results: " + names + "]";
			entry.rawJson = mCodec->PackToolResults( mPendingResults );
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
