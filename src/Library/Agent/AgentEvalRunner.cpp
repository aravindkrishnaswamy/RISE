//////////////////////////////////////////////////////////////////////
//
//  AgentEvalRunner.cpp - the replay source + scenario loader + scenario
//    runner (see AgentEvalRunner.h).
//
//  NO ENVIRONMENT-VARIABLE / CREDENTIAL READS ANYWHERE IN THIS FILE --
//  the replay source is keyless by construction (BuildRequest is called
//  with an empty api key), and nothing here ever calls getenv.  The E4
//  LIVE path (RunScenarioLive / RunEvalMatrix) receives the api key as a
//  PARAMETER: RunScenarioLive takes it via AgentEvalLiveRunOptions.apiKey
//  and forwards it to BuildRequest (auth header ONLY); RunEvalMatrix
//  resolves it through an INJECTED envLookup callback (the CLI passes a
//  getenv adapter -- getenv stays in commandconsole.cpp, out of this TU).
//  The key is never logged, never written to any trajectory/result file,
//  and never handed to the matrix's `log` sink.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "AgentEvalRunner.h"

#include "AgentSession.h"
#include "AgentRpc.h"
#include "AgentDiagnostic.h"
#include "../Cst/Cst.h"   // Eval-harness slice E3: the "document"/"untouched" checkpoint kinds walk the CST directly

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

			//! One round's outcome from a body source (replay OR live
			//! transport).  `proceed` false STOPS the whole scenario with
			//! `stopStatus` (and `stopError` for a transport_error).  On
			//! `proceed` true the {status, body, elapsedMs} feed
			//! RecordHttpRound + HandleResponse.  This is the ONLY thing
			//! that differs between the replay and live drive paths -- the
			//! rest of the loop (RunScenarioDriven, below) is byte-identical.
			struct FetchOutcome
			{
				bool        proceed = false;
				std::string stopStatus;   //!< terminalStatus to set when !proceed (e.g. "replay_exhausted", "transport_error")
				std::string stopError;    //!< errorMessage to set when !proceed (transport_error carries the header-free category)
				long        status   = 0;
				std::string body;
				int64_t     elapsedMs = 0;
			};

			using FetchFn = std::function<FetchOutcome( const ChatHttpRequest& )>;

			//! The SHARED scenario drive loop for both the replay
			//! (RunScenario) and live (RunScenarioLive) paths.  Everything
			//! below the body source -- scene load, session/dispatcher
			//! construction, the trajectory sink, the turn/tool loop, budget
			//! enforcement, and the trajectory + result file writes -- is
			//! identical; the two callers differ ONLY in `provider`/`apiKey`
			//! and the `fetch` callback (canned bodies vs. a real POST).
			//! Never throws.
			AgentEvalRunHandle RunScenarioDriven(
				const AgentEvalScenario&        scenario,
				const std::string&              runDir,
				const std::function<int64_t()>& optClock,
				ChatProvider                    provider,
				const std::string&              modelId,
				const std::string&              apiKey,
				const FetchFn&                  fetch )
			{
				AgentEvalRunHandle handle;
				handle.result.scenarioId = scenario.id;

				// 1. The scene: path as-is, or inline -> a throwaway temp file
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
					const std::filesystem::path tmpDir = std::filesystem::path( runDir ) / "tmp";
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

				// Eval-harness slice E3 (the "untouched" checkpoint seam): capture
				// the head's canonical text AS LOADED, before the first turn runs.
				handle.initialDocument = session->ReadDocument();

				std::unique_ptr<AgentRpcDispatcher> dispatcher(
					new AgentRpcDispatcher( std::move( session ), AutonomyForScenarioString( scenario.autonomy ) ) );

				// 2. The trajectory sink + the sans-IO loop.
				const std::string trajectoryPath = JoinPath( runDir, scenario.id + ".trajectory.jsonl" );

				AgentChatLoop loop;
				loop.SetProvider( provider, modelId );
				ChatTrajectoryConfig cfg;
				cfg.clock = optClock;
				cfg.scenePath = scenario.scenePath.empty() ? std::string( "<inline>" ) : scenario.scenePath;
				cfg.sceneHeadVersion = headVersionStart;
				loop.SetTrajectorySink( MakeTrajectoryFileSink( trajectoryPath ), cfg );

				const std::function<int64_t()> clock = optClock ? optClock : &TrajectoryNowMs;
				const int64_t startMs = clock();

				int llmCalls = 0;
				int toolCalls = 0;
				int nextRpcId = 1;
				bool budgetHit = false;
				std::string terminalStatus;
				std::string errorMessage;
				std::string finalText;

				// 3. Drive the turns: BuildRequest -> fetch (replay's next
				//    body OR transport.Post) -> RecordHttpRound ->
				//    HandleResponse -> dispatch any tool calls via the LIVE
				//    dispatcher -> AddToolResult -> repeat, exactly like
				//    tests/AgentChatLoopTest.cpp drives it by hand.
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

						const ChatHttpRequest req = loop.BuildRequest( apiKey );
						if( req.url.empty() ) {
							terminalStatus = "provider_error";
							errorMessage = "BuildRequest returned an empty request mid-scenario";
							break;
						}

						const FetchOutcome fo = fetch( req );
						if( !fo.proceed ) {
							terminalStatus = fo.stopStatus;
							errorMessage = fo.stopError;
							break;
						}

						loop.RecordHttpRound( fo.status, fo.body, fo.elapsedMs );
						++llmCalls;
						ChatStepResult st = loop.HandleResponse( fo.status, fo.body );

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

				// 4. The one-line result summary.
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

				const std::string resultPath = JoinPath( runDir, scenario.id + ".result.jsonl" );
				{
					std::error_code ec;
					std::filesystem::create_directories( std::filesystem::path( runDir ), ec );
					std::ofstream rf( resultPath.c_str(), std::ios::binary );
					if( rf ) rf << JsonSerialize( r ) << "\n";
				}
				handle.resultPath = resultPath;

				handle.dispatcher = std::move( dispatcher );
				return handle;
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

			// The replay source (either the caller's override, or one loaded
			// fresh from the scenario's named fixture) + its provider.
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

			// The replay fetch: hand out the next canned body (status 200,
			// elapsed 0), or STOP with "replay_exhausted" when the source
			// runs dry.  Keyless -- BuildRequest is called with an empty api
			// key (see the apiKey argument below).
			const FetchFn replayFetch = [source]( const ChatHttpRequest& ) -> FetchOutcome {
				FetchOutcome fo;
				if( !source->HasNext() ) { fo.proceed = false; fo.stopStatus = "replay_exhausted"; return fo; }
				std::string body;
				source->NextBody( body );
				fo.proceed = true; fo.status = 200; fo.body = body; fo.elapsedMs = 0;
				return fo;
			};

			return RunScenarioDriven( scenario, options.runDir, options.clock,
			                          provider, /*modelId=*/std::string(),
			                          /*apiKey=*/std::string(), replayFetch );
		}

		//====================================================================
		// 3b. RunScenarioLive (Eval-harness slice E4).
		//====================================================================
		AgentEvalRunHandle RunScenarioLive( const AgentEvalScenario& scenario,
		                                    const AgentEvalLiveRunOptions& options )
		{
			AgentEvalRunHandle handle;
			handle.result.scenarioId = scenario.id;

			if( options.runDir.empty() ) {
				handle.result.terminalStatus = "load_error";
				handle.result.errorMessage = "AgentEvalLiveRunOptions.runDir is required";
				return handle;
			}
			if( !options.transport ) {
				handle.result.terminalStatus = "load_error";
				handle.result.errorMessage = "AgentEvalLiveRunOptions.transport is required (null)";
				return handle;
			}

			// The live fetch: perform the POST through the transport.  A
			// status <= 0 means NO HTTP response reached us (DNS/connect/TLS/
			// timeout, or the Linux unsupported stub) -- STOP the run with
			// "transport_error" carrying the transport's HEADER-FREE error
			// category.  A status > 0 (any of 2xx..5xx) is a real response
			// the loop/codec interprets (a 4xx becomes a provider_error
			// downstream), NOT a transport failure.
			IChatHttpTransport* transport = options.transport;
			const FetchFn liveFetch = [transport]( const ChatHttpRequest& req ) -> FetchOutcome {
				FetchOutcome fo;
				const ChatHttpResponse resp = transport->Post( req );
				if( resp.status <= 0 ) {
					fo.proceed = false;
					fo.stopStatus = "transport_error";
					fo.stopError = resp.error.empty()
						? std::string( "transport failure (no HTTP status, no error detail)" )
						: resp.error;
					return fo;
				}
				fo.proceed = true;
				fo.status = resp.status;
				fo.body = resp.body;
				fo.elapsedMs = resp.elapsedMs;
				return fo;
			};

			return RunScenarioDriven( scenario, options.runDir, options.clock,
			                          options.provider, options.modelId,
			                          options.apiKey, liveFetch );
		}

		//====================================================================
		// 3c. LoadEvalRunConfig + RunEvalMatrix (Eval-harness slice E4).
		//====================================================================
		bool LoadEvalRunConfig( const std::string& path, AgentEvalRunConfig& out, std::string& err )
		{
			out = AgentEvalRunConfig();

			std::string text;
			if( !ReadWholeFile( path, text, err ) ) return false;

			JsonValue root;
			std::string perr;
			if( !JsonParse( text, root, perr ) ) {
				err = "run config '" + path + "' does not parse as JSON: " + perr;
				return false;
			}
			if( !root.isObject() ) {
				err = "run config '" + path + "' root must be a JSON object";
				return false;
			}

			// scenarios: required non-empty array of non-empty strings (each a
			// path OR a glob -- the CLI expands globs; the loader validates
			// only the shape).
			if( !root.has( "scenarios" ) || !root.get( "scenarios" ).isArray() ||
			    root.get( "scenarios" ).size() == 0 ) {
				err = "run config '" + path + "' missing required non-empty array field \"scenarios\"";
				return false;
			}
			{
				const JsonValue& a = root.get( "scenarios" );
				for( std::size_t i = 0; i < a.size(); ++i ) {
					if( !a.at( i ).isString() || a.at( i ).asString().empty() ) {
						err = "run config '" + path + "': scenarios[" + std::to_string( i ) +
							"] must be a non-empty string";
						return false;
					}
					out.scenarios.push_back( a.at( i ).asString() );
				}
			}

			// providers: required non-empty array of {provider, keyEnvVar,
			// model?} objects.
			if( !root.has( "providers" ) || !root.get( "providers" ).isArray() ||
			    root.get( "providers" ).size() == 0 ) {
				err = "run config '" + path + "' missing required non-empty array field \"providers\"";
				return false;
			}
			{
				const JsonValue& a = root.get( "providers" );
				for( std::size_t i = 0; i < a.size(); ++i ) {
					const std::string idx = std::to_string( i );
					const JsonValue& p = a.at( i );
					if( !p.isObject() ) {
						err = "run config '" + path + "': providers[" + idx + "] must be an object";
						return false;
					}
					AgentEvalProviderConfig pc;
					if( !p.has( "provider" ) || !p.get( "provider" ).isString() ||
					    p.get( "provider" ).asString().empty() ) {
						err = "run config '" + path + "': providers[" + idx +
							"].provider must be a non-empty string";
						return false;
					}
					pc.provider = p.get( "provider" ).asString();
					ChatProvider dummy;
					if( !ParseReplayProviderName( pc.provider, dummy ) ) {
						err = "run config '" + path + "': providers[" + idx + "].provider '" + pc.provider +
							"' is not a known provider (want \"anthropic\", \"gemini\", or \"openai\")";
						return false;
					}
					if( !p.has( "keyEnvVar" ) || !p.get( "keyEnvVar" ).isString() ||
					    p.get( "keyEnvVar" ).asString().empty() ) {
						err = "run config '" + path + "': providers[" + idx +
							"].keyEnvVar must be a non-empty string (the ENV VAR NAME holding the api key, never the key)";
						return false;
					}
					pc.keyEnvVar = p.get( "keyEnvVar" ).asString();
					if( p.has( "model" ) ) {
						if( !p.get( "model" ).isString() ) {
							err = "run config '" + path + "': providers[" + idx + "].model must be a string";
							return false;
						}
						pc.model = p.get( "model" ).asString();
					}
					out.providers.push_back( pc );
				}
			}

			// repeats: optional number >= 1 (default 3).
			out.repeats = 3;
			if( root.has( "repeats" ) ) {
				if( !root.get( "repeats" ).isNumber() ) {
					err = "run config '" + path + "': \"repeats\" must be a number";
					return false;
				}
				const int rep = static_cast<int>( root.get( "repeats" ).asNumber() );
				if( rep < 1 ) {
					err = "run config '" + path + "': \"repeats\" must be >= 1 (got " + std::to_string( rep ) + ")";
					return false;
				}
				out.repeats = rep;
			}

			// runDir: required non-empty string.
			if( !root.has( "runDir" ) || !root.get( "runDir" ).isString() ||
			    root.get( "runDir" ).asString().empty() ) {
				err = "run config '" + path + "' missing required non-empty string field \"runDir\"";
				return false;
			}
			out.runDir = root.get( "runDir" ).asString();

			return true;
		}

		namespace
		{
			//! Map an arbitrary string (scenario id / provider / model) to a
			//! filesystem-safe token for the per-run subdirectory name.
			std::string SanitizeForPath( const std::string& s )
			{
				std::string o;
				o.reserve( s.size() );
				for( std::size_t i = 0; i < s.size(); ++i ) {
					const unsigned char u = static_cast<unsigned char>( s[i] );
					if( std::isalnum( u ) || s[i] == '-' || s[i] == '.' || s[i] == '_' ) o += s[i];
					else o += '_';
				}
				if( o.empty() ) o = "x";
				return o;
			}
		}

		AgentEvalMatrixResult RunEvalMatrix( const AgentEvalRunConfig& config,
		                                     const std::vector<AgentEvalScenario>& scenarios,
		                                     const AgentEvalMatrixOptions& options )
		{
			AgentEvalMatrixResult result;
			auto logLine = [&]( const std::string& m ) { if( options.log ) options.log( m ); };

			if( !options.transport ) {
				logLine( "RunEvalMatrix: no transport supplied -- nothing run" );
				return result;
			}
			if( !options.envLookup ) {
				logLine( "RunEvalMatrix: no envLookup supplied -- nothing run" );
				return result;
			}

			const int reps = config.repeats > 0 ? config.repeats : 0;

			for( std::size_t pj = 0; pj < config.providers.size(); ++pj ) {
				const AgentEvalProviderConfig& prov = config.providers[pj];

				// The ONE place a key enters the matrix: resolve it from the
				// injected env lookup.  A missing/empty key SKIPS the whole
				// provider column (never a crash) -- logged, never with the key.
				const char* keyC = options.envLookup( prov.keyEnvVar );
				const std::string key = keyC ? std::string( keyC ) : std::string();
				if( key.empty() ) {
					++result.providersSkipped;
					result.runsSkipped += static_cast<int>( scenarios.size() ) * reps;
					logLine( "RunEvalMatrix: SKIP provider '" + prov.provider + "' -- env var " +
						prov.keyEnvVar + " is unset/empty (no key -> no live run for this provider)" );
					continue;
				}

				ChatProvider providerEnum;
				if( !ParseReplayProviderName( prov.provider, providerEnum ) ) {
					++result.providersSkipped;
					logLine( "RunEvalMatrix: SKIP provider '" + prov.provider + "' -- unknown provider name" );
					continue;
				}
				++result.providersUsed;

				for( std::size_t si = 0; si < scenarios.size(); ++si ) {
					const AgentEvalScenario& scenario = scenarios[si];
					for( int rep = 1; rep <= reps; ++rep ) {
						std::string leaf = SanitizeForPath( scenario.id ) + "__" + SanitizeForPath( prov.provider );
						if( !prov.model.empty() ) leaf += "__" + SanitizeForPath( prov.model );
						leaf += "__r" + std::to_string( rep );
						const std::string runDir = ( std::filesystem::path( config.runDir ) / leaf ).string();

						logLine( "RunEvalMatrix: RUN " + leaf );

						AgentEvalLiveRunOptions lo;
						lo.runDir   = runDir;
						lo.transport = options.transport;
						lo.provider = providerEnum;
						lo.modelId  = prov.model;
						lo.apiKey   = key;   // key stays in this local + BuildRequest; never logged/persisted
						lo.clock    = options.clock;

						AgentEvalRunHandle h = RunScenarioLive( scenario, lo );
						CheckScenario( h, scenario );
						++result.runsExecuted;
					}
				}
			}

			return result;
		}

			//====================================================================
			// 4. The checker engine (Eval-harness slice E3).
			//====================================================================
			namespace
			{
				using RISE::Cst::Document;
				using RISE::Cst::NodeRef;
				using RISE::Cst::NodeKind;

				//! The pass/fail + explanation a single checkpoint-kind handler
				//! returns; CheckOneCheckpoint folds this into an
				//! AgentEvalCheckpointResult (adding kind/weight).
				struct CheckOutcome { bool passed; std::string detail; };

				//----------------------------------------------------------
				// CST helpers.  AgentSession.cpp has near-identical local
				// helpers (SerializeNode et al.) but they are `static` to
				// that TU -- duplicated here rather than exported, since
				// they are each ~5 lines and exporting them would widen
				// Cst.h's surface for a checker-only need.
				//----------------------------------------------------------

				//! Serialize a green node's bytes (leaves carry text; internal
				//! nodes are the concatenation of their kids) -- the same
				//! lossless-CST contract used throughout src/Library/Cst.
				void CheckerSerializeNode( const NodeRef& n, std::string& out )
				{
					if( !n ) return;
					if( n->kids.empty() ) out += n->text;
					else for( const auto& k : n->kids ) CheckerSerializeNode( k, out );
				}

				std::string CheckerNodeBytes( const NodeRef& n )
				{
					std::string s;
					CheckerSerializeNode( n, s );
					return s;
				}

				//! A chunk's bare `name` param value ("" if unnamed / not found).
				std::string CheckerChunkName( const NodeRef& chunk )
				{
					if( !chunk ) return std::string();
					for( const auto& kid : chunk->kids ) {
						if( kid->kind != NodeKind::Param ) continue;
						std::string pname;
						std::vector<std::string> values;
						for( const auto& tk : kid->kids ) {
							if( tk->kind != NodeKind::Token ) continue;
							if( tk->role == "pname" ) pname = tk->text;
							else if( tk->role == "pvalue" ) values.push_back( tk->text );
						}
						if( pname == "name" && !values.empty() ) return values[0];
					}
					return std::string();
				}

				//! A chunk's `paramName` value, space-joined across every
				//! pvalue token (e.g. a vec3 "0.9 0.1 0.1").  Returns false
				//! when the chunk carries no such param at all.
				bool CheckerChunkParamValue( const NodeRef& chunk, const std::string& paramName, std::string& outValue )
				{
					if( !chunk ) return false;
					for( const auto& kid : chunk->kids ) {
						if( kid->kind != NodeKind::Param ) continue;
						std::string pname;
						std::vector<std::string> values;
						for( const auto& tk : kid->kids ) {
							if( tk->kind != NodeKind::Token ) continue;
							if( tk->role == "pname" ) pname = tk->text;
							else if( tk->role == "pvalue" ) values.push_back( tk->text );
						}
						if( pname != paramName ) continue;
						std::string joined;
						for( std::size_t i = 0; i < values.size(); ++i ) { if( i ) joined += ' '; joined += values[i]; }
						outValue = joined;
						return true;
					}
					return false;
				}

				//! Find a top-level chunk by optional kind (exact keyword
				//! match; "" = no narrowing) + bare name.  Returns 0 (not
				//! found), 1 (found -- fills outChunk), or -1 (AMBIGUOUS:
				//! more than one chunk matches -- a caller must refuse rather
				//! than guess which one the scenario author meant).
				int CheckerFindChunk( const Document& doc, const std::string& kindOrEmpty, const std::string& name,
				                      NodeRef& outChunk )
				{
					const int n = RISE::Cst::DocItemCount( doc );
					int matches = 0;
					for( int i = 0; i < n; ++i ) {
						const RISE::Cst::NodeId id = RISE::Cst::DocNodeIdAt( doc, i );
						if( !id ) continue;
						NodeRef item = RISE::Cst::DocResolveNodeId( doc, id );
						if( !item || item->kind != NodeKind::Chunk ) continue;
						if( !kindOrEmpty.empty() && item->role != kindOrEmpty ) continue;
						if( CheckerChunkName( item ) != name ) continue;
						++matches;
						outChunk = item;
					}
					if( matches == 0 ) return 0;
					if( matches > 1 ) return -1;
					return 1;
				}

				//! The OPTIONAL chunk-kind NARROWING field of a document/untouched
				//! checkpoint (exact chunk-keyword match; "" = no narrowing).  It is
				//! deliberately NOT named "kind": that name is already the top-level
				//! checkpoint DISCRIMINATOR ("document"/"untouched"/...), and the JSON
				//! codec is last-wins on duplicate keys, so a checkpoint carrying both
				//! a "kind" discriminator and a "kind" narrowing would read the
				//! narrowing value as the discriminator (dispatch then fails with
				//! "unknown checkpoint kind").  The narrowing field is "chunkKind".
				std::string OptKind( const JsonValue& cp )
				{
					return ( cp.has( "chunkKind" ) && cp.get( "chunkKind" ).isString() ) ? cp.get( "chunkKind" ).asString() : std::string();
				}

				//----------------------------------------------------------
				// Per-checkpoint-kind handlers.
				//----------------------------------------------------------

				//! "document": {op:"param_equals",target,param,value,chunkKind?} |
				//! {op:"chunk_exists",chunkKind?,name} | {op:"chunk_absent",chunkKind?,name}
				//! -- asserted against the POST-run document
				//! (session->ReadDocument(), reparsed via ParseToCst -- the
				//! same idiom AgentChunkCrudTest/CstFirstSliceTest use to
				//! inspect a document's chunks after an edit).
				CheckOutcome CheckDocumentKind( const JsonValue& cp, AgentSession* session )
				{
					if( !session ) return { false, "document checkpoint: no live session (run did not complete)" };
					if( !cp.has( "op" ) || !cp.get( "op" ).isString() )
						return { false, "document checkpoint missing string field \"op\"" };
					const std::string op = cp.get( "op" ).asString();
					const Document doc = RISE::Cst::ParseToCst( session->ReadDocument() );

					if( op == "param_equals" ) {
						if( !cp.has( "target" ) || !cp.get( "target" ).isString() )
							return { false, "param_equals missing string \"target\"" };
						if( !cp.has( "param" ) || !cp.get( "param" ).isString() )
							return { false, "param_equals missing string \"param\"" };
						if( !cp.has( "value" ) || !cp.get( "value" ).isString() )
							return { false, "param_equals missing string \"value\"" };
						const std::string target = cp.get( "target" ).asString();
						const std::string param  = cp.get( "param" ).asString();
						const std::string expected = cp.get( "value" ).asString();
						const std::string kind = OptKind( cp );

						NodeRef chunk;
						const int found = CheckerFindChunk( doc, kind, target, chunk );
						if( found == 0 )
							return { false, "param_equals: no chunk named '" + target + "' found" +
								( kind.empty() ? std::string() : ( " (kind '" + kind + "')" ) ) };
						if( found < 0 )
							return { false, "param_equals: AMBIGUOUS -- more than one chunk named '" + target +
								"' -- narrow with \"kind\"" };
						std::string actual;
						if( !CheckerChunkParamValue( chunk, param, actual ) )
							return { false, "param_equals: chunk '" + target + "' has no param '" + param + "'" };
						if( actual != expected )
							return { false, "param_equals: '" + target + "." + param + "' == '" + actual +
								"', expected '" + expected + "'" };
						return { true, "param_equals: '" + target + "." + param + "' == '" + expected + "'" };
					}

					if( op == "chunk_exists" || op == "chunk_absent" ) {
						if( !cp.has( "name" ) || !cp.get( "name" ).isString() )
							return { false, op + " missing string \"name\"" };
						const std::string name = cp.get( "name" ).asString();
						const std::string kind = OptKind( cp );
						NodeRef chunk;
						const int found = CheckerFindChunk( doc, kind, name, chunk );
						if( found < 0 )
							return { false, op + ": AMBIGUOUS -- more than one chunk named '" + name +
								"' -- narrow with \"kind\"" };
						const bool exists = ( found == 1 );
						if( op == "chunk_exists" )
							return { exists, exists ? ( "chunk_exists: '" + name + "' found" )
							                          : ( "chunk_exists: '" + name + "' NOT found" ) };
						return { !exists, !exists ? ( "chunk_absent: '" + name + "' correctly absent" )
						                            : ( "chunk_absent: '" + name + "' unexpectedly PRESENT" ) };
					}

					return { false, "document checkpoint: unknown op '" + op + "'" };
				}

				//! "untouched": {chunks:[{chunkKind?,name},...]} -- the PASS_TO_PASS
				//! guard: every listed chunk must be byte-identical in
				//! handle.initialDocument (the scene AS LOADED, before the
				//! agent's first turn) vs. the POST-run document.
				CheckOutcome CheckUntouchedKind( const JsonValue& cp, const AgentEvalRunHandle& handle )
				{
					if( !handle.dispatcher || !handle.dispatcher->Session() )
						return { false, "untouched checkpoint: no live session (run did not complete: " +
							handle.result.terminalStatus + ")" };
					if( !cp.has( "chunks" ) || !cp.get( "chunks" ).isArray() || cp.get( "chunks" ).size() == 0 )
						return { false, "untouched checkpoint missing non-empty array field \"chunks\"" };

					const Document initialDoc = RISE::Cst::ParseToCst( handle.initialDocument );
					const Document finalDoc = RISE::Cst::ParseToCst( handle.dispatcher->Session()->ReadDocument() );

					const JsonValue& chunksArr = cp.get( "chunks" );
					std::vector<std::string> problems;
					for( std::size_t i = 0; i < chunksArr.size(); ++i ) {
						const JsonValue& c = chunksArr.at( i );
						if( !c.isObject() || !c.has( "name" ) || !c.get( "name" ).isString() ) {
							problems.push_back( "chunks[" + std::to_string( i ) + "] must be an object with a string \"name\"" );
							continue;
						}
						const std::string name = c.get( "name" ).asString();
						const std::string kind = OptKind( c );

						NodeRef initChunk, finalChunk;
						const int foundInit  = CheckerFindChunk( initialDoc, kind, name, initChunk );
						const int foundFinal = CheckerFindChunk( finalDoc, kind, name, finalChunk );
						if( foundInit != 1 ) { problems.push_back( "'" + name + "' not uniquely found in the INITIAL scene" ); continue; }
						if( foundFinal != 1 ) { problems.push_back( "'" + name + "' not uniquely found in the FINAL scene" ); continue; }
						if( CheckerNodeBytes( initChunk ) != CheckerNodeBytes( finalChunk ) )
							problems.push_back( "'" + name + "' CHANGED (not byte-identical to the initial scene)" );
					}
					if( !problems.empty() ) {
						std::string detail = "untouched violated: ";
						for( std::size_t i = 0; i < problems.size(); ++i ) { if( i ) detail += "; "; detail += problems[i]; }
						return { false, detail };
					}
					return { true, "all " + std::to_string( chunksArr.size() ) + " named chunk(s) byte-identical to the initial scene" };
				}

				//! "render": {width?,height?,seed?,meanLumaMin?/Max?,meanRMin?/Max?,
				//! meanGMin?/Max?,meanBMin?/Max?} -- a FRESH render through the
				//! live session (AgentSession::Render, bypassing the JSON-RPC
				//! layer -- the checker is a privileged verifier), asserted
				//! against generous [min,max] bands.  `seed` is accepted (never
				//! rejected -- an author may want to note the seed a fixture
				//! was authored against) but has NO EFFECT: AgentSession::Render
				//! exposes no seed/RNG-pinning control (see AgentRenderParams --
				//! samples/width/height/camera/pinned/quality/mode only), so
				//! per the plan's tolerance-banded rule, bands MUST be wide
				//! enough to absorb ordinary MC noise between runs, not narrow
				//! enough to require a pinned seed.  NEVER an exact-pixel match.
				CheckOutcome CheckRenderKind( const JsonValue& cp, AgentSession* session )
				{
					if( !session ) return { false, "render checkpoint: no live session (run did not complete)" };

					AgentRenderParams rp;
					const bool haveW = cp.has( "width" )  && cp.get( "width" ).isNumber();
					const bool haveH = cp.has( "height" ) && cp.get( "height" ).isNumber();
					if( haveW && haveH ) {
						rp.width  = static_cast<unsigned int>( cp.get( "width" ).asNumber() );
						rp.height = static_cast<unsigned int>( cp.get( "height" ).asNumber() );
					} else if( haveW != haveH ) {
						return { false, "render checkpoint: \"width\"/\"height\" must both be supplied together" };
					}

					const AgentRenderResult rr = session->Render( rp );
					if( !rr.ok ) return { false, "render checkpoint: render failed: " + rr.message };

					// Rec.709 linear luma weights (RISEPel is Rec709RGBPel post
					// the 2026-05-24 colour-space migration) -- a single scalar
					// band that absorbs per-channel MC noise better than
					// checking each channel independently when the author only
					// cares about overall brightness.
					const double meanLuma = 0.2126 * rr.meanR + 0.7152 * rr.meanG + 0.0722 * rr.meanB;

					std::vector<std::string> failures;
					auto checkBand = [&]( const char* minKey, const char* maxKey, const char* label, double actual ) {
						if( cp.has( minKey ) && cp.get( minKey ).isNumber() ) {
							const double lo = cp.get( minKey ).asNumber();
							if( actual < lo )
								failures.push_back( std::string( label ) + "=" + std::to_string( actual ) + " < min " + std::to_string( lo ) );
						}
						if( cp.has( maxKey ) && cp.get( maxKey ).isNumber() ) {
							const double hi = cp.get( maxKey ).asNumber();
							if( actual > hi )
								failures.push_back( std::string( label ) + "=" + std::to_string( actual ) + " > max " + std::to_string( hi ) );
						}
					};
					checkBand( "meanLumaMin", "meanLumaMax", "meanLuma", meanLuma );
					checkBand( "meanRMin", "meanRMax", "meanR", rr.meanR );
					checkBand( "meanGMin", "meanGMax", "meanG", rr.meanG );
					checkBand( "meanBMin", "meanBMax", "meanB", rr.meanB );

					if( !failures.empty() ) {
						std::string detail = "render band(s) violated: ";
						for( std::size_t i = 0; i < failures.size(); ++i ) { if( i ) detail += "; "; detail += failures[i]; }
						return { false, detail };
					}
					char buf[256];
					std::snprintf( buf, sizeof( buf ), "render bands satisfied (meanR=%.4f meanG=%.4f meanB=%.4f meanLuma=%.4f)",
						rr.meanR, rr.meanG, rr.meanB, meanLuma );
					return { true, std::string( buf ) };
				}

				//! "objectmap": {legendContains?,pixelCountFor?,pixelCountMin?,
				//! pixelCountMax?,queryAt?:{x,y,expectName}} -- legendContains/
				//! pixelCountFor run ONE mode:"objectmap" render (only if
				//! needed); queryAt runs AgentSession::QueryObjectAt (which does
				//! its own internal objectmap render regardless).  At least one
				//! of the three assertions must be present.
				CheckOutcome CheckObjectmapKind( const JsonValue& cp, AgentSession* session )
				{
					if( !session ) return { false, "objectmap checkpoint: no live session (run did not complete)" };
					if( !cp.has( "legendContains" ) && !cp.has( "pixelCountFor" ) && !cp.has( "queryAt" ) )
						return { false, "objectmap checkpoint carries none of legendContains/pixelCountFor/queryAt" };

					std::vector<std::string> failures;
					const bool needsLegend = cp.has( "legendContains" ) || cp.has( "pixelCountFor" );
					AgentRenderResult rr;
					if( needsLegend ) {
						AgentRenderParams rp;
						rp.renderTarget = AgentRenderTarget::ObjectMap;
						rr = session->Render( rp );
						if( !rr.ok ) return { false, "objectmap checkpoint: render failed: " + rr.message };
					}

					if( cp.has( "legendContains" ) && cp.get( "legendContains" ).isString() ) {
						const std::string want = cp.get( "legendContains" ).asString();
						bool found = false;
						for( const auto& e : rr.legend ) if( e.name == want ) { found = true; break; }
						if( !found ) failures.push_back( "legend does not contain '" + want + "'" );
					}

					if( cp.has( "pixelCountFor" ) && cp.get( "pixelCountFor" ).isString() ) {
						const std::string name = cp.get( "pixelCountFor" ).asString();
						const LegendEntry* entry = nullptr;
						for( const auto& e : rr.legend ) if( e.name == name ) { entry = &e; break; }
						if( !entry ) {
							failures.push_back( "pixelCountFor: '" + name + "' not in legend" );
						} else {
							const double count = static_cast<double>( entry->pixelCount );
							if( cp.has( "pixelCountMin" ) && cp.get( "pixelCountMin" ).isNumber() && count < cp.get( "pixelCountMin" ).asNumber() )
								failures.push_back( "'" + name + "' pixelCount " + std::to_string( entry->pixelCount ) +
									" < min " + std::to_string( cp.get( "pixelCountMin" ).asNumber() ) );
							if( cp.has( "pixelCountMax" ) && cp.get( "pixelCountMax" ).isNumber() && count > cp.get( "pixelCountMax" ).asNumber() )
								failures.push_back( "'" + name + "' pixelCount " + std::to_string( entry->pixelCount ) +
									" > max " + std::to_string( cp.get( "pixelCountMax" ).asNumber() ) );
						}
					}

					if( cp.has( "queryAt" ) && cp.get( "queryAt" ).isObject() ) {
						const JsonValue& qa = cp.get( "queryAt" );
						if( !qa.has( "x" ) || !qa.get( "x" ).isNumber() || !qa.has( "y" ) || !qa.get( "y" ).isNumber() ) {
							failures.push_back( "queryAt requires numeric \"x\",\"y\"" );
						} else if( !qa.has( "expectName" ) || !qa.get( "expectName" ).isString() ) {
							failures.push_back( "queryAt requires string \"expectName\" (\"\" means expect a miss)" );
						} else {
							const int x = static_cast<int>( qa.get( "x" ).asNumber() );
							const int y = static_cast<int>( qa.get( "y" ).asNumber() );
							const std::string expectName = qa.get( "expectName" ).asString();
							const AgentSession::AgentQueryObjectResult qr = session->QueryObjectAt( x, y );
							if( qr.outOfRange ) {
								failures.push_back( "queryAt(" + std::to_string( x ) + "," + std::to_string( y ) + ") out of range" );
							} else if( expectName.empty() ) {
								if( qr.hit ) failures.push_back( "queryAt expected a miss but hit '" + qr.name + "'" );
							} else if( !qr.hit || qr.name != expectName ) {
								failures.push_back( "queryAt(" + std::to_string( x ) + "," + std::to_string( y ) + ") expected '" +
									expectName + "', got " + ( qr.hit ? ( "'" + qr.name + "'" ) : std::string( "a miss" ) ) );
							}
						}
					}

					if( !failures.empty() ) {
						std::string detail; for( std::size_t i = 0; i < failures.size(); ++i ) { if( i ) detail += "; "; detail += failures[i]; }
						return { false, detail };
					}
					return { true, "objectmap assertion(s) satisfied" };
				}

				//! "diagnostics": {expect:"clean"|"code",code?} -- validate run
				//! against the CURRENT (post-run) document.
				CheckOutcome CheckDiagnosticsKind( const JsonValue& cp, AgentSession* session )
				{
					if( !session ) return { false, "diagnostics checkpoint: no live session (run did not complete)" };
					if( !cp.has( "expect" ) || !cp.get( "expect" ).isString() )
						return { false, "diagnostics checkpoint missing string field \"expect\"" };
					const std::string expect = cp.get( "expect" ).asString();
					const std::vector<AgentDiagnostic> diags = AgentSession::ValidateText( session->ReadDocument() );

					if( expect == "clean" ) {
						if( diags.empty() ) return { true, "validate: clean (0 diagnostics)" };
						return { false, "validate: expected clean but found " + std::to_string( diags.size() ) +
							" diagnostic(s); first: " + diags[0].code + " " + diags[0].message };
					}
					if( expect == "code" ) {
						if( !cp.has( "code" ) || !cp.get( "code" ).isString() )
							return { false, "diagnostics checkpoint expect:\"code\" requires string field \"code\"" };
						const std::string wantCode = cp.get( "code" ).asString();
						for( const auto& d : diags )
							if( d.code == wantCode ) return { true, "validate: found expected code '" + wantCode + "'" };
						return { false, "validate: expected code '" + wantCode + "' not found among " +
							std::to_string( diags.size() ) + " diagnostic(s)" };
					}
					return { false, "diagnostics checkpoint: unknown \"expect\" value '" + expect + "' (must be \"clean\" or \"code\")" };
				}

				//! "trajectory": {maxToolCalls?,maxLlmCalls?,terminalStatus?,
				//! noAutonomyRefusal?,requiredToolInOrder?,noMechanicalLoop?}
				//! -- maxToolCalls/maxLlmCalls/terminalStatus read straight off
				//! handle.result (the SAME counters RunScenario wrote into the
				//! summary line -- no need to re-derive them from the JSONL).
				//! noAutonomyRefusal/requiredToolInOrder/noMechanicalLoop parse
				//! handle.trajectoryPath's "tool" records (name/args/
				//! jsonrpc.response), since those need per-call detail the
				//! summary doesn't carry.
				CheckOutcome CheckTrajectoryKind( const JsonValue& cp, const AgentEvalRunHandle& handle )
				{
					std::vector<std::string> failures;

					if( cp.has( "maxToolCalls" ) && cp.get( "maxToolCalls" ).isNumber() ) {
						const double m = cp.get( "maxToolCalls" ).asNumber();
						if( handle.result.toolCalls > m )
							failures.push_back( "toolCalls " + std::to_string( handle.result.toolCalls ) + " > maxToolCalls " + std::to_string( m ) );
					}
					if( cp.has( "maxLlmCalls" ) && cp.get( "maxLlmCalls" ).isNumber() ) {
						const double m = cp.get( "maxLlmCalls" ).asNumber();
						if( handle.result.llmCalls > m )
							failures.push_back( "llmCalls " + std::to_string( handle.result.llmCalls ) + " > maxLlmCalls " + std::to_string( m ) );
					}
					if( cp.has( "terminalStatus" ) && cp.get( "terminalStatus" ).isString() ) {
						const std::string want = cp.get( "terminalStatus" ).asString();
						if( handle.result.terminalStatus != want )
							failures.push_back( "terminalStatus '" + handle.result.terminalStatus + "' != expected '" + want + "'" );
					}

					const bool needsRecords = cp.has( "noAutonomyRefusal" ) || cp.has( "requiredToolInOrder" ) || cp.has( "noMechanicalLoop" );
					if( needsRecords ) {
						if( handle.trajectoryPath.empty() ) {
							failures.push_back( "trajectory file unavailable (run did not complete) -- cannot check "
								"noAutonomyRefusal/requiredToolInOrder/noMechanicalLoop" );
						} else {
							std::ifstream f( handle.trajectoryPath.c_str(), std::ios::binary );
							if( !f ) {
								failures.push_back( "trajectory file '" + handle.trajectoryPath + "' could not be opened" );
							} else {
								std::vector<JsonValue> toolRecords;
								std::string line;
								while( std::getline( f, line ) ) {
									if( line.empty() ) continue;
									JsonValue v; std::string perr;
									if( !JsonParse( line, v, perr ) ) continue;   // tolerate a stray non-JSON line (never emitted by the recorder)
									if( v.isObject() && v.get( "run_type" ).asString() == "tool" ) toolRecords.push_back( v );
								}

								if( cp.has( "noAutonomyRefusal" ) && cp.get( "noAutonomyRefusal" ).isBool() && cp.get( "noAutonomyRefusal" ).asBool() ) {
									for( const auto& t : toolRecords ) {
										// ChatTrajectory's EmbedJsonOrString embeds
										// jsonrpc.response as a parsed OBJECT when it
										// is valid JSON (every real response is), else
										// falls back to a raw string -- handle both.
										const JsonValue& resp = t.get( "jsonrpc.response" );
										double code = 0.0;
										bool hasCode = false;
										if( resp.isObject() && resp.has( "error" ) ) {
											code = resp.get( "error" ).get( "code" ).asNumber( 0.0 );
											hasCode = true;
										} else if( resp.isString() ) {
											JsonValue parsed; std::string perr2;
											if( JsonParse( resp.asString(), parsed, perr2 ) && parsed.isObject() && parsed.has( "error" ) ) {
												code = parsed.get( "error" ).get( "code" ).asNumber( 0.0 );
												hasCode = true;
											}
										}
										if( hasCode && code == -32011.0 ) {
											failures.push_back( "an autonomy refusal (-32011) occurred on tool '" + t.get( "name" ).asString() + "'" );
											break;
										}
									}
								}

								if( cp.has( "requiredToolInOrder" ) && cp.get( "requiredToolInOrder" ).isArray() ) {
									const JsonValue& req = cp.get( "requiredToolInOrder" );
									std::size_t ri = 0;
									for( std::size_t ti = 0; ti < toolRecords.size() && ri < req.size(); ++ti ) {
										if( req.at( ri ).isString() && toolRecords[ti].get( "name" ).asString() == req.at( ri ).asString() ) ++ri;
									}
									if( ri < req.size() )
										failures.push_back( "requiredToolInOrder: not satisfied as a subsequence (matched " +
											std::to_string( ri ) + "/" + std::to_string( req.size() ) + ")" );
								}

								if( cp.has( "noMechanicalLoop" ) && cp.get( "noMechanicalLoop" ).isBool() && cp.get( "noMechanicalLoop" ).asBool() ) {
									for( std::size_t ti = 1; ti < toolRecords.size(); ++ti ) {
										const std::string prevName = toolRecords[ti - 1].get( "name" ).asString();
										const std::string curName  = toolRecords[ti].get( "name" ).asString();
										if( prevName != curName ) continue;
										const std::string prevArgs = JsonSerialize( toolRecords[ti - 1].get( "args" ) );
										const std::string curArgs  = JsonSerialize( toolRecords[ti].get( "args" ) );
										if( prevArgs == curArgs ) {
											failures.push_back( "mechanical loop: identical tool call '" + curName +
												"' repeated consecutively (calls " + std::to_string( ti ) + "," + std::to_string( ti + 1 ) + ")" );
											break;
										}
									}
								}
							}
						}
					}

					if( !failures.empty() ) {
						std::string detail; for( std::size_t i = 0; i < failures.size(); ++i ) { if( i ) detail += "; "; detail += failures[i]; }
						return { false, detail };
					}
					return { true, "trajectory assertion(s) satisfied" };
				}

				//! Dispatch one checkpoint object by its "kind".  An
				//! unrecognized kind is a FAILED checkpoint (loud), never a
				//! silent skip -- see CheckScenario's doc.
				CheckOutcome CheckOneCheckpoint( const JsonValue& cp, const AgentEvalRunHandle& handle )
				{
					if( !cp.isObject() || !cp.has( "kind" ) || !cp.get( "kind" ).isString() )
						return { false, "checkpoint is not an object with a string \"kind\"" };
					const std::string kind = cp.get( "kind" ).asString();
					AgentSession* session = handle.dispatcher ? handle.dispatcher->Session() : nullptr;

					if( kind == "document" )    return CheckDocumentKind( cp, session );
					if( kind == "untouched" )   return CheckUntouchedKind( cp, handle );
					if( kind == "render" )      return CheckRenderKind( cp, session );
					if( kind == "objectmap" )   return CheckObjectmapKind( cp, session );
					if( kind == "diagnostics" ) return CheckDiagnosticsKind( cp, session );
					if( kind == "trajectory" )  return CheckTrajectoryKind( cp, handle );

					return { false, "unknown checkpoint kind '" + kind + "'" };
				}
			}

			AgentEvalCheckResult CheckScenario( const AgentEvalRunHandle& handle, const AgentEvalScenario& scenario )
			{
				AgentEvalCheckResult result;
				result.scenarioId = scenario.id;

				double weightSum = 0.0, passSum = 0.0;
				for( std::size_t i = 0; i < scenario.checkpoints.size(); ++i ) {
					const JsonValue& cp = scenario.checkpoints.at( i );

					AgentEvalCheckpointResult r;
					r.kind = ( cp.isObject() && cp.has( "kind" ) && cp.get( "kind" ).isString() )
						? cp.get( "kind" ).asString() : std::string( "<malformed>" );
					r.weight = ( cp.isObject() && cp.has( "weight" ) && cp.get( "weight" ).isNumber() )
						? cp.get( "weight" ).asNumber() : 1.0;
					if( r.weight < 0.0 ) r.weight = 0.0;   // a negative weight would corrupt the fraction -- clamp defensively

					const CheckOutcome oc = CheckOneCheckpoint( cp, handle );
					r.passed = oc.passed;
					r.detail = oc.detail;

					weightSum += r.weight;
					if( r.passed ) passSum += r.weight;
					result.checkpoints.push_back( r );
				}

				result.checkpointFraction = ( weightSum > 0.0 ) ? ( passSum / weightSum ) : 1.0;
				result.allPassed = true;
				for( const auto& c : result.checkpoints ) if( !c.passed ) { result.allPassed = false; break; }

				// Write the results ledger alongside the trajectory.  runDir is
				// recovered from trajectoryPath (or, failing that, resultPath)
				// rather than taken as a parameter -- CheckScenario's signature
				// is (handle, scenario) only, per the plan -- so a "load_error"
				// handle (neither path set) simply skips the write; there is
				// nothing on disk to sit "alongside" in that case anyway.
				std::string runDir;
				if( !handle.trajectoryPath.empty() )      runDir = std::filesystem::path( handle.trajectoryPath ).parent_path().string();
				else if( !handle.resultPath.empty() )     runDir = std::filesystem::path( handle.resultPath ).parent_path().string();

				if( !runDir.empty() ) {
					JsonValue root = JsonValue::MakeObject();
					root.set( "scenarioId", JsonValue::MakeString( result.scenarioId ) );
					root.set( "checkpointFraction", JsonValue::MakeNumber( result.checkpointFraction ) );
					root.set( "allPassed", JsonValue::MakeBool( result.allPassed ) );
					JsonValue arr = JsonValue::MakeArray();
					for( const auto& c : result.checkpoints ) {
						JsonValue o = JsonValue::MakeObject();
						o.set( "kind", JsonValue::MakeString( c.kind ) );
						o.set( "passed", JsonValue::MakeBool( c.passed ) );
						o.set( "weight", JsonValue::MakeNumber( c.weight ) );
						o.set( "detail", JsonValue::MakeString( c.detail ) );
						arr.push_back( o );
					}
					root.set( "checkpoints", arr );

					std::error_code ec;
					std::filesystem::create_directories( runDir, ec );
					const std::string resultsPath = ( std::filesystem::path( runDir ) / "results.jsonl" ).string();
					std::ofstream f( resultsPath.c_str(), std::ios::binary | std::ios::app );
					if( f ) f << JsonSerialize( root ) << "\n";
				}

				return result;
			}
	}
}
