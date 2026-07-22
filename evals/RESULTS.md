# RISE Agent-Eval Results

Running scoreboard for the agent-eval harness. Regenerate any table with
`python3 tools/eval_report.py report evals/runs/<runDir>`; this file is the
curated, human-readable snapshot with the findings and caveats that the raw
report does not carry.

**Conventions.** `pass@1` = mean full-pass rate over repeats. `meanCkpt` =
mean checkpoint fraction (partial credit). `$/success` is cache-corrected
(from `tools/eval_report.py` pricing) and only defined where a model has ≥1
success. `wall(s)` = mean wall-clock per run (runs are serialized, so it is a
fair speed metric). N is small (3 repeats/cell) — treat single-digit pass
gaps as suggestive, not conclusive, and lean on `meanCkpt` for the finer
signal.

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
- **Lighting is the universal ceiling** — every model fails the render-band
  checkpoint in most runs. The concrete next lever; capable models now
  *finish*, so they can act on lighting guidance (unlike the earlier
  never-render wall).
- **Local is not competitive here:** qwen3.6 is far behind on quality *and*
  ~10× slower. Confirmed capability wall (see §4).

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

---

_Last updated: 2026-07-22. Raw runs under `evals/runs/`; runconfigs under
`evals/runconfigs/`._
