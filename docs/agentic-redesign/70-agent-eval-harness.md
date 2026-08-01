# Agent Eval Harness — Trajectories, Scenarios, Evals

> **Status:** original plan 2026-07-09; **E1–E5 SHIPPED** and reviewed to zero P1
> (trajectory recording, replay backend + runner, checker engine, live headless runner,
> metrics/report/diff). **E6 SHIPPED through the first full baseline** (updated 2026-07-15;
> see the §6 execution log for the detail): the seed suite grew to 13 scenarios across the
> block-A editing/recovery verbs plus objectmap-verified framing, lighting luma bands,
> propose-mode, multi-turn context, and long-horizon build; the whole suite was
> **calibrated** (a whole class of vague-prompt-vs-exact-checkpoint mismatches converted to
> `param_range`/structural checks), **wall-time completion was added as a first-class
> metric**, the harness was **hardened via two external-review P1 batches**, and a **first
> full 8-provider × 13-scenario × 3-repeat baseline (312 runs) completed** with a trustworthy
> leaderboard. Still outstanding coverage: vision/image-input and adversarial/malformed
> tool args as their own categories, and repeats 3→5 to tighten the confidence intervals.
> The body below is the as-designed plan; where it
> describes intended scope not yet built (e.g. the 12–20 scenario target), that is a target,
> not the current state. Grounded in two research
> passes: an in-tree audit of the chat loop / drivers / instrumentation, and an external
> prior-art survey (OTel GenAI conventions, LangSmith/Langfuse/Braintrust trace models,
> SWE-bench/WebArena/OSWorld scenario design, pass@k practice, LLM-judge pitfalls,
> multi-provider fairness). Design premises: dependency-free (JSONL via the in-tree Json
> codec; no OTel SDK, no hosted dashboards), single-developer scale (~20–50 scenarios,
> sized by failure-mode saturation), and the sans-IO chat loop as the load-bearing center.

## 0. The vision, restated

Every chat (GUI or headless) produces a **trajectory**: an append-only log of the full
context — system prompt, tool definitions, each LLM request/response, each tool call and
its JSON-RPC result, token usage, timings, errors — stored as one JSONL file per session.
**Scenarios** define RISE tasks (initial scene + instruction + programmatic success
checkers). An **eval runner** executes scenarios through real agents (Anthropic / Gemini /
OpenAI, N repeats) or through a **replay backend** (recorded/scripted responses, zero API
cost), scores the trajectories, and emits a comparison report. Failed trajectories feed a
debugging loop: label the failure, fix the scaffold (prompt / tool descriptions / skills),
and promote the trajectory to a permanent replay regression fixture.

## 1. What the research established

**Historical pre-E1 baseline (all claims were file:line-verified when the plan
was written; the shipped E1–E5 work supersedes the absences below):**
- `ChatTranscriptEntry` already holds the replay-faithful conversation (`rawJson`
  provider-native + verbatim byte-preserved for assistant turns, `displayText`) — the
  trajectory's core exists.
- The loop **discards** what a trajectory needs most: full HTTP request/response bodies,
  token usage (no codec parses `usage`/`usageMetadata` today), all timings, and the
  tool-call JSON-RPC lines. The GUI drivers (Mac URLSession, Windows QNetworkAccessManager,
  both 300s timeouts, no auto-retry) are the only place HTTP reality is visible.
- **No headless LLM path exists** (`--agent-stdio`/`--agent-http` never touch the chat
  loop), and **no TLS exists in-tree**. Precedented escape hatches: `popen`/`fork+exec`
  (four test files), and the system `curl` binary (in-box on macOS and modern Windows).
  `tools/*.py` has zero HTTP precedent — a Python runner would be a new idiom AND would
  fork the provider codecs (divergence risk).
- `AgentChatLoopTest` already demonstrates the **scripted-agent pattern**: canned provider
  bodies + a live dispatcher + real renders, zero network, zero keys.
- Eval instrumentation is unusually strong: `read_document`+`headVersion` (exact-match),
  `validate` diagnostic codes, render `meanR/G/B`+`renderMode` (tolerance-banded — MC +
  OpenPGL nondeterminism), `mode:"objectmap"` legends + `query_object_at`, the proposal
  audit trail, and the autonomy-refusal code (-32011) as a structural check.
- Hygiene: keys are header-only by construction; a recorder that captures request headers
  MUST strip the auth header explicitly. `tests/baselines_refactor/` is the
  "regenerate-per-session, never commit" storage precedent.

