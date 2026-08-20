# 88 — Procedural texture expressiveness: candidates

**Status: APPROVED with decisions (user review 2026-08-19, see §7); Phase-1
implementation IN FLIGHT per the §8 slice plan — S1 (d91e5bfd), S2 (01bd9df7),
S3 (afe1a4cc), S4 (86ebc7a0), S4b (efc54324), S5 (1161e66d) shipped; S6's
OFFLINE half (rich_material_closeup scenario + fixture, the additive
expression-family census metric on both bare_prompt_build scenarios, harness
unit coverage, T10 + full suite green) shipped 2026-08-20 — see §10 for the
run protocol.  Gate (c)'s OWN scenario (`constant_materials_polish` — a
starting scene with three flat-roughness materials, a prompt naming neither
the mechanism nor the verb, and a `toolCallCount` trajectory checkpoint that
census-counts `vary_material` calls) shipped 2026-08-20 too — gate (c) is no
longer "out of scope for this slice" (§10's earlier text); it is measurable
by the same live run as (a)/(b), see §10.  The LIVE half (actually running
the census against a hosted model and reading the §5 gates) has NOT run in
this environment (no API keys) and is the next action item in §10.  §1's
gap table describes the PRE-ARC state.**

This is the texture half of the directive that closed the creative-richness arc
(CREATIVITY_JOURNAL.md, closing line: *"build even more expressive geometry and
procedural textures within RISE itself"*).  Doc 85 executed the geometry half
(C1 skeleton, C2 sweeps, C6 superellipsoid — all shipped).  This doc scopes the
texture half: a much richer, more flexible way to specify spatially-varying
material parameters, in the scene language and the UI, designed so that agents
actually adopt it.

Inputs: a full painter-system inventory, a material-slot binding audit, the
measured adoption-mechanics record (arcs 73–85 + the Aug-19 instancing
incident), and an external survey (Blender/Substance/Houdini node systems,
MaterialX/OSL/MDL/UsdShade standards, SeExpr, and the procedural-pattern +
LLM-authorship literature).  Key citations inline.

---

## 1. Where the painter system stands

What exists (verified against `ChunkParserRegistry.cpp` and
`src/Library/Painters/`):

- **38 painter chunk kinds** (2026-08-20 recount; several of this doc's own
  candidates -- `expression_painter`, `ramp_painter`, the
  `scalar_painter { painter ... }` bridge -- have since shipped and are
  counted in that total), including a strong 3D-solid noise family
  (perlin/simplex/wavelet/gabor/worley/perlin-worley/turbulence/curl/
  domain-warp/reaction-diffusion/SDF), art-directed voronoi, image painters,
  and combinators (`blend_painter`, `channel_painter`,
  `composite_function2d_painter`).
- **Real DAG composition already works**: every `colora`/`colorb`/`mask`/
  `source` slot is a `ValueKind::Reference` into the painter manager, so scenes
  can express arbitrarily deep painter graphs — but only by declaring every
  intermediate node as its own named top-level chunk (no inline nesting).
- **An expression evaluator already ships**: `expression_function2d`
  (`ExpressionEval.h` — recursive-descent → flat postfix stack machine,
  allocation-free, `def` let-bindings, `smoothstep/mix/clamp/step/select/...`
  builtins).  It is **UV-only (u,v), scalar-output-only**, and has **no noise
  builtins**.
- **Two disjoint pipes**: `IPainter` (color, JH-uplift) vs `IScalarPainter`
  (physical scalars, never uplifted).  The scalar pipe has 10 forms; the only
  two that vary across a surface are `function2d` (UV-only) and `texture`
  (raster images only).

Gaps (each verified, not guessed):

| # | Gap | Evidence |
|---|-----|----------|
| G1 | **No spatially-varying scalar from 3D noise.**  `scalar_painter` cannot see any of the 12 3D noise painters; `PerlinScalarPainter`/`WorleyScalarPainter` were explicitly deferred in the IScalarPainter refactor. | ISCALARPAINTER_REFACTOR.md "deferred"; `scalar_painter` descriptor text at ChunkParserRegistry.cpp:1473 says only `function2d`/`texture` vary |
| G2 | **No color ramp / remap / gain-contrast primitive.**  Every noise painter lerps between exactly two endpoint painters; the only contrast control is routing through UV-only `expression_function2d`. | procedural-textures.md documents this limitation |
| G3 | **No UV/domain transform chunk.**  `UVTransformPainter` exists in C++ (glTF/Blender bridge only) but has no ASCII chunk; no triplanar anywhere (`grep triplanar` = 0). | UVTransformPainter.h; no `uv_transform_painter` in registry |
| G4 | **No anti-repetition machinery for image textures** (hex-tiling, texture bombing) and no element-scattering generator (Substance FX-map class). | inventory |
| G5 | **`Voronoi3DPainter` samples object space** (`ptObjIntersec`) while every sibling 3D painter samples world space — unremarked inconsistency. | Voronoi3DPainter vs Perlin3DPainter et al. |
| G6 | **High per-chunk cost**: a new painter touches ~12 files (class, RISE_API, IJob, Job, registry, 5 build projects, chunk-count test, scene).  This taxes "many small nodes" architectures and rewards few, powerful, parameterized chunks. | GerstnerWavePainter (e980c9d9) and ControlledSmoothness2D (597c7509) commit footprints |
| G7 | **Painter values don't round-trip in either GUI** — rebinding a slot persists, but editing a bound painter's own parameters does not (no `PainterIntrospection`, no `Painter` DirtyTracker category). | docs/gui/MATERIAL_EDITOR.md §2.1 |

## 2. Design constraints from the measured adoption record

The single most important input.  The features in §4 are worthless if shipped
the way `scalar_painter` was; the record (arcs 73–85, instancing commits
289cddd2 / a105d6ea) is unambiguous:

- **C-ADV.  Advice ≈ 0.**  Design notes / Info diagnostics naming
  `scalar_painter` fired up to 30×/session, were demonstrably read, and drove
  **0/24 lifetime adoptions** across two models and multiple framings.
  Voluntary/opt-in tools: 0/64.
- **C-TYPE.  The failure is a slot-typing prior, not ignorance.**  Worked
  examples lifted painter *diversity* (floor 1→3) because models copy examples;
  the same lever moved spatially-varying roughness **0/24** because "roughness
  is a number" is a typing prior that prose and examples do not override.  Any
  proposal here must include a **structural or callable-verb lever** for that
  prior, or it will replay the 0/24.
- **C-VERB.  When advice fails, ship a verb.**  `collapse_to_instances` is the
  fresh precedent: note fired 5×, model made zero attempts; the escalation is a
  zero-required-argument mutating verb that does the rewrite itself, refusing
  unless it can prove no-op-on-render semantics.
- **C-READ.  The read-set is the knowledge boundary.**  Agents pull 2–5 skills
  by hook line.  `procedural-textures.md` — the skill every advisory points to —
  was read **0/6** in one probe batch.  Teaching content must live in the
  proven pulls (`object-modeling-recipes.md`, `materials-and-media-basics.md`)
  with exactly one execution-validated parsing example per mechanism.
- **C-TEXT.  LLMs author linear text far more reliably than graph-as-data.**
  External evidence: metaprogramming vs raw-JSON-graph benchmark (arXiv
  2409.00856: 76% vs 53% pass@1 on Web Audio graphs), MatFormer's staged
  decomposition, VLMaterial emitting *programs* rather than graphs (ICLR 2025).
  Internal evidence: doc 85's governing finding — LLMs are reliable at "small,
  declarative, named-parameter grammars with relative/low-dimensional numbers"
  and unreliable at cross-referenced structure at scale.  A deep painter DAG
  (G6: every node a named top-level chunk) is exactly the
  cross-reference-consistency shape LLMs fumble; a single expression string is
  exactly the shape they excel at.
- **C-MEAS.  Census, not vibes.**  The eval harness already has the checker
  ops this needs (`any_param_references_kind`, `distinct_chunk_kinds`,
  `objects_reaching_kinds`); N=3 minimum; cross-provider before believing a
  null; pre-committed stop rules.

## 3. External survey — condensed verdict

Full survey with citations lives in the review artifact; the load-bearing
conclusions:

- **The minimal expressive core is small**: generators (noise+fBm, worley
  F1/F2/ID, wave/brick/checker/gradient) + domain ops (TRS transform, triplanar,
  domain warp) + combinators (math, **color ramp**, mix/blend modes) + per-cell
  hash.  That set covers ~90% of what artists build in Blender/Substance;
  RISE already has most of the *generators* and lacks mostly the *domain ops
  and combinators* (G1–G3).
- **Architecture comparison** (expressiveness / C++ cost / chunk-language fit /
  LLM-authorability / UI-editability):
  - *Extend the native painter graph*: medium-high / **low-med** / excellent /
    high-for-flat-graphs / high.  Best infra reuse; G6 taxes node count.
  - *MaterialX subset*: high / med-high / poor (XML, second graph paradigm) /
    medium / med-high.  **Crib the vocabulary (e.g. `unifiednoise` type-selector
    precedent), not the format.**
  - *OSL*: highest / **high (LLVM JIT, sandboxing, closure model)** / good /
    high / low.  Overkill for painter slots; rejected.
  - *SeExpr-class expression language*: med-high / **low-med** / excellent
    (one string parameter) / **high (C-TEXT)** / medium.  No loops; ternary
    only; noise builtins as functions.  Disney shipped exactly this scope for
    exactly this job.
  - *GLSL-snippet JIT*: high / high / good / high / low.  Fragment-shader
    idioms don't map to spectral CPU painters; rejected.
- **Precedent for the hybrid**: Houdini VOPs are a node UI over VEX text — the
  graph is sugar over the language, not a second system.  That is the shape
  proposed below: expression core for the hard cases, chunk graph (already a
  DAG) for structure the UI can edit.
- **Filtering**: production procedurals anti-alias via footprint-aware octave
  fading (fade an fBm octave as `filterwidth × frequency → 1`) plus analytically
  filterable primitives (Gabor).  RISE already propagates `txFootprint` for
  image mips, and `Wavelet3D`/`Gabor3D` are band-limited — the plumbing exists
  to do this properly.

## 4. Candidates

### P1 — Texture expressions: grow `ExpressionEval` into the texture language

**The core bet.**  Extend the existing expression VM with a 3D evaluation
domain, a vector type, and noise/pattern builtins; expose it on **both pipes**:

- `expression_painter` — color pipe.  Body evaluates to a color (vec3, Rec.709
  linear) or a scalar (broadcast).  JH-uplifted per-sample on spectral paths
  (same cost class as image texels; §7 decision 1 — ships in Phase 1, with the
  uplift benchmark as a gate item).
- `scalar_painter { expression <body> }` — a new form on the existing chunk,
  scalar pipe, **no uplift**.  This is the G1 killer and the direct attack on
  the 0/24 deficit: spatially-varying roughness becomes one string on a chunk
  the model already writes.

Language surface (all reusing existing painter cores, factored into shared free
functions — no new noise implementations):

- Context: `u, v` (existing), `P`/`Po` (world/object position), `N` (shading
  normal), `fw` (filter width, from footprint), optional keyframable `time`.
- Types: scalar + `vec3` (constructors, `.x/.y/.z`, dot/cross/length/
  normalize, component-wise arithmetic).  Typed at compile time in the same
  postfix VM.
- Builtins added to the existing set: `perlin(p)`, `fbm(p, octaves, gain,
  lacunarity)` (footprint-aware octave fade), `turbulence`, `ridged`,
  `worley_f1/f2/f2f1/id(p, jitter)`, `cellhash(id)`, `ramp(t, pos₀,val₀, …)`
  (variadic stops, the G2 fix in expression space), plus the existing
  `smoothstep/mix/clamp/…`.  Domain warping needs no builtin — it's
  composition: `fbm(P + a*vec3(fbm(P+o1), fbm(P+o2), fbm(P+o3)), …)`.
- `seed` chunk parameter for determinism; `def` let-bindings already exist and
  keep bodies readable.

Why this is the center: C-TEXT says one linear string is the representation
agents author reliably; G6 says per-chunk cost punishes a wide node vocabulary —
an expression body adds unbounded pattern variety at **zero marginal chunks**;
and the existing VM means this is an extension, not a new engine.

Cost: **L** (VM vec3 type + ~10 builtins + 2 chunk surfaces + parse-diagnostic
quality + tests/scenes).  Risks: per-sample cost vs hand-coded painters
(mitigate: it's already a compiled flat VM; benchmark; optional bake-to-grid
escape hatch later); expression bodies are opaque to a future node UI (accepted:
the chunk DAG remains the structural layer, per the VOPs/VEX precedent).

### P2 — Vocabulary gap-fill: few, powerful, parameterized chunks

1. **`scalar_painter { painter <name> channel <R|G|B|A> scale bias }`** —
   generalize the existing `texture` form (raster-only) to **any** color
   painter.  One small form makes every colour painter + P1 expressions
   bindable to every physical-scalar slot.  Highest leverage-per-line in this doc.
   Cost: **S**.  (Note: reads `GetColor` channel post-uplift semantics — same
   caveat as `PainterToScalarAdapter`; fine for procedural masks, documented.)
2. **`ramp_painter`** — multi-stop color ramp (positions + colors +
   interpolation mode) driven by a source painter's channel.  The universal
   scalar→color remap every surveyed system has and RISE lacks (G2).  Keeps
   JH uplift *eager at the stop colors* (like `uniformcolor_painter`), so it's
   also the cheap way to get rich color from scalar-only expressions.
   Cost: **S–M**.
3. **`mapping_painter`** — domain transform wrapper: TRS on UV or on the 3D
   domain of a wrapped painter, plus `projection uv|world|object|triplanar`
   (blend-weighted triplanar for image painters on UV-less geometry).  Also
   finally registers the existing `UVTransformPainter` surface (G3).
   Cost: **M** (triplanar is the real work).
4. **Blend modes on `blend_painter`** (`mode multiply|screen|overlay|add`,
   default `mix`).  Cost: **S**.
5. Hygiene riders: `space world|object` parameter on `voronoi3d_painter`
   (G5, default preserves current behavior, documented).

### P3 — Stochastic richness: kill repetition, scatter elements

1. **`stochastic_tile_painter`** — hex-tiling with histogram-preserving
   blending (Heitz & Neyret 2018; Burley JCGT 2019) over a source image
   painter: one photo of bark/plaster/rust → unbounded non-repeating cover.
   This is the single biggest *realism-per-scene-byte* win for image-based
   materials, and nothing in the current set addresses it (G4).  Cost: **M**.
2. **`scatter_painter`** — texture-bombing / FX-map-lite: stamps a source
   painter (or a weighted list) per cell with jittered position/rotation/scale/
   value over a background painter.  Unlocks the discrete-element richness
   (leaves, rivets, scratches, stains, screws) that noise fundamentally cannot
   produce.  Cost: **M**.

### P4 — Adoption wiring (the make-or-break; C-ADV through C-MEAS)

Shipped **with** Phase 1, not after it:

1. **Summoned-category placement**: the worked examples go into
   `object-modeling-recipes.md` and `materials-and-media-basics.md` (the proven
   read-set), one execution-validated parsing example per mechanism
   (expression-roughness, ramp, stochastic tile); `procedural-textures.md`
   gets the full reference + a hook line rewrite; a pointer, not the content,
   goes anywhere else.
2. **`vary_material` verb** — the `collapse_to_instances` analog for the 0/24
   deficit.  Zero required arguments; finds the most prominent material whose
   microsurface slots are all numeric constants; rewrites roughness (and
   optionally tint) to a bound expression/noise graph derived from the
   material's family with deterministic jitter; refuses (document
   byte-identical) when nothing qualifies.  Note-condition and verb share one
   qualifying predicate so they can't disagree (the 289cddd2 discipline).
   The render/validate design note exists only to *name the verb* — advice
   asking for the rewrite itself is measured dead.
3. **`insert_material_scaffold` generalization**: rebase the 5 fixed families
   on P1 expressions and add families — or fold the scaffold into
   `vary_material`'s recipe table.  (Decide during implementation; don't build
   two recipe systems.)
