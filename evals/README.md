# RISE agent eval harness

This directory holds the on-disk inputs (and, locally, the outputs) for
RISE's agent eval harness: an automated way to drive the LLM scene-editing
chat loop ([src/Library/Agent/AgentChatLoop.h](../src/Library/Agent/AgentChatLoop.h))
through a scripted **scenario** — a scene, a sequence of user prompts, and a
set of pass/fail **checkpoints** — and grade the result. The C++ core lives
in [src/Library/Agent/AgentEvalRunner.{h,cpp}](../src/Library/Agent/AgentEvalRunner.h);
this file documents the on-disk formats and the CLI/report workflow around it.

## Replay vs. live

There are two ways to drive a scenario, sharing one drive loop
(`RunScenarioDriven` in `AgentEvalRunner.cpp`):

- **Replay** (`RunScenario`): the LLM side is a canned sequence of response
  bodies loaded from an `evals/fixtures/*.fixture.jsonl` file — no network,
  no API key, no `getenv` call anywhere in `AgentEvalRunner.cpp`. This is
  what CI runs (see [tests/AgentEvalCheckTest.cpp](../tests/AgentEvalCheckTest.cpp)
  T10, below): every committed scenario is replayed against its own fixture
  and its checkpoints are asserted to actually pass, so a scenario can never
  silently rot into "plausible JSON that doesn't check what it claims to."
- **Live** (`RunScenarioLive` / `RunEvalMatrix`): each LLM round is a real
  HTTPS POST through a platform-TLS transport (NSURLSession on macOS, WinHTTP
  on Windows; an honest "unsupported" stub on Linux), driven by
  `rise --agent-eval <runconfig.json>` (see below). This is what you run to
  actually evaluate a model/provider.

Both paths dispatch tool calls through the same real `AgentRpcDispatcher`
over a real (CST-loaded) scene, emit the same E1 trajectory format, and are
graded by the same checker (`CheckScenario`). Only the body source differs.

## Directory layout

```
evals/
  scenarios/    committed scenario definitions (evals/scenarios/*.json)
  fixtures/     committed canned-response fixtures (evals/fixtures/*.fixture.jsonl)
  runconfigs/   committed run configs for `rise --agent-eval` (evals/runconfigs/*.json)
  perception_ab/ paired local-vision cue-isolation manifest + methodology
  runs/         LIVE run output (gitignored — see .gitignore; never commit this)
```

The C++ core: [src/Library/Agent/AgentEvalRunner.h](../src/Library/Agent/AgentEvalRunner.h) /
`.cpp` (replay source, scenario loader, runner, run-config + matrix, checker
engine), [src/Library/Agent/ChatTrajectory.h](../src/Library/Agent/ChatTrajectory.h)
(the trajectory schema/recorder every run emits), and
[src/Library/Agent/AgentChatLoop.h](../src/Library/Agent/AgentChatLoop.h) (the
sans-IO chat loop + provider codecs both paths drive). The reporting tool:
[tools/eval_report.py](../tools/eval_report.py).

## How to run

### Live: `rise --agent-eval <runconfig.json>`

```sh
export ANTHROPIC_API_KEY=sk-ant-...
./bin/rise --agent-eval evals/runconfigs/smoke_anthropic.json
```

(`--agent-eval=<path>` also works.) This is a batch runner, not a live chat
session — it cannot be combined with `--agent-stdio`/`--agent-http`. Progress
and the final summary go to **stderr**; the machine-readable output is the
per-run JSONL files written under the run config's `runDir`.

#### The run-config JSON schema

```jsonc
// evals/runconfigs/smoke_anthropic.json
{
  "scenarios": [ "evals/scenarios/param_edit.json" ],  // >= 1 path or glob; CLI expands globs, de-dupes by canonical path
  "providers": [
    { "provider": "anthropic", "model": "claude-sonnet-4-5", "keyEnvVar": "ANTHROPIC_API_KEY" }
  ],
  "repeats": 1,        // optional, default 3, >= 1
  "runDir": "evals/runs/smoke_anthropic"   // required, non-empty
}
```

Fields (`LoadEvalRunConfig`, `AgentEvalRunConfig`/`AgentEvalProviderConfig` in
`AgentEvalRunner.h`):

