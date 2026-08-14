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

## 13. The free budget of 2 REMOVED (2026-08-13)

Owner decision, verbatim-ish: the free budget of 2 zero-area lights per
scene is removed.  Every `omni_light` / `spot_light` / `directional_light`
creation request is refused once with the facts and lands when the
identical request is re-issued -- from the FIRST one.  The owner's policy
for these three kinds is "only extremely rarely and only for very special
circumstances"; confirm-once makes every use deliberate, and a free
allowance exempted the first two uses of a session from exactly that test.

The mechanism itself -- confirm-once, canonical content fingerprints (sorted
params, collapsed whitespace), a batch charged as one request, a patch
judged as a delta so editing an existing zero-area light stays free,
unconditional / no phase-cap interaction / survives protocol-off, ambient
still a ban with no confirm -- is UNCHANGED by this.  §12's "free budget of
2, then refuse-once-confirm" becomes "refuse-once-confirm, unconditionally";
the shape_light + confirmation combination §12 credits for flipping the
distribution does not depend on which of those two shapes the confirmation
took, since neither run this doc records reached the (now nonexistent)
budget ceiling in the first place -- shape_light's five landed lights and
the sixth's single spot-light confirm turn are unaffected by removing an
allowance a 6-source answer never spent.

What did change, mechanically: `kZeroAreaLightSceneBudget` and the
per-document zero-area count it read
(`CountZeroAreaLightChunks_`) are deleted -- there is nothing left to count.
The refusal text (`DescribeZeroAreaLightConfirmationRefusal`) loses the
"this scene carries N... adds M more, against a free budget of 2" clause and
keeps the physics facts and the escape clause verbatim.  The palette's
zero-area group note, the two tool-surface descriptions
(`AgentChatCodecs.cpp`, `AgentMcpAdapter.cpp`), `SCENE_CONVENTIONS.md`
§3.5, and the `scene-skeleton-and-conventions` / `lighting-recipes` agent
skills all say the same unconditional-confirmation fact now, with
`SourceHygieneTest` pinning the wording (the budget-number parity pin that
used to tie both tool surfaces to `kZeroAreaLightSceneBudget` is gone with
the constant).

## 14. §4 AS BUILT — the session mode (2026-08-13)

§4 is implemented.  What follows is what actually landed, including the
two places the design as written could not be implemented as specified.

### 14.1 The rule, as coded

An explicit one-way session mode, `AgentSession::AgentSessionMode`:

- **BUILDING** — the document the session was constructed over carried no
  form, and no final answer has arrived yet.
- **REFINING** — every other case, and forever after the transition.

The one-sentence version is in the code, at the top of the public block
above `SessionMode()`: **compulsion belongs to the first build;
availability is forever.**

### 14.2 Determination 1 — "started empty"

Decided **at session construction**, from `IJobPriv::GetCstDocument()` —
the document actually loaded — and snapshotted in a `const bool
mStartedEmpty`.  Never recomputed: a count taken later would see a build
in progress and answer the wrong question.  Both factories load before
they construct (`LoadFromFile` parses the file first; `WrapJob` is handed
a Job the host already loaded), so the classifier sees the real starting
scene.

**Empty = no form-bearing chunk**, where form-bearing is the *existing*
`KindIsFormBearing_` predicate the compose-phase delete-ban already used:
registry category `Geometry` or `Object` (a `standard_object`).  One
classifier, so "what counts as form" cannot drift into two answers.
Cameras, film, rasterizers, shaders, painters, materials and **lights** in
a starter template do not make a scene non-empty.  A Job with no retained
CST head reads *started empty* — the fail-safe direction (gates on), the
same polarity both launch switches already take.

Exposed publicly as `SceneTextCarriesForm(text)` so a caller or a test can
classify a scene file without standing up a session.  Verified against the
real files: `scenes/Templates/empty_starter.RISEscene` classifies **empty**
(so every eval build scenario and the GUI start-screen path still get the
protocol), and `scenes/Benchmarks/dreamscape_coral_queens_hour.RISEscene`
classifies **non-empty**.  Checked the eval battery too: all four bare/build
scenarios load an empty document (the 265-byte and 369-byte inlines are
shader + rasterizer + film + camera, plus one painter); the refinement-shaped
scenarios (`remove_object`, `param_edit`, `multi_turn_edit`, …) load
documents with objects and are therefore REFINING — which is exactly §4's
conflict 2 being closed rather than a regression.