**Prior art (distilled, adoption-ready):**
- Adopt the **OTel GenAI field vocabulary** as JSONL key names (request+response model,
  input/output/cache-read tokens, finish_reasons, error.type) with **no SDK**.
- **Flat, append-only records** with `trace_id`/`parent_span_id`/`dotted_order` + a
  `run_type` discriminator (`llm`/`tool`/`user`/`history_edit`/`summary`) — what the
  mature systems converge on. Retries = sibling records (`attempt`, `retry_of`), never
  counters. One **summary line** per trajectory (n_turns, n_toolcalls, tokens, wall time,
  terminal status) for single-line regression diffs.
- Scenarios = (initial state, instruction, checker over FINAL STATE, not the tool path);
  **partial-credit checkpoints**; a **PASS_TO_PASS invariant** (untouched scene subtree
  unperturbed — a natural CST diff for us). Programmatic checkers first; LLM-judge only
  for subjective residue, later.
- **pass@1 headline + pass^k reliability gate** (unbiased estimator, N>k trials); Wilson
  intervals at small N; suite sized by failure-category saturation.
- **Record-once, replay-as-fixtures** is the highest-leverage CI pattern for a solo dev;
  tier: per-commit = replay only; nightly/manual = live multi-provider.
- 6-label failure taxonomy where each label implies a different scaffold fix:
  `wrong_tool` / `bad_args` / `hallucinated_state` / `constraint_violation` / `loop`
  (mechanically detectable) / `gave_up`.
- Multi-model fairness: ONE harness/prompt/tool-set (scaffold variance rivals model
  variance — documented 7-point swing); log forced provider deviations instead of faking
  parameter parity; normalize to **$-per-successful-task** (never tokens; count cache-read
  separately); N≥3 runs; pin model snapshot IDs.

## 2. Architecture

### 2.1 Trajectory recording — lives in the sans-IO loop, fed by both layers

New in `src/Library/Agent/` (attached to existing files where possible):

- **`ChatTrajectory`** — the schema: flat record structs (session_meta, user_msg,
  llm_call, tool_call, history_edit, summary) with OTel-vocabulary field names,
  `trace_id` (uuid) / `dotted_order` (timestamp+counter) / `run_type`; serialized to
  JSONL via the in-tree Json codec. Sans-IO: the recorder BUILDS records; a host-supplied
  sink callback persists lines.
- **Loop hooks** (`AgentChatLoop`): emits `user_msg` on AddUserMessage; emits
  `history_edit` on image elision (the loop knows exactly when it rewrites entries);
  emits `tool_call` records around ToolCallToJsonRpcLine/AddToolResult (capturing the
  JSON-RPC line + response envelope + post-call headVersion when available). New driver-
  called hook **`RecordHttpRound(requestSansAuth, httpStatus, rawBody, elapsedMs)`**
  invoked just before `HandleResponse` — the loop then owns the complete `llm_call`
  record, including **usage parsed from rawBody by the codecs** (new small codec method:
  `ParseUsage(rawBody) → {input, output, cacheRead}` for all three providers — the fields
  are already present in every real response and even in the test fixtures).
- **Redaction at the serialization boundary, unconditional**: the JSONL writer strips
  auth-header keys by name AND runs a fixed regex pass (sk-…, AIza…, bearer-shaped) over
  every line regardless of config. Red-prove in tests: a trajectory of a session with a
  known fake key never contains it.
- **GUI wiring**: both panels gain a "Record trajectories" toggle (default per §5 decision)
  writing to `evals/runs/gui/<timestamp>-<traceid>.jsonl`. Thin: the drivers already have
  the request/response/latency in hand; they call RecordHttpRound and hand the loop a
  file-sink.

### 2.2 The replay backend — the fourth "provider"

A **scripted-agent source** that yields canned LLM responses (from a fixture file or a
previously recorded trajectory) instead of HTTP. Runs the REAL loop, REAL dispatcher,
REAL renders — zero network, zero keys. This is: (a) the per-commit CI tier, (b) the
scaffold/regression isolator ("did I break the dispatcher, or did the model get worse"),
(c) the failed-trajectory replay debugger.

### 2.3 The headless live runner — `rise --agent-eval <run-config>`