| field | required | notes |
|---|---|---|
| `scenarios` | yes, >= 1 | path or `*`/`?` glob string; the CLI expands globs against the filesystem and de-dupes by canonical path |
| `providers` | yes, >= 1 | array of `{provider, model?, keyEnvVar?}` |
| `providers[].provider` | yes | one of `"anthropic"`, `"gemini"`, `"openai"`, `"xai"`, `"local"` |
| `providers[].model` | no | model id string; empty/omitted uses the provider codec's default |
| `providers[].keyEnvVar` | **required for every provider except `"local"`** | the ENV VAR NAME to read the api key from at run time — never the key itself. Present-but-empty is a load error. Only `"local"` may omit it (a keyless local server sends no `Authorization` header) |
| `repeats` | no, default 3 | must be >= 1 |
| `runDir` | yes, non-empty | e.g. `evals/runs/<stamp>` — gitignored |

`evals/runconfigs/local_shootout.json` shows the full matrix shape: 4
scenarios × 8 provider/model rows (gemini, five `local` Ollama models, openai,
xai) × 3 repeats.

### Local perception-atlas A/B

The perception-atlas experiment is deliberately separate from the general
tool-using scenario matrix. It needs to force one image observation per answer,
disable expensive hidden reasoning, and keep the beauty/atlas image-token budget
identical. It still drives the real RISE `render` and `read_image` JSON-RPC
surface, then sends each PNG to the installed local vision model:

```sh
python3 tools/perception_ab_eval.py
```

See [perception_ab/README.md](perception_ab/README.md) for the paired design and
[../docs/PERCEPTION_ATLAS_AB_RESULTS.md](../docs/PERCEPTION_ATLAS_AB_RESULTS.md)
for the first measured result. Live artifacts remain under gitignored
`evals/runs/` like the general harness.

#### Env-var keys per provider

| provider | env var |
|---|---|
| `anthropic` | `ANTHROPIC_API_KEY` |
| `gemini` | `GEMINI_API_KEY` |
| `openai` | `OPENAI_API_KEY` |
| `xai` | `XAI_API_KEY` |
| `local` | none (keyless by design) — base URL comes from `RISE_LOCAL_LLM_BASE_URL`, falling back to Ollama's default `http://127.0.0.1:11434/v1/chat/completions` (see `MakeCodec` in `AgentChatLoop.cpp`) |

The api key is read **only** in `commandconsole.cpp`'s `RunAgentEval`, only
via `getenv` on each provider's declared `keyEnvVar`, and handed to
`RunEvalMatrix` as an injected `envLookup` callback — `AgentEvalRunner.cpp`
itself never calls `getenv`. A provider whose env var is unset or empty
**skips that whole provider column** (logged to stderr, never a crash, never
an empty `Bearer` header sent live).

#### Resume / skip-if-completed

