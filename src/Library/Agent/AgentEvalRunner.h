//////////////////////////////////////////////////////////////////////
//
//  AgentEvalRunner.h - the REPLAY BACKEND + SCENARIO RUNNER CORE (Eval-
//    harness, slice E2).  Built on top of E1 (ChatTrajectory.h -- the
//    trajectory schema/recorder) and the sans-IO AgentChatLoop.
//
//    THREE pieces:
//
//    1. AgentEvalReplaySource -- "the fourth provider": yields canned
//       LLM response bodies in sequence instead of performing HTTP.
//       Loads from either of two file shapes (auto-detected):
//         (a) a RAW FIXTURE: JSONL, each line {"provider":"...",
//             "body":"..."} -- a hand-authored canned session.
//         (b) a RECORDED E1 TRAJECTORY: JSONL, the exact file
//             ChatTrajectoryRecorder emits -- the source extracts the
//             `llm` records' `response_body` fields (in file order) and
//             the leading `session` record's `provider`.  This is the
//             "record real session once, replay forever" path: point a
//             replay run at a PREVIOUSLY RECORDED trajectory (from a live
//             session, or from an earlier replay run) and it reproduces
//             the identical LLM response sequence with zero network.
//       NEVER reads an API key or any environment variable -- the whole
//       point of this class is that a replayed session needs no key
//       anywhere.
//
//    2. AgentEvalScenario + LoadEvalScenario -- the evals/scenarios/*.json
//       format (docs/agentic-redesign/70-agent-eval-harness.md §2.4):
//       {id, title, scene:{path|inline}, autonomy, prompts[], budgets?,
//       replay?:{fixture}, checkpoints?}.  The loader validates the BASIC
//       shape only (types, required fields, exactly-one-of scene.path/
//       scene.inline, autonomy in the known 3-value set) and fails LOUDLY
//       (returns false + a human message) on anything malformed.
//       `checkpoints` is parsed but carried OPAQUELY as a JsonValue array
//       -- interpreting checkpoint kinds is the E3 checker engine's job,
//       not this slice's.
//
//    3. RunScenario -- drives the REAL AgentChatLoop + REAL
//       AgentRpcDispatcher over a REAL (CST-loaded) Job/AgentSession,
//       exactly like tests/AgentChatLoopTest.cpp / AgentTrajectoryTest.cpp
//       drive them by hand: BuildRequest -> replay source's next body ->
//       RecordHttpRound(200, body, 0) -> HandleResponse -> dispatch any
//       tool calls via the live AgentRpcDispatcher -> AddToolResult ->
//       repeat until FinalText / ProviderError / a budget trips / the
//       replay source is exhausted.  Enforces per-scenario budgets
//       (maxToolCalls / maxLlmCalls / maxWallMs) as an HONEST STOP: the
//       call that would exceed a budget is never dispatched.  The
//       replayed session EMITS ITS OWN E1 trajectory to
//       <runDir>/<id>.trajectory.jsonl (replay in, trajectory out) plus a
//       one-line result summary to <runDir>/<id>.result.jsonl.  Returns
//       an AgentEvalRunHandle whose `dispatcher` (and therefore its
//       AgentSession / Job) stays ALIVE after RunScenario returns -- the
//       E3 checker engine's seam: a later checker can interrogate the
//       post-run scene state (ReadDocument, render, query_object_at, ...)
//       without RunScenario needing to know anything about checking.
//
//    HYGIENE: a scenario's `scene.inline` text is written to a throwaway
//    temp file under <runDir>/tmp/ (never under scenes/) and deleted
//    immediately after the load attempt, success or failure.
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
	//! Fwd-decls for AgentEvalRunHandle's headless propose-mode mock-Owner
	//! (see RunScenarioDriven / the two owning members on AgentEvalRunHandle).
	//! Kept as fwd-decls so this header does not pull the heavy IJobPriv /
	//! SceneEditController includes.  The handle's unique_ptr members over
	//! these incomplete types are safe ONLY because AgentEvalRunHandle declares
	//! its destructor + move operations here and DEFINES them out-of-line in
	//! AgentEvalRunner.cpp, where both types are complete -- so no consuming TU
	//! ever instantiates a special member over an incomplete SceneEditController.
	class IJobPriv;
	class SceneEditController;

	namespace Agent
	{
		class AgentRpcDispatcher;

		//----------------------------------------------------------------
		// 1. The replay source.
		//----------------------------------------------------------------

		//! A scripted-agent response source: yields canned LLM response
		//! bodies in sequence instead of performing HTTP.  See the file
		//! header for the two supported file shapes.  Never touches an
		//! environment variable or any credential -- keyless by
		//! construction.
		class AgentEvalReplaySource
		{
		public:
			AgentEvalReplaySource() : mIndex( 0 ) {}

			//! Load `path`, auto-detecting a raw fixture vs. a recorded E1
			//! trajectory (see the file header).  Returns true and fills
			//! `out` on success; returns false and fills `err` with a loud,
			//! human-readable diagnostic (empty file, unreadable path,
			//! malformed JSON on some line, a trajectory with no `llm`
			//! records, a raw fixture with a missing/non-string provider
			//! or body, or a raw fixture whose lines name more than one
			//! provider) on failure.  `out` is left in an unspecified but
			//! safe (empty) state on failure.
			static bool LoadFromFile( const std::string& path,
			                          AgentEvalReplaySource& out,
			                          std::string& err );

			//! The provider name ("anthropic" / "gemini" / "openai") this
			//! source's canned bodies were recorded/authored against.
			const std::string& Provider() const { return mProvider; }

			//! True while NextBody has at least one more body to hand out.
			bool HasNext() const { return mIndex < mBodies.size(); }

			//! How many bodies remain (including the one HasNext implies).
			std::size_t Remaining() const { return mBodies.size() - mIndex; }

			//! Total bodies this source was loaded with (Remaining() +
			//! however many NextBody has already handed out).
			std::size_t Total() const { return mBodies.size(); }

			//! Hand back the next canned body and advance.  Returns false
			//! (and clears `outBody`) when HasNext() is false -- callers
			//! MUST check HasNext() first for an honest "replay exhausted"
			//! stop rather than treating an empty body as a valid response.
			bool NextBody( std::string& outBody );

		private:
			std::string              mProvider;
			std::vector<std::string> mBodies;
			std::size_t               mIndex;
		};

		//----------------------------------------------------------------
		// 2. The scenario format.
		//----------------------------------------------------------------

		//! Optional per-scenario limits (evals/scenarios/*.json `budgets`).
		//! -1 (the default) means "no limit" for that dimension.  Enforced
		//! by RunScenario as an HONEST STOP: the call that would exceed a
		//! budget is never dispatched/sent.
		struct AgentEvalBudgets
		{
			int       maxToolCalls = -1;   //!< total tool calls across the whole scenario
			int       maxLlmCalls  = -1;   //!< total LLM (replay) rounds across the whole scenario
			long long maxWallMs    = -1;   //!< wall-clock budget, per RunScenario's clock (see AgentEvalRunOptions.clock)
		};

		//! One SCENARIO INTERVENTION (evals/scenarios/*.json optional
		//! `interventions`): a scripted co-editor edit the runner applies
		//! DIRECTLY to the live head partway through a run -- the "user
		//! edited the scene while the agent worked" simulation.  It fires
		//! AFTER the `afterToolCalls`-th dispatched tool call completes and
		//! BEFORE the next LLM round, mutating the shared head (bumping its
		//! headVersion) WITHOUT consuming a model turn -- so a subsequent
		//! agent patch built against the head the agent last read GENUINELY
		//! conflicts, making the read-conflict-reread-repropose contract
		//! live-gradable instead of fixture-staged.  Schema:
		//!   {"afterToolCalls": N>=1, "op": "param_edit",
		//!    "target": "<chunk name>", "param": "<param>", "value": "<text>"}
		//! `op` today is ONLY "param_edit" (a set-one-param commit, the same
		//! edit pathway ProposePatch takes with no baseVersion -> an
		//! unconditional Owner commit).  Applied via the runner's live
		//! AgentSession, recorded honestly in the trajectory as a
		//! `history_edit` (reason "scenario_intervention").
		struct AgentEvalIntervention
		{
			int         afterToolCalls = 0;   //!< fire AFTER this many dispatched tool calls (>= 1)
			std::string op;                   //!< "param_edit" (the only supported op today)
			std::string target;               //!< the chunk NAME to edit
			std::string param;                //!< the parameter to set
			std::string value;                //!< the new value as scene-language text
		};

		//! One parsed evals/scenarios/*.json.  See the file header for the
		//! schema; `checkpoints` is carried OPAQUELY (E3's job to
		//! interpret).
		struct AgentEvalScenario
		{
			std::string id;
			std::string title;

			//! The evals/scenarios/*.json file this was loaded from; empty for
			//! a programmatically-built scenario (e.g. a test that fills an
			//! AgentEvalScenario in memory without going through
			//! LoadEvalScenario).  Set by LoadEvalScenario to the `path` it
			//! was called with.  Used ONLY to stamp result.jsonl's
			//! "scenarioFileFnv" (a raw file-byte FNV-1a 64 hash a standalone
			//! Python reporter can trivially recompute) -- see
			//! RunScenarioDriven's result-summary write and
			//! tools/eval_report.py's check_staleness.
			std::string sourcePath;

			//! Exactly one of these is non-empty after a successful load
			//! (LoadEvalScenario enforces scene.path XOR scene.inline).
			std::string scenePath;     //!< scene.path: loaded AS-IS
			std::string sceneInline;   //!< scene.inline: written to a throwaway temp file, then loaded

			//! "commit" (default) / "propose" / "read".  Validated by
			//! LoadEvalScenario against exactly this 3-value set.
			std::string autonomy = "commit";

			//! Sequential user turns (evals/scenarios/*.json `prompts`).
			//! At least one, each a non-empty-typed (but possibly blank
			//! text is still a valid JSON string -- AgentChatLoop's own
			//! whitespace-only no-op rule applies at run time) string.
			std::vector<std::string> prompts;

			AgentEvalBudgets budgets;

			//! Path to a replay fixture/trajectory file (evals/scenarios's
			//! `replay.fixture`).  Empty when the scenario names no
			//! fixture -- RunScenario then REQUIRES
			//! AgentEvalRunOptions.replaySourceOverride to be supplied by
			//! the caller (the trajectory-extraction red-prove path uses
			//! this to feed a source built from an in-memory/previously-
			//! written trajectory without re-parsing it from a path the
			//! scenario file itself does not know about).
			std::string replayFixturePath;

			//! The parsed `checkpoints` array, carried OPAQUELY.  An empty
			//! array when the scenario omits the field.  E3 interprets
			//! kinds; this slice only validates that each element parses
			//! as a JSON object (LoadEvalScenario).
			JsonValue checkpoints = JsonValue::MakeArray();

			//! OPTIONAL scripted co-editor interventions (see
			//! AgentEvalIntervention).  Empty when the scenario omits the
			//! field.  LoadEvalScenario validates the shape LOUDLY (array of
			//! objects; afterToolCalls a number >= 1; op == "param_edit";
			//! target/param/value non-empty strings).  RunScenario /
			//! RunScenarioLive apply each after its afterToolCalls-th tool
			//! call through the live session.
			std::vector<AgentEvalIntervention> interventions;
		};

		//! Parse `path` (an evals/scenarios/*.json file) into `out`.
		//! Returns true on success; false + a loud human-readable `err` on
		//! any malformed shape (unreadable path, non-object root, a
		//! missing/wrong-typed required field, scene naming zero or both
		//! of path/inline, an unrecognized autonomy string, an empty
		//! prompts array or a non-string prompt element, a non-object
		//! budgets/replay field, a non-array checkpoints field, or a
		//! non-object checkpoints element).
		bool LoadEvalScenario( const std::string& path, AgentEvalScenario& out, std::string& err );

		//----------------------------------------------------------------
		// 3. Scenario execution.
		//----------------------------------------------------------------

		//! Construction-time options for RunScenario.
		struct AgentEvalRunOptions
		{
			//! REQUIRED: the directory the trajectory + result files land
			//! under (and, for an inline scene, a "tmp" subdirectory of
			//! this holds the throwaway scene file for the duration of the
			//! load).  RunScenario refuses (a "load_error" result) when
			//! this is empty -- a replayed run must never guess a
			//! location, and must never write under scenes/.
			std::string runDir;

			//! When non-null, used INSTEAD of loading
			//! scenario.replayFixturePath -- the seam the
			//! record-once-replay-forever red-prove uses to feed a source
			//! built from an already-recorded trajectory file.  Must
			//! outlive the RunScenario call; RunScenario does not take
			//! ownership.
			AgentEvalReplaySource* replaySourceOverride = nullptr;

			//! Optional injected epoch-ms clock, for both the emitted
			//! trajectory's timestamps (passed straight through to
			//! ChatTrajectoryConfig.clock) AND this runner's own
			//! maxWallMs budget check.  Empty (the default) uses a real
			//! wall clock (ChatTrajectory's TrajectoryNowMs).
			std::function<int64_t()> clock;
		};

		//! The result of one scenario run -- also what gets written (as
		//! one JSON object) to <runDir>/<id>.result.jsonl.
		struct AgentEvalRunResult
		{
			std::string scenarioId;

			//! One of: "final_text" (every prompt's turn ended cleanly),
			//! "budget_tool_calls" / "budget_llm_calls" / "budget_wall_ms"
			//! (an honest budget stop), "replay_exhausted" (the REPLAY source
			//! ran out of canned bodies before the scenario's turns did --
			//! replay path only), "transport_error" (the LIVE transport
			//! failed to reach the endpoint: DNS/connect/TLS/timeout, or the
			//! Linux unsupported stub -- see errorMessage; live path only),
			//! "provider_error" (HandleResponse returned ProviderError --
			//! see errorMessage), or "load_error" (the scenario/scene/
			//! replay source itself failed to load -- see errorMessage;
			//! no turns ran).
			std::string terminalStatus;

			int  llmCalls  = 0;    //!< total LLM requests SENT (counts every POST attempted, incl. a transport-failed attempt that may still have been billed; can exceed the number of recorded llm rounds, which count only received responses)
			int  toolCalls = 0;    //!< total tool calls actually dispatched
			bool budgetHit = false;   //!< true iff terminalStatus is one of the three budget_* stops

			//! Total WALL-CLOCK duration of the run in milliseconds (the whole
			//! turn loop: LLM round-trips + tool dispatch), measured via the
			//! same wall clock maxWallMs uses (TrajectoryNowMs, or the injected
			//! deterministic clock under replay).  This is the "time to
			//! completion" comparison metric across providers -- for LIVE runs
			//! it is dominated by network + remote/local inference; under replay
			//! (no network) it is near-zero or the injected-clock delta.  -1
			//! only on a load_error (the run never started its timer).
			long long wallMs = -1;

			//! The scene's CST head-version revision at session start /
			//! after the run (-1 when unknown -- a load_error before a
			//! session existed).  headVersionFinal > headVersionStart
			//! demonstrates a committing edit actually landed.
			long long headVersionStart = -1;
			long long headVersionFinal = -1;

			std::string finalText;      //!< the last FinalText turn's text (empty if none)
			std::string errorMessage;   //!< filled iff terminalStatus is "provider_error" or "load_error"
		};

		//! The E3 seam: RunScenario's full return value.  `dispatcher`
		//! (and, through it, its AgentSession / Job) stays ALIVE after
		//! RunScenario returns so a later checker can interrogate the
		//! post-run scene -- read the document, render, query an object --
		//! without RunScenario itself knowing anything about checking.
		//! Null iff terminalStatus == "load_error" (nothing was
		//! constructed).
		struct AgentEvalRunHandle
		{
			AgentEvalRunResult result;
			std::string trajectoryPath;   //!< "" iff terminalStatus == "load_error" (no sink was ever attached)
			std::string resultPath;       //!< the <id>.result.jsonl this run wrote ("" iff runDir itself was invalid)

			//! Headless propose-mode mock-Owner (see RunScenarioDriven).  Under
			//! autonomy:"propose" the runner loads the Job itself, wraps a
			//! (borrowing) External-authority AgentSession around it, and
			//! attaches an unstarted SceneEditController so ProposePatch has a
			//! live controller to STAGE proposals against.  Both are empty for a
			//! read/commit run (there LoadFromFile owns its own Job and no
			//! controller exists).  These two members are declared IMMEDIATELY
			//! BEFORE `dispatcher` so C++'s reverse-declaration-order destruction
			//! tears the handle down as session (inside `dispatcher`) -> controller
			//! -> Job: the session drains its async render against a still-live
			//! controller, and the borrowed Job outlives both the controller (whose
			//! ctor took `IJobPriv&`) and the session (which wraps it owns=false).
			//! ownedProposeJob's deleter calls IJobPriv::release() (it is
			//! refcounted -- mirrors AgentSession::LoadFromFile's error path).
			std::unique_ptr<IJobPriv, void(*)(IJobPriv*)> ownedProposeJob{ nullptr, +[](IJobPriv*){} };
			std::unique_ptr<SceneEditController>          ownedProposeController;

			std::unique_ptr<AgentRpcDispatcher> dispatcher;

			//! The unique_ptr members above are over INCOMPLETE types in a
			//! consuming TU (AgentEvalRunner.h fwd-decls SceneEditController /
			//! IJobPriv / AgentRpcDispatcher without their headers).  Declaring
			//! these special members here and DEFINING them out-of-line in
			//! AgentEvalRunner.cpp -- where all three are complete -- is the pimpl
			//! idiom: every instantiation of the destructor / move ops happens in
			//! that one TU, so no consumer needs the heavy includes.  Move-only
			//! (matching the pre-existing unique_ptr `dispatcher` member); never
			//! copied.
			AgentEvalRunHandle();
			~AgentEvalRunHandle();
			AgentEvalRunHandle( AgentEvalRunHandle&& );
			//! Move-ASSIGNMENT is DELETED, not defaulted: a defaulted move-assign
			//! assigns members in DECLARATION order (ownedProposeJob first), which
			//! would release the borrowed Job BEFORE the session/controller that
			//! reference it -- the inverse of the reverse-DESTRUCTION order this
			//! layout relies on.  No call site move-assigns onto a live handle (all
			//! consumers move-CONSTRUCT into a fresh variable), so deleting it costs
			//! nothing and turns a future misuse into a compile error, not a UAF.
			AgentEvalRunHandle& operator=( AgentEvalRunHandle&& ) = delete;

			//! Eval-harness slice E3 (the "untouched" / PASS_TO_PASS
			//! checkpoint seam): the head's canonical `.RISEscene` text AS
			//! LOADED, captured BEFORE the first turn ran -- i.e.
			//! session->ReadDocument() taken immediately after the scene
			//! loaded and before any AddUserMessage/tool dispatch.  "" iff
			//! terminalStatus == "load_error" (no session was ever
			//! constructed to read from).  A checker compares a named
			//! chunk's serialized bytes in THIS text against the same
			//! chunk in the POST-run dispatcher->Session()->ReadDocument()
			//! to prove an unrelated chunk was never touched.
			std::string initialDocument;
		};

		//! Execute one scenario end-to-end through the REAL AgentChatLoop +
		//! REAL AgentRpcDispatcher, driven by a replay source (either
		//! loaded from scenario.replayFixturePath or supplied via
		//! options.replaySourceOverride).  See the file header for the
		//! full drive-loop description.  Never throws.
		AgentEvalRunHandle RunScenario( const AgentEvalScenario& scenario,
		                                const AgentEvalRunOptions& options );

		//----------------------------------------------------------------
		// 3b. LIVE scenario execution (Eval-harness slice E4).
		//----------------------------------------------------------------

		//! Construction-time options for RunScenarioLive -- the LIVE twin of
		//! AgentEvalRunOptions.  Same drive loop as RunScenario, but each
		//! LLM round goes through `transport` (a real platform-TLS POST, or
		//! a MOCK in tests) instead of a replay source.
		struct AgentEvalLiveRunOptions
		{
			//! REQUIRED: where the trajectory + result files land (same
			//! contract as AgentEvalRunOptions.runDir).  Empty => a
			//! "load_error" result.
			std::string runDir;

			//! REQUIRED: the synchronous HTTPS transport.  RunScenarioLive
			//! does NOT own it (must outlive the call).  A null transport is
			//! a "load_error".  The SAME seam a mock plugs into: the mock
			//! and the real socket drive byte-identical runner code.
			IChatHttpTransport* transport = nullptr;

			//! The provider whose codec builds the wire request (and whose
			//! response bodies `transport` returns).
			ChatProvider provider = ChatProvider::Anthropic;

			//! The model id (empty => the provider codec's default model).
			std::string modelId;

			//! The api key forwarded to BuildRequest for the AUTH HEADER
			//! ONLY.  Held nowhere, logged nowhere, and never written to any
			//! trajectory/result file (E1 strips it; this runner never
			//! copies it anywhere but into BuildRequest).  Empty is allowed
			//! (the request goes out unauthenticated -- the endpoint will
			//! 401, surfaced honestly as a provider_error).
			std::string apiKey;

			//! Optional injected epoch-ms clock (trajectory timestamps + the
			//! maxWallMs budget check).  Empty uses the real wall clock.
			std::function<int64_t()> clock;
		};

		//! Execute one scenario end-to-end through the REAL AgentChatLoop +
		//! REAL AgentRpcDispatcher, driving each LLM round through
		//! `options.transport` (BuildRequest(apiKey) -> transport.Post(req)
		//! -> RecordHttpRound(status, body, elapsedMs) -> HandleResponse ->
		//! tool dispatch -> repeat).  Enforces the SAME per-scenario budgets
		//! as RunScenario; emits the SAME <runDir>/<id>.trajectory.jsonl +
		//! <runDir>/<id>.result.jsonl and returns the SAME
		//! still-alive-dispatcher handle for the E3 checker.  A transport
		//! that returns status 0 (DNS/TLS/timeout, or the Linux unsupported
		//! stub) terminates the run with terminalStatus == "transport_error".
		//! Never throws.  Shares its entire drive loop with RunScenario --
		//! only the per-round body source differs (replay vs. transport).
		AgentEvalRunHandle RunScenarioLive( const AgentEvalScenario& scenario,
		                                    const AgentEvalLiveRunOptions& options );

		//----------------------------------------------------------------
		// 3c. The run-config + provider matrix (Eval-harness slice E4).
		//----------------------------------------------------------------

		//! One provider entry of a run config's `providers[]`.
		struct AgentEvalProviderConfig
		{
			std::string provider;   //!< "anthropic" / "gemini" / "openai" / "xai" / "local" (validated)
			std::string model;      //!< model id ("" => the codec's default)
			std::string keyEnvVar;  //!< the ENV VAR NAME to read the api key from (e.g. "ANTHROPIC_API_KEY") -- NEVER the key itself; EMPTY only for a keyless "local" provider
		};

		//! A parsed run config (the `rise --agent-eval <runconfig.json>`
		//! input).  Schema:
		//!   {
		//!     "scenarios": [ path-or-glob, ... ],   // >= 1; globs expanded by the CLI
		//!     "providers": [ {"provider","model"?,"keyEnvVar"?}, ... ],  // >= 1; keyEnvVar required except for provider "local"
		//!     "repeats":   N,                        // optional, default 3, >= 1
		//!     "runDir":    "evals/runs/<stamp>"      // required, non-empty
		//!   }
		//! The api KEY is never in this file -- only the env-var NAME to
		//! read it from at run time.
		struct AgentEvalRunConfig
		{
			std::vector<std::string>             scenarios;   //!< path-or-glob strings, verbatim (CLI expands globs)
			std::vector<AgentEvalProviderConfig> providers;
			int         repeats = 3;
			std::string runDir;
		};

		//! Parse `path` (a run-config JSON file) into `out`.  Returns true on
		//! success; false + a loud human-readable `err` on any malformed
		//! shape (unreadable/non-object root, missing/empty scenarios, a
		//! non-string scenario entry, missing/empty providers, a provider
		//! entry missing a valid "provider", a non-"local" provider missing
		//! "keyEnvVar" (required for all but "local"), a present-but-empty
		//! "keyEnvVar", naming an unknown provider, a non-positive/non-number
		//! "repeats", or a
		//! missing/empty "runDir").  Reads NO environment variable and NO
		//! api key -- only the config's declared shape.
		bool LoadEvalRunConfig( const std::string& path, AgentEvalRunConfig& out, std::string& err );

		//! Construction-time options for RunEvalMatrix.
		struct AgentEvalMatrixOptions
		{
			//! REQUIRED: the live transport every run drives through (the
			//! real system transport in the CLI; a mock in tests).  Not
			//! owned; must outlive the call.
			IChatHttpTransport* transport = nullptr;

			//! REQUIRED: maps a provider's keyEnvVar NAME to its value, or
			//! returns nullptr/"" when unset.  The CLI passes a getenv
			//! adapter; a test passes a canned map.  This is the ONLY place
			//! a key enters the matrix -- keeping getenv out of the library
			//! TU (which must stay credential-read-free) while making the
			//! missing-key SKIP behaviour unit-testable.
			std::function<const char*(const std::string&)> envLookup;

			//! Optional injected epoch-ms clock (threaded to each run).
			std::function<int64_t()> clock;

			//! Optional human-progress sink (one line per matrix event:
			//! provider-skip, run start/finish).  The CLI wires this to
			//! stderr; a test may leave it empty.  NEVER receives a key.
			std::function<void(const std::string&)> log;
		};

		//! The aggregate outcome of a matrix run (also useful for a test to
		//! assert skip/execute counts).
		struct AgentEvalMatrixResult
		{
			int runsExecuted   = 0;   //!< (scenario x provider-with-key x repeat) runs actually driven
			int runsSkipped    = 0;   //!< runs skipped because a provider's key env var was unset
			int runsAlreadyComplete = 0; //!< runs SKIPPED because their subdir already held a non-empty <scenarioId>.result.jsonl from a prior invocation into the same runDir AND its stamped scenarioContentHash MATCHES the current scenario (still graded under the same oracle) -- the cross-invocation resume/idempotency guard.  A subdir with NO result.jsonl (a crash mid-run), OR one whose stamped hash is missing/different (a STALE cell graded under a since-changed scenario), is instead wiped and re-run, NOT counted here
			int providersUsed  = 0;   //!< providers whose key resolved (non-empty)
			int providersSkipped = 0; //!< providers skipped for a missing key

			//! Non-empty iff the matrix REFUSED to run (a fatal config error
			//! caught before any run executed) -- either a duplicate scenario
			//! `id` in the pre-loaded set, or a provider/model LEAF COLLISION
			//! (two distinct (provider, model) pairs that SanitizeForPath maps
			//! to the same per-run subdir fragment).  Both would collide two
			//! runs into the same per-run subdir (silent trajectory-append /
			//! result-overwrite corruption).  When set, all counts are 0.
			std::string errorMessage;
		};

		//! Run the full scenarios x providers x repeats matrix through
		//! RunScenarioLive + CheckScenario, writing each run to its OWN
		//! subdirectory under `config.runDir`:
		//!
		//!   <runDir>/<scenarioId>__<provider>[__<model>]__r<repeat>/
		//!       <scenarioId>.trajectory.jsonl   (E1 trajectory)
		//!       <scenarioId>.result.jsonl       (one-line run result)
		//!       results.jsonl                   (E3 check result)
		//!
		//! -- a clean, documented per-run layout E5 (metrics/report) walks.
		//! For each provider, the key is resolved via options.envLookup;
		//! when it is unset the provider's whole column is SKIPPED with a
		//! logged message (a missing key is never a crash).  `scenarios` are
		//! pre-loaded (the CLI expands globs + LoadEvalScenario before
		//! calling this, keeping matrix orchestration file-IO-light and
		//! unit-testable with in-memory scenarios).  Never throws.
		//!
		//! Cross-invocation resume (idempotent completion): before executing
		//! a run, its target subdir is checked for an already-present,
		//! NON-EMPTY <scenarioId>.result.jsonl.  If found AND its stamped
		//! scenarioContentHash MATCHES the current scenario's (still graded
		//! under the same oracle -- checkpoints[], prompts, budgets, scene),
		//! the run is SKIPPED (counted in result.runsAlreadyComplete) rather
		//! than re-executed -- re-running the SAME runconfig into an EXISTING
		//! runDir is therefore a no-op past the first invocation, and adding
		//! a provider column later (e.g. exporting a new provider's api key
		//! and re-running) executes ONLY the newly-added runs.  Without this,
		//! a re-run would reopen each run's trajectory.jsonl in APPEND mode
		//! (ChatTrajectory's file sink), concatenating two sessions into one
		//! file, while truncate-overwriting result.jsonl -- silent
		//! corruption.  A subdir whose result.jsonl is present but carries a
		//! MISSING (old pre-content-hash result) or DIFFERENT stamped hash is
		//! treated as STALE (the scenario changed since it was graded) and,
		//! like a subdir that exists but holds NO (or an empty) result.jsonl
		//! (a crashed/interrupted run), is wiped before re-running so the
		//! trajectory sink's append can't concatenate onto a partial file and
		//! the report never keeps publishing a score graded under an old
		//! oracle.
		AgentEvalMatrixResult RunEvalMatrix( const AgentEvalRunConfig& config,
		                                     const std::vector<AgentEvalScenario>& scenarios,
		                                     const AgentEvalMatrixOptions& options );

		//----------------------------------------------------------------
		// 4. The checker engine (Eval-harness slice E3).
		//----------------------------------------------------------------

		//! One checkpoint's outcome -- an entry of AgentEvalCheckResult.
		struct AgentEvalCheckpointResult
		{
			std::string kind;      //!< the checkpoint's "kind" field verbatim ("<malformed>" if the checkpoint itself was not a well-shaped object)
			bool        passed = false;
			double      weight = 1.0;   //!< the checkpoint's "weight" field (default 1.0; clamped to >= 0)
			std::string detail;    //!< a human-readable explanation -- ALWAYS filled, pass or fail (never silent)
		};

		//! The result of checking one scenario's checkpoints[] against a
		//! completed (or failed) AgentEvalRunHandle.  Partial credit:
		//! `checkpointFraction` is the WEIGHTED pass fraction (sum of
		//! passing checkpoints' weights / sum of all checkpoints'
		//! weights); 1.0 (vacuous pass) when the scenario carries no
		//! checkpoints at all.  `allPassed` is true iff every checkpoint
		//! passed (equivalently: no failed entries in `checkpoints`,
		//! independent of weight -- a single failed checkpoint, however
		//! small its weight, means NOT allPassed) AND the run met the
		//! terminal-success precondition: a run that ended on a non-success
		//! terminal status (anything but "final_text") is forced to
		//! allPassed=false unless a trajectory checkpoint explicitly asserts
		//! that status -- so `allPassed` can be false with every checkpoint
		//! passing (and `checkpointFraction` still 1.0); `terminalGateNote`
		//! is then non-empty and explains the override.
		struct AgentEvalCheckResult
		{
			std::string scenarioId;
			std::vector<AgentEvalCheckpointResult> checkpoints;
			double checkpointFraction = 1.0;
			bool   allPassed = true;
			std::string terminalGateNote;   //!< non-empty iff the terminal-success gate forced allPassed=false
		};

		//! Interpret and run `scenario.checkpoints` (see the file header's
		//! CHECKPOINT KINDS list) against `handle` -- the seam being the
		//! handle's STILL-ALIVE dispatcher/session (for document/
		//! untouched/render/objectmap/diagnostics checkpoints, which
		//! interrogate the post-run scene directly through AgentSession,
		//! bypassing the JSON-RPC autonomy gate -- the checker is a
		//! privileged verifier, not a further agent turn) and the emitted
		//! trajectory file at handle.trajectoryPath (for the "trajectory"
		//! checkpoint kind's structural asserts).  A checkpoint whose
		//! shape is malformed, whose kind is unrecognized, or whose
		//! verb/render call fails is a FAILED checkpoint carrying a
		//! `detail` message -- CheckScenario NEVER throws and NEVER
		//! crashes on a malformed checkpoint or a null/incomplete handle
		//! (e.g. a "load_error" run with a null dispatcher).  As a side
		//! effect, appends one JSON line (scenarioId, checkpointFraction,
		//! allPassed, terminalStatus, checkpoints[], and terminalGateNote
		//! when the terminal gate fired) to <runDir>/results.jsonl, where
		//! runDir is recovered from handle.trajectoryPath's (or, failing
		//! that, handle.resultPath's) parent directory -- a no-op when
		//! neither path is available (nothing was ever written for this
		//! run).  Multiple scenarios' results in the same runDir share
		//! one results.jsonl (append-only, like the trajectory's own
		//! JSONL contract).
		AgentEvalCheckResult CheckScenario( const AgentEvalRunHandle& handle, const AgentEvalScenario& scenario );

	}
}

#endif
