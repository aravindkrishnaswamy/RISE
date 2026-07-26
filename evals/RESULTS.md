# RISE Agent-Eval Results

Running scoreboard for the agent-eval harness. Regenerate any table with
`python3 tools/eval_report.py report evals/runs/<runDir>`; this file is the
curated, human-readable snapshot with the findings and caveats that the raw
report does not carry.

---

## 0. Definitions (what "success" and each metric mean)

### What counts as success

A single run is a **success** only if **both** hold (`AgentEvalRunner.cpp`
`CheckScenario`):

1. **Every checkpoint passes** — all of them, not a majority. Success is
   all-or-nothing per run.
2. **The run ended deliberately** — terminal status `final_text` — *unless*
   the scenario explicitly asserts a different ending via a `trajectory`
   checkpoint (an intentional budget/refusal test). A run that passes every
   checkpoint but was cut off by a budget stop or provider error is scored a
   **failure**: a model that would have kept editing never actually decided it
   was done.

So **success = all checkpoints pass AND the model finished on its own terms.**

Checkpoints are typed and each carries a `weight` (default 1.0). The kinds:
**document** (chunk counts, param equals/ranges, reference-kind), **render**
(mean-luma bands, `channelBalanceMax`, or `compareToImage` + `rmseMax`),
**objectmap** (a named object at a pixel), **diagnostics** (clean — no dangling
references or parse errors), **trajectory** (ended `final_text`, required tools
in order, no mechanical loop), plus `finalText`, `proposal`, `param_series_orbit`.

### Per-run metric

- **`checkpointFraction`** — the **partial-credit** score for one run: the
  weighted fraction of checkpoints that passed (`Σ weight(passed) / Σ weight`).
  0.8 = 4 of 5 equally-weighted checkpoints. A run is a success iff this is 1.0
  *and* the terminal-status gate above is satisfied.

### Aggregated metrics (over N repeats per model × scenario cell)

- **`pass@1`** — mean success rate: the fraction of the N runs that were full
  successes. The headline "did it solve the task" number. (At N=3: 0 / 33 / 67
  / 100%.)
- **`pass^k`** — **1 only if ALL N repeats passed.** A *reliability* indicator
  (consistently solves it), **NOT** the conventional pass@k (≥1 of k). `0`
  means at least one repeat failed.
- **`meanCkpt`** — mean `checkpointFraction` across repeats. The **finer-grained
  signal**: it separates "failed at 4/5" from "failed at 1/5," which `pass@1`
  collapses to the same 0. Lean on this when `pass@1` is noisy at small N.
- **`wilson95%`** — Wilson-score 95% confidence interval on `pass@1`; honest
  about small N (33% carries [7.5%, 70%]). Heavily overlapping intervals mean a
  gap is suggestive, not decisive.
- **`$/success`** — total (cache-corrected) cost ÷ number of successes.
  Cache-aware from `tools/eval_report.py` pricing. `n/a` when there are zero
  successes or no pricing entry. Note it *rises* on a hard eval — few successes
  make each one "expensive."
- **`wall(s)`** — mean wall-clock seconds per run. Runs are serialized, so it is
  a fair provider **speed** metric.
- **`failureBreakdown`** — counts of terminal statuses across the repeats
  (`final_text`, `budget_llm_calls`, `budget_tool_calls`, `budget_wall_ms`,
  `provider_error`, …). Shows *how* runs failed — capability (imperfect final
  scene, but `final_text`) vs. infrastructure/budget (cut off). This
  distinction is what separated the budget-80 vs budget-200 story below.
- **`†` / `!`** — staleness flags. `†` = pre-content-hash results not tied to
  current scenario definitions; `!` = graded under a different checkpoint count.
  Both mean "re-run for a clean number."

**Reading guide.** `pass@1` = solved it; `meanCkpt` = how close; `pass^k` =
reliably; `failureBreakdown` = why it failed. N is small (3 repeats/cell) —
treat single-digit pass gaps as suggestive, not conclusive, and lean on
`meanCkpt` for the finer signal.

