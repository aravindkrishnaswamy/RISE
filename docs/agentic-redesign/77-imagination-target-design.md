# Imagination Targets — Sketch-First Scene Building (Arc 77 Design, 2026-08-10)

> **Status:** DESIGN APPROVED by the user 2026-08-10; §12's three open
> decisions are RESOLVED (recorded inline below): (1) fold into
> `file_part_plan` — confirmed, and the just-in-time alternative is
> technically foreclosed by the chunk→part join problem; (2) force level
> = STRICTLY REQUIRED, no `"none"` escape — the escape valve lives at the
> GATE level (the 3-refusal give-up), not the field level, and any
> polygon is accepted so the anti-Goodhart property moves to the shape
> dimension; (3) Phase 2 provider image generation APPROVED IN PRINCIPLE
> for the whole-scene target, capability-gated per model, deferred — and
> consequently v1 carries NO scene-level target: the `region` field is
> CUT from the schema (parts only), which also forfeits the layout-vs-
> objectmap presence-check windfall (§6, retained below for the record;
> returns as its own lever if built-but-invisible persists post-v1).
> Implementation begins as slice G3a (artifact) then G3b (comparison).
> Successor
> mechanism to the 75-arc's post-arc slices (R1/R2/G1/G2); builds on every
> banked result there.  The user's framing (2026-08-10, verbatim intent):
> *force the model to first "imagine" a 2D image of the thing it wants to
> create, then use that as the target for iterating — for the whole scene
> AND per part.  Humans imagine a rough final image, then model toward it;
> the target is not for perfect reproduction, it forces continual checking
> of progress.*

---

## 1. Why this fits the evidence better than what we just built

The 48-trajectory census and the fantasy-scene forensic localized the
failure precisely:

- The looking loop FIRES (4 renders, exposure corrected stepwise) but only
  revises axes with an obvious "better" direction: 13 position / 10 power /
  6 color / **0 geometry**.
- Models CAN compose (14-part CSG gear, swept retort neck) but only when
  the subject has an obvious operational reading.  Organic parts get
  primitives.
- R2 (`replace_geometry_scaffold`) removed the *cost* of fixing a form.
  G1 (`render{isolate}`) removed the *visibility* barrier.  Neither
  supplies a **criterion**.  "Does this read as a wing?" is an aesthetic
  judgement, and the mechanism law says models do not act on judgements —
  they act on facts that block and on task-specific facts in results of
  calls they just made.

A self-authored target converts the aesthetic judgement into a fact:
*distance from the thing I said I was making*.  That is the only currency
this workstream has measured to work.  The user's human analogy is exactly
this: the sketch is not a spec, it is a **progress-checking mechanism**.

## 2. The mechanism in one paragraph

Before building, the model files an **imagined plan**: per part, a
construction method (G2, already shipped), a 2D **outline sketch** (a
closed polygon, authored as a point list), the **view** that outline
represents (front/side/top), and a **frame region** where the part should
sit in the final image.  The harness rasterizes each outline into a
deterministic silhouette mask and stores it as a session-lifetime target.
From then on, every isolated look at a part can carry a factual
shape-match report (IoU against the sketch), and every full-frame
objectmap render carries the layout intent next to the measured actuals.
Nothing ever gates on the match numbers — the gate is on *filing* the
imagination, never on *achieving* it.

## 3. The composed loop (why this completes the system)

Every post-arc slice slots into one loop this design closes:

```
imagine (G2v2: plan + sketch + layout)     <- THIS DESIGN
   -> build (scaffolds, batched inserts)   <- S2/S3b/E3, R1a
   -> look  (E4 forces cadence; G1 frames the part;
             R1b keeps looks cheap)        <- shipped
   -> measure (IoU vs sketch, layout vs objectmap)  <- THIS DESIGN
   -> fix   (R2 makes form revision one call)       <- shipped
   -> look again ...
```