Re-running the same run config into the **same** `runDir` is idempotent:
before executing a `(scenario, provider, repeat)` cell, `RunEvalMatrix` checks
its target subdirectory for an already-present, non-empty
`<scenarioId>.result.jsonl`. If found, the run is skipped
(`runsAlreadyComplete`) rather than re-executed — so adding a provider column
later (export a new key, re-run) only executes the newly-added cells. A
subdirectory that exists but has no (or an empty) result file is treated as a
crashed/interrupted run: it is wiped and re-run (this avoids the trajectory
sink's append-mode silently concatenating two sessions into one file).

#### Where output lands

```
<runDir>/<scenarioId>__<provider>[__<model>]__r<repeat>/
    <scenarioId>.trajectory.jsonl   E1 trajectory (see ChatTrajectory.h)
    <scenarioId>.result.jsonl       one-line run result (scenarioId, terminalStatus,
                                     llmCalls, toolCalls, budgetHit, headVersionStart,
                                     headVersionFinal, finalText, errorMessage?)
    results.jsonl                  E3 checker output, appended (scenarioId,
                                     checkpointFraction, allPassed, checkpoints[])
```

`__` is the literal directory-name separator; the model segment is present
only when the provider config supplied a non-empty model string; `r<repeat>`
is 1-based, not zero-padded.

## Reading results: `tools/eval_report.py`

```sh
python3 tools/eval_report.py report evals/runs/2026-07-10          # text table
python3 tools/eval_report.py report evals/runs/2026-07-10 --json
python3 tools/eval_report.py report evals/runs/2026-07-10 --markdown
python3 tools/eval_report.py diff runs/s1__anthropic__r1/s1.trajectory.jsonl \
                                  runs/s1__openai__r1/s1.trajectory.jsonl
python3 tools/eval_report.py --selftest   # unit-checks the pure math; no runDir needed
```

`report` aggregates every run subdirectory under `<runDir>`, grouped by
`(scenarioId, provider, model)`, and prints per group:

- **pass@1** — mean success rate over the group's repeats (`successes/n`).
- **wilson95%** — a 95% Wilson score interval on that pass@1 proportion.
- **pass^k** — 1 iff every repeat in the group passed (an "all-repeats-pass"
  reliability indicator), else 0.
- **meanCkpt** — mean `checkpointFraction` (partial credit) across the
  group's repeats.
- **$/success** — `estimated_cost_usd / successes`, or `"n/a"` variants when
  unpriced/no successes. An **OVERALL** line pools `sum(successes)/sum(n)`
  across all groups (not a naive mean-of-per-group pass@1 — those differ
  whenever group sizes differ).
- **failureBreakdown** — a count per distinct `terminalStatus` seen in the
  group.

`diff` aligns two trajectory files record-by-record (via `difflib`) and calls
out where they diverge: the tool-call sequence, LLM round count, terminal
status, and final text (read from each trajectory's sibling
`*.result.jsonl`, if present in the same directory).

**Pricing is an editable estimate**: `PROVIDER_PRICING` in `eval_report.py`
is a hand-maintained USD-per-1M-token table (marked `EDIT ME` in the file),
keyed by provider with per-model substring overrides and a `cached_in_input`
flag capturing whether that provider reports cached tokens as a subset of
`input_tokens` (openai, gemini — non-additive) or additively/disjoint
(anthropic). An unpriced provider/model reports cost as `"n/a (no pricing)"`
rather than crashing or guessing.

## Authoring a scenario

A scenario is one `evals/scenarios/<id>.json`. Required top-level fields
(`LoadEvalScenario` in `AgentEvalRunner.cpp`):

| field | notes |
|---|---|
| `id` | non-empty **bare token** — no `/`, `\`, or `..` (used to build filesystem paths under the run dir) |
| `title` | non-empty string |
| `scene` | object with **exactly one** of `path` (loaded as-is) or `inline` (scene-language text, written to a throwaway temp file under `<runDir>/tmp/`, deleted immediately after load) |
| `autonomy` | optional, default `"commit"`; one of `"commit"` / `"propose"` / `"read"` |
| `prompts` | non-empty array of sequential user turns — each entry is either a bare string (text only) or an object `{"text"?, "images"?}` (see below) |
| `budgets` | optional `{maxToolCalls?, maxLlmCalls?, maxWallMs?}`, each an honest stop: the call that would exceed the budget is never dispatched/sent |
| `replay.fixture` | path to an `evals/fixtures/*.fixture.jsonl` (required for the replay path unless the caller supplies `replaySourceOverride`) |
| `checkpoints` | optional array of checkpoint objects (see below) |
| `interventions` | optional array of scripted co-editor edits (see below) |

Example — `evals/scenarios/material_add_and_bind.json` (elided scene text):

```jsonc
{
  "id": "material_add_and_bind",
  "title": "Give the sphere a metallic look (insert material + rebind)",
  "scene": { "inline": "RISE ASCII SCENE 7\n..." },
  "autonomy": "commit",
  "prompts": [ "Give the sphere a shiny metallic look." ],
  "budgets": { "maxToolCalls": 7, "maxLlmCalls": 8 },
  "replay": { "fixture": "evals/fixtures/material_add_and_bind.fixture.jsonl" },
  "checkpoints": [
    { "kind": "document", "op": "param_references_kind", "target": "obj_sph", "param": "material",
      "referencedKinds": ["pbr_metallic_roughness_material", "ggx_material",
                           "cooktorrance_material", "perfectreflector_material"] },
    { "kind": "untouched", "chunks": [ { "chunkKind": "pinhole_camera", "name": "" },
                                       { "name": "obj_emit" }, { "name": "mat_emit" } ] },
    { "kind": "diagnostics", "expect": "clean" },
    { "kind": "trajectory", "maxToolCalls": 7,
      "requiredToolInOrder": ["read_document", "read_schema", "insert_chunk", "propose_patch"],
      "noMechanicalLoop": true }
  ]
}
```

### Reference-image prompts (image-reconstruction Wave 1)

A `prompts[i]` entry may carry reference images instead of (or alongside)
plain text, for scenarios that ask the agent to reconstruct or match a scene
from a photo:

```jsonc
{ "text": "Match this reference photo.", "images": [ "evals/refs/example.png" ] }
```

- `text` is optional when `images` is non-empty (an attachment-only turn);
  at least one of `text` / `images` is required.
- `images` is a non-empty array of repo-relative (or absolute) paths,
  resolved against the CWD exactly like `scene.path`. `..` and empty path
  strings are rejected at load time.
- Only `.png`, `.jpg`, and `.jpeg` are supported; any other extension is a
  loud `load_error` (`unsupported reference-image type`) before any LLM
  round runs.
- Every referenced file is pre-flight-loaded UP FRONT, right after the
  scene loads and before any prompt's turn is driven — an unreadable path
  in prompt 3 is caught before prompt 1 ever runs, so a run never burns
  partial progress on a scenario that was going to fail to load anyway.
- The image's bytes are folded into `scenarioContentHash` (the matrix
  resume/staleness guard) and into a `refImageFnvs` map stamped on
  `<id>.result.jsonl` — editing a reference image in place invalidates a
  cached matrix cell and marks past results STALE (`tools/eval_report.py`
  `check_staleness`), exactly like editing the scene file in place.
- Text-only scenarios are completely unaffected: a bare-string `prompts[i]`
  entry (the pre-Wave-1 shape) still parses and hashes byte-identically.

The two seed scenarios that exercise this end-to-end,
`evals/scenarios/image_reconstruct_single.json` (one reference photo) and
`evals/scenarios/image_reconstruct_multi.json` (four multi-view reference
photos, ground truth + poses + calibration under `evals/references/`), live
in their **own** runconfig, `evals/runconfigs/image_reconstruct.json`,
rather than in `full_baseline`/`local_shootout`: four of the five local Ollama
shootout models are text-only, so mixing a vision-required scenario into that
matrix would silently starve those columns of usable input. The hosted board is
`image_reconstruct.json`; the sole installed vision-capable local model has its
own `image_reconstruct_local.json` runconfig.

### The raw fixture format

`evals/fixtures/*.fixture.jsonl` is JSONL, one canned LLM response per line:

```json
{"provider": "anthropic", "body": "{\"id\": \"msg_1\", \"type\": \"message\", ...}"}
```

`provider` must be the same on every line of one fixture (a replay source is
single-provider); `body` is the exact raw response body text the provider
codec parses. `AgentEvalReplaySource::LoadFromFile` also accepts the other
shape — a previously-recorded E1 trajectory JSONL (the exact file
`ChatTrajectoryRecorder` emits) — auto-detected by the presence of a
`run_type` field on the first line; it extracts the `llm` records'
`response_body` fields in file order and the `session` record's `provider`.
This is the "record a real session once, replay it forever" path. A
trajectory that switches provider mid-file (a live provider switch) is
refused loudly — split it at the session boundary and replay one
provider-segment at a time.

### Checker ops reference

Every checkpoint is `{"kind": ..., "weight"?: <number, default 1.0>, ...}`.
`checkpointFraction` is the weight-normalized pass fraction across all of a
scenario's checkpoints (falls back to the unweighted fraction if every
checkpoint has weight 0; vacuously 1.0 for zero checkpoints); `allPassed` is
true iff **every** checkpoint passed regardless of weight. An unrecognized
`kind`, or a malformed checkpoint shape, is a **failed** checkpoint carrying
an explanatory `detail` — never a silent skip.

| kind | op / field | asserts |
|---|---|---|
| `document` | `op:"param_equals"` — `target`, `param`, `value`, `chunkKind?`, `numeric?` | `target` chunk's `param` value string-equals `value`. `chunkKind` narrows the by-name chunk lookup (needed when more than one chunk shares a bare name across kinds). `numeric:true` tokenizes both sides as space-separated floats and compares component-wise within `1e-4` (so `"0.0 0.0 1.0"` grades equal to `"0 0 1"`). **Any-of-kind**: omit `target` and supply `chunkKind` to pass iff ANY chunk of that kind satisfies the assertion (existential; the detail reports how many were examined) |
| `document` | `op:"param_range"` — `target`, `param`, `min:[...]`, `max:[...]`, `chunkKind?` | `target.param`'s float tokens are each within the matching `[min_i, max_i]` band, component-wise — a tolerant "any reasonable blue" grade instead of one fixture's exact numbers. Also supports the **any-of-kind** target-less form (omit `target`, supply `chunkKind`) |
| `document` | `op:"chunk_exists"` — `chunkKind?`, `name` | a chunk named `name` (optionally narrowed by `chunkKind`) exists exactly once |
| `document` | `op:"chunk_absent"` — `chunkKind?`, `name` | no chunk named `name` exists (narrowed the same way) |
| `document` | `op:"chunk_count"` — `chunkKind` (required), `min?`, `max?` (at least one required) | the count of top-level chunks whose keyword is exactly `chunkKind` falls in `[min, max]`. For UNNAMED, non-singleton kinds (e.g. `timeline`) where a by-name lookup would be ambiguous |
| `document` | `op:"param_references_kind"` — `target`, `param`, `referencedKind` **or** `referencedKinds:[...]` (exactly one), `chunkKind?` | `target.param`'s value names an existing chunk of `referencedKind`, or of ANY kind in the `referencedKinds` any-of array — grades a correctly-typed binding without caring what name the model chose or which of several equivalent material kinds it picked. Also supports the **any-of-kind** target-less form (omit `target`, supply `chunkKind`) — passes iff ANY chunk of that kind's `param` resolves a correctly-typed binding |
| `document` | any-of-kind `where:[...]` co-binding filter (on `param_equals` / `param_range` / `param_references_kind` / `param_series_orbit`) | narrows the any-of-kind candidate set BEFORE the per-chunk assertion: a chunk is considered only when it satisfies EVERY `where` entry — either an equality entry `{param, value}` (numeric 1e-4-per-component tolerance when both sides tokenize all-numeric, else exact string) or a range entry `{param, min:[...], max:[...]}` (mirrors `param_range`). Collapses two independent existentials (e.g. "some timeline drives `element_type camera`" AND "some timeline drives `param location`") into one AND'd-on-the-SAME-chunk assertion. Any-of-kind only (load-rejected with a named `target`); array must be non-empty |
| `document` | `op:"param_series_orbit"` — `chunkKind` (required), `where?`, `param`, `timeParam?`, `minKeyframes`, `minAngularSpreadDeg`, `centerWithin?:{x,z,radius}`, `maxRadiusRatio?` | ANY-OF-KIND only. Per candidate chunk (post-`where`), time progression is checked FIRST and is INTRINSIC to the op (always enforced, not opt-in): collect the RAW `param` and `timeParam` (default `"time"`) occurrence lists in document order and require them to pair 1:1 (a timeline that omits `time` lines — the parser defaults a missing time to `0.0` — or carries extras fails here), every `timeParam` token to parse as a single float, and the resulting time sequence to be STRICTLY increasing (subsumes an all-equal/degenerate range with no epsilon needed; catches duplicate, decreasing, or non-progressing sequences). THEN collect EVERY `param` occurrence in document order, parse each as 3 floats (skip malformed), dedupe CONSECUTIVE duplicates, and grade the XZ track as an ORBIT — `>= minKeyframes` points, an angular spread (about `centerWithin`'s point when given, else the points' XZ centroid) of `>= minAngularSpreadDeg` where spread = `360 − largestGapBetweenCircularlySortedAngles`, the centroid within `centerWithin.radius` (when given), and `maxDist/minDist <= maxRadiusRatio` (when given; a keyframe on the center is a degenerate failure). PROVES a single timeline is an orbit BOTH in shape and in time, rather than accepting two independent existentials or a geometry-only track that never actually animates; the pass detail reports the time span `t.front()..t.back()`. Load bounds: `minKeyframes >= 2`, `minAngularSpreadDeg` in `(0,360]`, `maxRadiusRatio > 1`, `timeParam` non-empty when present |
| `untouched` | `chunks:[{chunkKind?, name}, ...]` | every listed chunk is byte-identical between the scene AS LOADED (before the first turn) and the POST-run document — the PASS_TO_PASS "didn't touch unrelated stuff" guard |
| `render` | `width?`, `height?` (both or neither), `samples?` (`>= 1`), `seed?` (accepted, no effect — no RNG pinning is exposed), `camera?:{location:[x,y,z], lookat:[x,y,z], up?:[x,y,z], fov?}`, `meanLumaMin?/Max?`, `meanRMin?/Max?`, `meanGMin?/Max?`, `meanBMin?/Max?`, `channelBalanceMax?`, `compareToImage?` + `rmseMax?` | a fresh render's mean channel values (and Rec.709-weighted mean luma) fall within the given `[min,max]` bands — bands must be wide enough to absorb ordinary MC noise; **never** an exact-pixel match. `samples` overrides the grading render's SPP (honoured by the pixel-based rasterizer family) so `compareToImage` RMSE can be stabilised. `camera` overrides the ACTIVE camera's pose for THIS grading render only, so the image is taken from a KNOWN pose regardless of the scene's authored camera — `location`+`lookat` are required (each an array of 3 numbers), `up` defaults to `[0,1,0]`, `fov` (degrees, `0 < fov < 180`) omitted keeps the camera's current fov; composes with `width`/`height`/`samples` and every band (objectmap camera support is NOT in scope). `channelBalanceMax` (a number `> 1.0`, load-enforced) asserts the render is roughly NEUTRAL: `max(meanR,meanG,meanB) / max(min(meanR,meanG,meanB), 1e-6)` must not exceed it — a name-agnostic "grey" check. `compareToImage` (a repo-relative `.png` path — no `..`) + `rmseMax` (in `(0,1]`) are **both-or-neither** and assert an IMAGE match: `RMSE = sqrt(mean over all pixels·RGB of ((a−b)/255)^2)` of this render vs the committed reference, failing iff it exceeds `rmseMax`. The candidate pixels are `decode(read_image bytes)` and the reference was written by that SAME render→PNG path, so **tonemap/clamp/quantization are identical on both sides** (like-for-like). The grading render is forced to the reference's EXACT dimensions: if `width`/`height` are also supplied they MUST equal the reference's dims (a RUNTIME check — the load validator cannot open the file), else the reference's dims are adopted. The measured RMSE is ALWAYS reported in the checkpoint detail (pass or fail) so a calibration run can read the actual noise floor; a missing / non-PNG / mis-sized reference is a FAILED checkpoint with a clear detail, never a crash |
| `objectmap` | `legendContains?`, `pixelCountFor?` + `pixelCountMin?/Max?`, `queryAt?:{x,y,expectName,expectGeometryKind?}` (at least one of the three) | `legendContains`/`pixelCountFor` run one `mode:"objectmap"` render; `queryAt` calls `AgentSession::QueryObjectAt`. `expectName:""` means expect a MISS; `expectName:"*"` means expect ANY non-background object (name-agnostic — for open-ended "build a scene" scenarios where the model names entities freely); any other string expects a hit with that exact name. `expectGeometryKind` (optional string) additionally resolves the HIT object's chunk by name, reads its `geometry` param, and requires the referenced chunk's kind to equal it — so "the centered visible object is actually a `sphere_geometry`", killing a detached-geometry + wrong-centered-object dodge. Composes with `expectName:"*"` (any hit, then bind its geometry kind); load-rejected paired with `expectName:""` (a miss has no geometry) |
| `diagnostics` | `expect:"clean"` | `AgentSession::ValidateText` on the post-run document returns zero diagnostics |
| `diagnostics` | `expect:"code"`, `code` | at least one diagnostic with that `code` is present |
| `trajectory` | `maxToolCalls?`, `maxLlmCalls?`, `terminalStatus?` | read straight off the run result's own counters (`handle.result`) |
| `trajectory` | `noAutonomyRefusal?:true` | no dispatched tool call's JSON-RPC response carried error code `-32011` (an autonomy refusal) |
| `trajectory` | `requiredToolInOrder:[names...]` | the trajectory's `tool` records contain these names as an (not-necessarily-contiguous) **subsequence** in order |
| `trajectory` | `noMechanicalLoop:true` | no two consecutive `tool` records have the same name AND byte-identical args (a "stuck repeating itself" guard) |
| `trajectory` | `expectAutonomyRefusal:true` | at least one dispatched tool call's JSON-RPC response carried error code `-32011` (the inverse of `noAutonomyRefusal` — proves a read-mode mutation was actually refused) |
| `trajectory` | `toolOutcomes:[{name, occurrence?, expect, argsContains?}, ...]` | each spec selects the `first`/`last`/`any` (default `any`) `tool` record named `name` and asserts its result outcome `expect` ∈ `applied` (result.applied==true) / `staged` (status=="staged") / `rejected` (status=="rejected") / `conflict` (status=="conflict") / `error` (response has an error). `argsContains` (optional) FILTERS which same-named records the spec selects among — a non-empty string OR a non-empty array of non-empty strings; a record matches iff its name matches AND its serialized args contain EVERY listed substring (occurrence then selects among the filtered matches; the failure detail names the filter). Proves a specific call was rejected/applied/conflicted — e.g. malformed-then-recovered. Load-rejects an empty array, an unknown occurrence/expect, an empty argsContains (string or array element), or a missing name/expect |
| `trajectory` | `toolCallAfterUserTurn:{name, minUserTurns?, maxUserTurns?, argsContains?, expect?}` **or** an ARRAY of such specs | a `tool` record named `name` (optionally filtered by `argsContains` — a string or an array of substrings, all of which must be present) occurs at a running `user`-record count that is `>= minUserTurns` (when given) AND `<= maxUserTurns` (when given) — proves real turn separation (a later edit happened AFTER the Nth user prompt, or an early edit happened BEFORE it). The optional `expect` (same enum as `toolOutcomes`) requires the matching call to ALSO satisfy that outcome, so an early *rejected*/irrelevant edit no longer counts as "the edit happened here". The single-object form is back-compat; the ARRAY form checks several specs (all must pass), each with its own failure line. Load-rejects a spec carrying neither `minUserTurns` nor `maxUserTurns`, an empty array, an empty `argsContains`, or an unknown `expect` |
| `proposal` | `countMin?`, `countMax?`, `match?:{kindIs?, target?, param?, status?, valueMin?:[...], valueMax?:[...]}` | asserts on the live session's staged-proposal queue (`AgentSession::ListProposals` — real under `autonomy:"propose"`, where a headless mock-Owner stages edits; empty for read/commit runs). `countMin`/`countMax` bound the TOTAL staged entries; `match` requires at least one entry satisfying ALL its given fields (`kindIs` = the proposal op `param_edit`/`insert_chunk`/`remove_chunk`; `valueMin`/`valueMax` grade the entry's whitespace-tokenized value floats component-wise, requiring the token count to equal the band length). At least one of `countMin`/`countMax`/`match` is required (a checkpoint with none is load-rejected). A null session is a failed checkpoint |
| `finalText` | `containsAll?:[...]`, `containsAny?:[...]`, `absent?:[...]`, `caseSensitive?:false` | keyword assertions over the agent's final assistant message (`handle.result.finalText`): every `containsAll` substring present, at least one `containsAny` present, no `absent` substring present — all present conditions must hold. Case-insensitive (ASCII) unless `caseSensitive:true`. Empty/missing needles assert nothing; a checkpoint with NO meaningful needle fails loudly (never a silent vacuous pass). Grades a refusal/clarification that must SURFACE something (e.g. `reserved_name_clarify` asserts the ask-back names the conflict) without an LLM judge |