### Per-run artifacts (diagnose without re-running)

Each run directory `evals/runs/<runDir>/<scenarioId>__<provider>__<model>__rN/`
holds what the runner writes plus what the enrich tool derives:

Written by the runner:
- `<scenarioId>.trajectory.jsonl` — every LLM turn (with the raw `response_body`)
  and tool call. The source of truth.
- `<scenarioId>.result.jsonl` / `results.jsonl` — status, budgets, checkpoints.
- `<scenarioId>.final.RISEscene` — the final assembled scene, re-loadable and
  re-renderable (present only for runs made after this was added).

Derived by **`python3 tools/eval_enrich.py enrich <runDir> [--render] [--samples N]`**
(a pure post-processor — runs retroactively on any existing run; `--render`
needs the `.final.RISEscene`):
- `digest.json` — the per-run analysis rollup, so behavior is a file-read, not a
  trajectory re-parse:
  - `tools` — call histogram by verb;
  - `edits` — `patchesByParam`, `insertsByKind`, and `rejections`
    (count + `byReason` + items with param/value/reason from the `issues`
    payloads) — mine `byReason` across the corpus for recurring failures →
    skill-lesson candidates;
  - `renders` — count + a **per-render `series`** of `{meanR,meanG,meanB,luma,
    channelBalance}` in order — the convergence-vs-thrash signal (this is what
    localized gemini-3.6's over-iteration);
  - `scene` — final-scene `objectsByKind` + `lights` (kind, color, scale);
  - `tokens` (llmCalls, input/output/cacheRead totals, peak context) and
    `timing`;
  - `reasoning` — `{available, provider_field, turnsWithReasoning}`.
- `reasoning.jsonl` — one line per turn `{i, reasoning}`, **only for providers
  that expose it**: local Ollama models (`message.reasoning`) and xAI/grok
  (`message.reasoning_content`). gpt and gemini do not return reasoning content,
  so those runs have none. Reasoning is captured in the trajectory's
  `response_body` at run time, so this works retroactively too.
- `final.beauty.png` — a render of the final scene (with `--render`); objectmap
  is not CLI-renderable and is skipped.

Regenerate anything at will; these artifacts live under the gitignored
`evals/runs/` and are never committed.

---

## 1. Text baseline — `full_baseline` (2026-07, epoch 1)

17 scenarios × 3 repeats = 45 runs/model. General agent-editing competence
(param edits, inserts, conflict recovery, multi-step builds, observe modes).

| # | model | pass@1 | notes |
|---|---|---|---|
| 1 | qwen3.6:27b **(local, $0)** | 68.9% | ties the frontier for free |
| 1 | gpt-5.6-terra | 68.9% | fastest (≈9s/run) |
| 1 | gemini-3.5-flash | 68.9% | |
| 4 | grok-4.5 | 66.7% | |
| 4 | claude-opus-4-8 | 66.7% | (excluded from later boards — token cost) |
| 6 | qwen3-coder-next (local) | 57.8% | |
| 7 | glm-4.7-flash (local) | 46.7% | |
| 8 | qwen3:32b (local) | 44.4% | ≈277s/run |
| 8 | qwen3-coder:30b (local) | 44.4% | |
| 10 | llama3.3:70b (local) | 6.7% | **dropped** as not worth running |

**Takeaway.** The top five are within one flipped run — a single tied band,
not an ordering. qwen3.6:27b is the standout local model (ties frontier at
$0), the only local worth carrying forward; it is ~10× slower than the
frontier, though.

---

## 2. Complex-scene-build board — `scene_build_board` (2026-07-22, epoch 5, budget 200/300)

The `scene_build_family`: build a complete furnished, lit scene with a hero
object the camera focuses on. Three members (`build_watch_scene`,
`build_stilllife_scene`, `build_study_scene` — the last is **held out** for
validating guidance changes, never iterated against). One shared
**type-agnostic** battery: ≥3 objects, render-band (lit), objectmap-center
(hero framed), diagnostics-clean, trajectory (read→insert→**render**→final).
5 models × 3 scenes × 3 repeats.

| model | pass@1 | meanCkpt | rendered | reached final | lighting (cp1) | wall/run | $/success |
|---|---|---|---|---|---|---|---|
| **gemini-3.5-flash** | **33%** | **0.84** | 9/9 | 9/9 | **3/9** | 363s | $3.47–8.07 |
| gemini-3.6-flash | 11% | 0.78 | 9/9 | 9/9 | 1/9 | **296s** | $10.53 |
| gpt-5.6-terra | 11% | 0.69 | 8/9 | 8/9 | 1/9 | 393s | $3.31 |
| grok-4.5 | 0% | 0.56 | 9/9 | 4/9 | 1/9 | 550s | — |
| qwen3.6:27b (local) | 0% | 0.57 | 4/9 | 1/9 | 1/9 | **3901s** | $0 |

**gemini-3.6 vs 3.5.** The newer, cheaper-output 3.6 (output $7.50 vs 3.5's
$9.00/1M) performs **worse and costs more per success** here — it burns more
tokens and converts fewer to passes. It wins only on speed. "Newer ≠ better"
for this agentic-build workload. *Caveat:* N=9, Wilson intervals overlap
(33% [7.5–70] vs 11% [0.6–43]); meanCkpt and lighting corroborate 3.5's edge,
so probably real, but not decisively.

**Board takeaways.**
- **gemini-3.5-flash leads** and is the only model clearing the lighting
  checkpoint with any regularity.
- **Local is not competitive here:** qwen3.6 is far behind on quality *and*
  ~10× slower. Confirmed capability wall (see §4).

### 2a. Lighting arc — recalibration + fill lesson (2026-07-22, epoch 6)

The "lighting ceiling" (every model failing the render-band checkpoint) was
diagnosed model-free (authored + rendered control scenes): scenes fail on
**channel imbalance, not darkness** — a single warm light with no fill
crushes shadows to near-zero blue (ratios 3–329). Rendered calibration
showed `channelBalanceMax: 3.0` was slightly too strict — it failed
legitimately-warm rooms the prompts ask for (a good warm scene renders at
~1.4–2.9; only no-fill crush exceeds ~5). **Raised to 4.0.**

Calibration effect, isolated by re-grading the *same* epoch-5 renders at 4.0
(render is unchanged; only grading flips):

| model | render-band 3.0 → 4.0 | meanCkpt 3.0 → 4.0 |
|---|---|---|
| gpt-5.6-terra | 1/9 → **4/9** | 0.69 → 0.76 |
| grok-4.5 | 1/9 → 4/9 | 0.56 → 0.62 |
| gemini-3.6-flash | 1/9 → 2/9 | 0.78 → 0.80 |
| gemini-3.5-flash | 3/9 → 3/9 | 0.84 (unchanged) |
| qwen3.6:27b | 1/6 → 1/6 | 0.57 (unchanged — its scenes crush past 4.0) |

Plus a lighting-skill lesson ("a warm key alone crushes to orange — always
add a fill"). Measured on gpt (the only tier where lighting is the binding
constraint — it finishes builds), fresh epoch-6 run at 4.0 + the lesson:

| gpt-5.6-terra | render-band | meanCkpt | full pass |
|---|---|---|---|
| epoch-5 (3.0, no lesson) | 1/9 | 0.69 | 1/9 |
| re-graded @4.0 (calibration only) | 4/9 | 0.76 | — |
| **epoch-6 (4.0 + fill lesson)** | **6/9** | **0.91** | **5/9** |

**Honest attribution.** The calibration is a clean, isolated win (re-grade
proves it). The epoch-6 run is a large overall jump (0.69→0.91, 1→5 full
passes), and it **generalized to the held-out `build_study`** (2/3
render-band). But the *marginal* gain of the fill lesson over calibration
alone (4→6/9) is 2 runs — within N=9 noise; the meanCkpt jump (0.76→0.91) is
more convincing but still N=9. Models lit via omni/directional/spot lights
(not the single-luminaire pattern), consistent with the lesson but not
proven caused by it. **The full board has NOT been re-run at epoch-6** — only
gpt has; the other rows above are epoch-5 renders re-graded at 4.0.

### 2b. Chattiness fix — batch inserts + scenario-based render skill (2026-07-22, epoch 7)

The digests showed `insert_chunk` (one chunk per call) was ~2/3 of all tool
calls, so an N-chunk scene cost N LLM round-trips and re-prefilled a
one-chunk-at-a-time-growing context. Two changes: a batch `insert_chunks` verb,
and rewriting the observe-loop skill from "render every iteration" to
scenario-based "render to answer a question." Measured on gpt (epoch-7 vs the
epoch-6 gpt run):

| gpt-5.6-terra | tool calls | LLM calls | renders/run | meanCkpt |
|---|---|---|---|---|
| epoch-6 (no batch, prescriptive skill) | 86 | 142 | 6.9 | 0.91 |
| epoch-7 (batch + scenario-render) | **30** | **23** | 2.8 | **0.84** |

**A large, real efficiency win — 84% fewer LLM calls, 65% fewer tool calls —
at a modest quality cost.** gpt *adopted* `insert_chunks` (1.7 batch calls/run,
building whole scenes in ~2 calls) and *reasoned to a sensible render cadence*
(2.8/run — down from 6.9, but not zero) — the capability probe passed.

**Attribution, honest.** The raw epoch-7 meanCkpt looked like 0.73, but most of
that was a MEASUREMENT ARTIFACT: the trajectory checkpoint hard-coded the literal
`insert_chunk` tool name in `requiredToolInOrder`, so a run that correctly built
via the new `insert_chunks` verb failed it (perfect correlation: every pure-batch
run failed `traj`, every run that also called the singular verb passed). Fixed —
`requiredToolInOrder` is now `[read_document, render]`, verb-agnostic (the ≥3-object
checkpoint already proves building happened). Re-graded, epoch-7 is **0.84** (3/9
full passes, `traj` 9/9). The remaining 0.07 gap vs epoch-6 is in the lighting
checkpoint (render_band 6/9 → 3/9) — fewer renders plausibly means less lighting
correction, the genuine tradeoff, though at N=9 that 6-vs-3 gap carries real
uncertainty.

**Resolution (epoch 8 → 9).** The chattiness turned out to be dominated by
INSERTS (which the batch verb fixes), not renders (only 2.8–6.9 of 30–86 calls);
the render-softening had attacked the small lever and cost lighting. So: keep
the batch verb, restore the regular render cadence (epoch 9), and re-measure on
gpt + gemini-3.5 + gemini-3.6 at one epoch.

| model | calls (no batch → e9) | meanCkpt (no batch → e9) | render_band |
|---|---|---|---|
| **gemini-3.6-flash** | 143 → **62** (−57%) | **0.78 → 0.91** | 1/9 → **5/9** |
| gemini-3.5-flash | ~108 → **45** (−58%) | 0.84 → **0.89** | 3/9 → 5/9 |
| gpt-5.6-terra | 86 → **30** (−65%) | 0.91 → 0.84 | 6/9 → 3/9 |

Three findings:
1. **The batch verb generalizes to all three (−55…−65% calls) and all adopt it.**
   The durable win — keep it regardless of the cadence question.
2. **It fixed the worst offender.** gemini-3.6 (the over-iterator) went from
   crushed (0.78, worst) to tied-best (0.91) while nearly halving its calls —
   batch helped the model that needed it most. (It still over-patches, 27.8/run;
   the batch verb cut inserts, not patches — the next chattiness lever.)
3. **Render-cadence *prose* moves gemini but not gpt.** Restoring the cadence
   recovered gemini lighting (3.5 to its best 0.89; 3.6's jump) while keeping the
   cost win — but gpt renders ~2.6×/run regardless of wording (2.8/2.3/2.6 across
   epochs 7/8/9), so its lighting stays at 3/9. gpt is prose-insensitive here; its
   lighting is the one unsolved gap and would need a non-prose lever (a nudge or a
   hard requirement), not more skill text.

### 2c. Clarifying questions — `ask_user` board (2026-07-25, epoch 11)

Research finding: **0 of 132** build runs ever asked a clarifying question
spontaneously. The `ask_user` tool + a materially-ambiguous scenario
(`build_ambiguous_scene`: "my prized collection piece" — the scripted responder
answers "a vintage brass pocket watch, about 5cm across") measures whether
models ask when piece identity matters, ask BEFORE building, and reflect that
identity in their final response. Setting and mood are deliberately left to the
agent's ordinary judgment and are not an adoption metric.

Two run dirs feed this table: `ask_user_board` (N=3) for every model, and
`ask_user_openai_responses` (N=5) for the two GPT-5.6 tiers after the OpenAI
Responses-API migration (`607d7302`). The pre-migration GPT row is kept as the
BEFORE half of that comparison — it is not a current result.

| model | N | pass@1 | meanCkpt | asked | $/success | wall(s) |
|---|---|---|---|---|---|---|
| claude-opus-4-8 | 3 | **100%** | **1.000** | 3/3 | $13.75 | 387 |
| **gpt-5.6-sol** (Responses) | 5 | **80%** | 0.967 | **5/5** | **$1.83** | 171 |
| gemini-3.5-flash | 3 | 66.7% | 0.889 | 2/3 | $1.03 | 221 |
| gemini-3.6-flash | 3 | 66.7% | 0.833 | 2/3 | $5.25 | 394 |
| gpt-5.6-terra (Responses) | 5 | 40% | 0.867 | 4/5 | $0.84 | **73** |
| qwen3.6:27b (local) | 3 | 0% | 0.611 | 1/3 | $0 | 1552 |
| ~~gpt-5.6-terra (pre-fix, Chat Completions)~~ | 3 | 0% | 0.611 | 1/3 | n/a | 35 |

**The OpenAI Responses migration (`607d7302`) — a harness bug, not a model
limit.** gpt-5.6-terra scored 0/3 with two near-black renders and asked only
1/3. Root cause: OpenAI 400-rejects function tools + `reasoning_effort` on
`/v1/chat/completions`, and the harness's documented recovery was to force
`reasoning_effort:"none"` — which ran gpt with **reasoning disabled for the
whole task**. Migrating the OpenAI codec to `/v1/responses` (where tools and
reasoning coexist) restored it: **pass@1 0% → 40%, meanCkpt 0.611 → 0.867,
asking 1/3 → 4/5, and both dark-render failures vanished.** The migration is
scoped to OpenAI only — xAI, local Ollama, and other OpenAI-*compatible*
providers keep Chat Completions and keep the 400-recovery.

**Where the models stand.** Opus is the only perfect score but costs **7.5×**
sol per success and runs 2.3× slower; sol's 80% at N=5 and Opus's 100% at N=3
have heavily overlapping Wilson intervals ([37.6%, 96.4%] vs [43.8%, 100%]), so
**sol is the value pick and Opus the reliability pick** — the two are not
statistically separated. gemini-3.5 remains the cheap middle. gemini-3.6 is
strictly worse than 3.5 here (same pass@1, 5× the cost, one budget exhaustion) —
its chattiness regression persists. qwen3.6 fresh-graded at 0/3 (a stale earlier
reading showed 1/3): it asks, but loops mechanically and mis-builds.

**`channelBalance` is now the dominant residual failure** (sol 1, terra 2,
gemini-3.6 1, qwen 1) — near-black renders are gone; what fails now is
over-warm colour. A brass pocket watch under warm key light legitimately pushes
R/B past the 4.0 ratio cap. **Open question: is `channelBalanceMax 4.0` fair to
a warm-lit brass subject, or is it grading the scenario's own scripted answer as
a failure?** Resolve with reference renders before tuning it — and never tune it
per-model.

**Takeaways.** (1) **Tool adoption holds under stricter grading** — 12/14 runs
asked on the corrected scenario, against 0/132 spontaneous asks without the
tool: the tools-over-prose lesson, fifth confirmation. (2) **A provider
integration detail can look exactly like a capability wall** — gpt's 0/3 read as
"gpt can't build," and was a forced-off reasoning flag. Check the wire before
concluding anything about a model. (3) **Reactive ≠ proactive clarification**
still holds, but the gap narrowed once reasoning was restored.

---

### 2d. Batch parameter edits — `propose_patches` (2026-07-25, epoch 12)

**Historical — superseded by epoch 13.** These cells were driven and graded
before the four follow-up fixes described at the end of this section, three of
which change how a run is driven or graded. They are kept as the record of what
the batch verb did and did not buy; do not compare them against anything run at
epoch 13 or later.

The affordance gap 2b closed for INSERTS was still open for EDITS:
`insert_chunks` batched adds, but every parameter change was a single
`propose_patch(target, param, value)`. gemini-3.6-flash's chattiness regression
(noted in 2c) was mostly this — it patched one parameter per round. `epoch 12`
added `propose_patches`, the batch sibling.

Two run dirs measure the same cell at N=5, differing only in which other models
shared the board: `gemini_only_e12` and `ask_user_board_e12`. Both carry the
same scenario content hash, so they are **10 independent repeats of one
configuration** and are pooled here.

| model | epoch 11 (N=3) | e12 run A (N=5) | e12 run B (N=5) | **pooled e12 (N=10)** |
|---|---|---|---|---|
| gemini-3.6-flash | 66.7% | **80%** | **40%** | **60%** [31.3%, 83.2%] |
| gemini-3.5-flash | 66.7% | 40% | 60% | 50% [23.7%, 76.3%] |
| claude-opus-4-8 | 100% (N=3) | — | 100% | 100% (N=5) |

**pass@1 did not move.** 60% pooled vs a 66.7% baseline whose own Wilson
interval is [20.8%, 93.9%] — the two are indistinguishable, and 3.5-flash if
anything drifted down. **The 80% cell is the better half of a 40/80 split on
identical inputs**, which is what N=5 run-to-run variance looks like on this
scenario; quoting it alone would have been cherry-picking. Opus stayed at 100%
and roughly halved its cost per success ($13.75 → $6.19), tracking the general
efficiency win rather than anything specific to the batch verb.

**Efficiency is the real result, and it is large.** For gemini-3.6-flash:

| | epoch 11 (N=3) | pooled e12 (N=10) |
|---|---|---|
| total tool calls / run | 160.0 | **57.7** |
| single `propose_patch` calls | 78–150 | ~1 |
| `propose_patches` calls | — | 9.2 |
| parameter edits landed / run | 78–150 | **112.5** |
| $/success | $5.25 | ~$2.00 |

**2.8× fewer calls while landing MORE edits**, and the budget exhaustion seen at
epoch 11 did not recur in any of the 10 runs. The verb does exactly what it was
built to do; what it does not do is make the model build a better scene.

**Takeaways.** (1) **An affordance fix buys headroom, not quality** — the same
lesson 2b taught for inserts. Freeing ~100 calls per run did not convert into
passed checkpoints, so the residual failures on this scenario are judgment, not
budget. Stop optimising call counts here. (2) **Two N=5 runs of one cell
disagreed 40% vs 80%** — a single N=5 board is not enough to claim a pass@1
delta on this scenario; pool repeats or raise N before reporting one.
(3) **Adding a verb is not adding it to one list.** The follow-up audit found
`propose_patches` registered in the tool surface but missing from four sibling
registries: the MCP transport's mutating-verb **rate limiter** (an uncapped
mutation path), this harness's `kMutatingToolNames` (so
`askUserBeforeMutation` read a batch patch as non-mutating and could vacuously
pass), the **blind-edit nudge** (a batching model never accrued the streak, so
it was never nudged to render), and `ToolOutcomeLine` (the model was told
`"ok"` whether 12/12 or 0/12 elements applied). Epoch 13 fixes all four, makes
a stale-base conflict batch-fatal, and teaches the verb in the modeling skill.
When you add a verb, grep for a sibling's name and check every hit.

