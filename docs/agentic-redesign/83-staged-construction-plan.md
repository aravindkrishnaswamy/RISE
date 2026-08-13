# Harness-Driven Construction — Units, Budgets, and Refinement (Arc 83 Plan, 2026-08-12)

> **Status:** PLAN FOR DISCUSSION.  Nothing built.  User direction 2026-08-12:
> *"let's step back and apply this holistically, the harness now has to
> maintain the context and drive the build step by step.  What other aspects
> of scenes should have its own construction step and associated budget?
> Also let's make sure this won't conflict / will work well with a user who is
> then giving specific instructions in the chat once the scene is built."*

---

## 1. The principle, stated once

**One fresh completion per unit of work, and the harness drives the loop.**

Arc 79 measured it: a builder in a fresh minimal context produces ~22 SDF
parts for a figure, where the same model deep in a session produces 1–2.
Arc 82 §10.4 measured the corollary the hard way — a single completion yields
roughly **one response-worth of output whatever you ask of it**:

| clean room | calls/run | output/run |
|---|---|---|
| `build_element` | 5 (one per element) | ~90–100 SDF parts |
| `light_scene` | 1 (all lighting) | 6–8 lights |
| `populate_scene` | 1 (all population) | ~10–20 objects |

Richness scales with the NUMBER OF CALLS, not with instructions inside a
call.  So the unit of a call must be one *thing*, never one *category*.

Corollary the harness must own: **voluntary re-use is zero.**  The model may
already call `light_scene` four times; it calls it once in every run
measured.  A loop that depends on the model choosing to iterate will not run.

## 2. The general shape: PLAN → per-item completions

`build_element` already has the right structure, and it generalizes:

```
file_build_plan  -> N elements          -> build_element  x N   (fresh each)
plan_lighting    -> N lighting intents  -> build_light    x N   (fresh each)
plan_population  -> N repeat groups     -> place_group    x N   (fresh each)
```

The plan step is one cheap completion that enumerates units.  Each unit then
gets its own fresh completion.  **The harness runs the per-item loop inside a
single verb call**, so the model's turn count does not grow with scene
complexity — which matters, because turn cost is already 40–68 per build.

## 3. Which aspects deserve a construction step

Each entry states the evidence, since this project's failures have come from
building against assumed causes.

### 3.1 Fix the unit on what exists

| step | current unit | proposed unit | evidence |
|---|---|---|---|
| **Lighting** | the whole scene (1 call) | one lighting intent (key / fill / rim / practical / sky) | 6–8 lights per run regardless; ≤1 area light because 7 four-chunk chains do not fit one answer (82 §10.4) |
| **Population** | the whole scene (1 call) | one repeat group, per element | ~10–20 objects in one answer; benchmark has 38 repeats across 8 geometry groups |

### 3.2 New steps worth adding

| step | unit | evidence it is needed |
|---|---|---|
| **Environment / atmosphere** | one call (it is one thing) | Agent scenes render on flat fields or a **black void** (gpt run, 82 §8); the benchmark has graded water with depth falloff. No agent run has ever authored an environment gradient, fog, or god-rays. |
| **Camera / framing** | one call (there is one camera) | Arc 79 cropped both figures at the frame edge; s6 put the horizon at the top with an empty middle. Framing is authored inline today, never as a considered step. |

Both are single-unit steps, so one completion each is the CORRECT unit — no
loop needed.  They are cheap and they target the two most visible remaining
gaps against the benchmark.

### 3.3 Considered and rejected

- **Materials / texture** — already the agent's strongest axis.  The user's
  own read at the start of this workstream: *"the model was creative with
  textures, and the shapes are very sadly simple."*  Do not spend a step
  where there is no deficit.
- **Arrangement** — `place_element` is a mechanical transform, not a
  completion, and it converged in one call per element in recent runs.  The
  arc-79 thrashing (24 calls) has not recurred.