New commandconsole mode: loads a scenario, constructs Job + AgentSession + dispatcher +
AgentChatLoop in-process, drives the turn loop headlessly. **HTTPS via the system `curl`
binary** (popen; body via temp file; `-w` for status+timing; env-var keys only, never
Keychain; the same fork/exec shape four tests already use). Honest failure when curl is
absent. Budgets enforced per scenario (max tool calls / tokens / wall time). Emits the
trajectory + a result line. (Alternative transports considered: a Python driver — rejected
for codec divergence + no idiom precedent; platform TLS APIs — rejected as per-platform
work for no gain over curl. Decision point §5.)

### 2.4 Scenarios + checkers

- **`evals/scenarios/*.json`** (committed): `{id, title, scene (path|inline), autonomy,
  prompts[] (multi-turn capable), budgets, config?: {seed}, checkpoints[]}`.
- **Checkpoint kinds** (each weighted, partial credit):
  - `document`: param equals / chunk exists / chunk absent (via read_document + CST
    reparse); `untouched`: the PASS_TO_PASS subtree guard (named chunks byte-identical).
  - `render`: mean luma / channel bands at a pinned seed + dims (tolerance-banded;
    reuses the variance-protocol philosophy).
  - `objectmap`: legend contains name; pixelCount in range; `query_object_at(x,y)` hits
    an expected object (the observe toolkit as ground truth).
  - `diagnostics`: validate returns clean / returns a specific code.
  - `trajectory`: structural — max tool calls, no autonomy refusals, required-call-order
    (strict mode used sparingly), no mechanical `loop` label, terminal status.
- **Checker engine** runs post-trajectory against the live session (in-process) and
  writes `results.jsonl`: per-checkpoint pass + fraction + all-pass bit.

### 2.5 Runner matrix, metrics, report

- Run config: scenarios × provider/model configs × N repeats (default 3), sequential
  (one render slot; also keeps costs legible).
- `evals/runs/<timestamp>/` (gitignored): trajectories + `results.jsonl` per run. Reporting
  is a SEPARATE step: `tools/eval_report.py <run-dir>` prints a table to stdout (text, or
  `--format markdown` for a Markdown table); it does not auto-write a `report.md`.
- Metrics: pass@1 (headline), pass^k, checkpoint fractions, Wilson intervals, tokens by
  class, **$-per-successful-task** (price table in a small committed config), wall time,
  tool-call counts, failure labels (v1: mechanical `loop` + budget/refusal-derived labels;
  the rest hand-assigned in a `notes` field — no judge in v1).
- Report + trajectory-diff live in `tools/eval_report.py` (offline JSONL crunching — the
  one place Python is idiomatic; no HTTP).

### 2.6 CI integration

`tests/AgentEvalReplayTest.cpp`: runs 2–3 committed fixture scenarios through the replay
backend end-to-end (loop + dispatcher + renders + checkers) — keyless, deterministic,
red-provable. Every real-world failure trajectory that leads to a scaffold fix gets
promoted to `evals/fixtures/` and joins this test.

## 3. Slices (each through the implementation-review-loop to zero P1)

| Slice | Scope | Tier |
|---|---|---|
| **E1** | Trajectory schema + recorder + loop hooks + codec ParseUsage + redacting JSONL writer + GUI toggles (Mac/Windows) + tests incl. redaction red-prove | Opus (concurrency-adjacent loop surgery + security boundary) |
| **E2** | Replay backend + scenario format/loader + in-process scenario execution + AgentEvalReplayTest with 2–3 seed fixtures | Sonnet |
| **E3** | Checker engine (document/untouched/render/objectmap/diagnostics/trajectory) + results.jsonl + partial credit | Sonnet |
| **E4** | `--agent-eval` live runner: IChatHttpTransport + platform TLS legs (Mac NSURLSession .mm — verify make OBJCXX support FIRST; Windows WinHTTP), budgets, provider matrix, N repeats | Opus (platform transport + key hygiene) |
| **E5** | Metrics + report + trajectory diff (`tools/eval_report.py`), price table | Sonnet |
| **E6** | Seed scenario suite (~12–20 across categories: param edit, add/remove chunk, camera framing w/ objectmap verify, lighting w/ luma band, multi-step build, propose-mode, fix-the-broken-scene) + first 3-provider baseline run + failure-label pass | Sonnet, be-the-agent review |
| **E7** | Docs: skills cross-refs + this doc reconciled to shipped reality | inline |

Dependencies: E1 → E2 → E3 → {E4, E5} → E6. E2+E3 alone already deliver keyless CI evals;
E4 unlocks live model comparison.

## 4. Risks / honest notes