---

## 3. Image→scene reconstruction (vision) — `image_reconstruct` (2026-07-18)

Model sees RISE-rendered image(s) of a known SDF object and must rebuild
object+lighting+stage+env; graded by RMSE vs committed references.

- **All frontier models: 0% pass.** Best RMSE 0.12–0.22 vs caps 0.012–0.04 —
  reconstructions wholesale wrong, not near-misses. Discriminates only by
  partial credit (gpt ≈0.44 meanCkpt > grok > gemini).
- The `compare_to_reference split:true` assist was built, validated, and A/B
  tested — it did **not** move outcomes (see §4).
- Board is methodology-stale (epoch/checkpoint drift); numbers are a record,
  not current.

---

## 4. Methodology findings (things the numbers taught us)

- **Adoption ≠ efficacy.** Three mechanisms tried to change a local model's
  build behavior — prose guidance, a directive mandate, a drive-loop nudge —
  all measured **null** on qwen3.6:27b (the mandate even hit ~2× adoption at
  zero benefit and +cost). The bottleneck is model capability, not harness
  affordance. Always measure an assist against a real control before shipping
  guidance for it.
- **The frontier control confirmed it:** gpt renders 9/9 (qwen 2/9), builds
  fewer chunks (renders-and-refines vs emits-blind), and *consumes the
  actionable diagnostics* (recovers from rejects, 9/9 clean). The harness
  works — for a model capable of using it.
