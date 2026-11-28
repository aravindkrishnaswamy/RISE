//////////////////////////////////////////////////////////////////////
//
//  AgentEvalRunner.h - replay backend, scenario runner, provider matrix,
//    and checkpoint engine for the agent eval harness
//    (docs/agentic-redesign/70-agent-eval-harness.md §2.4).
//
//    Components:
//      1. AgentEvalReplaySource -- keyless canned-response source auto-detecting
//         raw JSONL fixtures ({"provider","body"}) or recorded trajectories
//         (`session` + `llm` records).
//      2. AgentEvalScenario / LoadEvalScenario -- parses & validates scenario
//         JSON ({id, title, scene:{path|inline}, autonomy, prompts[], budgets?,
//         replay?, checkpoints?, interventions?, askUserResponses?}); carries
//         `checkpoints` opaquely for CheckScenario.
//      3. RunScenario / RunScenarioLive / RunEvalMatrix -- drives the real
//         AgentChatLoop + AgentRpcDispatcher over a CST-loaded AgentSession
//         with honest budget stops, emitting `<id>.trajectory.jsonl` and
//         `<id>.result.jsonl` under `runDir` and returning a live
//         AgentEvalRunHandle for post-run checking. Inline scenes use a
//         throwaway temp file under `<runDir>/tmp/`, deleted right after load.
//      4. CheckScenario -- evaluates `scenario.checkpoints` against the live
//         handle + trajectory and appends a JSONL summary to `<runDir>/results.jsonl`.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_AGENT_AGENTEVALRUNNER_
#define RISE_AGENT_AGENTEVALRUNNER_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "AgentChatLoop.h"
#include "ChatHttpTransport.h"
#include "Json.h"

namespace RISE
{
	// Forward-declared for AgentEvalRunHandle's headless propose-mode mock-Owner
	// (unique_ptr special members are defined out-of-line in AgentEvalRunner.cpp).
	class IJobPriv;
	class SceneEditController;

	namespace Agent
	{
		class AgentRpcDispatcher;

		//----------------------------------------------------------------
		// 1. Replay source.
		//----------------------------------------------------------------

		//! Keyless scripted LLM response source (raw fixture or recorded trajectory).
		class AgentEvalReplaySource
		{
		public:
			AgentEvalReplaySource() : mIndex( 0 ) {}

			//! Auto-detect and load raw fixture or recorded trajectory JSONL.
			//! Fails loudly with diagnostic `err` on malformed/empty inputs.
			static bool LoadFromFile( const std::string& path,
			                          AgentEvalReplaySource& out,
			                          std::string& err );

			const std::string& Provider() const { return mProvider; }
			bool HasNext() const { return mIndex < mBodies.size(); }
			std::size_t Remaining() const { return mBodies.size() - mIndex; }
			std::size_t Total() const { return mBodies.size(); }

			//! Pop next canned body (returns false and clears `outBody` when exhausted).
			bool NextBody( std::string& outBody );

		private:
			std::string              mProvider;
			std::vector<std::string> mBodies;
			std::size_t               mIndex;
		};

		//----------------------------------------------------------------
		// 2. Scenario format.
		//----------------------------------------------------------------

		//! Optional per-scenario limits (`-1` = unlimited); enforced as honest
		//! stops before dispatching the exceeding call.
		struct AgentEvalBudgets
		{
			int       maxToolCalls = -1;   //!< max dispatched tool calls
			int       maxLlmCalls  = -1;   //!< max LLM rounds
			long long maxWallMs    = -1;   //!< wall-clock limit (per injected or real clock)
		};

		//! Scripted co-editor mutation applied directly to the live head after
		//! `afterToolCalls` (>= 1) dispatched tool calls (before next LLM round),
		//! recorded as a `scenario_intervention` history_edit.
		struct AgentEvalIntervention
		{
			int         afterToolCalls = 0;   //!< trigger threshold (>= 1)
			std::string op;                   //!< currently "param_edit" only
			std::string target;               //!< target chunk name
			std::string param;                //!< parameter name
			std::string value;                //!< new scene-language value text
		};

		//! Case-insensitive substring matcher for scripted `ask_user` responses.
		struct AgentEvalAskUserMatch
		{
			std::string contains;   //!< non-empty case-insensitive substring
			std::string answer;     //!< non-empty answer returned as {"answer": answer}
		};

		//! Optional deterministic responder for `ask_user` (first matching entry in
		//! `matches`, else `defaultAnswer` if `hasDefault`, else `{available:false}`).
		struct AgentEvalAskUserResponses
		{
			std::vector<AgentEvalAskUserMatch> matches;
			bool        hasDefault   = false;
			std::string defaultAnswer;
		};

		//! One user turn: plain string or `{"text"?, "images"?}` (at least one
		//! non-empty; image paths resolve like `scene.path` and reject `".."`).
		struct AgentEvalPrompt
		{
			std::string text;
			std::vector<std::string> imagePaths;

			AgentEvalPrompt() = default;
			AgentEvalPrompt( std::string t ) : text( std::move( t ) ) {}
		};