- **Post / exposure** — no measured defect.

### 3.4 Budgets

Each step gets an explicit call budget, stated in its result so a census can
see it, and the harness stops there:

| step | budget | rationale |
|---|---|---|
| elements | ≤ 8 build calls | today's plans hold 4–5 |
| lighting intents | ≤ 6 | every run converges on 6–8 lights |
| repeat groups | ≤ 8 | benchmark uses 8 repeated geometries |
| environment | 1 | one environment |
| camera | 1 | one camera |

**Cost, stated honestly up front.** A build goes from ~7 host completions to
~20.  At current sizes that is roughly a 2–3× increase in provider spend per
build.  The budgets above are the ceiling that keeps it bounded, and they
must be reported so a run that costs more can be explained.  If the owner's
budget will not carry it, the order in §5 is designed so the two cheapest
high-value steps land first.

## 4. Refinement — the part that must not break

**This is the biggest risk in the plan.**  `BuildProtocolActive_` keys on
launch switches only.  It never checks whether the scene already exists, and
**nothing ever ends the protocol**.  Three concrete conflicts follow, all
real today:

1. User opens a finished scene and says *"move the jellyfish left"* → session
   starts in PLAN with every gate armed; the first hand-authored edit can be
   refused and the model is pushed toward `file_build_plan` for a scene that
   is already built.
2. User says *"delete the manta ray"* → COMPOSE delete-ban **refuses removing
   a form-bearing chunk**, against an explicit user instruction.
3. User says *"just show me a render"* → the first-COMPOSE-render gate
   demands `populate_scene` first.

A harness that refuses what the user just asked for is worse than one that
never gated anything.

### 4.1 The rule: gates govern autonomous construction, never refinement

Introduce an explicit session mode:

- **BUILDING** — from session start until the agent's first final answer,
  AND only when the scene started empty (a starter/empty document).
- **REFINING** — every other case: a session that opens a non-empty scene the
  agent did not author, and every turn after the first build completes.

In REFINING **no construction gate fires**: no phase refusals, no delete-ban,
no forced clean rooms, no populate-before-render. Every verb stays
AVAILABLE — `build_element`, `light_scene`, `populate_scene` can all be
called, and should be for a substantial addition — but nothing is compelled.

The one-sentence version, worth putting in the code: **compulsion belongs to
the first build; availability is forever.**

### 4.2 Why not detect user intent instead

Sniffing "the user asked me to delete this" from chat text is an intent
classifier in the refusal path, and it fails open or closed at the worst
moment.  The mode rule is coarse but predictable, and predictability is what
a user tweaking a scene needs.

### 4.3 The existing safety valve, and its limit

Even during BUILDING, every gate shares a 3-refusal cap and a give-up, so a
user fighting a gate escapes within three turns.  That is a real mitigation
and it is why this is not currently catastrophic — but three wasted turns
against an explicit instruction is still bad, and the mode rule removes it.

**Residual risk, named:** a user interjecting mid-build ("no, remove that")
while still in BUILDING can still hit a gate for up to three turns.  Accepted
for now; revisit if it shows up in practice.

## 5. Proposed order

1. **Fix the lighting unit** (plan → per-intent builds).  It has three
   pre-committed predictions from 82 §10.4 that this either confirms or
   kills, and it needs no new concepts.
2. **Refinement mode.**  Small, and it removes three live conflicts with the
   product's actual use.  Arguably first if any user is driving the GUI today.
3. **Environment / atmosphere step.**  Biggest visible gap to the benchmark.
4. **Fix the population unit** (per element).
5. **Camera / framing step.**

## 6. Measurement

Per step: units planned, completions spent, output per unit, and the
category's headline (lights and area-light share; objects and reuse ratio;
tonal spread for environment; subject coverage for camera).

Standing rule from 82 §10: **decompose before repeating.**  Report per-unit
numbers, never a category total that multiplies a stable per-unit figure by a
variable unit count.

