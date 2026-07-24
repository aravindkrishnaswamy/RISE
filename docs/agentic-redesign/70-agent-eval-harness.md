# Agent Eval Harness — Trajectories, Scenarios, Evals

> **Status:** original plan 2026-07-09; **E1–E5 SHIPPED** and reviewed to zero P1
> (trajectory recording, replay backend + runner, checker engine, live headless runner,
> metrics/report/diff). **E6 partial** (updated 2026-07-14): 8 seed scenarios committed
> (block A — editing verbs + recovery) and a first multi-provider baseline run done; the
> remaining coverage categories (objectmap-verified framing, lighting luma bands,
> propose-mode, multi-turn context, vision, long-horizon assembly, adversarial/malformed
> tool args) are still outstanding. The body below is the as-designed plan; where it
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