### 14.3 Determination 2 — the transition seam

`AgentChatLoop` deliberately holds **no session pointer** (it speaks the
protocol and performs no I/O), so there is no single library-level seam.
The true structural signal is `ChatStepResult::Kind::FinalText` — a model
reply carrying no tool call — and it is reached **per host**.  The
transition is therefore a public one-way, idempotent
`AgentSession::NoteFinalAnswer()`, called from each host's FinalText arm.
No chat-text sniffing, no wall-clock, no call counts.

| surface | flips? | where |
|---|---|---|
| eval runner | yes | `AgentEvalRunner::RunScenario`, the `Kind::FinalText` arm |
| macOS GUI | yes | `ChatViewModel.swift` `.finalText` → `RISEViewportBridge -agentNoteFinalAnswer` → all three in-app sessions |
| Windows GUI | yes | `ChatPanel::networkFinished`'s FinalText arm → `ViewportBridge::agentNoteFinalAnswer` → same three |
| MCP stdio (`AgentMcpAdapter`) | **no** | the external client owns the model loop |
| MCP over loopback HTTP (`AgentLoopbackHttpServer`) | **no** | same |

**The MCP surfaces cannot flip, and that is stated rather than papered
over.**  Nothing about a turn ending crosses the MCP wire and MCP has no
notification for it, so an MCP session that *started empty* stays in
BUILDING for its whole life.  The alternative was a heuristic, and a wrong
guess in the refusal path is worse than a predictable one-way rule.  An MCP
session that opens a non-empty scene — the overwhelmingly common case — is
REFINING from construction and never needed the transition.

### 14.4 What went inert, and what did not

| mechanism | in REFINING | why |
|---|---|---|
| build-plan gate (`BuildPlanGateArmed_`) | **inert** | construction compulsion |
| cross-element edit refusal (`CheckElementWindowForEdit_`) | **inert** | " |
| compose-phase geometry-creation refusal (`CheckComposePhaseForCreate_`) | **inert** | " |
| compose-phase delete-ban (`CheckComposePhaseForRemove_`) | **inert** | " |
| first-geometry clean room (`CheckFirstGeometryThroughCleanRoom_`) | **inert** | " |
| first-light clean room (`CheckFirstLightThroughCleanRoom_`) | **inert** | " |
| populate-before-compose-render (`CheckPopulateBeforeComposeRender_`) | **inert** | " |
| `ambient_light` ban | **ACTIVE** | renderer policy — a wall |
| zero-area light confirmation | **ACTIVE** | renderer policy — a toll the owner priced |
| chunk attribution / phase transitions / finish / reopen | **ACTIVE** | recording is harmless and the census wants it |
| every verb (`build_element`, `light_scene`, `populate_scene`, `place_element`, `file_build_plan`) | **AVAILABLE** | their own do-nothing prologues still apply |
| render caps, rasterizer allowlist, autonomy postures | **ACTIVE** | never build protocol |

Mechanically: one predicate, `ConstructionCompulsionActive_()`, added to
`BuildPlanGateArmed_` and to `RefuseForPhase_` *plus* each of its six arms.
It is deliberately **not** folded into `BuildProtocolActive_()`, because
that predicate also governs attribution and the phase transitions, which
must keep running.  Putting it in `RefuseForPhase_` as well as in each arm
means a seventh arm added later inherits it by construction.

The mode travels on the wire beside `phase`, in the three results that
already carry session state (`file_build_plan`, `finish_element`,
`reopen_element`): `"mode": "building" | "refining"`.  No new surface was
invented for it.

### 14.5 Where the design could not be implemented as written

**One place, and it is a test-construction problem, not a semantics
compromise.**  Every existing construction-gate test wraps a fixture that
carries pre-existing geometry *on purpose* — a cross-element refusal and
the "pre-existing content is never attributed" RED-PROVEs need chunks the
session did not author.  Under the new rule those sessions classify
REFINING, and ~2 900 assertions would have gone green for the wrong reason.
Migrating them to an empty fixture would have deleted the very thing they
test.