### 6.1 Correcting one of the falsifiers before it is used

82 §10.4 predicted "N lighting calls yield roughly N answer-fulls of lights".
Taken literally that is absurd — six intents × one answer-worth (6–8 lights)
would be forty lights.  The error is in the unit, and it matters:

**A unit must be sized so that ONE response is ONE unit's work.**  For an
element that is ~20 SDF parts.  For lighting it is ONE LIGHT SOURCE, not
"the lights for this intent".  So the per-intent call authors a single light
— which may be a four-chunk area light, and that is the point: with a whole
response to spend on one light, the expensive physical form is affordable
where it was not when seven lights had to share one answer.

The corrected predictions:

1. **Light COUNT stays ~6–8** (it is set by the plan step, not by the loop).
   A large jump in count means the unit is still wrong.
2. **Area-light SHARE rises sharply, with NO change to palette prose.**  This
   is the real falsifier: if share does not move when each light gets its own
   completion, then cost was never the mechanism and §1's principle does not
   transfer from construction to lighting.
3. Per-element population scales repeats with element count.

---

## 7. SLICE 1 RESULT — the unit fix works; the share claim was OVERREAD (2026-08-12)

> **§7 correction, same day:** "FALSIFIED" below was itself an N=1 overreach
> — the pre-slice record is 1/0/0/0/2, so ZERO was already the modal outcome
> and one post-slice zero cannot distinguish "share dropped" from "nothing
> changed".  The owner's challenge ("something must be off") exposed the part
> that IS established: a structural defect.  The intent line binds the
> builder — an intent saying "point light" makes authoring an area light an
> act of disobedience — so the design GUARANTEED the palette could not
> matter, regardless of share.  The pre-slice area lights happened precisely
> because the old single call chose kinds while looking at the palette.
> Fixed same day: the planner now describes JOBS and is forbidden to name a
> kind, and it is told as fact that the authoring step chooses among real
> emitting surfaces, a sun-and-sky model, and zero-area idealizations.

`evals/runs/imagine_s9_lightunit`, gemini, N=1, `kLightPalette` byte-identical.

| falsifier (§6.1) | prediction | result |
|---|---|---|
| light COUNT stays ~6–8 | yes | **6** ✓ |
| area-light SHARE rises | yes | **0 of 6** ✗ |

The loop itself works exactly as designed: 6 intents planned, 6 built, 7
completions, one light per intent, no over-production.  **And it produced
fewer area lights than the single-call version it replaced** (which managed
1, 0, 0, 0, 2 across five runs).

**So room was not the mechanism, and 82 §10.4's "chunk economy" explanation
is dead too.**  Both of this workstream's explanations for area-light
avoidance are now falsified — first taste, then cost.

### 7.1 What the data actually says

The plan step's own output is the tell.  All six intents:

1. "High above **directional light** casting cyan godrays…"
2. "Soft white key **spot light** from upper right…"
3. "Warm magenta and gold coral reef **point light** near lower left…"
4. "Deep cyan **fill light** from camera position…"
5. "Bright teal bioluminescent accent **point light** near upper right…"
6. "Cool blue **rim light** from top behind the sunken ruin…"

**Every intent names an instrument, and the planning prompt contains no
palette.**  It was given none deliberately — it writes no scene text, so a
palette looked like waste.  With no vocabulary offered it fell back on
conventional CG lighting language (key/fill/rim, spot/point/directional), and
each per-intent builder then faithfully executed the instrument it had
already been handed.  The builder sees the palette but the decision was made
one step earlier.

**The kind is chosen where the options are not visible.**  That is a design
error with a name, and it is a new instance of a pattern this project keeps
hitting: a fact must be attached to the call that MAKES the decision, not to
the call that carries it out.

### 7.2 What follows