		//! Parsed `evals/scenarios/*.json` scenario.
		struct AgentEvalScenario
		{
			std::string id;
			std::string title;
			std::string sourcePath;    //!< set by LoadEvalScenario for result.jsonl `scenarioFileFnv`
			std::string scenePath;     //!< XOR with sceneInline
			std::string sceneInline;   //!< written to `<runDir>/tmp/` for load then deleted
			std::string autonomy = "commit"; //!< "commit" / "propose" / "read"
			std::vector<AgentEvalPrompt> prompts;
			AgentEvalBudgets budgets;
			std::string replayFixturePath;   //!< optional if replaySourceOverride is supplied
			JsonValue checkpoints = JsonValue::MakeArray(); //!< opaque array evaluated by CheckScenario
			std::vector<AgentEvalIntervention> interventions;
			AgentEvalAskUserResponses askUserResponses;
		};

		//! Parse and validate an `evals/scenarios/*.json` file into `out`.
		bool LoadEvalScenario( const std::string& path, AgentEvalScenario& out, std::string& err );

		//----------------------------------------------------------------
		// 3. Scenario execution.
		//----------------------------------------------------------------

		//! Options for replay execution (RunScenario).
		struct AgentEvalRunOptions
		{
			std::string runDir; //!< required output directory (empty -> load_error)
			AgentEvalReplaySource* replaySourceOverride = nullptr; //!< borrowed; overrides scenario.replayFixturePath
			std::function<int64_t()> clock; //!< optional epoch-ms clock (trajectory + maxWallMs)
		};

		//! Outcome of one scenario run (also written to `<runDir>/<id>.result.jsonl`).
		struct AgentEvalRunResult
		{
			std::string scenarioId;
			//! One of: "final_text", "budget_tool_calls", "budget_llm_calls",
			//! "budget_wall_ms", "replay_exhausted", "transport_error",
			//! "provider_error", "load_error".
			std::string terminalStatus;
			int  llmCalls  = 0;             //!< total LLM requests attempted
			int  toolCalls = 0;             //!< total tool calls dispatched
			bool budgetHit = false;         //!< true iff terminalStatus is budget_*
			int  degenerateTurnRetries = 0; //!< same-round blank-turn retries actually issued (<= 1/round)
			long long wallMs = -1;          //!< run wall duration in ms (-1 on load_error)
			long long headVersionStart = -1;
			long long headVersionFinal = -1;
			std::string finalText;          //!< last FinalText turn (empty if none)
			std::string errorMessage;       //!< set on provider_error / transport_error / load_error
		};

		//! Post-run handle keeping `dispatcher` (and its AgentSession/Job) alive
		//! for CheckScenario (null on load_error).
		struct AgentEvalRunHandle
		{
			AgentEvalRunResult result;
			std::string trajectoryPath;   //!< "" on load_error
			std::string resultPath;       //!< "" if runDir invalid

			//! Headless propose-mode mock-Owner (empty for read/commit). Declared
			//! BEFORE `dispatcher` so reverse-order destruction tears down
			//! dispatcher/session -> controller -> borrowed refcounted Job.
			std::unique_ptr<IJobPriv, void(*)(IJobPriv*)> ownedProposeJob{ nullptr, +[](IJobPriv*){} };
			std::unique_ptr<SceneEditController>          ownedProposeController;

			std::unique_ptr<AgentRpcDispatcher> dispatcher;

			//! Out-of-line pimpl special members (incomplete types above).
			//! Move-assignment is deleted to prevent declaration-order teardown UAF.
			AgentEvalRunHandle();
			~AgentEvalRunHandle();
			AgentEvalRunHandle( AgentEvalRunHandle&& );
			AgentEvalRunHandle& operator=( AgentEvalRunHandle&& ) = delete;

			//! Canonical `.RISEscene` text captured right after load (before turn 1)
			//! for "untouched" / PASS_TO_PASS checkpoints ("" on load_error).
			std::string initialDocument;
		};

		//! Execute one scenario against a replay source. Never throws.
		AgentEvalRunHandle RunScenario( const AgentEvalScenario& scenario,
		                                const AgentEvalRunOptions& options );

		//----------------------------------------------------------------
		// 3b. Live scenario execution.
		//----------------------------------------------------------------

		//! Options for live HTTPS execution (RunScenarioLive).
		struct AgentEvalLiveRunOptions
		{
			std::string runDir;                //!< required output directory
			IChatHttpTransport* transport = nullptr; //!< required borrowed transport
			ChatProvider provider = ChatProvider::Anthropic;
			std::string modelId;               //!< "" -> codec default
			std::string apiKey;                //!< forwarded to auth header only; never stored/logged
			std::function<int64_t()> clock;    //!< optional epoch-ms clock
			std::function<void(int64_t ms)> sleeper; //!< optional HTTP 429 wait hook (default: sleep_for)
		};

