# Expressive-Surface Adoption via Authoring-Surface Structure — Arc 75 Design (2026-07-31)

> **Status:** design APPROVED by the user 2026-07-31 (sequencing confirmed;
> preset composite materials REJECTED — monoculture worry — and replaced by
> the §3 expanding-scaffold design; the 74-log false-fact correction landed
> as `435df037`).  S0 (instrument phase) in flight.  Successor to
> [74-creative-richness-arc-log.md](74-creative-richness-arc-log.md) /
> [73-creative-richness-design.md](73-creative-richness-design.md); builds
> on every settled result there and re-litigates exactly one — on new
> evidence (§1).
>
> **Goal:** harness/product mechanisms that get agents to use more of
> RISE's real expressive surface (procedural painters, the scalar pipe,
> SDF/sweep composition, volumes, spectral effects) on ORDINARY prompts,
> without structured-prompt authoring by the user.

---

## 1. New evidence that reorders the ladder (2026-07-31 forensics)

**Finding A — the settled bindability map was false.**  The 74-log
(§4.3/§5) closed on "only ggx/ward expose Reference-kind roughness slots;
pbr_metallic_roughness's `roughness` and cooktorrance's slot are baked
`ValueKind::Double`".  False when written: `pbr_metallic_roughness_material
.roughness`/`.metallic` and `cooktorrance_material.facets` are
Reference-kind painter slots (ChunkParserRegistry.cpp:3958-3959 + the
cooktorrance descriptor), and have been since `64ca16bc` (2026-04-30) —
three months before the arc.  Only the three SSS skin materials bake
`Double` roughness.  The round-3 reviewer's "fix" was itself an unreviewed
truth defect — §4.3's lesson, one level deeper.  Corrections landed in the
74-log (§4.3 note + §5 rewritten bullet) and the AgentSession.cpp advisory
comment (comment-only; the note text is measured-inert and wording changes
are a measured arc-75 variable).

**Finding B — construct-level census of all 24 bare-prompt runs.**  Models
overwhelmingly instantiate `pbr_metallic_roughness` + `lambertian` (qwen's
P2.b runs are lambertian-only).  Every `roughness`/`metallic` value in
every insert payload across all 24 runs is a numeric constant.  So the
0/24 verdict *stands at construct level* (no run spatially varied
microsurface by ANY path) — but the instrument only measured the
`scalar_painter` vocabulary, and the P1 worked examples targeted the
highest-friction path (switch material family to ggx + learn
scalar_painter + function2d) while the lowest-friction path (bind a
painter to the pbr `roughness` slot models already write `0.5` into every
run) was never exampled and never measured.

**The refined mechanism model this licenses** (consistent with every 74
datapoint): *examples move behavior when they are drop-in edits to what
the model already writes* (uniformcolor→perlin in an existing binding
slot: floor 1→3); *they fail when the example requires restructuring*
(ggx+scalar_painter 0/24; sdf recipes 2/24).  "Examples move copying" has
a locality condition the 74 arc didn't isolate.  That is the new-evidence
license for one cheap rung BELOW schema-structural — falsifiable in one
run batch (S1).

## 2. The ladder (one behavioural variable per phase; every rung has a
pre-committed stop rule)

### S0 — Instrument phase (unconditional; no behavioural variable)

1. **`param_binding` checker op** — `{slots:[{chunkKind, params[]}...],
   excludeReferencedKinds[], min, max?, metricLabel}`: counts chunk
   instances where ≥1 listed param resolves to an existing non-excluded
   chunk.  Configured as `varied_microsurface` over pbr
   roughness/metallic, ggx/ward alphax/alphay, cooktorrance facets,
   excluding uniformcolor_painter.  Measures the construct, not the
   vocabulary.  The old scalar_painter checkpoint stays (strict
   sub-metric, 0/24 continuity).  Red-proof both directions.
2. **Headless `SetSkillIndex` parity** (74-log open item 1) — lands now or
   never mid-arc (it changes turn structure).
3. **Second subject** `bare_prompt_build_cozy_study` — anti-overfit
   control for all subsequent skill edits (74-log open item 3).  Indoor
   luma bands provisional until the anchor calibrates them (the §6.3/§6
   lesson: the first run's first job is calibrating the measurement).
4. **Geometry friction audit** (read-only): minimal-correct-chunk cost per
   advanced verb, census of the advanced-geometry runs, skill copy-source
   sizes.  Output picks S3a vs S3b as geometry's starting rung.
5. **Fresh 6-run anchor** on the parity'd harness: gemini-3.5-flash N=3 +
   qwen3.6:27b N=3, courtyard + cozy-study + `build_ambiguous_scene`
   counterweight, one runDir (`bare_prompt_s0_anchor`), serialized.

### S1 — Aligned examples (the locality-hypothesis test)

In the skills' existing pbr snippets, bind an existing procedural painter
to `roughness` where the snippet now writes a constant.  Vary the painter
family across skills (74 P1 anti-monoculture rule); execution-validate
every snippet against `bin/rise`; no new prose; no scenario vocabulary
(courtyard OR study).

- **Failure modes:** models copy the base-colour painter but simplify
  roughness back to a constant (strong prior); subject overfit (cozy-study
  control catches it).
- **Stop rule:** `varied_microsurface ≥ 1` in **≥2/6** runs → bank;
  proceed to S2 for GEOMETRY only.  **0/6** → locality hypothesis dead,
  example ceiling confirmed at construct level; S2 proceeds for both
  domains.  Exactly 1/6 → one N=3 re-run, then decide; no third try.

### S2 — Expanding material scaffolds (schema-structural; REDESIGNED after
user rejection of preset composite materials)

The original S2 (new composite material chunk kinds à la
pbr_metallic_roughness composing rich internals) was rejected by the user
2026-07-31: an encapsulated preset's diversity ceiling IS the preset
library — agents would converge on "nicer flat".  Replacement — a
**scaffold tool** (`insert_material_scaffold {family, ...}`, engine-side in
the dispatcher; headless + both GUIs) that expands a family template into
ORDINARY, EDITABLE document chunks: a small wired painter graph (2–4
painters + material, painter bound to the microsurface slot).  Three
properties carry the anti-monoculture design:

1. **Required creative params with no defaults** (`tone`, `wear`,
   `scale`): the model must make creative micro-decisions per expansion.
   Required-params-where-honest — honest here because the surface is new
   and parameterization is its purpose.  Structure, not advice.
2. **Seeded jitter** on internal graph constants at expansion time — a
   guaranteed cross-scene diversity floor even at zero post-edits.
3. **Scaffolds decay into example code inside the model's own scene** —
   every read_document re-exposes the working graph at the exact site of
   future edits; mutation is one set_param away.  A preset's ceiling is
   the library; a scaffold's ceiling is the full painter surface.  (The
   P1 copying lever relocated to the highest-reach carrier: the document.)

Provenance naming (`tmpl_<family>_*`) lets the eval split **authored /
expanded / expanded-then-edited**; `scaffold_edit_rate` is the creativity
signal; raw expansion counts are the disclosed floor and are never summed
with anything.

- **Failure modes:** never called (reach); called-but-never-edited with
  collapsed family distribution (the user's worry realized); decoy
  expansion (inherited caveat, disclosed).
- **Stop rules:** 0/6 runs call it → kill (drop from skills; keep-or-drop
  tool on product merit).  Called but never edited AND one family >80% of
  expansions → one family/param iteration, one re-measure, then revert.
- If S3b geometry structure is ever wanted it inherits this
  scaffold-not-prefab pattern.

### S3 — Geometry (start rung picked by the S0 audit — RESULT below, §7)

- **S3a** (CONFIRMED by the S0.4 audit as the starting rung, verb-ranked):
  1. `displaced_geometry` — the audit's headline: CHEAPEST verb to author
     (11 lines / 16 tokens as a bolt-on to an existing shape; the
     colora/colorb overhead implied by painter descriptors does NOT apply
     to displacement sources, Job.cpp:5338) yet has ZERO code examples in
     any skill doc and zero lifetime adoption — pure example-absence.
     One ~11-line drop-in recipe is the S3a centrepiece.
  2. `sweep_geometry` — the only skill example is scenario-glued (flask
     neck, coordinates tuned to its vessel); re-anchor on the compact
     standalone idiom that already exists in
     `scenes/Tests/Geometry/sweep_instances.RISEscene` (2–3 point path,
     no cross-object coordination).
  3. `sdf_geometry` — the one verb models ALREADY reach for unprompted
     (the only advanced verb ever inserted: 3/18 courtyard runs, always
     freshly composed from the taught pattern, never pasted).  More
     examples are the wrong lever here; the friction is that per-primitive
     `a b c` semantics live only in skill prose, not in the
     machine-readable descriptor.  Candidate structural fix (its own
     measured variable, NOT folded into S3a): per-primitive field
     semantics in the sdf descriptor — structure on the `read_schema`
     carrier both models measurably read.
  Stop rule: `advanced_geometry` ≥2/6 → bank; 0/6 → S3b decision point.
- **S3b** (structural; needs explicit user taste sign-off before detailed
  design): parametric scaffold expansion into editable sdf/sweep chunks.
  Same stop-rule shape as S2.
- **Accept-the-deficit exit, explicit:** a primitives courtyard is a
  legitimate design answer.  If S3b is declined or fails, the arc records
  "advanced geometry is not bare-prompt behaviour; reachable via intent"
  and closes that side without escalation.

### S4 — Plan-schema gate (conditional: only if S1 AND S2 both fail on
microsurface)

First oversized `insert_chunks` on a bare build before any plan → refused
ONCE with a blocking error requiring a filed design plan (structured tool;
one required field per named material: `finish: "varied"|"flat"`).
Blocking + required schema fields = both halves of the mechanism law.
Function is **priming at decision time, not policing** — anti-Goodhart by
construction: ANY plan is accepted, fires once per session, never
re-fires, `flat` is a legal answer.

- **Stop rule:** ≥ half of runs file a plan and still produce zero
  `varied_microsurface` → revert the gate (a turn of cost that bought
  nothing) and the microsurface side of the arc CLOSES: after S1 (one
  binding cheap), S2 (one call cheap), S4 (decision-time salient), the
  residual is model disposition, already proven non-purchasable.
- **Guards in the same batch:** counterweight ask-rate unchanged; median
  turns delta ≤ +2.

## 3. Eval extension

- **Ops:** `param_binding` (S0).  `rich-material`/scaffold metrics reuse
  `distinct_chunk_kinds` + provenance-prefix filtering; no other new ops
  anticipated.
- **Labels:** `varied_microsurface`, `scaffold_edit_rate` (if S2 ships),
  plus existing `painter_kinds` / `advanced_geometry` / strict
  scalar-pipe.  **Per-label columns only — never aggregated**, not even
  for us (§4).
- **Batches:** every phase = one runDir, gemini N=3 + qwen N=3,
  courtyard + cozy-study + counterweight; one behavioural variable per
  run; archive never delete; exposure-check before any negative verdict
  (74-log §4.7 — for qwen, confirm the model actually read the edited
  skill/schema).
- **qwen thinking probe:** decisive at S1 and S4 — distinguishes
  "considered-and-declined" (S4 priming might work) from
  "never-in-option-set" (only structure helps) via `message.reasoning`
  at the insert-authoring moment.
- **Carrier-reach rule:** all new signal rides skills + chunk/tool
  descriptors + the document itself — surfaces both models measurably
  read.  Nothing new rides validate.
- **Disclosed blind spots** (scenario `//` comments): `param_binding` is
  taste-blind (a crude binary checker on roughness counts); CST-side
  decoys inherited; 32×24 render checkpoints cannot see any of this —
  document-side metrics are the signal.  Per successful phase: one manual
  512×384 before/after eyeball so no rung ships whose only effect is a
  metric column.