`conflict_retry`'s checkpoints show `param_range` (grading "close enough to
blue" rather than one exact fixture literal) plus `requiredToolInOrder:
["read_document", "propose_patch", "read_document", "propose_patch"]` proving
the read-conflict-reread-repropose recovery actually happened.
`camera_orbit_timeline.json` shows `chunk_count` used for an unnamed,
non-singleton `timeline` chunk kind (`{"op":"chunk_count","chunkKind":"timeline","min":15,"max":15}`).

## Interventions (the scripted co-editor)

A scenario's optional `interventions` array simulates "the user edited the
scene while the agent was working." Each entry:

```jsonc
{ "afterToolCalls": 1, "op": "param_edit", "target": "pnt_albedo", "param": "color", "value": "0.9 0.9 0.1" }
```

- `afterToolCalls` (>= 1): fires **after** the N-th dispatched tool call
  completes and **before** the next LLM round.
- `op`: only `"param_edit"` is supported today.
- `target` / `param` / `value`: a set-one-param edit, applied through the
  live `AgentSession` via `ProposePatch` with **no** `baseVersion` — an
  unconditional Owner commit that bumps the head's revision — WITHOUT
  consuming a model turn. This makes a subsequent agent patch built against
  the head it last read genuinely conflict, so the checker can grade the
  read-conflict-reread-repropose contract live rather than fixture-staging
  it. Recorded honestly in the trajectory as a `history_edit` with reason
  `"scenario_intervention"`.

`conflict_retry.json` is the canonical example: its intervention recolors
`pnt_albedo` out from under the agent right after its first `read_document`,
forcing the fixture's `propose_patch` → conflict → `read_document` →
`propose_patch` recovery sequence to be real, not scripted.

## Providers + the parameterized OpenAI codec

`ChatProvider` (`AgentChatLoop.h`) is `Anthropic` / `Gemini` / `OpenAI` /
`XAI` / `Local`. `XAI` and `Local` both reuse `OpenAIChatCodec` (the
OpenAI-Chat-Completions-compatible wire shape) with a different
`OpenAIChatCodec::Config` rather than separate codec classes:

- `openai`: the codec's built-in default config.
- `xai`: `baseUrl = "https://api.x.ai/v1/chat/completions"`, default model
  `"grok-4.5"`, `requiresAuth = true` (Bearer auth via `XAI_API_KEY`).
- `local`: `baseUrl` from `RISE_LOCAL_LLM_BASE_URL` (config, not a
  credential — read directly via `getenv` in `MakeCodec`, deliberately
  **outside** `AgentEvalRunner.cpp`'s env-read-free contract), falling back
  to Ollama's default `http://127.0.0.1:11434/v1/chat/completions`; default
  model `"qwen3:32b"`; `requiresAuth = false` (no `Authorization` header
  unless a key is actually supplied); a longer `requestTimeoutSeconds`
  (900s) than the hosted default, since a cold local model swap can load
  tens of GB before the first token.