		//! Execute one scenario live via `options.transport` (status 0 ->
		//! "transport_error"). Handles HTTP 429 backoff (up to 5 attempts/round;
		//! body retry hint or 10/20/40/60/60s capped at 90s; aborts with
		//! "budget_wall_ms" if wait exceeds remaining maxWallMs; 429 retries do
		//! not consume maxLlmCalls). Never throws.
		AgentEvalRunHandle RunScenarioLive( const AgentEvalScenario& scenario,
		                                    const AgentEvalLiveRunOptions& options );

		//----------------------------------------------------------------
		// 3c. Run-config & provider matrix.
		//----------------------------------------------------------------

		//! One entry of a run config's `providers[]`.
		struct AgentEvalProviderConfig
		{
			std::string provider;   //!< "anthropic" / "gemini" / "openai" / "xai" / "local"
			std::string model;      //!< "" -> codec default
			std::string keyEnvVar;  //!< env-var name (never raw key); optional only for "local"
		};

		//! Parsed `rise --agent-eval <runconfig.json>`:
		//! `{scenarios:[...], providers:[...], repeats?:3, runDir:"..."}`.
		struct AgentEvalRunConfig
		{
			std::vector<std::string>             scenarios;   //!< paths/globs (expanded by CLI)
			std::vector<AgentEvalProviderConfig> providers;
			int         repeats = 3;
			std::string runDir;
		};

		//! Parse and validate a run-config JSON file (reads no env vars or keys).
		bool LoadEvalRunConfig( const std::string& path, AgentEvalRunConfig& out, std::string& err );

		//! Options for RunEvalMatrix.
		struct AgentEvalMatrixOptions
		{
			IChatHttpTransport* transport = nullptr; //!< required borrowed transport
			std::function<const char*(const std::string&)> envLookup; //!< required keyEnvVar lookup
			std::function<int64_t()> clock;          //!< optional epoch-ms clock
			std::function<void(int64_t ms)> sleeper; //!< optional HTTP 429 backoff hook
			std::function<void(const std::string&)> log; //!< optional progress sink (never receives keys)
		};

		//! Aggregate counts for a matrix invocation.
		struct AgentEvalMatrixResult
		{
			int runsExecuted   = 0;      //!< cells executed
			int runsSkipped    = 0;      //!< cells skipped (missing key or failed auth probe)
			int runsAlreadyComplete = 0; //!< cells skipped on idempotent resume (matching scenarioContentHash)
			int providersUsed  = 0;      //!< columns with valid credentials (or keyless local)
			int providersSkipped = 0;    //!< columns skipped (unset key or auth probe failure)
			int providersAuthFailed = 0; //!< subset of providersSkipped whose auth probe failed
			std::string errorMessage;    //!< fatal config error (duplicate scenario id or subdir leaf collision)
		};

		//! Run scenarios x providers x repeats through RunScenarioLive + CheckScenario
		//! under `<runDir>/<scenarioId>__<provider>[__<model>]__r<repeat>/`.
		//! Runs a one-request pre-flight auth probe per keyed provider (skipping the
		//! column on 401/403 or auth/credit 4xx, never on transient 5xx/transport
		//! errors). Resumes idempotently when `<id>.result.jsonl` matches the
		//! current `scenarioContentHash`; wipes stale or incomplete subdirs before
		//! re-running. Never throws.
		AgentEvalMatrixResult RunEvalMatrix( const AgentEvalRunConfig& config,
		                                     const std::vector<AgentEvalScenario>& scenarios,
		                                     const AgentEvalMatrixOptions& options );

		//----------------------------------------------------------------
		// 4. Checkpoint engine.
		//----------------------------------------------------------------

		//! Outcome of one evaluated checkpoint.
		struct AgentEvalCheckpointResult
		{
			std::string kind;             //!< checkpoint "kind" (or "<malformed>")
			bool        passed = false;
			double      weight = 1.0;     //!< clamped >= 0
			std::string detail;           //!< human-readable explanation (always populated)
			bool        hasMetricValue = false; //!< true for numeric richness metrics (distinct_chunk_kinds / objects_reaching_kinds)
			double      metricValue = 0.0;
			std::string metricLabel;      //!< explicit metricLabel or op name (unique per scenario; set iff hasMetricValue)
		};

		//! Aggregate check outcome for one scenario. `checkpointFraction` is the
		//! weighted pass fraction (1.0 if empty). `allPassed` requires every
		//! checkpoint to pass AND `terminalStatus == "final_text"` (unless a
		//! trajectory checkpoint explicitly asserts another status; overrides set
		//! `terminalGateNote`).
		struct AgentEvalCheckResult
		{
			std::string scenarioId;
			std::vector<AgentEvalCheckpointResult> checkpoints;
			double checkpointFraction = 1.0;
			bool   allPassed = true;
			std::string terminalGateNote;   //!< non-empty iff terminal-success gate forced allPassed=false
		};

		//! Evaluate `scenario.checkpoints` directly against `handle`'s live
		//! AgentSession (bypassing RPC autonomy) and trajectory file, appending one
		//! JSON line to `<runDir>/results.jsonl`. Never throws.
		AgentEvalCheckResult CheckScenario( const AgentEvalRunHandle& handle, const AgentEvalScenario& scenario );

	}
}

#endif