The fix is cheap and testable: either give the planning step the palette, or
require intents to describe the JOB rather than the INSTRUMENT ("light
falling on the mermaid from above through the water surface" instead of
"directional light from above").  Probably both — the job phrasing removes
the premature commitment, the palette gives the planner something to commit
to when it must.

Prediction, pre-committed: with the palette moved (or added) to the planning
step, area-light share rises and light count stays 6–8.  **If share STILL
does not move, then no mechanism this project has proposed explains it, and
the honest conclusion is that these models will not author a four-chunk
emitter for a scene's general lighting whatever the harness does — at which
point the answer is to stop asking and make the physical light a first-class
one-chunk affordance in the scene language itself.**

### 7.3 Cost

7 completions for lighting where there was 1.  Justified only if the fix in
§7.2 moves the number; a 7× spend that produces the same lighting is a
regression, and slice 1 alone currently IS that.  This must be re-measured
before the remaining slices assume the pattern is sound.

## 8. SLICE 1 REDESIGN — source-first lighting (owner design, 2026-08-12)

The owner replaced both my candidate fixes with a better frame: *"ask the
model to think about the most physically correct lighting for the scenario
and then build that in one go — for a lamp on a desk a light bulb that
glows, for an outdoor scene the hosek sky, for an interior a series of
rectangles emulating ceiling lights."*

**Why this is the right diagnosis: the question asked selects the form of
the answer.**  "What lighting jobs does this scene need?" is a cinematography
question whose native vocabulary is key/fill/rim — words that REFER to studio
instruments — which is why 11 of 11 intents produced zero-area lights even
with kind-naming forbidden and the palette in view.  "What in this world
physically emits light?" has THINGS as answers — a bulb, a window, the sky,
ceiling panels, a glowing creature — and things become emissive objects.
The physics falls out of the vocabulary with no preference language anywhere.

Structure: enumeration completion (sources that physically exist, one per
line, budget 6) then ONE build completion for all of them.  The per-intent
loop dies: its justification (room in the answer) was falsified — gpt wrote
three full chains in one answer — and cost returns from 7 completions to ~2.

Ledger of the three lighting designs, kept honest:

| design | completions | area lights | verdict |
|---|---|---|---|
| single call, category unit (arc 81) | 1 | 1/0/0/0/2 in 5 runs | thin |
| per-intent loop (slice 1 v1) | 7 | 0 in 11 intents, 2 runs | WORSE, and binds the builder to the planner's kind |
| source-first (this) | ~2 | to measure | — |

