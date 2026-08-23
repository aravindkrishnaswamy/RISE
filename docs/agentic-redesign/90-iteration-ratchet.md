# 90 — The iteration ratchet: stopping regression-by-refinement

Status: DESIGN — slices R1–R3 pending
Evidence base: trajectory `20260823T030556Z-e3800bc9.jsonl` (gemini-3.7-flash,
the doc-89 dragon probe, 2026-08-22)

## 1. The failure this doc exists to fix

The dragon probe was the best adoption result of the geometry workstream —
first-ever live use of `skin_geometry` (+`billow`), `mirror` in the exact
`source`+`mirror x` idiom, skeleton + displaced scales — and the worst
outcome: **render 00 was a good dragon, and twelve self-directed edit
rounds later the final render was strictly worse.**  7.5M input tokens,
most spent degrading the scene.

Measured mechanisms, in causal order:

1. **Regenerate-and-rebind, never delete.**  Every "fix" was a NEW
   geometry chunk plus a `geometry` rebind on the same object.  Final
   document: 28 geometry chunks, **17 orphaned** (`dragon_body_displaced`
   v3–v8, wing skins v2–v6), 57KB, 1019 joints for one dragon.  The model
   re-reads the document every round, so its own context filled with dead
   generations — the context-volume law operating inside one session.
2. **No memory of "better", no way back.**  14 renders, an edit burst
   after each.  The model judges only the CURRENT render against the
   prompt; nothing anchors best-so-far; regressions are permanent.
3. **Lighting death spiral.**  Lights patched in 10 of 12 bursts.  A
   model cannot meter absolute exposure from a thumbnail; successive
   "darker" edits overshot into murk.  The design note was NOT the driver
   (it fired on renders 0–2 only); pure image self-critique was.
4. **A wasted plan slot.**  `dragon ['chain','primitive']` — `primitive`
   summons nothing, so sweep/loft never entered the builder prompt, the
   flattening-cross-section asks were unbuildable, and the model
   compensated with six rounds of displacement fiddling.

The sharpest fact: `imagine_scene` held a generated target the whole
session and **`compare_to_reference` was never called**.  The instrument
that turns "is this better?" from a vibe into a number existed, loaded,
unused.  Delivery is not compliance; a pull-channel instrument is not an
instrument.

The engine itself held: zero derive failures, zero fold/mirror warnings
across 179 revisions.  The failure is the iteration loop's epistemics.

## 2. Slice R1 — the ratchet: the anchor composite (REVISED; the scored
form is FALSIFIED — see the history note at the end of this section)

Every `render` result composites the session's ANCHOR RENDER beside the
current render, both labelled with their head revisions.  No number.

- Default anchor: the FIRST completed post-compose render (in the failed
  dragon run, exactly the render that was best).
- `set_render_anchor` — the model re-pins the anchor when it judges a
  newer render better.  Pairwise visual judgment is the instrument;
  vision models compare two images far more reliably than they score
  one, and a side-by-side cannot be gamed by deleting geometry.
- The anchor carries its revision, so R2's verb has a target: once R2
  lands, the render note may say "the anchor (rev N) is beside this
  render -- revert_to_revision N if this is worse."  Until R2 lands the
  verb is NOT named (a stale promise burns the repair budget).
- Advisory, self-disarming.  Anchor state is per-session bookkeeping
  (G2-gate discipline).  The Phase 2b guard (below) stays armed.

**History note (2026-08-23): the original R1 spec'd a `targetScore` +
`bestScore` scalar against the imagine_scene target.  The implementation
worker refused it, with evidence.**  (a) Phase 2b (2026-08-11,
AgentSession.h ~1327 + doc 77 §15) had already shipped, measured, and
DELETED exactly that score from exactly that surface -- 16 score
consultations drove 23 emissive/power-cranking edits into a blown-out
frame -- and left a money assertion (AgentRenderAsyncTest ~7035) pinning
its absence.  The doc's author missed it.  (b) A fresh harness over the
real CompareToReference scorer showed the metric is INVERTED in the
operative regime: against a divergent (i.e. normal) generated target,
deleting the hero object improves the score (measured -0.148, monotone
crossover as target similarity drops).  A ratchet on that scalar would
anchor bestRevision at the most-mangled document.  Phase 2b's law --
"the composite IS the comparison" -- is the design that survives.

## 3. Slice R2 — `revert_to_revision`

Agent verb: restore the DOCUMENT to its text as of an earlier head
revision this session.  Design constraints:

- **Revert-as-new-commit.**  The restore is one new edit producing a new
  head revision whose content equals the old — append-only history, one
  undo step, GUI-visible like any other agent edit.  Never rewinds the
  undo stack, never rewrites history.
- Whole-document restore only (no partial/selective revert in R1 scope).
- The session records (revision → document text) at every mutation the
  agent performs; bounded ring (document texts are ~10–60KB; keep a
  generous cap and refuse beyond it with the oldest available named).
- Composes with the element ledger the way a full re-derive already does;
  the worker must resolve what happens to `mChunkAttribution` /
  element-active state on revert and pin it.
- Registered as MUTATING on every surface (RPC, codecs, MCP adapter +
  IsKnownToolName, chat loop, rate limiter, eval runner) — the
  collapse_to_instances checklist.

## 4. Slice R3 — orphan pressure + plan-slot guidance (small, bundled)

- **Orphan pressure on rebind**: when a patch changes a Reference param
  (`geometry`, `base_geometry`, `source`, material/painter slots) and the
  OLD target becomes unreferenced, the patch result appends the same
  "now unreferenced and NOT removed ... pass them to remove_chunks"
  sentence the scaffold verbs already use.  Same message, same mechanic,
  at the site this run actually used seventeen times.  Detection must
  share the reference-graph machinery, not re-implement it.
- **Plan-slot guidance**: the `construction` schema prose (codec + MCP
  adapter, both surfaces) gains one sentence: `primitive` summons no
  schema and costs a slot — declare it ALONE, never as a second method.
  (The gate mechanics are untouched; this is wording.)

## 5. What is deliberately NOT here

- No auto-revert, no auto-stop: the ratchet informs, the model decides.
  (An agent that cannot be trusted to stop is a provider problem; an
  agent that cannot KNOW it got worse is our problem.  R1 fixes ours.)
- No exposure/histogram meter in render results (plausible future: mean
  luma + clipped fraction, so "too dark" is a number; deferred until the
  ratchet's effect is measured).
- No score-gated refusals ("your score dropped, edit refused") — the
  anti-churn escape stays load-bearing everywhere.

## 6. Measurement

Re-run the dragon probe (same prompt).  Success is NOT a higher final
score than render 00 of the failed run — it is: final render >= best
render of its own session (the ratchet held), orphan count near zero
(the rebind pressure worked), and ideally a `revert_to_revision` call
observed doing its job.  Census the document as always.
