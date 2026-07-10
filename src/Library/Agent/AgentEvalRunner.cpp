//////////////////////////////////////////////////////////////////////
//
//  AgentEvalRunner.cpp - the replay source + scenario loader + scenario
//    runner (see AgentEvalRunner.h).
//
//  NO ENVIRONMENT-VARIABLE / CREDENTIAL READS ANYWHERE IN THIS FILE --
//  the replay source is keyless by construction (BuildRequest is always
//  called with an empty api key; nothing here ever calls getenv).
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "AgentEvalRunner.h"

#include "AgentSession.h"
#include "AgentRpc.h"

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace RISE
{
	namespace Agent
	{
		namespace
		{
			//----------------------------------------------------------
			// Small file-IO helpers (kept local -- no other TU needs
			// them).
			//----------------------------------------------------------

			bool ReadNonBlankLines( const std::string& path, std::vector<std::string>& outLines,
			                        std::string& err )
			{
				std::ifstream f( path.c_str(), std::ios::binary );
				if( !f ) { err = "cannot open file: " + path; return false; }
				std::string line;
				while( std::getline( f, line ) ) {
					if( !line.empty() && line.back() == '\r' ) line.pop_back();   // CRLF-authored files
					bool blank = true;
					for( std::size_t i = 0; i < line.size(); ++i ) {
						if( !std::isspace( static_cast<unsigned char>( line[i] ) ) ) { blank = false; break; }
					}
					if( blank ) continue;
					outLines.push_back( line );
				}
				return true;
			}

			bool ReadWholeFile( const std::string& path, std::string& out, std::string& err )
			{
				std::ifstream f( path.c_str(), std::ios::binary );
				if( !f ) { err = "cannot open file: " + path; return false; }
				std::ostringstream ss;
				ss << f.rdbuf();
				out = ss.str();
				return true;
			}

			std::string JoinPath( const std::string& dir, const std::string& leaf )
			{
				std::filesystem::path p( dir );
				p /= leaf;
				return p.string();
			}
		}

		//====================================================================
		// 1. AgentEvalReplaySource.
		//====================================================================
		bool AgentEvalReplaySource::NextBody( std::string& outBody )
		{
			if( mIndex >= mBodies.size() ) { outBody.clear(); return false; }
			outBody = mBodies[mIndex++];
			return true;
		}

		bool AgentEvalReplaySource::LoadFromFile( const std::string& path, AgentEvalReplaySource& out,
		                                           std::string& err )
		{
			out = AgentEvalReplaySource();

			std::vector<std::string> lines;
			if( !ReadNonBlankLines( path, lines, err ) ) return false;
			if( lines.empty() ) {
				err = "replay fixture '" + path + "' has no non-blank lines";
				return false;
			}

			JsonValue first;
			std::string perr;
			if( !JsonParse( lines[0], first, perr ) ) {
				err = "replay fixture '" + path + "' line 1 does not parse as JSON: " + perr;
				return false;
			}
			if( !first.isObject() ) {
				err = "replay fixture '" + path + "' line 1 is not a JSON object";
				return false;
			}

			// Auto-detect: a recorded E1 trajectory carries "run_type" on
			// every line; a raw fixture line does not.
			if( first.has( "run_type" ) ) {
				std::string provider;
				std::vector<std::string> bodies;
				for( std::size_t i = 0; i < lines.size(); ++i ) {
					JsonValue j;
					std::string lerr;
					if( !JsonParse( lines[i], j, lerr ) ) {
						err = "replay fixture '" + path + "' (trajectory) line " + std::to_string( i + 1 ) +
							" does not parse as JSON: " + lerr;
						return false;
					}
					if( !j.isObject() ) continue;   // tolerate a stray non-object line (never emitted by the recorder)
					const std::string runType = j.get( "run_type" ).asString();
					if( runType == "session" ) {
						// Review-round P2: the replay source models exactly ONE
						// provider's wire format, but a recorded trajectory can
						// carry MULTIPLE session records -- a mid-chat provider
						// switch (on-by-default; SetProvider resets the transcript
						// and rolls a fresh session).  Collecting every llm body
						// under the first provider would silently feed
						// gemini-shaped bodies to the anthropic codec.  Refuse
						// loudly and tell the user to replay one provider-segment
						// at a time.
						const std::string thisProvider = j.get( "provider" ).asString();
						if( provider.empty() ) {
							provider = thisProvider;
						}
						else if( thisProvider != provider ) {
							err = "replay fixture '" + path + "' (trajectory) switches provider mid-file ('" +
							      provider + "' -> '" + thisProvider + "'): a provider switch resets the "
							      "transcript and changes the wire format, so the recording must be replayed "
							      "one provider-segment at a time (split it at the session boundary)";
							return false;
						}
					}
					else if( runType == "llm" ) {
						bodies.push_back( j.get( "response_body" ).asString() );
					}
				}
				if( provider.empty() ) {
					err = "replay fixture '" + path + "' (trajectory) has no `session` record (provider unknown)";
					return false;
				}
				if( bodies.empty() ) {
					err = "replay fixture '" + path + "' (trajectory) has no `llm` records to replay";
					return false;
				}
				out.mProvider = provider;
				out.mBodies = bodies;
				return true;
			}

			// Raw fixture: each line {"provider":"...", "body":"..."}.
			std::string provider;
			std::vector<std::string> bodies;
			for( std::size_t i = 0; i < lines.size(); ++i ) {
				JsonValue j;
				std::string lerr;
				if( !JsonParse( lines[i], j, lerr ) ) {
					err = "replay fixture '" + path + "' line " + std::to_string( i + 1 ) +
						" does not parse as JSON: " + lerr;
					return false;
				}
				if( !j.isObject() || !j.has( "provider" ) || !j.get( "provider" ).isString() ||
				    !j.has( "body" ) || !j.get( "body" ).isString() ) {
					err = "replay fixture '" + path + "' line " + std::to_string( i + 1 ) +
						" must be a JSON object with string fields \"provider\" and \"body\"";
					return false;
				}
				const std::string lineProvider = j.get( "provider" ).asString();
				if( provider.empty() ) provider = lineProvider;
				else if( lineProvider != provider ) {
					err = "replay fixture '" + path + "' line " + std::to_string( i + 1 ) +
						" names provider '" + lineProvider + "' but the fixture's leading provider is '" +
						provider + "' -- a replay source is single-provider";
					return false;
				}
				bodies.push_back( j.get( "body" ).asString() );
			}
			if( provider.empty() ) {
				err = "replay fixture '" + path + "' has no fixture lines";
				return false;
			}
			out.mProvider = provider;
			out.mBodies = bodies;
			return true;
		}

		//====================================================================
		// 2. LoadEvalScenario.
		//====================================================================
		bool LoadEvalScenario( const std::string& path, AgentEvalScenario& out, std::string& err )
		{
			out = AgentEvalScenario();

			std::string text;
			if( !ReadWholeFile( path, text, err ) ) return false;

			JsonValue root;
			std::string perr;
			if( !JsonParse( text, root, perr ) ) {
				err = "scenario '" + path + "' does not parse as JSON: " + perr;
				return false;
			}
			if( !root.isObject() ) {
				err = "scenario '" + path + "' root must be a JSON object";
				return false;
			}

			if( !root.has( "id" ) || !root.get( "id" ).isString() || root.get( "id" ).asString().empty() ) {
				err = "scenario '" + path + "' missing required non-empty string field \"id\"";
				return false;
			}
			out.id = root.get( "id" ).asString();

			// Review-round P2: the id is concatenated into filesystem paths
			// (temp scene, trajectory, result) -- it MUST be a bare token so a
			// scenario can never write outside runDir.  Mirrors the read_skill
			// bare-name rule (AgentRpc.h: '/', '\\', ".." rejected).  The
			// scenario JSON is dev-committed today, but this closes the hole
			// before E4's live runner or any less-trusted scenario source.
			if( out.id.find( '/' ) != std::string::npos ||
			    out.id.find( '\\' ) != std::string::npos ||
			    out.id.find( ".." ) != std::string::npos ) {
				err = "scenario id '" + out.id + "' must be a BARE token -- no '/', '\\\\', or \"..\" "
				      "(it is used to build filesystem paths under the run dir)";
				return false;
			}

			if( !root.has( "title" ) || !root.get( "title" ).isString() ||
			    root.get( "title" ).asString().empty() ) {
				err = "scenario '" + out.id + "' missing required non-empty string field \"title\"";
				return false;
			}
			out.title = root.get( "title" ).asString();

			if( !root.has( "scene" ) || !root.get( "scene" ).isObject() ) {
				err = "scenario '" + out.id + "' missing required object field \"scene\"";
				return false;
			}
			{
				const JsonValue& sceneObj = root.get( "scene" );
				const bool hasPath = sceneObj.has( "path" ) && sceneObj.get( "path" ).isString() &&
					!sceneObj.get( "path" ).asString().empty();
				const bool hasInline = sceneObj.has( "inline" ) && sceneObj.get( "inline" ).isString() &&
					!sceneObj.get( "inline" ).asString().empty();
				if( hasPath == hasInline ) {
					err = "scenario '" + out.id + "': scene must specify EXACTLY ONE of non-empty \"path\" or \"inline\"";
					return false;
				}
				if( hasPath ) out.scenePath = sceneObj.get( "path" ).asString();
				else out.sceneInline = sceneObj.get( "inline" ).asString();
			}

			out.autonomy = "commit";
			if( root.has( "autonomy" ) ) {
				if( !root.get( "autonomy" ).isString() ) {
					err = "scenario '" + out.id + "': \"autonomy\" must be a string";
					return false;
				}
				out.autonomy = root.get( "autonomy" ).asString();
				if( out.autonomy != "commit" && out.autonomy != "propose" && out.autonomy != "read" ) {
					err = "scenario '" + out.id + "': \"autonomy\" must be \"commit\", \"propose\", or \"read\" (got '" +
						out.autonomy + "')";
					return false;
				}
			}

			if( !root.has( "prompts" ) || !root.get( "prompts" ).isArray() || root.get( "prompts" ).size() == 0 ) {
				err = "scenario '" + out.id + "' missing required non-empty array field \"prompts\"";
				return false;
			}
			{
				const JsonValue& prompts = root.get( "prompts" );
				for( std::size_t i = 0; i < prompts.size(); ++i ) {
					if( !prompts.at( i ).isString() ) {
						err = "scenario '" + out.id + "': prompts[" + std::to_string( i ) + "] must be a string";
						return false;
					}
					out.prompts.push_back( prompts.at( i ).asString() );
				}
			}

			out.budgets = AgentEvalBudgets();
			if( root.has( "budgets" ) ) {
				if( !root.get( "budgets" ).isObject() ) {
					err = "scenario '" + out.id + "': \"budgets\" must be an object";
					return false;
				}
				const JsonValue& b = root.get( "budgets" );
				if( b.has( "maxToolCalls" ) ) {
					if( !b.get( "maxToolCalls" ).isNumber() ) {
						err = "scenario '" + out.id + "': budgets.maxToolCalls must be a number";
						return false;
					}
					out.budgets.maxToolCalls = static_cast<int>( b.get( "maxToolCalls" ).asNumber() );
				}
				if( b.has( "maxLlmCalls" ) ) {
					if( !b.get( "maxLlmCalls" ).isNumber() ) {
						err = "scenario '" + out.id + "': budgets.maxLlmCalls must be a number";
						return false;
					}
					out.budgets.maxLlmCalls = static_cast<int>( b.get( "maxLlmCalls" ).asNumber() );
				}
				if( b.has( "maxWallMs" ) ) {
					if( !b.get( "maxWallMs" ).isNumber() ) {
						err = "scenario '" + out.id + "': budgets.maxWallMs must be a number";
						return false;
					}
					out.budgets.maxWallMs = static_cast<long long>( b.get( "maxWallMs" ).asNumber() );
				}
			}

			out.replayFixturePath.clear();
			if( root.has( "replay" ) ) {
				if( !root.get( "replay" ).isObject() ) {
					err = "scenario '" + out.id + "': \"replay\" must be an object";
					return false;
				}
				const JsonValue& rep = root.get( "replay" );
				if( rep.has( "fixture" ) ) {
					if( !rep.get( "fixture" ).isString() || rep.get( "fixture" ).asString().empty() ) {
						err = "scenario '" + out.id + "': replay.fixture must be a non-empty string";
						return false;
					}
					out.replayFixturePath = rep.get( "fixture" ).asString();
				}
			}

			out.checkpoints = JsonValue::MakeArray();
			if( root.has( "checkpoints" ) ) {
				if( !root.get( "checkpoints" ).isArray() ) {
					err = "scenario '" + out.id + "': \"checkpoints\" must be an array";
					return false;
				}
				const JsonValue& cps = root.get( "checkpoints" );
				for( std::size_t i = 0; i < cps.size(); ++i ) {
					if( !cps.at( i ).isObject() ) {
						err = "scenario '" + out.id + "': checkpoints[" + std::to_string( i ) + "] must be an object";
						return false;
					}
				}
				out.checkpoints = cps;   // carried OPAQUELY -- E3 interprets kinds
			}

			return true;
		}

		//====================================================================
		// 3. RunScenario.
		//====================================================================
		namespace
		{
			// Mirrors commandconsole.cpp's AuthorityForAutonomy: only the
			// Propose posture pairs with an External-authority session (a
			// staged, not committed, edit surface).
			AgentAuthority AuthorityForScenarioAutonomy( const std::string& autonomy )
			{
				return ( autonomy == "propose" ) ? AgentAuthority::External : AgentAuthority::Owner;
			}

			AgentAutonomy AutonomyForScenarioString( const std::string& autonomy )
			{
				if( autonomy == "read" ) return AgentAutonomy::Read;
				if( autonomy == "propose" ) return AgentAutonomy::Propose;
				return AgentAutonomy::Commit;   // "commit", or anything else (LoadEvalScenario already rejects other values)
			}

			bool ParseReplayProviderName( const std::string& name, ChatProvider& out )
			{
				if( name == "anthropic" ) { out = ChatProvider::Anthropic; return true; }
				if( name == "gemini" )    { out = ChatProvider::Gemini;    return true; }
				if( name == "openai" )    { out = ChatProvider::OpenAI;    return true; }
				return false;
			}
		}

		AgentEvalRunHandle RunScenario( const AgentEvalScenario& scenario, const AgentEvalRunOptions& options )
		{
			AgentEvalRunHandle handle;
			handle.result.scenarioId = scenario.id;

			if( options.runDir.empty() ) {
				handle.result.terminalStatus = "load_error";
				handle.result.errorMessage = "AgentEvalRunOptions.runDir is required";
				return handle;
			}

			// 1. The replay source (either the caller's override, or one
			//    loaded fresh from the scenario's named fixture).
			AgentEvalReplaySource ownedSource;
			AgentEvalReplaySource* source = options.replaySourceOverride;
			if( !source ) {
				if( scenario.replayFixturePath.empty() ) {
					handle.result.terminalStatus = "load_error";
					handle.result.errorMessage = "scenario '" + scenario.id +
						"' names no replay.fixture and no replaySourceOverride was supplied";
					return handle;
				}
				std::string err;
				if( !AgentEvalReplaySource::LoadFromFile( scenario.replayFixturePath, ownedSource, err ) ) {
					handle.result.terminalStatus = "load_error";
					handle.result.errorMessage = "replay fixture load failed: " + err;
					return handle;
				}
				source = &ownedSource;
			}
			ChatProvider provider;
			if( !ParseReplayProviderName( source->Provider(), provider ) ) {
				handle.result.terminalStatus = "load_error";
				handle.result.errorMessage =
					"replay source names an unrecognized provider '" + source->Provider() + "'";
				return handle;
			}

			// 2. The scene: path as-is, or inline -> a throwaway temp file
			//    under <runDir>/tmp (never scenes/), deleted immediately
			//    after the load attempt.
			std::string scenePathToLoad = scenario.scenePath;
			std::string tempScenePath;
			if( scenePathToLoad.empty() ) {
				if( scenario.sceneInline.empty() ) {
					handle.result.terminalStatus = "load_error";
					handle.result.errorMessage =
						"scenario '" + scenario.id + "' has neither scene.path nor scene.inline";
					return handle;
				}
				std::error_code ec;
				const std::filesystem::path tmpDir = std::filesystem::path( options.runDir ) / "tmp";
				std::filesystem::create_directories( tmpDir, ec );
				tempScenePath = ( tmpDir / ( scenario.id + ".inline.RISEscene" ) ).string();
				std::ofstream f( tempScenePath.c_str(), std::ios::binary );
				if( !f ) {
					handle.result.terminalStatus = "load_error";
					handle.result.errorMessage =
						"could not write the inline scene temp file '" + tempScenePath + "'";
					return handle;
				}
				f.write( scenario.sceneInline.data(), static_cast<std::streamsize>( scenario.sceneInline.size() ) );
				f.close();
				scenePathToLoad = tempScenePath;
			}

			std::unique_ptr<AgentSession> session =
				AgentSession::LoadFromFile( scenePathToLoad, AuthorityForScenarioAutonomy( scenario.autonomy ) );

			// Hygiene: clean up the throwaway inline-scene temp file
			// regardless of load outcome -- the Job has already parsed its
			// content by the time LoadFromFile returns.
			if( !tempScenePath.empty() ) std::remove( tempScenePath.c_str() );

			if( !session ) {
				handle.result.terminalStatus = "load_error";
				handle.result.errorMessage =
					"scene '" + scenePathToLoad + "' failed to load (not native-v7, or a derive error)";
				return handle;
			}

			const long long headVersionStart = static_cast<long long>( session->HeadVersion().revision );
			handle.result.headVersionStart = headVersionStart;
			handle.result.headVersionFinal = headVersionStart;

			std::unique_ptr<AgentRpcDispatcher> dispatcher(
				new AgentRpcDispatcher( std::move( session ), AutonomyForScenarioString( scenario.autonomy ) ) );

			// 3. The trajectory sink + the sans-IO loop.
			const std::string trajectoryPath = JoinPath( options.runDir, scenario.id + ".trajectory.jsonl" );

			AgentChatLoop loop;
			loop.SetProvider( provider );
			ChatTrajectoryConfig cfg;
			cfg.clock = options.clock;
			cfg.scenePath = scenario.scenePath.empty() ? std::string( "<inline>" ) : scenario.scenePath;
			cfg.sceneHeadVersion = headVersionStart;
			loop.SetTrajectorySink( MakeTrajectoryFileSink( trajectoryPath ), cfg );

			const std::function<int64_t()> clock = options.clock ? options.clock : &TrajectoryNowMs;
			const int64_t startMs = clock();

			int llmCalls = 0;
			int toolCalls = 0;
			int nextRpcId = 1;
			bool budgetHit = false;
			std::string terminalStatus;
			std::string errorMessage;
			std::string finalText;

			// 4. Drive the turns: BuildRequest -> replay's next body ->
			//    RecordHttpRound -> HandleResponse -> dispatch any tool
			//    calls via the LIVE dispatcher -> AddToolResult -> repeat,
			//    exactly like tests/AgentChatLoopTest.cpp drives it by hand.
			for( std::size_t pi = 0; pi < scenario.prompts.size() && terminalStatus.empty(); ++pi ) {
				loop.AddUserMessage( scenario.prompts[pi] );

				bool turnDone = false;
				while( !turnDone ) {
					if( scenario.budgets.maxWallMs >= 0 && ( clock() - startMs ) > scenario.budgets.maxWallMs ) {
						terminalStatus = "budget_wall_ms"; budgetHit = true; break;
					}
					if( scenario.budgets.maxLlmCalls >= 0 && llmCalls >= scenario.budgets.maxLlmCalls ) {
						terminalStatus = "budget_llm_calls"; budgetHit = true; break;
					}

					const ChatHttpRequest req = loop.BuildRequest( std::string() );   // keyless: replay needs no api key
					if( req.url.empty() ) {
						terminalStatus = "provider_error";
						errorMessage = "BuildRequest returned an empty request mid-scenario";
						break;
					}
					if( !source->HasNext() ) { terminalStatus = "replay_exhausted"; break; }

					std::string body;
					source->NextBody( body );
					loop.RecordHttpRound( 200, body, /*elapsedMs=*/0 );
					++llmCalls;
					ChatStepResult st = loop.HandleResponse( 200, body );

					if( st.kind == ChatStepResult::Kind::ToolCalls ) {
						for( std::size_t ci = 0; ci < st.toolCalls.size(); ++ci ) {
							if( scenario.budgets.maxToolCalls >= 0 && toolCalls >= scenario.budgets.maxToolCalls ) {
								terminalStatus = "budget_tool_calls"; budgetHit = true; break;
							}
							const ChatToolCall& call = st.toolCalls[ci];
							const std::string line = loop.ToolCallToJsonRpcLine( call, nextRpcId++ );
							const std::string resp = dispatcher->HandleLine( line );
							loop.AddToolResult( call, resp );
							++toolCalls;
						}
						if( !terminalStatus.empty() ) break;   // a budget tripped mid tool-batch -- honest stop
						// else: more rounds remain in this turn -- loop back for the next BuildRequest.
					}
					else if( st.kind == ChatStepResult::Kind::FinalText ) {
						finalText = st.finalText;
						turnDone = true;
					}
					else {   // ProviderError
						terminalStatus = "provider_error";
						errorMessage = st.errorMessage;
						break;
					}
				}
			}

			if( terminalStatus.empty() ) terminalStatus = "final_text";
			loop.FinishTrajectory( terminalStatus );

			handle.result.terminalStatus = terminalStatus;
			handle.result.llmCalls = llmCalls;
			handle.result.toolCalls = toolCalls;
			handle.result.budgetHit = budgetHit;
			handle.result.finalText = finalText;
			handle.result.errorMessage = errorMessage;
			handle.result.headVersionFinal = dispatcher->Session()
				? static_cast<long long>( dispatcher->Session()->HeadVersion().revision )
				: headVersionStart;
			handle.trajectoryPath = trajectoryPath;

			// 5. The one-line result summary.
			JsonValue r = JsonValue::MakeObject();
			r.set( "scenarioId", JsonValue::MakeString( handle.result.scenarioId ) );
			r.set( "terminalStatus", JsonValue::MakeString( handle.result.terminalStatus ) );
			r.set( "llmCalls", JsonValue::MakeNumber( static_cast<double>( handle.result.llmCalls ) ) );
			r.set( "toolCalls", JsonValue::MakeNumber( static_cast<double>( handle.result.toolCalls ) ) );
			r.set( "budgetHit", JsonValue::MakeBool( handle.result.budgetHit ) );
			r.set( "headVersionStart", JsonValue::MakeNumber( static_cast<double>( handle.result.headVersionStart ) ) );
			r.set( "headVersionFinal", JsonValue::MakeNumber( static_cast<double>( handle.result.headVersionFinal ) ) );
			r.set( "finalText", JsonValue::MakeString( handle.result.finalText ) );
			if( !handle.result.errorMessage.empty() )
				r.set( "errorMessage", JsonValue::MakeString( handle.result.errorMessage ) );

			const std::string resultPath = JoinPath( options.runDir, scenario.id + ".result.jsonl" );
			{
				std::error_code ec;
				std::filesystem::create_directories( std::filesystem::path( options.runDir ), ec );
				std::ofstream rf( resultPath.c_str(), std::ios::binary );
				if( rf ) rf << JsonSerialize( r ) << "\n";
			}
			handle.resultPath = resultPath;

			handle.dispatcher = std::move( dispatcher );
			return handle;
		}
	}
}