Falsifier: if source enumeration on the undersea scene does not name mostly
THINGS (surface light, glowing creatures) — or names things and the build
still authors only zero-area kinds — then vocabulary was not the lever
either, and the language-level one-chunk physical light (§7.2's exit) is
next with no further prompt work.

## 9. SOURCE-FIRST RESULT — the falsifier fired; prompt work on this is OVER (2026-08-12)

`evals/runs/imagine_s11_sourcefirst`, gemini, N=1 — but this falsifier was
designed so one run answers it, because the enumeration half SUCCEEDED:

Sources enumerated (verbatim): sunlight through the ocean surface;
bioluminescent coral and anemones; glowing jellyfish; glowing fish.  All
THINGS, zero instruments.  The vocabulary hypothesis got its fair test.

The build then authored **7 zero-area lights and 0 area lights** — an omni
for the glowing jellyfish, omnis for the glowing fish — with the palette in
full view.  Completions: 2, so the cost regression is gone.

Three mechanism families are now falsified on this question:
| hypothesis | mechanism | result |
|---|---|---|
| taste / salience | palette order + sole worked example (arc 81) | 1/0/0/0/2 |
| cost / room | one completion per light (slice 1 v1) | 0 of 11 |
| vocabulary / framing | source-first enumeration (owner design) | 0 of 7, with perfect source lines |

One convergent observation makes the conclusion sharper: in CONSTRUCTION,
the builders write emissive materials routinely (`*_emiss_painter` chunks on
anemones, orbs, jellyfish in arc-79/82 runs).  The model will make a thing
glow when it is building the THING.  Asked for LIGHTING, it writes chunks
from the lighting category — whatever they are.  The category of the task
summons the category of the chunk, and no prompt inverts that.

**Per the pre-commitment: the fix moves to the scene language.**  A
first-class one-chunk physical light — e.g. `rect_light { name, corners (or
center/size/facing), color, exitance }` — that the parser deterministically
expands into painter → luminaire material → clippedplane → object.  It IS a
lighting-category chunk, so the model's reach lands on it; it IS an area
light, so the owner's physics policy is satisfied by construction rather
than persuasion.  Design and build pending owner approval — it is a
scene-language change, which is the owner's domain, not an agent-surface
tweak.  **Approved and shipped the same day — see §10.**

---

## 10. SLICE 3 — `rect_light` SHIPPED (2026-08-12)

Owner approval, verbatim: *"Yes do rect_light, expand at parse time,
exitance only."*  Built as approved; nothing measured yet, and nothing here
claims a result.

### 10.1 What the chunk is

```
rect_light
{
	name		window_light
	center		0 4 0
	size		2 1
	facing		0 -1 0
	color		1.0 0.95 0.85
	exitance	6000
}
```

`name`, `center`, `size` (width height) and `facing` are required, as is
`exitance` (> 0).  `color` defaults to `1 1 1`.  **There is no `power`
parameter and no alias** — the descriptor IS the accepted set, so a `power`
line fails the load rather than being ignored.

### 10.2 Parse-time expansion, and nothing else

`RectLightAsciiChunkParser::Finalize` makes the same four `IJob` calls a
hand-authored chain makes — `AddUniformColorPainter`,
`AddLambertianLuminaireMaterial` (exitance as scale, inner material `none`),
`AddClippedPlaneGeometry`, `AddObject`.  **The renderer core learns no new
concept**: no new entity, no new `ILight`, nothing downstream to teach.  The
CST keeps the compact text, so save round-trips it verbatim and every derive
expands it identically — and because scene load and the agent insert path
both run through the one chunk-parser registry, both got the chunk with no
second edit (verified: `RectLightChunkTest` round-trip, `AgentChunkCrudTest`
A83e).

Derived names are `<name>__pnt` / `<name>__mat` / `<name>__geo`; the chunk's
own `name` names the OBJECT, so solo / object-map / isolate report it.  A
collision on any of the four fails the derive with the manager's ordinary
duplicate-name error.

One consequence worth carrying forward: `Cst::DeriveToJobIncremental` had to
refuse `rect_light` alongside `gltf_import`.  Its category is `Light` (right
for every other consumer), but there is no `ILight` for `DropChunkByCategory`
to remove, so an incremental re-derive would leave three orphans.  Editing one
falls back to a full derive.

### 10.3 The sidedness contract, and how it was proved

The panel emits toward `facing` only.  That rests on two facts:
`LambertianEmitter::emittedRadiance` returns black when `Dot(out, N) <= 0`,
and `ClippedPlaneGeometry` takes `N = normalize(Cross(ptb-pta, ptd-pta))`
from the corner winding.  So the corners are laid out
`(-U,-V) (+U,-V) (+U,+V) (-U,+V)` about `center`, giving
`Cross(width·U, height·V) = width·height·F`, and `doublesided` is FALSE
(left TRUE, a back-face hit flips the normal toward the ray and the panel
would emit both ways).  `U` comes from world up, falling back to world +Z
when `facing` is (anti)parallel to up; a zero-length `facing` fails the load.

This repo shipped a false sidedness claim eight commits earlier (97a96d34),
so the claim is proved twice rather than asserted: `RectLightChunkTest` asks
the derived geometry for its own normal across nine `facing` directions
including both up-degeneracies (exact to 1e-9), and renders the same panel
over the same floor facing down (mean 0.241) and facing up (mean exactly 0).
`scenes/Tests/Lights/rect_light_sidedness.RISEscene` is the eyeball version.

### 10.4 Surfaces

The `light_scene` palette's AREA entry now leads with `rect_light` and its
schema, carries the one-chunk example FIRST, and keeps the four-chunk chain
below it as the general form — because a `rect_light` is a *rectangle*, and a
sphere lamp or mesh fixture still needs the chain.  Both blocks are literal
and both are lifted from the shipped prompt and pushed through the real
insertion path (A81h).  The ambient-light refusal names `rect_light` first.
Both tool-description surfaces and `SCENE_CONVENTIONS.md` §3.5,
`effective-rise-scene-authoring`, `lighting-recipes` say the same thing, with
`SourceHygieneTest` pinning it.  `light_scene`'s report counts a landed
`rect_light` as an AREA light, not as "a light chunk of another kind" — that
number is what this arc measures.

### 10.5 What is NOT yet known

Whether the model reaches for it.  §9's three falsified families were all
prompt-side; this one changes what is *reachable in a single reach*, which is
a different mechanism — but it is a hypothesis until a run says otherwise.
The falsifier is the same as before: an imagine-and-build run whose lighting
pass produces a non-zero area-light count with the sources it enumerates.

## 11. FIRST LIVE rect_light (2026-08-12, N=1)

`evals/runs/imagine_s12_rectlight`.  Enumeration again perfect (5 sources,
all things).  The build wrote **one rect_light** — `coral_floor_glow_panel`,
center on the sea floor, `facing 0 1 0.2` (up into the scene), size 1.5×1.2,
exitance 80 — correctly formed on the first live use, plus 9 zero-area
lights for the other sources.  2 completions.

Read against the priors (area sources per run: 1/0/0/0/2 single-call, 0+0
per-intent, 0 source-first pre-rect_light): nonzero, correctly used, in the
lighting category as predicted — the reach hypothesis survives its first
test.  But 1 of 10 is not "most scenes use area lights"; the glowing
creatures still became omnis.  At N=1 the honest statement is: the chunk
WORKS end-to-end live and the distribution has not flipped.  The remaining
gap is plausibly that a jellyfish is not a rectangle — the natural emitter
for a glowing creature is the creature's own geometry wearing an emissive
material, which the builders already do in construction.  That suggests the
eventual bridge is between the enumeration's "things that glow" and the
EXISTING objects' materials (make-this-object-emit), not another light
chunk.  Recorded as the next hypothesis, unmeasured.

## 12. shape_light + the zero-area budget: THE DISTRIBUTION FLIPPED (2026-08-12, N=1)

`evals/runs/imagine_s13_shapelight`.  Five of six sources landed as PHYSICAL
lights, no retry, no confirm needed -- the model went physical voluntarily:
the water surface as a 6x6 rect_light facing down, the mermaid / jellyfish /
anemone / coral as glowing ellipsoids and a sphere.  One spot survived for
the caustic shaft (a legitimate special case, inside the free budget).

The full ledger on one line each:
  palette order + sole example ......... 1/0/0/0/2 of ~7
  one completion per light ............. 0 of 11
  source-first vocabulary .............. 0 of 7
  rect_light alone ..................... 1 of 10
  shape_light + budget ................. **5 of 6**

What flipped it was the combination the owner designed: a one-chunk PHYSICAL
form for every shape the enumeration actually names (glowing creatures are
solids, not rectangles) + a cost on the zero-area path (free budget of 2,
then refuse-once-confirm).  Availability alone (rect_light) moved it 1/10;
availability for the right SHAPES plus a price moved it 5/6.

Caveats, honest: N=1; the render composition has regressed (subjects
clustered small, heavy black surround -- lighting got physical while framing
got worse, and framing is a later slice); and whether the budget's friction
or shape coverage did more of the work is not separable in this run and does
not need to be -- both are shipped policy now, not competing hypotheses.