4. **Eval**: extend the bare-prompt build scenario's checkpoints
   (`any_param_references_kind: scalar_painter|expression_painter` — the 0/24
   metric; painter-kind diversity floor) and add one `rich_material_closeup`
   scenario (macro single-object prompt where texture richness *is* the task,
   with render-band + document checkpoints).  N=3, gemini-3.5-flash instrument,
   cross-provider check before believing any null.  Pre-committed success
   gates below.

Cost: **M** total, dominated by the verb.

### P5 — UI round-trip and the human-editability contract

**Design principle: the LLM authors structure; the human tweaks through
surfaces the language exposes on purpose.**  The expression grammar already
has the seam that makes this work — the `param` / `def` split — and P1's
language design must treat it as the *contract* between machine authorship and
human editing, not an implementation detail.  Three tiers:

1. **Tier 1 — params are auto-generated controls (the ~90% path).**
   Extend the existing `param <name> <value>` line with optional UI metadata:
   `param ring_scale 4.0 min 0.5 max 20 label "Ring density"` (compiler
   ignores metadata; introspection consumes it).  The inspector renders every
   param as a slider/field with live re-render through the same property-panel
   path every chunk already uses.  Humans scrub named knobs; they never read
   the body.  The matching agent-side teaching rule (one worked example, per
   C-READ): **every art-directable number goes in a `param` with a range,
   never a literal in the body** — which makes LLM output human-editable *by
   construction*.  This is the Shadertoy-uniform / SeExpr-control / ISF
   pattern.  `seed` gets a scrub/dice button for free.
