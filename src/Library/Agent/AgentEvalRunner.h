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
#include "Json.h"

namespace RISE
{
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

		//! One parsed evals/scenarios/*.json.  See the file header for the
		//! schema; `checkpoints` is carried OPAQUELY (E3's job to
		//! interpret).
		struct AgentEvalScenario
		{
			std::string id;
			std::string title;

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
			//! (an honest budget stop), "replay_exhausted" (the source ran
			//! out of canned bodies before the scenario's turns did),
			//! "provider_error" (HandleResponse returned ProviderError --
			//! see errorMessage), or "load_error" (the scenario/scene/
			//! replay source itself failed to load -- see errorMessage;
			//! no turns ran).
			std::string terminalStatus;

			int  llmCalls  = 0;    //!< total LLM (replay) rounds actually driven
			int  toolCalls = 0;    //!< total tool calls actually dispatched
			bool budgetHit = false;   //!< true iff terminalStatus is one of the three budget_* stops

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
			std::unique_ptr<AgentRpcDispatcher> dispatcher;

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
		//! small its weight, means NOT allPassed).
		struct AgentEvalCheckResult
		{
			std::string scenarioId;
			std::vector<AgentEvalCheckpointResult> checkpoints;
			double checkpointFraction = 1.0;
			bool   allPassed = true;
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
		//! allPassed, checkpoints[]) to <runDir>/results.jsonl, where
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