So `WrapJobWithSessionMode(job, mode, …)` exists: `WrapJob` with the mode
**pinned** instead of classified.  Every product surface still classifies;
the pin's only caller is `AgentChunkCrudTest`'s gate helper (and one
hand-rolled External session in the G2o staged-resolve arm).  It exempts a
session from nothing — a pinned-Building session still transitions on
`NoteFinalAnswer`, a pinned-Refining one still never gates — and
`StartedEmpty()` keeps reporting the document's real answer, so a pinned
session cannot lie about what it opened.  The A83M block below then tests
the *classification* through the ordinary `WrapJob` path, so the pin never
hides the rule it works around.

One shipped-behaviour test genuinely changed rather than being
accommodated: `AgentAutonomyPolicyTest`'s G2/wire arm launches a real
headless `rise --agent-stdio` child and asserts the gate fires by default.
It was launching over a scene with a sphere in it; that scene is now
REFINING and correctly does not gate, so the arm launches over a new
form-free `kEmptyScene` fixture.  The claim it makes ("a shipped process
gates by default") is unchanged and still measured end-to-end.

**Not implemented, and named:** §4.3's residual is unchanged — a user
interjecting mid-build is still in BUILDING and can still hit a gate for up
to three turns.  It is pinned as an *accepted* assertion (A83M/c), so if it
ever flips, that is a design change and not a fix.

### 14.6 Tests

All in `tests/AgentChunkCrudTest.cpp`, block A83M.  Each conflict arm has a
pinned-Building **control** beside it, so neither half can pass vacuously.

- `TestSessionModeClassification` (A83M/a) — the shipped starter template
  classifies empty; a shipped benchmark classifies non-empty; construction
  snapshots both ways; a light + painter + material still reads empty while
  one geometry chunk or one `standard_object` flips it; the pin reports the
  pinned mode and the document's real `StartedEmpty`.
- `TestRefiningDisarmsTheThreeConflicts` (A83M/b) — the three §4 conflicts:
  removing a form-bearing chunk (single and batch) proceeds; the first
  compose-phase render proceeds with no `populate_scene` demand (on a
  build-capable session, so the arm's own precondition is met); hand-authored
  geometry lands with no build-plan demand and, after a plan is filed, with
  no `build_element` demand — while attribution keeps running.  Three
  controls prove each is still refused while BUILDING.
- `TestSessionModeTransitionIsOneWay` (A83M/c) — an empty-start session is
  BUILDING and really is refused; the mid-build interjection is still
  refused (the accepted residual); `NoteFinalAnswer` flips it; the
  previously-refused insert lands; a second build plan filed in REFINING
  does not resurrect the gates; a second `NoteFinalAnswer` is a no-op.
- `TestRefiningKeepsRendererPolicy` (A83M/d) — the ambient ban still
  refuses with its full physics text in REFINING; a zero-area light still
  costs its one confirmation bounce and lands on the identical re-issue;
  with the launch switches off nothing gates in **either** mode.

### 14.7 Verification

`make -C build/make/rise -j8 all && make -C build/make/rise -j8 tests`,
clean rebuild, warning-free.  Verbatim results: AgentChunkCrudTest 2982/0
(was 2893/0 — 89 new assertions), AgentAutonomyPolicyTest 388/0, and the
other 20 Agent\* binaries plus SourceHygieneTest 146/0, RectLightChunkTest
56/0, ShapeLightChunkTest 71/0, CstSaveFidelityTest 29/0,
CstIncrementalDeriveTest 24/0 — all zero failures.  No eval run: this slice
was explicitly scoped without provider calls, so the live claim "a refining
user's instruction is no longer refused" is proved by unit test and by the
scenario-scene audit in §14.2, not by a measured run.

## 15. §3.2 AS BUILT — the ENVIRONMENT and CAMERA steps (2026-08-13)

§3.2's two remaining steps are implemented.  What follows is what actually
landed, including the two places the design as sketched could not be built
as specified and the mechanism that replaced each.

### 15.1 The two verbs, as coded

Both are SINGLE-UNIT steps and both are exactly **one completion** on the
happy path (plus at most one repair retry) — §1's sizing rule: there is one
environment and one camera, so a loop would be the wrong unit.

| | `environment_scene` (slice 5) | `frame_scene` (slice 6) |
|---|---|---|
| completions | 1 (+1 repair) | 1 (+1 repair) |
| per-session cap | 2 | 2 |
| what it writes | painter / function / medium chunks | ONE camera chunk |
| what the harness adds | the DOME BINDING onto the rasterizer | PATCH-or-REPLACE of the camera chunk |
| measured headline | frame TONE before and after | object COVERAGE before and after |
| its gate | half of the compose-render checklist | the first COMPOSE camera edit |

Both take only `notes` (capped at 2000, truncation stated), both are
BuildCapable-gated with a capability statement, both are MUTATE on the wire
(not read-safe, not on the Propose allowlist), both are mirrored on the
canonical chat codec and the MCP adapter, and both are REFINING-inert as
*compulsion* while staying AVAILABLE as *verbs* — §14's rule unchanged.

### 15.2 The environment palette, verified against the registry

Surveyed from `ChunkParserRegistry.cpp`'s descriptors, not from prose.  The
headline finding: **there is no `environment` chunk in this language.**  The
surround is two things and neither is a dedicated keyword.

**Shipped, with the registry's own schema printed via `ReadSchema`:**

- **THE DOME — a PAINTER bound as the rasterizer's `radiance_map`.**  Worked
  example: `uniformcolor_painter` ×2 + `expression_function2d` (the ramp over
  the direction-derived `u,v`) + `blend_painter`.  Schemas printed:
  `expression_function2d`, `blend_painter`, `hdr_painter`, `exr_painter`,
  `uniformcolor_painter`.  Noise domes (`perlin2d_painter`,
  `perlinworley3d_painter`, `turbulence3d_painter`, `curlnoise3d_painter`)
  are named in the headline as the same two-colour shape.
- **THE MEDIUM — `homogeneous_medium` + `global_medium`.**  Worked example
  ships both.  `painter_heterogeneous_medium`'s schema is printed beside them
  for structured cloud / god-ray density.  The headline states the one thing
  a parameter list cannot carry: `phase` is a composite token
  (`isotropic` or `hg <g>` on ONE line), and absorption/scattering are
  per-unit-distance so they scale with the scene.

**Verified and deliberately NOT shipped, each for a stated reason:**

| family | why not |
|---|---|
| `hdr_painter` / `exr_painter` **worked example** | schemas yes, example no: an example must name a FILE, and one naming a file the scene does not have would fail to insert and spend the model's one repair retry on this harness's own placeholder (A82g's finding in the negative) |
| `ambient_light` | already banned outright on every creating path; and it fills *illumination*, not the *frame* — a missed primary ray is still black |
| per-object `radiance_map` on `standard_object` / `csg_object` | object-local, not the surround — and it rides a FORM chunk |
| `file_rasterizeroutput`'s `exposure` / `display_transform` | output-side tone mapping, not environment |
| `gerstnerwave_painter`, `iridescent_painter` | a water *surface* and a view-dependent term; neither is a dome |
| a backdrop plane / sky sphere / water mesh | **FORM.** Refused by name — see 15.4 |

Two registry traps the admissibility had to know about, both confirmed in
code: `expression_function2d` is `ChunkCategory::**Function**` (it
dual-registers as a painter *reference*), so the accept set is
Painter ∪ Function ∪ Medium; and the two MLT rasterizers do **not** declare
`radiance_map` at all, which `RasterizerAcceptsRadianceMap_` asks the
descriptor rather than a hand-kept list.

### 15.3 The hosek decision — one verb owns the sky

`hosek_wilkie_skylight` is in `light_scene`'s palette and is **refused by
name** in `environment_scene`.  The reason is mechanical, not tidiness:
`Job::AddHosekWilkieSkylight` and `Job::SetPixelBasedRasterizer` both call
`Job::SetGlobalRadianceMap`, **last writer wins**, so a scene carrying both a
hosek chunk and a rasterizer `radiance_map` silently discards one.  Two verbs
installing a dome by different routes is exactly how that happens.

So the sharing rule is that ONE of them owns it, and the other is told:
`environment_scene` reads the DOCUMENT (not the live scene — what collides is
the chunk's call on the *next* derive) for a hosek chunk, and when one is
present it **suppresses the whole DOME palette entry** and makes no binding at
all, writing only the medium.  The prompt says so as a fact.  Offering a model
a form the call will then refuse is the false-clause class §8.1 of arc 79
records the cost of.

**Known residual, named:** if `light_scene` runs *after* `environment_scene`
and authors a hosek, it will replace the painter dome.  Nothing here detects
that ordering; it is stated rather than fixed.

### 15.4 Where the design could not be built as sketched — TWO places

**(a) THE DOME BINDING IS A PATCH, NOT A SECOND RASTERIZER CHUNK.**  The
plan assumed "bind a painter as `radiance_map`" was an insert.  It is not:
`radiance_map` is a *parameter group on the rasterizer chunk*, so something
has to write it.  The first implementation appended a fresh rasterizer chunk
carrying the binding — and `Job::ApplyCstInsertChunk` **refuses a second
unnamed chunk of the same keyword outright** (the duplicate would mask the
original last-wins and bare-name `remove_chunk` could never delete it).
Unnamed rasterizer chunks are also unremovable.  So a rasterizer chunk is
editable **in place and in no other way**, and the binding is a two-parameter
patch (`radiance_map`, `radiance_background TRUE`) against the kind-addressed
singleton.

The forward-reference hazard this raised is real and turned out to be already
handled: references resolve in DOCUMENT ORDER and an unresolvable
`radiance_map` is a **log warning, not a failure** — so a wrong-order patch
would commit, derive, and install no dome, silently.  It does not happen
because `ApplyCstInsertChunk` **positions** a Painter/Function chunk ahead of
the first material/geometry/shader (its tier-0 rule) rather than appending it.
A83N/a asserts that ordering directly, so a change to that classification
fails a test instead of quietly un-binding every dome.  And the call
**verifies** rather than assumes: it reads `IScene::GetGlobalRadianceMap` back
and, in the one case where the patch commits and no dome exists, says exactly
that and names the fix.

**(b) WHICH PAINTER IS THE DOME IS POSITIONAL, AND STATED.**  A graded dome is
a chain (two colours → a ramp → a blend), so "bind the painter" is ambiguous.
The contract is: **the LAST chunk the answer lands whose descriptor category
is `Painter` is the dome** — strictly Painter, not Painter-or-Function, because
`radiance_map` is declared `Reference -> {Painter}` and the
`expression_function2d` ramp exists to be the blend's MASK.  Binding the ramp
would make a greyscale gradient the sky.  The prompt states the rule; the
result reports the painter actually bound; A83N/a pins that the blend and not
the ramp is what got picked.

### 15.5 `frame_scene` — patch or replace, and the ordering that is load-bearing

The five camera keywords are a **named list** (`IsCameraKeyword_`), not a
`ChunkCategory::Camera` test, because the registry files two non-cameras under
that category — `scene_options` (world scale) and `camera_defaults` (thinlens
fallbacks).  Cst.cpp's own classifier says so.  Refusing a scene-unit
declaration on behalf of a framing verb would be the over-refusal arc 78 names
as this family's worst failure mode.  Same named-list precedent as
`IsZeroAreaLightKeyword_`.

- **PATCH** when the answer's kind matches the scene's camera: one
  `ProposePatches` batch — one head bump, one undo step — of every parameter
  the answer names except `name` (there is no rename verb; patching a chunk's
  own name would break every reference to it).  A parameter the answer omits
  keeps its value, and the prompt says so.
- **REPLACE** when the kind differs: **insert first, remove second**, so a
  failed insert can never leave the scene with no camera.  Removing the old
  chunk is not tidiness — `Job::RederiveCstDocumentFull_` restores the
  previously active camera BY NAME across the re-derive, so until the old chunk
  is gone the inserted camera is in the document but is not the one rendering.
  An unnamed old camera is removed by its LIVE derived name under the generic
  `camera` kind, the one address the resolver's unnamed-camera fallback
  accepts.  The camera that ended up live is **read back**, never assumed.
- **`film` is refused by name.**  Width/height/pixelAR moved to `film` in
  scene format v6; it is raster-size policy, and a framing pass that quietly
  re-sized every render would be changing the budget, not the shot.

The measurement is `ComputeSceneInventory_` run BEFORE the edit and again
AFTER, with the per-object on-screen flags diffed by name.  A reframe that
covers FEWER objects is stated plainly, with the number and the names, and is
**never auto-reverted** — a close-up covers fewer objects on purpose, and a
harness that undid it would be overriding the judgement it just paid a
completion to obtain.

One further change the FULL inventory forced: `FormatSceneInventory_`'s two
line caps (12 on-screen / 40 zero) became **parameters** defaulted to those
same constants, and `frame_scene` passes 120/120.  For every other consumer
the inventory is context beside a picture; for this one the per-object list IS
the input, and an object cut from the listing is an object the reframe cannot
know it is missing.  One formatter, two caps — a second formatter would have
been the drift surface.

### 15.6 The two gates

**The compose-render arm became a CHECKLIST**, not a second arm.
`CheckPopulateBeforeComposeRender_` → `CheckBuildChecklistBeforeComposeRender_`.
The first agent-surface COMPOSE render is refused **ONCE for the whole
checklist** — not once per item — naming exactly the ones that have not yet
reached the provider.  Each item lifts independently and permanently; a model
that has run one is never told to run it again.  The one-shot is load-bearing
and does **not** loosen as items are added: §6's finding is that a
repeat-refusable render arm burns the whole shared 3-refusal budget by itself
and trips the give-up, silently disarming every sibling gate.

**The camera arm (`CheckFrameSceneBeforeCameraEdit_`) is ALSO one-shot**, which
is the render arm's rule rather than the light arm's, and the reason is §6's
finding applied *in advance*: every measured run patches the camera
repeatedly.  A repeat-refusable arm on a call a model makes constantly is
precisely the starvation hazard.  It fires on an INSERT of a camera chunk and
on a PATCH aimed at one — the patch arm being the one that matters, since
re-aiming is a patch in every run measured, and an insert-only gate would be a
gate with a one-word bypass.  COMPOSE-only (arc 78 §2.3 exempts the whole
Camera category from element-window rules so a model can re-aim at the part it
is building); inert in REFINING; dead with `--agent-build-protocol=off`;
exempt for `frame_scene`'s own edit.

Six arms now share `RefuseForPhase_`'s 3-refusal counter, four of them in
COMPOSE.  A82e was **extended rather than duplicated** — the property under
test is a property of the shared counter, and a test that counted three of
four would pass while the fourth starved them.

### 15.7 One wire-shape dedup this slice paid for

`AgentPatchResult`'s per-element JSON shape existed in **two inline copies**
(`propose_patches` and `place_element`, the second carrying a comment noting
there was "no shared helper to call").  This slice needed a third.  Three
copies of a wire shape is the drift surface `IsReadSafeVerb`'s own doc argues
against, so `PatchResultJson` is now the one definition and both existing
sites call it; the emitted keys are unchanged byte for byte.

### 15.8 Tests

All in `tests/AgentChunkCrudTest.cpp` unless noted.

- **A83N/a** `TestEnvironmentSceneHappyPath` — one completion; six chunks land;
  the LAST **painter** (not the Function ramp) is what gets bound; the document
  really carries the binding AND the painter really precedes the rasterizer;
  the live scene's dome and global medium flip false→true; the frame tone is
  measured on both sides; the prompt carries the inventory, the camera, the
  bounds, what environment already exists, the binding contract, both literal
  examples, and the two image-painter schemas *without* examples; the
  advice-vocabulary ban on the prompt and the verdict ban on the result.
- **A83N/b** `TestEnvironmentSceneAdmissibility` — a backdrop plane and its
  object refused with "is FORM"; `hosek_wilkie_skylight` refused and pointed at
  `light_scene`; a camera and a `film` both refused; none reaches the document.
- **A83N/c** `TestEnvironmentSceneHosekSuppression` — with a hosek chunk present
  nothing is bound, the reason says why, the medium half still runs, and the
  DOME palette entry is absent from the prompt entirely.
- **A83N/d** `TestEnvironmentSceneCapabilityAndCap` — capability statement +
  byte-identical document; the cap (capability refusals never count); available
  and working in REFINING and with the protocol off.
- **A83N/e** `TestEnvironmentExamplesParse` — both worked examples LIFTED FROM
  THE SHIPPED PROMPT and pushed through the real insertion path: all six chunks
  land, no repair retry, and the example's own `blend_painter` is what the
  binding contract picks.  Includes a RED-PROVE that the lift is reading the
  prompt and not a test literal.
- **A83N/f** `TestEnvironmentSceneWireShape` — the JSON-RPC shape including
  `boundPainter` / `bindingApplied` / `rasterizer` / both tonal readings /
  `patchResults`; the one `-32602`.
- **A83N/checklist** (inside A82d) — the single refusal names BOTH verbs;
  after `populate_scene` alone it names ONLY `environment_scene`; after both,
  the render proceeds; a failed pass on either still lifts its item.
- **A83P/a** `TestFrameSceneHappyPath` — one completion; `action == "patched"`;
  four parameters applied; still exactly ONE camera chunk; coverage measured on
  both sides; the prompt carries the FULL inventory, the camera chunk VERBATIM,
  its resolved pose, the frame size declared out of scope, the tonal fact, all
  five camera kinds and one literal example (and no example on the other four);
  advice and verdict bans.
- **A83P/b** `TestFrameSceneFewerObjectsIsReportedNotReverted` — the close-up
  lands, nothing is reverted, and the payload says "covers FEWER objects" with
  the names.
- **A83P/c** `TestFrameSceneAdmissibilityAndReplace` — a film, a geometry and a
  light refused (the film refusal naming "raster-size policy"); a SECOND camera
  refused with the first still used; the REPLACE path leaving the old pinhole
  chunk gone and the ortho read back as live.
- **A83P/d** `TestComposePhaseFirstCameraRefusal` — the gate fires on a PATCH
  and on an INSERT, exactly once each session; `frame_scene` lifts it; a FAILED
  pass lifts it too; PIECES-phase camera edits never refused; protocol-off dead;
  REFINING inert with the verb still available.
- **A83P/e** `TestFrameSceneCapabilityAndCap`, **A83P/f**
  `TestFramingExampleParses` (the palette's example lifted from the shipped
  prompt and applied through the real path), **A83P/g**
  `TestFrameSceneWireShape`.
- **A82e** `TestComposeArmsShareOneCap` (renamed from
  `TestThreeComposeArmsShareOneCap`) — the fourth refusable call is now the
  camera arm and it triggers the same global give-up; a new arm proves ten
  camera patches produce exactly ONE refusal, leaving two slots for the others.
- **`tests/SourceHygieneTest.cpp`** — seven pins per verb on BOTH tool
  surfaces: for the environment, that there is no environment chunk, the
  positional binding contract, the FORM boundary case, the hosek division, what
  the medium half is for, and the tonal measurement; for framing, one camera
  only, `film` refused and why, what an omitted parameter does, the replace
  ordering, the coverage measurement, and the fewer-objects rule.

### 15.9 Verification

`make -C build/make/rise clean && make -C build/make/rise -j8 all &&
make -C build/make/rise -j8 tests` — clean rebuild, **warning-free**.

Verbatim: AgentChunkCrudTest **3372/0** (was 3156/0 before this slice's tests
and 2982/0 at §14), SourceHygieneTest **146/0**, AgentAutonomyPolicyTest
**388/0**, AgentMcpAdapterTest **283/0**, AgentChatLoopTest **1738/0**,
AgentSkillsTest **450/0**, AgentMcpStdioSmokeTest **21/0**, AgentEvalCheckTest
**1909/0**, AgentEvalLiveTransportTest **452/0**, AgentEvalReplayTest **263/0**,
AgentFirstSliceTest **350/0**, AgentFrameStoreIsolationTest **562/0**,
AgentHeadVersionTest **56/0**, AgentLiveCommitTest **871/0**,
AgentLoopbackHttpTest **166/0**, AgentObjectMapTest **252/0**,
AgentProposeRenderTest **485/0**, AgentReadValidateTest **185/0**,
AgentRenderAsyncTest **849/0**, AgentStdioSmokeTest **13/0**,
AgentTrajectoryTest **180/0**, AgentViewModeRenderTest **657/0**,
AgentViewportReadTest **340/0**, RectLightChunkTest **56/0**,
ShapeLightChunkTest **71/0**, CstSaveFidelityTest **29/0**,
CstIncrementalDeriveTest **24/0**.  All zero failures.

Tool-surface counts moved with the two verbs: MCP `tools/list` 32 → **34**,
chat tool definitions 27 → **29**, read-refusal-annotated tools 13 → **15**.

**No eval run.**  This slice was scoped without provider calls, so nothing here
claims a measured result about what a model does with either verb.  What is
established is that both verbs work end-to-end against the real insertion,
patch and remove paths; that both worked-example families really parse and
apply; and that both gates fire, lift and stay bounded.  The falsifiers are the
same shape as every prior slice's: an imagine-and-build run whose environment
pass produces a non-zero tonal spread against a scene that previously rendered
on a flat field, and whose framing pass raises the object-coverage figure it
now reports on both sides.