2. **Tier 2 — defs are visible, previewable stages.**  The `def` chain is a
   linear dataflow (`def rings…`, `def grain…`, `def wear…`).  Because the VM
   is a flat compiled slot machine, the inspector can evaluate any def's slot
   over a preview patch and render a per-stage thumbnail strip — most of a
   node graph's explanatory value with none of the canvas machinery.  Click a
   stage → its params focus.  If P5.3 ever builds, defs render as nodes.
3. **Tier 3 — body text as the guarded last resort.**  Monospace editor with
   validate-on-edit via the one validation authority; extend the expression
   builder's errors to carry **character offsets** so mistakes underline in
   place; revert; edits ride shared-undo like any painter edit.

**Composition-boundary discipline (matters more than any widget): visual
decisions live in visual chunks.**  Expressions compute scalar *fields*;
`ramp_painter` colorizes (gradient-stop editor), `blend_painter` combines
(mode dropdown + mask thumbnails), `mapping_painter` places (TRS controls).
The agent is taught "field in the expression, color in the ramp" — so the
thing humans most want to tweak (color) always has a visual editor and never
requires touching expression text.

Work items:

1. **`PainterIntrospection` + `Painter` DirtyTracker category + painter edit
   ops** — the already-flagged G7 gap.  Prerequisite for *any* painter editing
   in either GUI; also what makes agent-side `propose_patch` on painter chunks
   and human-side inspector edits converge on one path.  **Pulled into
   Phase 1** (see §5): without it, Tier-1 param sliders have no round-trip.
   Cost: **M**.