## 4. The expressivity-score question, settled

- **(b) agent-facing feedback score — rejected on the 74 record**: a score
  in a tool result is advice with digits; 0/24, two models, two framings,
  two carriers, 30 exposures, one written confession.
- **(c) gate — rejected**: Goodhart demonstrated in-arc (decoy fixtures);
  flat/primitive scenes are sometimes correct, so any hard richness gate
  is dishonest somewhere and teaches gaming.  The only surviving gate is
  S4's, which gates *sequencing*, never *content*.
- **(a) instrument — yes, and it already exists**: the per-label metric
  columns ARE the "subscores with disclosed blind spots".  No aggregate
  number is built, even us-facing: every scalar summary in the 74 arc
  misled at least once (§4.5).  Trend lines per label, per model, per
  phase — the whole score story.

## 5. Framing flags carried from design review

1. The settled 74 record contained a false fact (Finding A) — corrected
   `435df037`; the derived conclusion ("models must switch to ggx/ward to
   vary roughness") was wrong, though the 0/24 behavioural verdict
   survives.
2. The 74 instrument measured vocabulary, not construct — no verdict
   flips (both paths were 0/24), but without `param_binding` the next arc
   would miss the likeliest success path entirely.
3. "Use more of the surface" is a proxy: each rung names its user-visible
   payoff, geometry carries an explicit "maybe the models are right"
   exit, and the 512×384 eyeball guards against metric-only wins.

## 6. Logistics

Tiering: Fable supervises/plans only; sonnet implements (opus reviewer on
any lock/lifetime-adjacent internals); trailers name the actual tier.
Gate per slice: `make` all zero-warnings + full tests + `xcodebuild
RISE-GUI`; five-project checklist for any new source file.  Reviewers:
fresh every round, patch+tar bundle protocol (worktrees do NOT carry
uncommitted changes); supervisor gates every build and re-verifies the
main checkout after any worktree agent.  Runs: gemini via
`evals/.secrets.env`, qwen local, serialized.

## 7. Phase log

- **2026-07-31 — S0 opened.**  Correction commit `435df037`.  S0.1
  (param_binding, sonnet writer) + S0.4 (geometry audit, read-only) in
  flight; S0.2 (SetSkillIndex parity) and S0.3 (cozy-study scenario)
  serialized behind S0.1; anchor after gate.
- **2026-07-31 — S0.4 geometry audit COMPLETE** (sonnet, read-only; all
  minimal chunks parse+render-verified against `bin/rise`).  Costs:
  sdf 6 lines/39 tokens (densest — per-primitive semantics absent from
  the descriptor), sweep 9/25, displaced 11/16 (cheapest as a bolt-on;
  no colour vocabulary actually required).  Census (18 courtyard runs):
  only 3 ever inserted an advanced verb — ALWAYS `sdf_geometry`, always
  freshly composed from the taught pattern; sweep and displaced were
  never inserted once.  Copy-sources: sdf has one true drop-in (lamp
  shade_taper); sweep's only example is scenario-glued;
  displaced has ZERO examples anywhere.  Bonus first-party confirmation
  of the mechanism law: one qwen run received the geometry-census
  advisory 57 times and acted zero times.  S3a ordering fixed as
  displaced-first (see §2 S3 amendment); the sdf descriptor-semantics
  enrichment is registered as a separate structural candidate.
  Incidental: a qwen run inserted `standard_object` chunks with a
  hallucinated `gallery` field that reportedly failed SILENTLY — spun
  off as its own investigation (agent-side insert validation vs the
  descriptor hard-fail scene loading gets).
