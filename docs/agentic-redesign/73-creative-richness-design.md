# Creative Richness on Bare Prompts — Materials AND Geometry — Design (2026-07-29)

> **Scope expansion (user, 2026-07-29, mid-P0):** originally "material
> richness" only; the user directed that everything here also apply to
> **geometry** — general creativity, not just surfaces. The measurement ops
> were built filter-generic, so the extension is mostly checkpoint
> configuration plus one newly-load-bearing fix (metric labeling, §2 P0).
> Geometry-side *behaviour* changes (skills, advisory) follow the same
> evidence-gated discipline: the baseline decides whether geometry has a
> deficit before anything ships to change it.

> **Status:** P0 (measurement layer, materials + geometry) SHIPPED 2026-07-29
> and reviewed to zero P1 over two fresh rounds — four checker ops,
> metricLabel per-label aggregation, the 11-checkpoint bare-prompt scenario +
> fixture, runconfig; gate green (make zero-warn, 225/225, xcodebuild).
> P1 (B1+C1 skills) / P2 (A1 advisory) / P3 (A2 detector) remain design-only
> pending the baseline run. Originally written to settle the §7 "live design
> question" of `72-agent-turn-efficiency-and-quality-log.md` before any code;
> every mechanism claim below was file:line-verified by a fresh code audit on
> 2026-07-29 (advisory pipeline, ask_user, skills, starter scene, checker
> engine); none of it is re-derived from memory.
>
> **The problem:** a heavily-structured prompt naming material intents got six
> distinct procedural painter families out of the agent; a bare prompt
> ("build me a courtyard") gets flat uniformcolor surfaces. Real users write
> bare prompts.

---

## 0. The recommendation, in one paragraph

Ship, in this order: **(P0)** the eval scenario + three new checker ops, and
run the baseline **before any behaviour change lands**; **(P1)** B1 —
re-anchor the four recipe skills whose snippets are still uniformcolor-only,
with C1's "state your material plan" folded into the same commit; **(P2)** A1
— a flatness advisory carried on beauty-render and validate results,
engine-side, using the same observed-state-note mechanism as the
empty-skill-index advisory. Re-run the eval after each phase; one variable at
a time. **Drop B2 and C2 as actively wrong** (§4). **Defer A2**, reframed as a
collapsed-painter trap detector, and **defer C3** to the scene-variants
feature (`63-scene-variants-feature-spec.md`) if that ever ships. No new
asking machinery of any kind: the already-shipped `ask_user` description
(`AgentChatCodecs.cpp:585-612`) plus `build_ambiguous_scene` as a permanent
regression guard is the whole elicitation story.

Weighed against §5.1: P2 is the mechanism class that went 2-for-2
(observed-state diagnostic, code-adjacent fact). P1 is *not* prose-as-rules —
it is example-anchoring, and §6.6's own finding is that the model copies the
snippet it is given; changing the snippet changes the prior. C1 is the one
50%-lever item kept, because its cost is one sentence and its value survives
partial adherence (§2). Everything purely-prose beyond that is dropped.

---

## 1. The tension, resolved

**Creative by default. Elicitation never blocks on style. The stated plan is
the elicitation.**

Concretely:

- On a bare build prompt the agent guesses richly — real material families,
  scalar-pipe roughness where it earns its keep — and **states the guess** in
  its reply ("I'm doing weathered sandstone walls, mossy flagstones, a
  verdigris fountain") so the user has a steering point at zero round-trip
  cost. A wrong rich guess is a *better product outcome* than a question: the
  user sees a render and says "make it Mediterranean", which is exactly the
  interaction they came for.
- `ask_user` stays reserved for **material ambiguity** — subject identity and
  choices that change *what gets built*, not how it's dressed. This line is
  not new design: it is already the shipped tool description ("Do NOT ask
  about details you can decide yourself with reasonable taste (exact colours,
  minor placement, secondary props) -- pick something sensible and note the
  choice in your final summary"). The evidence says this works: 0/132
  spontaneous asks *without* the tool, 12/14 asks *with* it on a
  materially-ambiguous scenario (`evals/RESULTS.md` §2c). The grain of every
  model is to not ask; the tool's description successfully channels asking to
  the one case it's wanted.
- The tension is kept resolved **by measuring both sides forever**: the new
  bare-prompt scenario penalises interrogation (`askUserMax`), and
  `build_ambiguous_scene` (already in the suite) penalises not-asking when it
  matters (`askUserMin:1`). Any future change that trades one for the other
  shows up as a red checkpoint, not as an opinion.

Why not the other landing spots:

- *Always-ask (even once, gated)* — fights the 0/132 grain, contradicts the
  shipped tool description, and the user's own product judgment: an agent
  that interrogates on every scene request is worse than one that guesses
  richly and says what it guessed.
- *Silent rich guessing (no stated plan)* — loses the steering point and
  makes a wrong guess feel like the agent being wilful rather than
  collaborative. The statement is what makes boldness safe.

---

## 2. What ships, per phase

### P0 — the measurement, first (checker ops + scenario + baseline)

Nothing behavioural lands until this has run. Rationale: every guidance
change in the prior arc was validated by a single manual run (§7 item 8),
and the harness's own history says the first job of a new measurement is to
calibrate the measurement (`70-agent-eval-harness.md` §6.3).

**Three new `document` checkpoint ops** in `AgentEvalRunner.cpp`. The audit
confirmed none are expressible today (`chunk_count` is single-exact-kind
only, `AgentEvalRunner.cpp:3774-3787`; nothing scans document-wide
references). All three should be thin layers over the *existing*
`RISE::Cst::BuildReferenceGraph` (`Cst.h:822`, already O(N log N), already
the mechanism `AttachChunkIssueWarnings` trusts) rather than a new scan:

1. `distinct_chunk_kinds` — `{kindSuffix?|kinds?, exclude?, distinctMin,
   distinctMax?}`: count **unique** chunk keywords present, filtered by
   suffix (`"_painter"`) or explicit list, minus exclusions.
2. `any_param_references_kind` — `{referencedKind}`: true iff any edge in the
   reference graph terminates in a chunk of that kind. (The forward
   direction of the missing primitive.)
3. `no_orphan_chunks` — `{kindSuffix?|kinds?}`: every chunk of the matching
   kind(s) has ≥1 referrer in `graph.dependents`. (The reverse direction;
   this is also the anti-gaming guard — painter chunks inserted but never
   bound score nothing.)
4. `objects_reaching_kinds` — `{rootKind?="standard_object", kindSuffix?|
   kinds?|category?, exclude?, min, max?}` (added after review round 1's
   P1-b): counts root-kind chunks whose reference CLOSURE (BFS over
   `graph.edges` from the object — material → painter → nested painters)
   contains ≥1 chunk matching the filter. `min:2` on qualifying painters
   defeats the single-decoy gaming pattern the round-1 test reviewer proved
   (all diversity bound to one hidden 0.001-unit box passes every other
   checkpoint). This op is also, deliberately, the exact object→material→
   painter scan the P2 flatness advisory needs — built once here, reused
   there. **Honest residual caveat (documented, not solved):** CST-side
   checks cannot see visibility; a determined multi-decoy scene still
   passes. This scenario is a measurement, not an anti-adversarial gate;
   the render-side closer is A2/P3. The scenario's `//` comment must carry
   this caveat so it survives into any future gate-ification debate.

Each op gets a red-proof test (crafted documents where it must fail), per the
§5.5/§5.6 lesson that unprobed assertions don't go red. Optionally, the
distinct-painter count is surfaced as a structured `metricValue` on the
checkpoint result and a column in `eval_report.py` — the two-file change the
audit sized as contained — so richness is a *trend line*, not just pass/fail.

**The scenario** — `evals/scenarios/bare_prompt_build_courtyard.json`:

```json
{
  "id": "bare_prompt_build_courtyard",
  "title": "Bare build prompt -- material richness without elicitation",
  "scene": { "path": "scenes/Templates/empty_starter.RISEscene" },
  "autonomy": "commit",
  "prompts": [ "Build me a courtyard." ],
  "budgets": { "maxToolCalls": 60, "maxLlmCalls": 45, "maxWallMs": 1800000 },
  "replay": { "fixture": "evals/fixtures/bare_prompt_build_courtyard.fixture.jsonl" },
  "checkpoints": [
    { "kind": "document", "op": "chunk_count", "chunkKind": "standard_object",
      "min": 3, "max": 40 },
    { "kind": "document", "op": "distinct_chunk_kinds", "kindSuffix": "_painter",
      "exclude": ["uniformcolor_painter", "scalar_painter"],
      "distinctMin": 2, "weight": 2 },
    { "kind": "document", "op": "no_orphan_chunks", "kindSuffix": "_painter" },
    { "kind": "document", "op": "any_param_references_kind",
      "referencedKind": "scalar_painter", "weight": 0.5 },
    { "kind": "render", "samples": 512, "width": 32, "height": 24,
      "meanLumaMin": 0.015, "meanLumaMax": 0.35, "channelBalanceMax": 4.0 },
    { "kind": "diagnostics", "expect": "clean" },
    { "kind": "trajectory", "terminalStatus": "final_text",
      "noMechanicalLoop": true, "askUserMax": 1 },
    { "kind": "finalText",
      "containsAny": ["stone", "wood", "marble", "brick", "moss", "tile",
                       "weather", "grain", "texture", "material"],
      "weight": 0.5 }
  ]
}
```

Calibration notes (each a deliberate choice, most learned from §6.3's
vague-prompt-vs-exact-checkpoint failures):

- **The prompt is truly bare.** No "make it look nice" — that phrase is
  itself a nudge, and the real-user case under test is its absence.
- **`distinctMin: 2` excluding `uniformcolor_painter`** is the headline
  checkpoint (weight 2). `scalar_painter` is also excluded from the
  *diversity* count because a flat scalar is not texture — scalar-pipe use
  is measured separately. `checker_painter` is deliberately NOT excluded:
  checkered courtyard tiles are a legitimate design choice, and banning
  vocabulary instead of asserting truth is the §5.5 anti-pattern.
- **The scalar-pipe checkpoint is low-weight (0.5), not gating.** A good
  courtyard can have uniform roughness; spatially-varying scalar work is
  partial credit, not a requirement.
- **`askUserMax: 1`** tolerates a single question without failing the run —
  one concrete ask is defensible product behaviour; an interrogation is not.
  In headless runs there is no scripted responder, so any ask returns
  `available:false` and the model must proceed on its own judgment — which is
  itself part of what's measured.
- **`finalText.containsAny` is the C1 proxy and is knowingly weak** (it greps
  vocabulary, §5.5's own caveat) — hence weight 0.5. The document checks are
  the real signal; this one only distinguishes "stated a material story" from
  "said nothing".
- **`scene.path` rather than inline**: the scenario should measure what real
  users get from the shipping starter. Safe because this design explicitly
  does NOT change the starter (§4); if that ever changes, pin inline first.
- **Anti-overfit rule:** no skill edit in P1 may mention courtyards or any
  scenario vocabulary. The skills teach materials; the eval picks an
  arbitrary subject. (A second subject — e.g. "build me a cozy study" — is
  worth adding when repeats go 3→5, to detect subject-overfit.)

**Geometry checkpoints (scope expansion, added to the same scenario):**

```json
{ "kind": "document", "op": "distinct_chunk_kinds", "category": "geometry",
  "distinctMin": 3, "weight": 1.5, "metricLabel": "geometry_kinds" },
{ "kind": "document", "op": "distinct_chunk_kinds",
  "kinds": ["sdf_geometry", "sweep_geometry", "displaced_geometry"],
  "distinctMin": 1, "weight": 1, "metricLabel": "advanced_geometry" }
```

- The first measures geometry-kind diversity (`category:"geometry"` — the
  registry token verified in `CheckerCategoryFromName`); ≥3 distinct kinds
  means the scene is not a box-monoculture. Inclusive bounds; the op is
  unchanged, only configured.
- The second uses the `kinds`-list filter to express "at least one advanced
  modeling verb" — SDF composition, sweeps, or displacement — which is where
  RISE's real geometric vocabulary lives (profiles of revolution are
  `sdf_geometry` roundcone+smin per the §6.1 domain facts; there is no lathe
  verb). Partial credit, not gating: a legitimate courtyard *can* be mostly
  primitives.
- The painter-diversity checkpoint keeps the top weight (2); geometry enters
  at 1.5 + 1. Rebalance only on baseline evidence.
- **Geometry anti-gaming asymmetry (round-2 review P2, disclosed not
  solved):** unlike the painter side (where `objects_reaching_kinds min:2`
  imposes a two-decoy floor), the geometry checkpoints have NO reachability
  guard — one tiny decoy chunk per missing kind satisfies them. This is
  structural: a geometry decoy IS an object reaching the advanced kind, so
  a reachability floor adds nothing, and `min:2` on advanced geometry would
  over-prescribe legitimate one-fountain scenes. Measurement, not gate; the
  render-side closer remains A2/P3. The scenario `//` comment carries this.

**Metric labeling (was reviewer P3, now load-bearing):** `mean_metric_value`
pools every `metricValue`-carrying checkpoint in a group into one flat mean.
With ONE metric checkpoint that was a documented latent limitation; with the
geometry expansion plus `objects_reaching_kinds` the scenario carries **four**
(`painter_kinds`, `textured_objects`, `geometry_kinds`, `advanced_geometry`),
and pooling would silently blend unrelated metrics into a meaningless number.
Required fix, same slice: an optional `metricLabel` string on any
metric-carrying checkpoint (default: the op name), carried through
results.jsonl, with `eval_report.py` aggregating **per label** — never across
labels. Loader hard-errors on duplicate labels within one scenario.

**Fixture:** `AgentEvalCheckTest` T10 auto-discovers scenario JSONs but
requires a matching fixture that passes every checkpoint — so P0 includes
authoring a canned trajectory that builds a small courtyard with ≥2 bound
non-uniform painters, ≥3 distinct geometry kinds, and ≥1 advanced modeling
verb. This doubles as the red-proof: mutate the fixture to uniformcolor-only
(or all-box geometry) and the new ops must fail it.

**Baseline protocol (narrowed to gemini-only, user decision 2026-07-29):**
run at the P0 commit, `gemini-3.5-flash` alone, N=3, serialized;
`build_ambiguous_scene` re-run in the same batch as the counterweight. The
full hosted+local matrix was judged too expensive/slow, local models
deficient on build tasks. Single-provider is an accepted trade: gemini-3.5-
flash is the same instrument the entire 72-log arc was measured on, so
before/after deltas remain valid; what is lost is any cross-provider claim
("half the hosted providers" criteria below reduce to gemini-specific
medians). Expected result: distinct-painter count ~0, pass@1 near 0%. That
is the point — this is a headroom scenario, like `multi_step_build` at 21%.

### P1 — B1: re-anchor the four uniformcolor-only skills (+ C1, same commit)

The audit found the post-`48f68963` state is: `procedural-textures.md`
demonstrates spatially-varying painters; `materials-and-media-basics.md` has
checker + a *flat* scalar; and **four recipe skills remain uniformcolor-only
in every code fence**: `lighting-recipes.md` (6 sites),
`modeling-workflow-and-geometry.md` (6), `object-modeling-recipes.md` (9 —
the mug/table/lamp recipes), `observe-modes.md` (3).

Scope rules for the edit:

- Convert only snippets that show an **object's visible surface material**.
  Sky-dome fills, light colours, and flat-by-nature demo props stay
  `uniformcolor` — forcing texture where flat is correct would teach the
  opposite lie.
- **Vary the family across skills** (perlin3d wood on the table recipe,
  worley3d stone where masonry appears, domainwarp marble, etc.) so the
  copy-target is diverse. One repeated example would just move the
  monoculture from uniformcolor to a single texture (see B2, §4).
- Keep deltas tight — skills are read on demand but they are still token
  cost; a snippet swap is a painter chunk + binding, not a new recipe.
- **C1 rides along:** one sentence in `procedural-textures.md`'s opening (and
  nowhere else): before building from a bare prompt, state a one-line
  **design plan — materials and forms** — in the reply and proceed; do not
  ask about style. This is
  prose, it is a ~50% lever, and it is kept anyway because its cost is one
  sentence, its checkpoint exists (the weak finalText one), and even 50%
  adherence delivers the steering point that makes rich guessing safe (§1).

Honesty note: run 5's "6 distinct painters" was achieved **under the
structured prompt** — B1's effect on *bare* prompts is unproven. That is
precisely what the P0→P1 eval delta exists to establish. Do not skip the
re-run on the strength of run 5.

**Geometry side of P1 (scope expansion — audit first, edit second):** the
materials audit was painter-specific; nobody has audited which skills'
snippets instantiate only primitive `box`/`sphere` geometry. P1 therefore
gains a geometry-snippet audit of the same four recipe skills (plus
`scene-skeleton-and-conventions.md`), with the same rules: convert a snippet
to `sdf_geometry`/`sweep_geometry`/displacement only where the snippet shows
an object whose real-world form warrants it; vary the verb across skills; no
scenario vocabulary. Note `object-modeling-recipes.md` is already deep on
SDF technique — the geometry prior gap is expected to be smaller than the
painter gap, and the baseline's geometry metrics will say whether it exists
at all. If the baseline shows geometry diversity is already healthy on bare
prompts, the geometry half of P1 is a no-op — do not edit skills to fix a
deficit the measurement doesn't show.

### P2 — A1: the flatness advisory (observed-state note)

The §5.1 table says this mechanism class is the reliable one; the audit says
it is cheap and safe:

- **Scan:** engine-side in `AgentSession` (shared C++ — works identically on
  Mac GUI, the un-compiled Qt half, and headless evals; unlike `DriverNote`,
  which is Mac-only and a different layer). On the already-retained
  `Document` snapshot (the same under-lock copy pattern
  `AttachChunkIssueWarnings` uses — no new synchronization),
  `BuildReferenceGraph`, walk object → material → painter, classify each
  object "flat" iff every colour-slot painter it reaches is
  `uniformcolor_painter`. Objects with no material binding are skipped.
- **Fire condition:** `objectCount ≥ 3 && flatCount/objectCount ≥ 0.5`.
  Pure-CST connectivity + counts; **no size proxy in v1**. The log's "large
  surfaces" qualifier is dropped deliberately: per-geometry-kind size
  formulas are real scope, and the audit showed true size is either
  unproven-safe (live `IObject::getBoundingBox()` off the render path — the
  exact race class `979097f3` was about) or render-cost-bearing. Fraction-of-
  objects is a fine discriminator at bare-build scene sizes.
- **Carriers: beauty `render` results (production/draft) and `validate`.
  Deliberately NOT `insert_chunk(s)`/`propose_patch(es)`** — mid-build the
  model legitimately inserts geometry before materials; advising "everything
  is flat" after insert #2 is premature, wrong, and a nag-loop risk. The
  render is the moment of self-assessment — the model just looked at its
  work; attach the fact exactly there. At the measured 3-4 renders per
  build, worst-case firing is a handful of short notes per session.
- **Wire shape:** the established convention — a `note` string on the tool
  result, key omitted entirely when clean (same as `read_skill`'s
  empty-index note, `AgentRpc.cpp:1043-1044`, and the `issues` array).
- **Text (draft):**
  > `MATERIAL NOTE: {flat} of {n} objects bind only uniformcolor painters.
  > If flat/stylised colour is what the user asked for, this is fine —
  > ignore this note and do not churn. Otherwise, real-world materials need
  > spatially-varying painters: read_skill {"name":"procedural-textures"}.`

  The escape clause is load-bearing and present from day one — the §5.3
  lesson is that a loud signal without an anti-loop clause just buys a
  different wasted-turn loop. v1 fires whenever the condition holds (no
  dedupe state machine); if trajectories show churn on
  deliberately-flat scenes, add emit-on-transition dedupe then. Ship simple,
  measure, harden on evidence.
- **Follow-up (noted, not v1):** when `renderMode == "objectmap"`, the legend
  already carries per-object `pixelCount` at zero extra cost — a
  screen-coverage-weighted flatness fraction is nearly free there. Worth
  doing only if the object-count fraction proves noisy.
- **Geometry sibling (scope expansion — evidence-gated, designed not
  scheduled):** the same mechanism extends to a geometry-census note — e.g.
  "all {n} objects use box_geometry" — fired under an analogous
  monoculture condition (objectCount ≥ 4 && one geometry kind covers ≥ 80%).
  Same carriers, same escape clause, same `note` convention. Do NOT build it
  in the same commit as the material advisory, and do not build it at all
  unless the baseline (or the P1 re-run) shows geometry monoculture is a
  real failure mode on bare prompts — the material deficit is measured;
  the geometry deficit is so far only suspected. Known limitation either
  way: the flatness/census scans read DIRECT bindings and kinds; they do
  not judge composition quality — that residue belongs to A2/P3 if ever.
- **Tests:** red-prove both directions — a crafted flat document must
  produce the note; a textured one must omit the key entirely; and the
  render-result path must be exercised in the loop tests, not just the unit.

### P3 (conditional) — A2, reframed

A raw texture-variance statistic is the weakest candidate as specified: it
duplicates A1's message, arrives later (post-render vs. at-edit-graph), and
needs a Monte-Carlo noise-floor threshold that is real calibration work (the
audit confirmed `meanR/G/B` differ between runs only by MC noise — a naive
variance would false-positive on noise atop genuinely flat paint).

Its *principled* version is a different feature: a **collapsed-painter trap
detector**. Fire only when the CST says textured but the pixels say flat —
i.e. non-uniform painters are bound (A1's scan says "rich") AND the rendered
luma variation sits at the noise floor. That is precisely the §6.3
3D-painter-as-`function2d` trap, which today fails silently. Near-zero
false-positive surface because it requires *both* signals to disagree.
Build only if, after P1+P2, trajectories show painters bound but renders
still flat.

---

## 3. Sequencing, gate, and success criteria

```
P0: checker ops + scenario + fixture + report column
    → gate (make all zero-warn / 225 tests / xcodebuild RISE-GUI)
    → BASELINE RUN (pre-change HEAD): bare_prompt + build_ambiguous, N=3
P1: B1 skill re-anchor + C1 sentence   → gate → eval re-run
P2: A1 advisory                        → gate → eval re-run
P3: only if the P2 trajectories show the collapsed-painter gap
```

One behavioural variable per eval run; `build_ambiguous_scene` rides in every
batch as the ask-side regression guard. All implementation delegated per the
tiering directive (checker ops + advisory: sonnet; skill edits: sonnet;
fixture: sonnet); writers serialized or worktree-isolated; each slice through
the implementation-review-loop to zero P1.

**Success criteria, stated before the first run:**

- **P1 succeeds** if the gemini-3.5-flash median distinct-non-uniform-painter
  count goes 0 → ≥2 (gemini-only baseline per the narrowed protocol above),
  with `askUserMax` and `build_ambiguous_scene` both still green.
- **Geometry (scope expansion):** the baseline's `geometry_kinds` /
  `advanced_geometry` metric columns are the decision input, not a target —
  if median distinct geometry kinds is already ≥3 on hosted providers, the
  geometry halves of P1/P2 are dropped as solving a non-problem; if below,
  the P1 geometry-snippet audit proceeds and its success bar is the same
  shape as the painter one (median ≥3, ask-side guards green).
- **P2 earns its keep** if it lifts the *residual* — runs that still rendered
  flat after P1 — and specifically if trajectories show the note followed by
  a material fix within the same session (the diagnostics-get-fixed pattern).
- **A regression** on `build_ambiguous_scene` (ask-rate drops below 1) or on
  any existing scenario's pass@1 outside its Wilson interval blocks the
  phase that caused it.

If P1 alone saturates the scenario, P2 still ships — the advisory covers the
case skills structurally cannot: the model that read nothing, or built flat
anyway. But P2's scope stays v1-simple in that world.

---

## 4. Rejected candidates, and why (flagged as requested)

- **B2 (textured starter scene) — actively wrong, drop.** Three independent
  reasons. (1) It reverses an explicit, recorded product decision — the
  starter is the structural minimum and renders black *by design*
  ("start-screen spec decision 2, 2026-07-15", stated in the file's own
  header). (2) §6.6's mechanism cuts both ways: the model copies the one
  example it is given — a starter with one oak table means oak tables
  bleeding into every unrelated build. Anchoring belongs in skills, where
  there can be *many* varied examples selected by intent, not one universal
  prior. (3) It is a three-surface change (canonical scene + Mac bundle copy
  + Qt `$(OutDir)` copy, byte-identity-guarded by `SourceHygieneTest`) for
  negative expected value.
- **C2 (gated single ask_user with style options) — actively wrong, drop.**
  It contradicts the *shipped* tool description, which explicitly instructs
  the model NOT to ask about style it can decide with reasonable taste — C2
  would require rewriting that description against the 0/132 grain and
  against the ask-side eval that currently passes. It also solves the wrong
  problem: the bare-prompt failure is impoverished priors, not missing
  permission to ask. If the user wants style input, C1's stated plan gives
  them the hook without the toll-booth.
- **C3 (render two variants, user picks) — defer, don't build here.** Twice
  the render cost per build, a variant-picker product surface that belongs
  to the scene-variants feature spec (`63-scene-variants-feature-spec.md`),
  and it does nothing about the prior — two flat variants is not a choice.
  Revisit only as a thin consumer of the variants feature if that ships.
- **A2 as specified — defer and reframe** (§2, P3).

## 5. What this design deliberately does not do

- No system-prompt additions beyond what exists. Every pure-prose lever
  except C1's single sentence is rejected on the §5.1 record.
- No forced asking, no ask-rate target above zero on clear prompts.
- No starter-scene changes.
- No new synchronization: the advisory reuses the retained-snapshot pattern;
  the checker ops run on the post-run document like every existing op.
- No claim survives one manual run: every phase re-runs the scenario matrix,
  and the checkpoint metrics (not vibes) decide whether the phase held.

---

## 6. Baseline outcome (2026-07-29, gemini-3.5-flash only, 9 bare-prompt runs across 3 calibration passes)

**Anchor (v3, final calibration):** pass@1 0/3, meanCkpt **0.903**, all runs
terminal `final_text` (never budget); metric columns: `painter_kinds` 2.00,
`geometry_kinds` 4.67, `advanced_geometry` 0.33, `textured_objects` 27.33.
Counterweight `build_ambiguous_scene`: 66.7% pass, 2/3 asked — byte-for-byte
the epoch-11 gemini form; the ask-side guard is healthy and unmoved.
Archived v1 (pre-recalibration) rows: `evals/runs/archive/bare_prompt_baseline_v1_luma_artifact`.

**Calibration artifacts the baseline itself surfaced (the §6.3 lesson, again):**
1. `meanLumaMax 0.35` was an indoor ceiling; courtyards are outdoor and render
   legitimately at 0.67–0.82 → widened to 0.9 (blown-out is ~1.0).
2. `standard_object max:40` failed the single richest run of all nine (a
   50-object, 7-painter-kind courtyard) — ambition, not runaway; budgets catch
   true runaways → widened to 100.

**Real findings (stable across all 9 runs, band-independent):**
- **The "impoverished prior" premise is partially STALE.** Bare-prompt painter
  diversity is median 2 distinct non-uniform kinds (spread 1–7), not ~0 — the
  prior arc's `procedural-textures` skill (48f68963) is already working. §0's
  "flat uniformcolor surfaces" framing over-states today's failure mode.
- **Scalar-pipe use: 0 of 9 runs.** Not one `scalar_painter` chunk ever. THE
  confirmed deficit.
- **Advanced geometry verbs: 2 of 9 runs.** `sdf_geometry`/`sweep_geometry`/
  `displaced_geometry` almost never reached for. The second confirmed deficit.
- **Geometry-kind diversity: healthy (4–5).** Per §3's decision rule the
  geometry-DIVERSITY halves of P1/P2 are DROPPED as a non-problem; the
  geometry story narrows to advanced verbs only.
- **Variance is the other headline:** painter_kinds 1→7 across identical
  prompts. Raising the FLOOR (the 1-kind runs) matters as much as the median.

**P1 re-target (supersedes the §2 P1 emphasis):** B1's snippet re-anchoring
should lean on (a) spatially-varying `scalar_painter` examples (roughness/
displacement via `expression_function2d` — the 0/9 deficit), (b) SDF/sweep
forms where object shape warrants (the 2/9 deficit), and (c) painter-family
breadth to lift the floor above 1 — generic uniformcolor→procedural
conversion is now the least urgent third. Success bar restated: scalar-pipe
≥1/3 runs, advanced-geometry ≥2/3 runs, painter_kinds floor ≥2 in every run,
`build_ambiguous_scene` unmoved.

---

## 7. P1 outcome (2026-07-30, commit 526f6f99, 3 runs post-skill-re-anchor)

| bar (from §6) | result | verdict |
|---|---|---|
| painter_kinds floor ≥2 every run | 3, 2, 3 (was floor 1, median 2) | **MET** — floor lifted |
| scalar-pipe ≥1/3 runs | **0/3** (0/12 lifetime) | **FAILED** |
| advanced geometry ≥2/3 runs | **0/3** (2/12 lifetime) | **FAILED** |
| build_ambiguous_scene unmoved | ask-side 3/3 asked (≥ baseline 2/3); pass@1 noisy on its own tight luma/center-pixel bands at N=3 | guard HELD |

**The reading:** snippet re-anchoring moved exactly what models copy —
painter families (floor 1→2, breadth up) — and moved NEITHER mechanism-
adoption deficit, despite the skills now carrying prominent, execution-
validated scalar-pipe examples and pre-existing sdf/sweep recipes that
trajectories prove the model reads. This is §5.1 quantified: examples move
copying; they do not move mechanism adoption. Prose/example levers are now
measured at their ceiling for this problem.

**P2 re-target (design amendment, needs user sign-off):** A1-as-approved
counts surfaces bound to uniformcolor — but colour flatness is now a
SOLVED problem (floor 2 post-P1); as approved, the advisory would fire
rarely and address yesterday's deficit. The same mechanism, same carriers
(beauty render + validate), same `objects_reaching_kinds` scan should
instead report the two MEASURED deficits as observed state:
- "all {n} materials use constant roughness — no spatially-varying
  scalar_painter is bound anywhere" (the 0/12 deficit), and
- a one-line geometry census ("{n} objects: {k} box, {m} sphere…; no
  sdf/sweep/displaced forms") for the 2/12 deficit,
with the same escape clause and skill pointer. Everything else in the §2
P2 spec (engine-side scan, fire thresholds, anti-churn clause, red-proof
tests) carries over unchanged.

---

## 8. P2 outcome (2026-07-30, commit 9b3f19a6, 3 runs with the advisory live)

| §6 bar | result | verdict |
|---|---|---|
| scalar-pipe ≥1/3 | **0/3** (0/15 lifetime) | **FAILED** |
| advanced geometry ≥2/3 | 1/3 (natural rate; census clause never fired in that run — the sdf was authored unprompted at construction time) | **FAILED** |
| painter floor ≥2 | 3, 3, 3 — floor AND median now 3 | exceeded (P1's lever, still compounding) |
| ambiguity guard | 2/3 asked — historical form | HELD |

**Trajectory forensics (the §6.3 discipline applied to our own feature):**
the note FIRED in every run (render + validate carriers, transport
verified), was first seen with 1–5 actionable calls remaining (r1: five
calls after first note; r3: four), and the model acted on it **zero
times**. Two prior mechanism successes (empty-index note, derive
diagnostics) were BLOCKING facts — the model could not proceed without
addressing them. This note is non-blocking advice competing with
end-of-build momentum, and its own anti-churn escape clause licenses
ignoring it. **Refined §5.1 model: models fix facts that block; they
skim advice that doesn't.** Also structural: all construction precedes
the first render (the 72-log's unlanded render-cadence item), so the
note only ever arrives at the verification tail.

**Where this leaves the ladder** (decision points, not decisions):
1. **Diagnostic-framing hypothesis**: same fact, surfaced as a
   `validate` Info-severity diagnostic (a to-fix list entry) instead of
   a note field — §5.1's strongest row is "models fix derive
   diagnostics every time". Cheap to test; risks churn on deliberately
   simple scenes (the escape clause doesn't fit the diagnostics shape).
2. **Render-cadence gate** (72-log item 1, structural): refuse an
   oversized first `insert_chunks` before any render — moves the note
   mid-build. Weakened as a pure theory by r1/r3 (turns remained,
   unused), but earlier arrival + repetition may compound.
3. **Instrument limitation, honestly**: all 15 runs are
   gemini-3.5-flash (user's cost decision, §"Baseline protocol"). The
   ask-scenario board showed large cross-model behavioral spreads;
   scalar-pipe adoption may differ on stronger models. One hosted
   opus/gpt spot-check (~6 runs) would disambiguate "advice ignored by
   models generally" from "advice ignored by flash".
4. **Stop here**: painter richness floor 1→3 and median 2→3 are real,
   shipped, measured wins; the scalar pipe and advanced-geometry verbs
   may simply not be bare-prompt behaviors worth forcing.

---

## 9. Cross-model probe (2026-07-30, qwen3.6:27b local, thinking recorded)

Per the user's modification of §8 option 3: a local thinking model whose
chain-of-thought lands in the trajectory (`message.reasoning`), so note
reaction is observable directly instead of inferred. 3 runs, ~54 min each.

**The model dismissed the note IN WRITING, twice, with the same triage
frame:**

> r1: "The full candidate validated clean - no errors. The 'note' about
> scalar_painter is just a design suggestion, not an error."

> r3: "The scene validates cleanly - no errors or warnings beyond the
> expected design note about scalar textures (which is a feature
> suggestion, not an issue)."

r3's "the EXPECTED design note" is habituation: by its 16th firing the
note is ambient. r1 fired the note 30 times (qwen render-spams; 90 tool
calls) — repetition converts advice into wallpaper, not action. Scalar
adoption 0/3 (0/18 lifetime across both models); r3 built a 65-object
scene with 14 painters, every one uniformcolor.

**Conclusion: not flash-specific.** Two models, one written confession of
the shared triage rule: *models act on errors; they skim suggestions.*
The §5.1 refinement (blocking facts vs advice) is now directly evidenced,
not just inferred. The empty-index note worked because the model was
BLOCKED (it needed the index); derive diagnostics work because they are
in the errors list. This note is neither.

**Recommended next (P2.b, one small slice):** move the same two facts
into `validate`'s diagnostics array as an advisory-severity entry
(e.g. code `DESIGN_SCALAR_PIPE_UNUSED`, severity info) — the exact list
qwen consulted ("validated clean - no errors") before moving on. Keep
the render-result note as-is (harmless, and the GUI may surface it).
Measure once more on both instruments; if diagnostic framing also fails,
stop — the painter-floor win is banked and further forcing is not worth
the ladder.

Caveat to carry into P2.b: no escape-clause slot exists in the
diagnostics shape, so a deliberately-flat scene will carry a permanent
info diagnostic — the message text must self-disarm ("intentional flat
styling: ignore") and the severity must stay below anything the GUI
badges as a problem.

---

## 10. P2.b outcome and ARC CLOSE (2026-07-30, commit af764f67 measured)

| instrument | scalar adoption | exposure to the diagnostics |
|---|---|---|
| gemini-3.5-flash (3 runs) | **0/3** | seen every run (1–2 validate results each) |
| qwen3.6:27b (3 runs) | **0/3** | r1/r2 NEVER called validate (zero exposure); r3 saw it twice, zero reasoning engagement |

**Lifetime: 0/24 runs ever bound a scalar_painter**, across two models, two
framings (note, Info diagnostic), two carriers (render, validate), and up
to 30 exposures in a single run. gemini's runs are the clean test — full
exposure, zero action. qwen's add a structural lesson: moving the fact
from render results (which a render-spamming agent sees 10–57×/run) to
validate-only (0–2×/run) REDUCED reach; carrier choice must follow the
agent's actual tool habits, not our notion of "the right moment".

**Per the §9 pre-committed decision rule: STOP.** Scalar-pipe and
advanced-geometry adoption on bare prompts is not purchasable with
result-payload scaffolding at reasonable cost for current models. The
next plausible lever class (schema-structural: a required parameter, a
material template that ships a scalar slot pre-bound) changes the
product's authoring surface, not the agent's guidance — out of this
arc's scope and worth its own design if ever wanted.

### What the arc banked (all shipped, measured, zero-P1 reviewed)
- **Painter richness floor 1→3, median 2→3** on bare prompts (P1 skills;
  confirmed still holding at painter_kinds=2.0 floor under P2.b's runs).
- **Permanent measurement infrastructure**: 4 checker ops, per-label
  metric columns, the bare-prompt scenario + counterweight pairing, and
  a three-instrument comparison discipline (before/after runDirs).
- **The design-note + Info-diagnostics plumbing**: truthful, tested,
  harmless; left in tree deliberately — zero runtime cost, and a
  stronger future model may act where these did not (re-measure with
  the same runconfigs when the model roster changes).
- **A mechanism law, twice evidenced and once confessed in writing**:
  models act on facts that BLOCK (errors, empty prerequisites); they
  habituate to advice regardless of framing, severity vocabulary,
  or repetition. Prose ≈ 50%; examples move copying; advice moves
  nothing; structure moves behavior. (§5.1 → §8 → §9 → here.)

### Ask-side, final state
`build_ambiguous_scene` held its epoch-11 form through every phase
(2/3–3/3 asked). The tension the arc opened with — "more creative" vs
"asks more questions" — closed exactly as designed in §1: richer
defaults landed without a single unwanted question appearing.