2. **Param-metadata grammar + introspection mapping** (Tier 1) and
   **offset-carrying expression diagnostics** (Tier 3) — small, but must land
   in P1's grammar day one; retrofitting ranges later strands every
   already-authored body without them.  Cost: **S**.
3. **Property-panel affordances**: param sliders, multiline expression field
   with live `validate`, stop-list editor for `ramp_painter`, def-stage
   thumbnail strip; the roadmapped thumbnail engine (MATERIAL_EDITOR.md §4)
   supplies the previews.  Cost: **M**.
4. **Node-graph canvas: deferred.**  The named-chunk DAG *is* the graph model
   and descriptors already carry typed ports, so a canvas (MATERIAL_EDITOR.md
   C2) is generatable later; it should be justified by adoption data, not
   built on faith.

## 5. Recommended bundle and phasing

**Phase 1 — the bet (P1 + P2.1 + P2.2 + P4 + P5.1/P5.2):** texture
expressions on both pipes **including the color form from day 1 (decision 1)**
— with the param-metadata grammar, offset-carrying diagnostics, `time`, and
reserved `fw` from day one — the any-painter→scalar bridge, `ramp_painter`,
the full adoption wiring + eval scenario, and the painter introspection/edit-op
round-trip that makes params human-scrubbable.  This is the smallest set that
attacks the measured deficit (0/24 spatially-varying scalars) with a
structural lever, a verb, and a census, while keeping the human co-edit story
real from the first ship.

**Phase 2 — richness breadth (P2.3–P2.5 + P3 + P5.3 panel affordances +
filtering plumbing):** mapping/triplanar, blend modes, stochastic tiling,
scatter, def-stage thumbnails + ramp editor, footprint (`fw`) plumbing +
`fbm` octave fade.  Re-measure; these mostly widen what Phase 1's mechanisms
can express.

**Phase 3 — P5.4 node canvas (committed, decision 6).**