Provider+model selection happens via `AgentChatLoop::SetProvider(provider,
modelId)` (live path) or is read straight off the replay fixture's leading
`provider` field (replay path, `ParseReplayProviderName`); an empty
`modelId` uses the codec's built-in default.

## How CI proves scenarios: T10

[tests/AgentEvalCheckTest.cpp](../tests/AgentEvalCheckTest.cpp)'s `T10`
(`TestSeedScenariosCheckpointsAreTrue`) loads every committed
`evals/scenarios/*.json`, runs it end-to-end through **its own** committed
fixture via `RunScenario` (the replay path — no network, no key), and
asserts every one of its own wired `checkpoints[]` actually **passes**
against that fixture (`CheckScenario`'s `allPassed` and
`checkpointFraction == 1.0`). This is what keeps a scenario honest: it
proves the checkpoints are true of the canned session that's supposed to
satisfy them, not just plausible-looking JSON that happens to parse. It
**dynamically enumerates every committed `evals/scenarios/*.json`** (via a
`std::filesystem::directory_iterator`, sorted for determinism — there is no
hard-coded id list), so a new scenario is covered by CI automatically the
moment its `.json` lands alongside a matching `evals/fixtures/*.fixture.jsonl`.
It runs as part of the normal test suite (`./run_all_tests.sh` /
`run_all_tests.ps1`). The one requirement is the paired fixture: a scenario
whose `replay.fixture` file is missing or whose own checkpoints aren't all
true of that fixture fails T10 loudly.