E4 already forces looks every ≤10 mutations.  The target gives each look
a criterion.  G1 gives the look a subject.  R2 makes acting on it cheap.
The sketch is the missing first step and the missing measuring stick.

## 4. The target artifact — generation, precisely

### 4.1 Why model-authored geometry, not generated bitmaps (v1)

Three forms were considered for HOW the imagination artifact comes to be:

| Form | Verdict v1 |
|---|---|
| **Provider image generation** (the user's literal proposal) | Deferred to Phase 2 (§10).  Richest signal (colour, mood, composition) but: needs a second model/provider call and new codec plumbing; is nondeterministic run-to-run (noisier trajectory comparison); and the local qwen measurement arm cannot generate images at all, which would halve the roster discipline this workstream runs on. |
| **Model-authored coarse bitmap** (e.g. 32×32 of 0/1 as text) | Rejected: token-heavy at useful resolution, and produces an artifact nothing else can consume. |
| **Model-authored closed polygon** (point list; harness rasterizes) | **v1.**  Deterministic, provider-independent, works on both roster models, cheap in tokens — and the census already proved models author point lists well (`blended_chain` paths were the one creative act that measured as working).  Decisive extra: a 2D outline is exactly what `sweep_geometry.profile_point` consumes, so the imagined shape and a construction input are the SAME artifact — the target teaches the fix. |

### 4.2 The plan schema (G2's `file_part_plan`, version 2)

One artifact, one gate.  We EXTEND the shipped tool rather than adding a
second gate (two gates stacking refusals on the first insert would fight,
and the plan IS the imagination artifact under the user's framing).

```
file_part_plan { parts: [ {
    part:         string            (required)
    construction: enum              (required; unchanged from G2)
    outline:      "x y; x y; ..."   (required; >= 3 points, closed
                                     implicitly; NO opt-out value)
    view:         front|side|top    (optional, default front)
    note:         string            (optional)
} ] }
```

(`region` was cut per the resolved decisions — the whole-scene target is
Phase 2's generated image, not a layout-intent field.)

**Force level (RESOLVED: strictly required, user 2026-08-10).**
`outline` is a required field with no `"none"` escape.  Every part gets
a sketch; fog gets a blob, and a blob is LEGAL — any polygon is
accepted, so the gate still forces a decision rather than a particular
answer, with the anti-Goodhart freedom moved from the presence dimension
to the shape dimension (blob-sketching is a measurable census outcome,
not a hidden failure).  Two properties keep this from stranding anyone:
schema rejections (`-32602`) never touch the gate's refusal counter, and
the gate's own 3-refusal give-up remains the bounded, session-level
escape valve.

Re-filing REPLACES the plan (humans re-sketch; imagination refines).
Re-filing is always legal, never gated, and each filing is a trajectory
record — the census sees sketch evolution for free.

### 4.3 Rasterization (deterministic, in-memory)

- Parse points as doubles; reject `< 3` points, non-finite values, or a
  zero-area bounding box with a clean `-32602` naming the offending part.
- Canvas: fixed **256×256**, matching the R1b agent render cap.
- Normalize: fit the outline's own bbox into the canvas at **fill 0.85**
  (the same constant G1's auto-framing uses — the two masks are then
  directly comparable), preserving aspect, letterboxed, centered.
- Fill: scanline, **even-odd rule**, top-left pixel-center convention, no
  anti-aliasing (it is a mask, not art).  Pure double→int math, no
  platform-dependent rounding.  Self-intersecting polygons are therefore
  well-defined rather than rejected.
- One ring in v1.  Multi-ring (holes) is a recorded extension, not built.
- The filing RESULT echoes each rasterized mask back as a small inline
  PNG plus facts: point count, filled-area fraction, aspect ratio.  The
  model SEES what it sketched — the imagination loop closes at filing
  time, and a model can immediately re-file if the sketch reads wrong.
- **Retention interaction (G3a review finding, decision recorded
  2026-08-10):** the transport keeps only the MOST RECENT tool-result
  image live in context (a single global slot, verb-agnostic), so the
  sketch composite's PIXELS are elided by the model's next look; the
  per-part FACTS survive.  Decision: accept this (option b) rather than
  pin sketches in a persistent image pool — G3b's comparison composite
  `[sketch | silhouette | overlay]` re-surfaces the sketch at exactly
  the moment of consultation, making the comparison call the act that
  refreshes the target, which IS the mechanism.  A pinned pool would
  charge permanent per-request vision cost during stretches with no
  consultation.  Risk accepted and already covered by the §11 stop
  rule: if sketches are filed but comparisons never happen, the
  escalation (auto-compare on isolate) re-surfaces the sketch anyway.
  G3b's brief MUST treat the composite re-embedding as a requirement,
  not a nicety.

Wire security is preserved by construction: the wire carries only numbers
and names — never bytes, never paths — so `SetReferenceImages`' host-only
rationale is untouched.  Targets are session-lifetime state like the plan
itself; they are NEVER written into the scene document.

## 5. The comparison — part level

### 5.1 Call shape

Additive param on the shipped verb:

```
render { isolate: "<object>", target: "<part name>", ... }
```

`target` requires `isolate` (clean -32602 otherwise).  The join between
the plan's free-text part name and a concrete object is made BY THE MODEL
at comparison time — the one moment it is unambiguous and free.  This
dissolves G2's declined per-part join problem: no naming convention, no
optional field the model must volunteer at authoring time.

### 5.2 Vantage

The comparison render uses an **axis-aligned vantage matching the
target's declared `view`** (front = 0° azimuth / 0° elevation, side =
90°, top = looking down), auto-framed exactly as G1 does.  Axis-aligned
silhouettes are what humans sketch; G1's fixed three-quarter constant
generalizes to a small named-vantage set.  A caller-supplied camera
overrides this (as in G1) and the result then states the vantage fact.

### 5.3 The metric

- The rendered part's silhouette mask comes from an internal single-pass
  identity render at the same pose (the objectmap machinery, not the
  beauty pixels) — exact, mode-independent.
- Crop the rendered mask to its own 2D bbox, nearest-neighbour scale onto
  the target's 256×256 normalized canvas, compute **IoU**.  This makes
  the metric translation- and scale-invariant — a pure SHAPE match,
  deliberately: "is it wing-shaped", not "is it in the right place"
  (place is the layout's job, §6).
- Report as facts: `iou`, `mirroredIou` (X-flip; costless and models
  sketch mirrored views often), area fractions, aspect ratios, and the
  part's 3D thinnest-axis ratio (already in the G1 payload via
  bboxMin/Max) — the last makes a billboard cheat visible as a fact
  without any judgement attached.
- Include a small side-by-side composite PNG: `[ sketch | silhouette |
  overlay ]` — `compare_to_reference` has exact precedent, and models act
  on what they see.

### 5.4 What it does NOT do

No thresholds.  No pass/fail.  No gating on IoU, ever — per the user's
own framing, the sketch is a progress mechanism, not a reproduction spec.
The number is reported and never characterized.  (Goodhart analysis §8.)

## 6. The comparison — scene level (CUT from v1; retained for the record)

RESOLVED 2026-08-10: v1 has NO scene-level target.  The user's call is
that the whole-image target should be a provider-GENERATED image (§10),
not a model-authored layout; regions are therefore cut rather than
shipped as a stopgap.  The design below is retained only so the
presence-check windfall it carried is not forgotten — if
built-but-invisible persists after v1, this returns as its own lever.

The layout intent (`region` per part) would have been checked against
the real frame WITHOUT any join machinery:

- On any full-frame **objectmap** render, when a plan with regions
  exists, the result gains a `layout` block: the filed intent verbatim
  (`part`, `region`) alongside the measured actuals the legend already
  carries (`object`, pixel bbox, coverage) — side by side, unjoined.
  The model joins by reading; it is acting on facts about its own two
  artifacts.  Zero new render cost, zero heuristics that could be wrong.
- A part whose intended region is `"none"` appears with intent `none`.
- An intended part with NO plausible actual (a region intent filed,
  nothing rendered there) is exactly the built-but-invisible case —
  E4-lite's presence check falls out of this design for free, without a
  new mechanism.

## 7. Gate semantics (unchanged machinery, heavier artifact)

The G2 gate machinery is reused verbatim: refuse-until-filed on
geometry-creating calls (all six gated paths including the patch arm),
3-refusal cap, give-up notice, shared counter, `retriable:false`,
`--agent-part-plan-gate` switch, census anchors.  The only change is the
plan schema itself (§4.2) and the refusal text naming the new fields.

Risk accepted and measured: a heavier first call may raise the give-up
rate.  That is a pre-committed stop-rule condition (§11), not a silent
hope.

## 8. Goodhart analysis

| Cheat | Why it doesn't pay |
|---|---|
| Flat billboard matching the front silhouette | Nothing gates on IoU, so there is no score to farm; the thinnest-axis ratio sits in the same payload as a plain fact; and the E4-forced full-scene looks show a cutout from the camera's real (non-axis-aligned) vantage. |
| Sketch a trivial blob so anything matches | The filing echo shows the model its own blob; the census measures outline complexity (point count, area fraction) as a first-class distribution — a degenerate-sketch strategy is a visible, reportable outcome, not a hidden win. |
| ~~`outline: "none"` everywhere~~ | STALE ROW — superseded by resolved decision 2 (§12): there is NO `"none"` value; outline is strictly required.  The nearest real strategy is blob-everywhere, covered above. |
| Re-file the sketch to match whatever got built | Legal and VISIBLE — every filing is a trajectory record; "sketch revised toward the build" vs "build revised toward the sketch" is distinguishable in the census by ordering. |

## 9. What is deliberately NOT in v1

- **No provider image generation** (Phase 2, §10).
- **No enforcement of sketch↔build correspondence** — non-binding
  throughout, same reasoning as G2.
- **No multi-object isolate** — a part built from several objects can
  only be compared object-by-object in v1.  Known limitation; extending
  `isolate` to accept a name list is the natural follow-up if the census
  shows multi-object parts dominating.
- **No persistence of targets into the scene document** — session-only.
- **No GUI surfacing of the sketches** — product follow-up, recorded.

## 10. Phase 2 (APPROVED IN PRINCIPLE 2026-08-10, deferred): generated-image whole-scene target

The user's original form — an actual generated image as the whole-scene
target — adds what outlines cannot: colour, lighting mood, composition.
RESOLVED: this IS the whole-scene mechanism (v1 ships no scene-level
target at all), it is approved in principle now, and it is
**capability-gated per model** — "enabled for any model that supports
it", detected per provider rather than hardcoded per roster arm; a model
without image generation simply runs parts-only, and cross-arm
comparison then reports that difference honestly instead of hiding it.
Deferred to a future stage; when built, it lands as a host-mediated
generation step registered through the EXISTING
`SetReferenceImages`/`compare_to_reference` machinery (host-curated
bytes — its security model already fits exactly).  Costs accepted at
that point: provider dependency and per-run nondeterminism.  v1's
registration path should be shaped so this is additive.

## 11. Measurement plan (pre-committed before any result exists)

Census fingerprints, all from trajectory tool-call args + results:
1. Plans filed with real outlines vs `none` (per model, per part class).
2. Outline complexity distribution (points, area fraction).
3. `render{isolate, target}` calls made at all (does the criterion get
   consulted?).
4. **Geometry revisions occurring AFTER a target comparison** — the
   headline metric; baseline is the measured ZERO of the fantasy run.
5. IoU trajectory within a run (does the number move over iterations?).
6. Re-filings (sketch refinement) and their ordering vs builds.
7. Gate give-up rate vs the G2-baseline rate.

Stop rules:
- Give-up rate rises materially over G2's → the artifact is too heavy
  for one call; SPLIT (construction at gate time, sketch on first
  isolate) rather than abandoning.
- Outlines filed but `target` never consulted → the fact must move into
  results automatically (auto-compare on isolate of a bound object) —
  the payload-fact escalation, pre-named here.
- Comparisons happen, IoU reported, still zero form revisions → the
  criterion was not the binding constraint; the accept-the-deficit exit
  for the whole imagination hypothesis, recorded honestly.
- Success = fingerprint 4 goes nonzero on multiple subjects with IoU
  (5) climbing — then Phase 2 becomes worth pricing.

Method rules as always: counterweight `build_ambiguous_scene` in every
batch, both roster models, one runDir, instrument changes recorded at
phase boundaries, census from authored tool-call args validated against
the final scene.

## 12. Decisions (RESOLVED by the user, 2026-08-10)

1. **Fold into `file_part_plan`: YES.**  Beyond the machinery-reuse
   argument, the alternative is technically foreclosed: just-in-time
   per-part sketch forcing requires the gate to know which part a chunk
   belongs to — the chunk→part join problem G2's review already
   established as unsolvable without unreliable volunteered fields.
   Upfront, all-parts-at-once is the only clean force point, and it
   matches the human analogy (sketch the whole figure, then refine).
2. **Force level: strictly required, no `"none"` escape.**  See §4.2 for
   the two properties that keep this bounded (schema errors never burn
   the gate counter; the 3-refusal give-up is the session-level escape).
3. **Phase 2: approved in principle, capability-gated per model,
   deferred.**  See §10.  Consequence: v1 is parts-only; `region` cut.

Implementation plan: **slice G3a** (schema v2 + rasterizer + target
store + filing echo) then **slice G3b** (`render{isolate, target}`
comparison), sequential, each through the full gate + review loop —
split deliberately small given that G1 alone produced four review P1s.


## 13. Battery record — gemini arm (2026-08-11, runDir `evals/runs/imagine_g3_gemini`)

9/9 runs (figure ×3, courtyard anchor ×3, counterweight ×3), gemini-3.5-flash,
tree `67ea79b7`; qwen arm deferred by the user.  Verdict as lookup:

- **Filing: 9/9 PROACTIVE — the gate never fired.**  Zero refusals, zero
  give-ups in the entire battery; every run filed before its first geometry
  call.  Stop rule (a) not fired.  Law refinement worth recording: a stated
  BLOCKING precondition in a tool schema is complied with proactively —
  unlike advice, which measures zero, a requirement the model believes binds
  moves behaviour before its enforcement is ever exercised.
- **Constructions: honest and subject-matched.**  Courtyard: csg×3 per run
  (columns/arches); figure: chain dragon + csg wizard + displaced ground in
  all three runs; counterweight: predominantly primitive (correct for a
  pedestal display).  The representation-choice translation happens when
  forced to decide.
- **Outlines: pro-forma.**  89% are 3–5 points; max 8.  Filing happened;
  imagining barely did.  (The chain-dragon declarations DID translate into
  serpentine blended_chain bodies — visibly better than the plank-wing era —
  but the sketch artifact itself carried little information.)
- **Consultation: ZERO.  Stop rule (b) FIRED** — 0 `render{isolate,target}`
  comparisons in 9/9 runs.  Headline metric (geometry revisions after a
  comparison) is vacuously 0 against its thrice-measured baseline of 0.
- **The escalation named in (b) is REFUTED by the same census**: isolate was
  called 0 times in 64 renders battery-wide, so auto-compare-on-isolate
  would never fire either.  `replace_geometry_scaffold`: also 0.  Every
  consultation surface built this arc sits UNVISITED; the one look surface
  models actually visit — 64/64 — is the full-frame render.
- Counterweight guard: ask-rate 3/3 (anchor 2/3) — the gate did not disturb
  ask-for-ambiguity behaviour.  Wall: 3.3–8.3 min/run, ~50 min total.

**Where the evidence points**: a whole-scene target compared at FULL-FRAME
render time — §10's Phase 2, the user's original proposal — is the only
target that sits on the visited surface, and it needs no part↔object join.
The per-part sketch pipeline stays (filing works, declarations translate);
its comparison half awaits either Phase 2's pull or a payload-fact
attachment to full-frame renders (which would need the join solved).
Decision deferred to the user.

## 14. Phase 2 live result — the zero moved (2026-08-11, `evals/runs/imagine_p2_demo`)

One live gemini run, figure subject, full mechanism.  **The headline metric
that survived every previous lever at zero is no longer zero.**

| | pre-Phase-2 battery (9 runs) | this run |
|---|---|---|
| target comparisons consulted | **0** | **16** |
| geometry verbs within 3 calls of a comparison | **0** | **7** |
| param patches after a comparison | — | 21 |

Sequence: the model called `imagine_scene` as its **6th call, before any
geometry**, authoring its own description ("a surreal psychedelic landscape
at twilight ... a magnificent dragon with iridescent glowing scales coiling
through a vibrant cosmic ring"); Gemini generated it; thereafter all 16
full-frame renders carried the RMSE facts and the [target | render] strip.

**Why this worked where the per-part comparison did not**: it is on the
surface models already visit.  Every voluntary consultation surface built
this arc measured 0/64 (isolate, target-compare, replace_geometry_scaffold);
this one fired 16 times in one run because receiving it costs nothing.  The
law extends: *a fact the model must ASK for is not consulted; the same fact
attached to a call it already makes is acted on.*

**Honest limits, recorded:** RMSE did not converge (0.27 -> 0.44 -> 0.32 ->
0.41) -- the model looks and acts but does not hill-climb, which is expected
and arguably correct (a photoreal AI painting is not reachable by SDF
primitives, and nothing gates on the number).  The render still does not
resemble the target -- but the dragon it built IS serpentine and coiling,
matching both its own description and its filed `chain` declaration.
`aspectMatched:false` on all 16 (512x279 target vs 4:3 renders); honest per
the contract, but a scene-target-aware aspect request would make the
comparison fairer -- recorded, unbuilt.

**API-shape verdict (the thing mocks could not settle):** `gemini-3.6-flash-
image` does not exist; the account offers `gemini-3.1-flash-image` (and
`gemini-3-pro-image`; the `imagen-4.0-*` family is `predict`-method, a
different shape).  The first run 404'd and **the anti-stranding rule fired
exactly as designed** -- status-only message, imagine requirement disarmed,
run completed plan-only with a full five-part plan.  Fixed by env override
with no rebuild, then in the compiled default.  OpenAI's `gpt-image-1`
verified present on the live account.

### 14.1 OpenAI arm verified (2026-08-11, `evals/runs/imagine_p2_openai`)

`gpt-5.6-terra` + `gpt-image-1`, same figure subject, one run.  The second
provider path — different endpoint (`/v1/images/generations`), different auth
header (`Bearer`), different response shape (`data[0].b64_json` vs gemini's
nested `inlineData`) — worked **first live attempt, no override needed**.

- `imagine_scene` called 8th, again BEFORE any geometry; 512x512 image.
- 4 comparisons consulted, 2 geometry verbs within 3 calls of one.
- RMSE 0.198 / 0.197 / 0.194 / 0.195 — flat but *monotonically slightly
  improving*, and notably tighter than gemini's 0.27–0.44 wander (square
  target, so `aspectMatched` is less punishing).
- Its filed plan is the best decomposition seen in any run to date:
  terrain=displaced, dragon body+tail=**chain**, dragon **wings=sweep**,
  wizard=primitive, crystals=primitive, mist=displaced.  This is the first
  time any model declared `sweep` for wings — the exact plank-wing defect
  that started this whole line of work (75-arc POST-ARC R2).

Cross-provider read: both models imagine unprompted and both consult the
comparison, at different rates (gemini 16, openai 4 — openai reads the
document far more between edits).  The mechanism is not gemini-specific.
## 15. Phase 2b — the corrective slice (2026-08-11)

Phase 2 moved the headline metric off zero (§14) and the imagining half
worked structurally — primitives 50 % → 25 %, the first-ever `sweep`
declared for wings.  The **comparison** half did not.  Rendering the
gemini run's result settled it: the 16 consultations were followed by 96
position / 45 scale / 32 orientation / **23 emissive_scale / 10 power / 9
radiance_scale / 6 scattering** edits, `emissive_scale` cranked to 25.0 and
15.0, light power to 80/50 — a blown-out, washed-out frame the user judged
**worse than the pre-Phase-2 baseline**.

**Diagnosis, confirmed by looking at the render: RMSE over full-frame RGB is
a tone metric, not a structure metric.**  It is dominated by large flat
areas, so the only gradient it exposes to a path-traced SDF scene chasing a
painterly AI image is global exposure / emission / fog.  Nothing in it can
ever reward a better wing.  A REPRODUCTION metric was shipped to serve a
PROGRESS-CHECK purpose — which §1 and §2 say was never the intent.  The
artifact stays; only the comparison changed.

Three changes, all landed:

1. **No score, anywhere.**  `rmse` and the six per-channel means are gone
   from the `sceneTarget` block, both tool descriptions, the transcript
   outcome line and the skill.  What remains cannot be chased: `imagined`,
   `targetWidth`/`targetHeight`, `composite`, `compositeWidth`/`Height`.
   *Models act on facts, and a fact whose only movable axis degrades the
   picture is worse than no fact.*  The composite IMAGE is the comparison —
   a vision model can judge two pictures side by side without a number.
   (The RMSE machinery itself is untouched; `compare_to_reference` and the
   eval grader still use it, where a host-registered reference genuinely IS
   a reproduction spec.)
2. **The render is never shown smaller.**  Phase 2's `[target | render]`
   strip REPLACED the frame, so the model saw its own scene at half of an
   already-shrunken canvas.  The composite is now **target stacked ABOVE
   the render**, on a canvas exactly as wide as the frame the same call
   would have returned — same `imageMaxEdge`, same never-upscale rule,
   pinned in a test against a real `ReadImage(maxEdge)`.  Consequence: the
   composite exists only when an inline image was asked for; a render that
   wanted no picture gets the facts and no bytes, exactly as before the
   mechanism existed.  The exactly-one-`png_base64` discipline is unchanged
   (an encode failure now falls back to the plain frame rather than
   returning nothing).
3. **The target is reachable.**  The host appends a fixed STYLE directive
   to the model's description — plain matte flat shading, simple geometric
   forms, neutral even lighting, plain background, no painterly texture, no
   photographic lens effects, no text (`Agent::ChatImageStyleDirective`,
   overridable by `RISE_IMAGE_STYLE_PROMPT`).  The description remains the
   SUBJECT and comes first, unaltered: the imagining stays the model's act,
   only the rendering style is constrained.  Both tool surfaces state this
   factually.

Unchanged by this slice: the per-part sketch comparison (§5) — it measures
structure and is reachable — plus the part-plan gate, the imagine
requirement, disarm-on-provider-failure, the spend cap, and all
capability/credential handling.

**The law this refines.**  §14 established that *a fact attached to a call
the model already makes is acted on*.  Phase 2b adds the other half: *which*
fact decides what the acting looks like.  An attached fact is not free — it
selects the axis the model will move.  Attaching the wrong one does not
merely fail to help; it actively steers, and it steered this run into a
worse picture than no mechanism at all.