Pre-committed Phase-1 success gates (C-MEAS): (a) `rich_material_closeup` —
≥2/3 runs bind ≥1 spatially-varying microsurface scalar without being told to;
(b) bare-prompt build — painter-kind diversity floor holds ≥3 and ≥1 run uses a
new kind; (c) `vary_material` — when the note fires, the verb is called in
≥1/3 runs (the collapse_to_instances hypothesis, currently unmeasured).  Miss
all three → the mechanism is wrong, stop and re-diagnose before adding chunks.

## 6. Rejected alternatives

- **OSL embedding** — LLVM JIT toolchain, sandboxing a real language, closure
  execution model: the integration cost buys expressiveness P1 already
  captures at painter-slot scope.
- **MaterialX literal adoption** — XML + a second graph paradigm alongside
  chunks; we crib its node vocabulary and `unifiednoise` compaction instead.
- **GLSL snippets** — fragment-shader idioms (implicit derivatives, texture
  units) don't map to a spectral CPU renderer's painter contract.
- **Big-vocabulary node set** (Blender-parity chunk-per-node) — G6 makes each
  chunk expensive and C-TEXT says deep named-chunk graphs are the shape agents
  fumble; expressions invert both.

## 7. Decisions (user review, 2026-08-19)

1. **Color expressions ship from the get-go.**  `expression_painter` (color
   pipe, per-sample JH uplift) lands in Phase 1 alongside the scalar form —
   not deferred behind a benchmark.  The uplift-cost benchmark becomes a
   Phase-1 *gate item* (measure vs `png_painter`-class cost) rather than a
   decision input.
2. **Verb + note first; no tolls.**  `vary_material` + the naming note are the
   Phase-1 mechanism.  A toll on bare-numeric microsurface slots is only
   revisited if Phase-1 gate (c) fails.
3. **Filtering sequenced as recommended:** `fw` is reserved in the grammar
   from day 1 (evaluates to 0.0 = "point sample, no filter info" until the
   plumbing lands) so bodies stay stable; footprint plumbing + `fbm` octave
   fade wire in Phase 2.
4. **`time` is in**: exposed as a keyframable chunk param (Gerstner
   precedent), available as a variable in bodies from day 1.
5. **Name is `expression_painter`.**  `expression_function2d` stays as-is for
   now; a later refactor may fold it into/behind the new VM surface (tracked
   as a deferred follow-up, not Phase-1 scope).
6. **The node-graph canvas is ON the roadmap** (Phase 3, committed — no longer
   "only if data demands").  Defs-as-nodes and descriptor-typed ports are the
   design basis.

## 8. Phase-1 slice plan (post-review; each slice runs the implementation-review-loop to zero P1)

- **S1 — VM core** (worker: sonnet; opus reviewer on the type system).
  `ExpressionEval` gains: compile-time-typed `vec3` (constructors, `.x/.y/.z`,
  dot/cross/length/normalize, component-wise arith); context variables `P`,
  `Po`, `N`, `fw` (0.0 until Phase 2), `time`; noise builtins (`perlin`,
  `fbm`, `turbulence`, `ridged`, `worley_f1/f2/f2f1/id`, `cellhash`, variadic
  `ramp`) **factored from the existing painter cores into shared free
  functions** — no new noise implementations; param-metadata grammar
  (`min/max/step/label`, parsed + stored, ignored by eval); errors carry
  character offsets.  Headless unit test: golden values, type errors, offset
  positions.  No chunks yet.
- **S2 — chunk surfaces** (sonnet).  `expression_painter` (color pipe;
  per-sample JH uplift on spectral paths — reuse the image-painter
  `GetColorNM`/`GetSpectrum` uplift route) + `scalar_painter { expression …
  }` form (no uplift).  Full 12-file checklist (IJob/Job/RISE_API/registry/5
  build projects/chunk-count tests) + regression scenes.  **Gate item:
  benchmark** expression eval vs `perlin3d_painter` and uplift cost vs
  `png_painter` on a canonical scene (decision 1 made this a gate, not a
  fork).
- **S3 — P2.1 + P2.2** (sonnet).  `scalar_painter { painter <name> channel …
  }` bridge; `ramp_painter` with eager-uplift stops.
- **S4 — introspection round-trip** (sonnet; the DirtyTracker deferred
  follow-up from the agentic-surface arc lands here).  `PainterIntrospection`,
  `Painter` DirtyTracker category, painter edit ops, param sliders via the
  property-snapshot bridges (Mac now; Windows mirror edits carry the standing
  owed-MSVC-compile caveat).
- **S5 — adoption wiring** (sonnet).  Skill examples into the proven read-set
  (execution-validated: parse + derive + render + luma band, both taught
  angles); `vary_material` verb + naming note over one shared qualifying
  predicate; `insert_material_scaffold` rebased on expressions (or folded into
  the verb's recipe table — implementer's call, one recipe system);
  MCP + chat tool-list parity test covers the new verb (the 1ed4e7c3
  two-name-lists lesson).
- **S6 — eval census** (haiku for runs, Fable arbitration).
  `rich_material_closeup` scenario + checkpoint extensions; N=3
  gemini-3.5-flash + one cross-provider check; evaluate the §5 gates; write
  the arc log.  **Offline half shipped 2026-08-20** (scenario + fixture +
  checkpoint extensions + harness coverage, all keyless/replay-only — see
  §10).  **Gate (c)'s scenario shipped 2026-08-20 too** (`constant_materials_polish`
  + its fixture + the new `toolCallCount` trajectory checkpoint op, wired
  into both `s6_census_gemini.json` and `s6_census_crossprovider.json` — see
  §10); the live half (actually running N=3 against a hosted model and
  reading all three gates, now including (c)) is the pending next action,
  tracked in §10.