- **Budget was the completion ceiling.** At 80 llm calls capable models died
  mid-refine (gpt final_text 1/9); lifting to 200 → 8/9 and the first full
  pass. The scored board uses 200/300.
- **Held-out discipline:** `build_study_scene` is never iterated against, so
  a guidance change that only helps the tuned members reveals itself as
  overfit.
- **One N=5 board cannot carry a pass@1 claim.** Two runs of the SAME cell
  (same model, same scenario hash, N=5 each) came back 40% and 80% — the
  whole apparent effect of a change, twice over, from variance alone. Pool
  repeats across run dirs before reporting a delta, and quote the Wilson
  interval: at N=5 it is roughly [38%, 96%] and rules out almost nothing.
  A result cited from the better of two available runs is not a result.
- **Report the metric that actually moved.** The batch-edit verb (2d) was a
  real 2.8× call-count win and a null pass@1 win. Both are worth knowing;
  conflating them turns a solid efficiency result into an unsupported
  capability claim.
- **A grader hole is a silent scoring change.** `askUserBeforeMutation` kept
  passing after a new mutating verb shipped, because the checker's verb list
  never learned about it — the scenario JSON was untouched, so nothing in the
  diff looked like a grading change. When a tool-surface change lands, audit
  every harness-side list that enumerates verbs, and treat those lists as
  checkpoint definitions.

---

_Last updated: 2026-07-25 (ask_user board + OpenAI Responses migration; `propose_patches` batch-edit verb and its epoch-13 follow-up audit). Raw runs under `evals/runs/`; runconfigs under
`evals/runconfigs/`._