- **curl subprocess** is the plan's one new runtime dependency (a binary, not a library);
  Windows needs `curl.exe` presence-checked with an honest failure. If this ever chafes,
  the transport is one narrow interface — swappable later.
- **GUI trajectory recording** captures user scene content to disk — default-off toggle
  unless decided otherwise (§5); the redaction pass runs regardless.
- Render checkers inherit MC tolerance; scenario authors must use bands (the checker API
  won't offer exact-match on pixels at all — deliberately).
- Multi-turn scenario prompts run open-loop in v1 (scripted user turns, no adaptive user
  simulation) — adaptive users are a later, separate question.
- LLM-as-judge is deliberately deferred; the taxonomy's manual labels + mechanical checks
  come first, judge calibration later if subjective scenarios appear.

## 5. Decisions (user, 2026-07-09)

1. **Headless HTTPS transport: PLATFORM TLS APIs.** A narrow `IChatHttpTransport`
   interface in shared C++ (`src/Library/Agent/`), with per-platform implementations:
   macOS = NSURLSession via a small ObjC++ TU; Windows = WinHTTP (pure C API, links
   `winhttp.lib`). Linux headless live-runs are honestly unsupported in v1 (no platform
   TLS without a library dependency) — the replay backend still works everywhere.
   E4 risk checkpoint: the make build must compile a `.mm` for the Mac CLI leg (no `.mm`
   exists in the make build today — the implementer verifies OBJCXX rules + `-framework
   Foundation` linkage first, before building on it). Optional later unification: the GUI
   drivers (URLSession in Swift / QNetworkAccessManager) could migrate onto the same
   transport interface — out of scope for this arc.
2. **GUI recording: ON BY DEFAULT, with rotation.** Every chat session writes a
   trajectory; a size/count cap prunes oldest (keep last ~50 sessions or ~200 MB,
   whichever first); a settings toggle can disable. Redaction remains unconditional.
3. **v1 scope: full E1–E6**, ending with the first three-provider baseline over the seed
   scenario suite.

## 6. Execution log — E6 completion & the first full baseline (2026-07-14/15)

This is the as-built record of taking E6 from "8 scenarios + a first run" to a
trustworthy, calibrated, 8-provider baseline. It sits alongside the compaction +
prompt-cache work of the same arc (design + shipped status in `71-context-compaction-design.md`;
that work — Anthropic `cache_control:ephemeral` breakpoints on the tools+system prefix,
and the S1 token estimator + S2 structural span-dropper — is not re-described here).

### 6.1 Harness hardening — two external-review P1 batches

An external reviewer pass surfaced correctness defects in the runner/metrics. Both
batches were fixed to zero-P1 and the fixes were gated by `AgentEvalCheckTest` (the
replay-truth suite) + `AgentEvalLiveTransportTest`:

- **Batch 1**
  - *Terminal status recorded, not gated.* `AgentEvalRunResult` now carries the
    `terminalStatus`; the report surfaces it, but a non-`final_text` terminal does not
    silently pass/fail — the checker decides (user decision: "record but don't gate").
  - *Inner-loop budget check.* The per-turn loop re-checks the token/call budget so a
    scenario can't overrun between turns.
  - *Strict checkpoint-field-type loader (fail-fast).* `ValidateCheckpointFieldTypes`
    hard-errors on a malformed scenario at load instead of silently skipping a checkpoint
    (user decision: "hard load error").
  - *`$/success` honesty.* An unlisted hosted model now reports **unpriced** rather than
    falling through to a generic provider `_default` rate — a wrong number is worse than
    an honest `n/a`. The old selftest had *blessed* the bug (asserted a model priced via
    `_default`); code and tests were corrected together.
- **Batch 2**
  - *Transport-retry could exceed `maxLlmCalls`.* `++llmCalls` was incremented only after
    a *received* response, so a transport-failed POST (which the provider may still bill)
    bypassed the cap. Fixed to count **every POST attempted**. The existing transport tests
    had **encoded** the bug (asserted `llmCalls==0` for 2 transport POSTs); four assertions
    were corrected and a `maxLlmCalls:1 → exactly 1 POST` regression added.
  - *Live pass@1 counted incomplete runs as success.* Added an opt-in per-scenario
    `trajectory.terminalStatus:"final_text"` checkpoint so a run that ends on
    `budget_llm_calls`/`provider_error` mid-task no longer scores as a pass.

Recurring process note (worth remembering): delegated build-and-verify workers repeatedly
produced correct code but **yielded on backgrounded builds** ("I'll wait for the build
notification") instead of finishing in the foreground; the supervisor gated every build
personally. Data-only (JSON-editing) workers did not hit this.

### 6.2 Wall-time as a first-class metric

Per user directive ("add wall time for completion in this shootout and all comparisons
going forward; since we're doing wall time we can't parallelize — serialize them"):
`wallMs = endMs − startMs` (wall-clock via `TrajectoryNowMs`) is recorded per run and
reported as a **mean `wall(s)` column** in `eval_report.py` (text + markdown), alongside
pass@1 / pass^k / `$/success`. **Runs are serialized** (no parallelism) so wall-times are
comparable across providers. This immediately earned its keep: it exposed that qwen3:32b
nearly matches qwen3.6 on accuracy but is ~3× slower, and that gpt-5.6 wins the
gpt/gemini accuracy tie purely on speed (10s vs 27s).

### 6.3 Scenario calibration — the baseline's real first job

The first full run's dominant finding was **not** about the models — it was that the
*measurement* needed calibrating. Three defect classes, each masquerading as model
performance, were found and fixed (all gated by `AgentEvalCheckTest` staying green):

1. **Budgets floored to the replay fixtures.** Seed `maxLlmCalls`/`maxToolCalls` were sized
   to the minimal one-response-per-turn fixtures, so a *live* model correctly running the
   `read_document → propose_patch → render → read_image` verify loop hit `budget_llm_calls`
   mid-verification and scored a correct-but-incomplete trajectory. Fix: raise per-scenario
   budgets to fit the full verify loop, add a `maxWallMs` backstop, and drop the over-tight
   `trajectory.maxToolCalls` efficiency gate (efficiency stays visible via the report's
   toolCalls/`wall(s)` columns).
2. **Vague prompt + exact-value checkpoint (6 scenarios).** e.g. prompt "recolor to red"
   with `param_equals color == "0.9 0.1 0.1"`. A model that set a *perfect* red (`1 0 0`) or
   a reasonable radius (`1.0`) scored **0** for not guessing the author's magic constant —
   proven from trajectories (llama3.3 set `1.0 0.0 0.0`; qwen3 set radius `1.0`). Fix:
   component-wise **`param_range`**, mirroring the one color scenario already done right
   (`conflict_retry`'s blue-dominant range). Red-dominant range `R≥0.5,G≤0.45,B≤0.45`
   (grey no-op still fails); radius "bigger" range; `lighting_luma_band` scale → a *loose*
   `[12,600]` range deliberately calibrated **below** the render `meanLuma` band's effective
   floor, so the render band (the scenario's namesake, correctly calibrated) stays the real
   "much brighter" gate.
3. **Name-over-specification (`multi_step_build`).** A build-from-scratch prompt whose
   checkpoints required the created objects be named *exactly* `obj_sph`/`obj_emit`/`sph` —
   names no model can guess from scratch. gpt-5.6 built a correct 2-object lit scene and
   scored 0 on the three name-gated `chunk_exists` checks. Fix: name-agnostic structural
   checks (`sphere_geometry` count ≥1, `standard_object` count in `[2,4]`), keeping the
   render `[0.015,0.2]` "clearly visible when rendered" band (which the prompt explicitly
   asks for and which stays a genuine discriminator — from-scratch exposures ranged 0.005
   to 25.4).

**Meta-lesson (belongs in the same family as the CLAUDE.md measurement-artifact rule):**
*the first baseline's first job is to calibrate the harness, not to rank the models.* A
leaderboard is only trustworthy once the measurement has itself been measured — here by
cross-checking each surprising 0% against the actual trajectory before believing it.

### 6.4 The first full baseline — 8 providers × 13 scenarios × 3 repeats (312 runs)

Providers at the time of this baseline: 4 hosted — `anthropic/claude-opus-4-8`,
`gemini/gemini-3.5-flash`, `openai/gpt-5.6-terra`, `xai/grok-4.5` — and 4 local (Ollama):
`qwen3.6:27b`, `qwen3:32b`, `qwen3-coder:30b`, `llama3.3:70b`. (Later work — a *different*
chat — added `glm-4.7-flash` / `qwen3-coder-next` locally and the `reserved_name_clarify` /
`malformed_tool_recovery` scenarios; those post-date this baseline and are not part of the
numbers below.) Split runconfigs (`full_baseline_local.json` keyless, `full_baseline_hosted.json`)
write into one shared `evals/runs/full_baseline` dir; runs serialized.

Leaderboard (pass@1, all 13 scenarios; `rel` = scenarios passed all-3; overall pass@1 63.8%):

| # | Provider · Model | pass@1 | rel | mean wall | cost |
|---|---|---|---|---|---|
| 1 | openai gpt-5.6-terra | 79.5% | 9/13 | 10.1s | unpriced |
| 2 | gemini gemini-3.5-flash | 79.5% | 9/13 | 27.4s | unpriced |
| 3 | xai grok-4.5 | 76.9% | 7/13 | 12.5s | unpriced |
| 4 | local qwen3.6:27b | 71.8% | 5/13 | 90.5s | $0 |
| 5 | anthropic claude-opus-4-8 | 71.8% | 7/13 | 22.4s | $0.25/succ (~$6.89) |
| 6 | local qwen3:32b | 61.5% | 7/13 | 250.8s | $0 |
| 7 | local qwen3-coder:30b | 48.7% | 4/13 | 44.0s | $0 |
| 8 | local llama3.3:70b | 20.5% | 1/13 | 54.6s | $0 |

Findings:
- **Bigger ≠ better.** The 42 GB llama3.3:70b is last (20.5%), beaten by every 17–20 GB qwen;
  newer tool-use tuning dominates raw parameter count on this structured-editing task.
- **pass@1 ties hide reliability + speed.** gpt beats gemini on the 79.5% tie via speed
  (10s vs 27s); opus beats qwen3.6 on the 71.8% tie via reliability (7/13 vs 5/13) *and*
  speed (22s vs 90s).
- **A cross-lab failure mode.** gpt, opus, **and** grok all score **0/3 on
  `reserved_name_recovery`** (asked to name a camera the reserved `'none'`, they get
  cautious/stuck) while gemini and the qwen models recover by renaming — three different
  labs, same behavior. A concrete lead for improving how the agent surfaces the
  reserved-name conflict.
- **Interpretation variance.** grok nails `lighting_luma_band` 3/3; opus and gemini overshoot
  the band (opus over-brightens "much brighter" to meanLuma 0.16–0.26 vs the 0.13 ceiling).
- **`multi_step_build` (build-from-scratch) is the discriminator** at 21% — even frontier
  models mostly loop on chunk syntax or misexpose; a good hard ceiling for the suite.
- **Cost honesty.** Only opus is priced (its `claude-opus-4-8` entry added to
  `eval_report.py`: $5/$25 per 1M, cached $0.50 — the generic `claude-opus` key was stale
  Opus-3-era $15/$75 and would mis-price 4.8 3× high). gemini/gpt/grok are new model IDs
  absent from the table → honestly reported `n/a` rather than guessed.
- **One measurement artifact caught live:** grok's first run was 39/39 `provider_error` at
  ~0.1s — an **invalid `XAI_API_KEY`**, not a 0% model. Cleared the stale rows, re-ran on a
  valid key → 76.9% (3rd). The lesson from §6.3 in miniature: a 0% that's really a config
  failure must be recognized before it pollutes the board.

### 6.5 Provider keys — operational

Runconfigs reference keys by `keyEnvVar` (`ANTHROPIC_API_KEY` / `GEMINI_API_KEY` /
`OPENAI_API_KEY` / `XAI_API_KEY`); a missing key **skips** that provider (no hard fail).
Secrets live only in a **gitignored** `evals/.secrets.env` the user sources before a hosted
run (`.env` / `*.secrets.env` / `evals/.secrets.env` added to `.gitignore`); raw key values
are deliberately **never** committed or written into agent memory (the boundary held even
when the user asked to "remember the keys" — the correct fulfillment was remembering the
provider→env-var mapping and run flow, not the secret values). This convention is mirrored
in the two auto-memory files (`eval-hosted-provider-keys`, `feedback-eval-key-handling`).

### 6.6 Open follow-ups (deferred, not yet done)

- Add `gemini-3.5-flash` / `gpt-5.6-terra` / `grok-4.5` pricing so `$/success` ranks across
  all hosted models (env keys still valid).
- Bump repeats 3→5 and re-run to tighten the wide Wilson intervals (N=3 gives ±~25pp).
- Investigate `reserved_name_recovery` — three labs trip on the reserved `'none'`; trace how
  the agent surfaces that conflict.
- Compaction **S3** (Option-B head-anchored hybrid, gated on the long "torture" scenario per
  `71` §6) and **S4** (optional LLM-summary layer); harness P2 backlog (rubric-tightening,
  reproducibility recording, vision/cancellation coverage categories).