Sequencing: S1→S2 strictly ordered; S3 can overlap S2 reviews; S4 after S2;
S5 after S2+S3 (examples must parse against shipped chunks); S6 last.

## 9. Source reports

Produced 2026-08-19 by four parallel research agents (painter inventory,
material-binding audit, adoption-record distillation, external survey); key
factual claims (ExpressionEval, UVTransformPainter, scalar_painter forms,
36-tool MCP surface) spot-verified against the tree.  External survey citations
(MaterialX/OSL/SeExpr specs, Heitz & Neyret 2018, Burley 2019, Lagae 2010,
Quilez, MatFormer 2022, VLMaterial 2025, arXiv 2409.00856) are listed in the
review artifact for this doc.

## 10. S6 census protocol

The handoff for whoever runs the live half (a keyed session, or a human)
with zero rediscovery.  Everything below the live-run commands has already
been built and gated keylessly in this environment (no API keys were
available here) — see the "offline half shipped 2026-08-20" status line at
the top of this doc.  **All THREE §5 gates are now measurable off one live
run** (2026-08-20 update): gate (c)'s own scenario shipped alongside (a)/(b)'s
and is wired into both runconfigs below — the "gate (c) out of scope for
this slice" / "gate (c) separately tracked" language from the original
version of this section is gone; read all three off the SAME
`s6_census_gemini`/`s6_census_crossprovider` run.

### What's already in place

- **`evals/scenarios/rich_material_closeup.json`** — the new gate-(a)
  scenario (macro brass-doorknob-on-a-wooden-plinth product shot; texture
  richness IS the task, no mechanism named in the prompt).  Its checkpoint
  battery carries the three P4.4 checks as GATING, weighted checkpoints:
  `any_param_references_kind:scalar_painter` (metricLabel
  `spatially_varying_scalar`, weight 3 — the headline), `distinct_chunk_kinds`
  over `["expression_painter","ramp_painter"]` (metricLabel
  `expression_family`, weight 2), and painter-kind diversity (metricLabel
  `painter_kinds`, `distinctMin:3`, weight 1.5) — plus the same
  render/diagnostics/trajectory structural checks the
  `build_watch_scene`/`build_stilllife_scene` family uses.  Paired fixture:
  `evals/fixtures/rich_material_closeup.fixture.jsonl` (a scripted build that
  satisfies every checkpoint — proven by T10 below).
- **`evals/scenarios/bare_prompt_build_courtyard.json` and
  `bare_prompt_build_cozy_study.json`** — each gained one ADDITIVE, non-
  gating checkpoint (`weight:0`, `distinctMin:0`, metricLabel
  `expression_family`) that counts `expression_painter`/`ramp_painter`
  presence without disturbing either scenario's existing pass criteria — the
  `any_param_references_kind:scalar_painter` checkpoint these two scenarios
  need for gate (b) already existed before this slice (S0/material-richness
  P0); only the expression/ramp half was missing.
- **`tests/AgentEvalCheckTest.cpp`** — `TestExpressionRampFamilyAndScalarExpressionFormCheckpoints`
  (T-s6) exercises both new checkpoint shapes against synthetic pass/fail
  documents (the `scalar_painter { expression ... }` FORM specifically, not
  just the pre-S1 `function2d` form T-mr already covered; `expression_painter`
  alone; `ramp_painter` alone, proving the family check is genuinely
  either-kind; the `distinctMin:0` vacuous-pass shape the two bare_prompt
  scenarios rely on).  T10 (`TestSeedScenariosCheckpointsAreTrue`) dynamically
  enumerates every `evals/scenarios/*.json` and proves each one's checkpoints
  are ALL literally true of its own committed fixture — `rich_material_closeup`
  and the two edited bare_prompt scenarios are covered automatically, no
  hard-coded id list to update.
- **`evals/scenarios/constant_materials_polish.json`** — the new gate-(c)
  scenario.  Unlike `rich_material_closeup` (an EMPTY starting scene the
  model builds from scratch), this one's `scene.inline` is an
  ALREADY-BUILT scene carrying three `ggx_material` chunks whose
  `alphax`/`alphay` are bare numeric constants (0.3/0.25/0.4), each bound to
  its own `standard_object` — exactly `AgentSession`'s
  `kMicrosurfaceKinds`/`kConstantMicrosurfaceGate=3` predicate
  (`src/Library/Agent/AgentSession.cpp`'s `ComputeDesignNoteConditionsFromDoc_`),
  so design-note condition D fires on the document's VERY FIRST render — no
  build-up needed across turns.  The prompt ("The materials in this scene
  feel flat and uniform — make them feel physically real.") names neither
  the mechanism nor the verb.  Checkpoint battery: the HEADLINE is a
  `trajectory` checkpoint with the new `toolCallCount` field
  (`{"name":"vary_material","min":1}`, metricLabel `vary_material_calls`,
  weight 3) — this is the literal gate-(c) measurement, "was `vary_material`
  called at least once in this run"; alongside it, an
  `any_param_references_kind:scalar_painter` document checkpoint (metricLabel
  `spatially_varying_scalar`, the SAME check gate (a) uses, so the two
  scenarios' census numbers read on one vocabulary) proving a
  spatially-varying binding actually landed, plus the usual chunk_count/
  render/diagnostics/trajectory structural battery (`requiredToolInOrder:
  ["render","vary_material"]` — the literal "note fires, then the verb is
  called" sequencing).  Paired fixture:
  `evals/fixtures/constant_materials_polish.fixture.jsonl` (render → note
  fires → `vary_material` → render → done — proven by T10 below; the
  luminaire's `scale` was tuned 30.0 → 9.0 to land the render checkpoint's
  meanLuma band, see the checkpoint's own inline comment).
- **`src/Library/Agent/AgentEvalRunner.cpp`**'s `trajectory` checkpoint kind gained the
  `toolCallCount` field (`{name, min, max?}`) — the gate-(c) census
  primitive, generalizing the existing `askUserMin`/`askUserMax` counting
  shape to any named tool.  It is the ONE `trajectory` field that populates
  `CheckOutcome::metricValue` (an explicit, non-empty `metricLabel` is
  REQUIRED on it for exactly that reason — unlike a `document` op it has no
  `op` name for the cross-checkpoint metricLabel-dedupe pass to fall back
  to).  Both the per-field doc comment on
  `ValidateTrajectoryCheckpointTypes` and the metric-op enumeration comment
  above `ValidateCheckpointFieldTypes` (previously "today only five
  `document` ops ... populate a metricValue") now name it.
- **`tests/AgentEvalCheckTest.cpp`** — `TestToolCallCountLoaderValidation`
  (T-tcc(a)) exercises every load-time validation rule the new field carries
  (missing/empty/wrong-typed `name`, missing/negative `min`, an inverted
  `max<min` band, the required `metricLabel`, and a metricLabel-collision
  RED case mirroring `param_binding`'s); `TestTrajectoryToolCallCountAssertion`
  (T-tcc(b)) drives it through REAL `RunScenario` runs (one call, zero calls,
  two calls; min/max bounds both directions; metricValue reported on BOTH
  the pass and the fail branch, mirroring `any_param_references_kind`'s
  CheckDocumentKind precedent; a DIFFERENT tool name observing zero, proving
  the count is keyed on `name`).
- **`evals/runconfigs/s6_census_gemini.json`** and
  **`s6_census_crossprovider.json`** — the two runconfigs below, now
  carrying FOUR scenarios each (rich_material_closeup,
  bare_prompt_build_courtyard, bare_prompt_build_cozy_study,
  constant_materials_polish), already written and JSON-valid.

### Commands

Primary instrument, N=3, gemini-3.5-flash (the same instrument
`bare_prompt_baseline.json` was measured on — comparable to that prior
series):

```sh
export GEMINI_API_KEY=...
./bin/rise --agent-eval evals/runconfigs/s6_census_gemini.json
python3 tools/eval_report.py report evals/runs/s6_census_gemini
```

Cross-provider check — run this BEFORE concluding any gate below read a
genuine null (C-MEAS: "cross-provider before believing a null"), not only
when a gate looks surprising:

```sh
export OPENAI_API_KEY=...
./bin/rise --agent-eval evals/runconfigs/s6_census_crossprovider.json
python3 tools/eval_report.py report evals/runs/s6_census_crossprovider
```

Both runconfigs are idempotent-resumable (`evals/README.md` "Resume /
skip-if-completed") — re-running the same command after a partial/crashed
run only executes the missing cells.  Read the `spatially_varying_scalar`,
`expression_family`, `painter_kinds`, and `vary_material_calls` metric
columns from `tools/eval_report.py`'s per-checkpoint breakdown (or the raw
`<runDir>/.../results.jsonl` `checkpoints[].metricValue`/`metricLabel`
fields) — these are the census numbers the three gates below read, not the
scenarios' `allPassed`/pass@1 (which also folds in the unrelated structural
checks).

### The three pre-committed gates (§5, verbatim)

> (a) `rich_material_closeup` — ≥2/3 runs bind ≥1 spatially-varying
> microsurface scalar without being told to; (b) bare-prompt build —
> painter-kind diversity floor holds ≥3 and ≥1 run uses a new kind; (c)
> `vary_material` — when the note fires, the verb is called in ≥1/3 runs
> (the collapse_to_instances hypothesis, currently unmeasured).  Miss all
> three → the mechanism is wrong, stop and re-diagnose before adding chunks.

Reading each gate off the census output — **all three are now measured by
the SAME `s6_census_gemini`/`s6_census_crossprovider` run** (gate (c)'s
"currently unmeasured" parenthetical above is the pre-2026-08-20 state; the
scenario now exists):

- **Gate (a)**: over the 3 `rich_material_closeup` repeats (gemini arm),
  count how many have `spatially_varying_scalar` (the
  `any_param_references_kind:scalar_painter` checkpoint) passing —
  `checkpoints[].passed` where `metricLabel=="spatially_varying_scalar"`.
  Need ≥2/3.
- **Gate (b)**: over the 6 bare-prompt repeats (3 courtyard + 3 cozy_study,
  gemini arm), the `painter_kinds` checkpoint's `metricValue` (the distinct
  qualifying-painter-kind count) must hold `>= 3` on every run that was
  already passing before this slice (this is a PRE-EXISTING gate — the S6
  slice added no new obligation here), AND at least 1 run across the 6 must
  show a NEW kind — read the `expression_family` metric's `metricValue`
  (`0`, `1`, or `2`): any run with `metricValue >= 1` is a "new kind used"
  hit, since these scenarios' fixtures predate `expression_painter`/
  `ramp_painter` and read `0` today by construction (see the checkpoint's
  own inline comment in both scenario files).
- **Gate (c)**: over the 3 `constant_materials_polish` repeats (gemini arm),
  count how many have `vary_material_calls` (the `toolCallCount` trajectory
  checkpoint, `{"name":"vary_material","min":1}`) passing —
  `checkpoints[].passed` where `metricLabel=="vary_material_calls"`, which is
  identical to reading `metricValue >= 1` on that same checkpoint (the
  checkpoint's own `min:1` bound IS the ">= 1 call" test).  Need ≥1/3.  This
  scenario's starting scene already carries three all-numeric-microsurface
  `ggx_material` chunks, so design-note condition D — and therefore the
  note naming `vary_material` — fires on the run's FIRST `render` call, not
  something that has to accumulate across a multi-turn build; a run that
  never calls `vary_material` after that first render is a genuine miss, not
  an artifact of the note not having fired yet.

### What to do on each outcome

- **All three gates hit their bar on the gemini arm**: run the cross-provider
  check.  If it agrees (same qualitative verdict, not necessarily the same
  exact fraction), the offline harness's job is done — write the arc log
  entry (mirror the style of the 73–85 arc logs already in this repo: what
  was measured, the numbers, the verdict) and update this doc's status line.
  Phase 1's adoption-wiring mechanism is validated; Phase 2 breadth work (§5)
  can proceed.
- **A gate misses on the gemini arm**: run the cross-provider check BEFORE
  concluding anything (C-MEAS).  If the second provider ALSO misses, this is
  very likely a real null, not an instrument quirk — do not add more chunks
  or expand the vocabulary (§6's rejected-alternatives discipline); instead
  re-diagnose why the mechanism (verb + naming note, painter diversity
  already proven to move on worked examples per C-TYPE) isn't reaching this
  specific gate, the same way the 2026-05/06 VCM-MIS arc re-diagnosed
  instead of re-guessing.  If the second provider PASSES where gemini
  missed, this is a single-instrument artifact — widen the cross-provider
  run to N=3 there too before treating either number as the headline, and
  consider whether gemini-3.5-flash specifically needs its own worked
  example (mirroring the `procedural-textures.md` 0/6-read lesson: the fix
  is usually in the read-set, not the mechanism).  A gate-(c) miss
  specifically ("the note fired zero `vary_material` calls across all 3
  repeats") is the `collapse_to_instances`-style null §5's parenthetical
  anticipated — read it exactly like an (a)/(b) miss, not as some lesser
  signal, now that it carries the same N=3-repeats measurement shape.
- **All three gates miss on BOTH providers**: §5's literal stop condition —
  "miss all three → the mechanism is wrong, stop and re-diagnose before
  adding chunks."  This is no longer a two-gate reading propped up by a
  pending follow-up (the pre-2026-08-20 state of this section, when gate
  (c) had no scenario yet): all three gates now come off the SAME run, so a
  clean triple-miss on both providers is the real, fully-measured signal §5
  describes.  Do not add more chunks or expand the vocabulary; re-diagnose
  the mechanism itself per §6's discipline.
- **Some gates miss, some hit, on BOTH providers**: NOT §5's stop condition
  (that reads "miss all three").  Surface the partial result to the user —
  which gate(s) hit, which missed, on which provider — and treat each
  missing gate per the "a gate misses" bullet above.  A mechanism that lands
  gate (a) (the model reaches for the scalar pipe when texture richness IS
  the task) but misses gate (c) (the model doesn't reach for `vary_material`
  specifically when a design note names it) is real, actionable information
  about WHICH half of the adoption story needs work, not a reason to invoke
  the all-three stop rule.

## 11. Census results

**Round 1 (2026-08-20, gemini-3.5-flash, N=3, gemini-only per user
scoping; run dir `evals/runs/s6_census_gemini`):**

- **Gate (a): PASS 3/3.**  `rich_material_closeup`
  `metric[spatially_varying_scalar] = 1.00` — every run bound a
  spatially-varying microsurface scalar unprompted.  The 0/24 lifetime
  deficit is closed when the task summons material work.
- **Gate (b): MISS, provisional.**  Both bare-prompt builds read
  `metric[expression_family] = 0.00` (no spontaneous expression/ramp use)
  and `metric[painter_kinds]` mean 2.33 (< the ≥3 floor).  Provisional
  because §10's own protocol requires a cross-provider check before
  believing a null and this round was scoped to gemini only.  Consistent
  with the summoned-category law (generic prompts don't summon texture
  work).
- **Gate (c): unmeasured in round 1** (scenario shipped after the run);
  `constant_materials_polish` is wired into the runconfig for round 2.
- **Bonus finding:** one run drove `vary_material` against a
  `pbr_metallic_roughness_material` and the derive gate refused — pbr
  resolves `roughness` in the COLOUR painter manager, so the verb's
  scalar_painter emission was invisible to it.  Refusal semantics held
  (document byte-identical).  Fixed same-day: the verb emits an
  `expression_painter` for pbr (colour pipe; PainterToScalarAdapter reads
  only GetColor, so no uplift distortion), shared-predicate-consistent
  with condition D's clause text.
- Per the pre-committed rules, gate (a)'s pass means Phase 2 proceeds;
  gate (b)'s tuning continues via measurement, and the toll decision
  (§7 decision 2) still waits on gate (c).
