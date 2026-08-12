# Population — Repeats Are Most of the Richness (Arc 82 Design, 2026-08-12)

> **Status:** BUILDING.  User direction 2026-08-12, after arc 81's first live
> result: *"Let's do the population arc."*  Depends on arc 80's inventory and
> arc 81's `light_scene`, whose pattern it copies almost exactly.

---

## 1. What the frontier scene is actually made of

The held benchmark (`scenes/Benchmarks/dreamscape_coral_queens_hour.RISEscene`
— same prompt, hand-authored by a full-context model) has **47 objects built
from 18 distinct geometries.  38 of the 47 are REPEATS.**

| geometry | objects |
|---|---|
| `fish` | **14** |
| `mote` | 5 |
| `rock_crag`, `staghorn`, `kelp_blade`, `jelly_bell`, `jelly_tentacles` | 3 each |
| `brain_coral` | 2 |

And it uses **no generator or instancing chunks at all**.  Every repeat is an
ordinary `standard_object` naming the same geometry and material with a
different transform — four lines of scene text.

The best agent run has **16 objects with almost no reuse**.  So the gap is
not modelling: the agent builds good elements, then builds one of each and
stops.  **Population is placement, and it is cheap.**  A school of fourteen
fish costs one geometry and thirteen more four-line chunks.

## 2. Mechanism — `populate_scene`

One fresh minimal completion, modelled directly on `light_scene`: same
host-mediated transport, same validated insertion, same single repair retry,
same honest partial reporting.  Fed by arc 80's inventory, so repeats are
placed in relation to real geometry rather than into empty space, plus the
camera and world bounds so it fills the FRAME.

**Hard contract: it may create ONLY `standard_object` chunks**, each naming a
geometry and material that already exist.  No new geometry, materials,
painters or lights.  A chunk naming an unknown geometry is rejected with that
reason.  This keeps the pass from becoming a second builder — new form
belongs to `build_element`.

It is given ONE worked example of a repeat, and no number.  Object count is
what this arc measures, so a count in the prompt would manufacture the
result.  The example is the steering, per the measured lever; the prose is
only the contract.

## 3. Forced first use

Voluntary verbs measure ~0 in this project.  So the **first COMPOSE-phase
render is gated** when `populate_scene` has never run: population belongs
before you judge the picture, and the first compose render is the moment the
model turns to judging it.  PIECES renders are untouched — a model must
always be able to look at the part it is building.

Shares `RefuseForPhase_`'s 3-refusal cap and give-up; dies with
`--agent-build-protocol=off`; lifts as soon as the pass reaches the provider
whatever came back, so a failed populate cannot strand a session.

**Three arms now share one counter** (arc-80 delete-ban, arc-81 first-light,
arc-82 first-compose-render).  Whether a session can be starved by hitting
all three is checked explicitly, not assumed.

## 4. A population fact on the inventory (free)

The inventory already walks every object, so it now also states how many
distinct geometries those objects draw on and how many are used by more than
one.  Facts only — no target, no comparison to any reference.  It costs
nothing.

## 5. Measurement (pre-committed)

1. **HEADLINE: object count and reuse ratio.**  Agent 16 objects with almost
   no reuse; benchmark 47 with 38 repeats (81%).
2. Does the gate fire, and does the pass run once or get worked around?
3. Rejection rate on the repeats (the contract's own health) — how often does
   it name a geometry that does not exist?
4. Do the repeats land somewhere sensible, or interpenetrate and float?  This
   is the honest visual judgement and the one the count cannot make.
5. Turn cost.

Stop rules: object count unchanged → forcing a population pass is not enough
and repetition is not something the model will do on request; repeats land
in obviously wrong places → placement needs the same treatment construction
got, and a scatter that has to be hand-fixed is worse than none; rejection
rate high → the inventory is not giving the builder what it needs to name
geometries correctly.

Explicitly NOT done: no instancing chunk (the benchmark does not use one, so
neither does this), no automatic count, no judgement of how many is right.

---

## 6. AS BUILT (2026-08-12)

Both mechanisms shipped.  Nothing in §1–§5 changed; this section records the
decisions the design left to the build, and the answer to the question §3
said would be checked rather than assumed.

The §1 counts were re-verified against the file before anything was written:
47 `standard_object`, 18 distinct `geometry` references, 38 objects on a
geometry that more than one object uses, and a keyword census confirming no
generator or instancing chunk of any kind.

### 6.1 Where the pieces live

`AgentSession::PopulateScene` (`AgentSession.cpp`, the arc-82 block after
`MeasureLightContributions_`), with `CollectPopulationStock_`,
`ComposePopulationExample_` and `ComposePopulationPrompt_` beside it.  The
gate is `CheckPopulateBeforeComposeRender_`, next to
`CheckFirstLightThroughCleanRoom_`, and it is consulted from
`AgentSession::Render(params)` **and** `AgentSession::RenderAsync` — the
async path too, because `render{"async":true}` is the same request on the
same wire handler with the same params, and a gate on only one of the two
would be a gate with a one-word bypass.  `Render(params)` gained a
`RenderInner_` split so the gate has one place to stand and no existing
return path below it changed.

**Which renders the gate can see** is decided by two conditions, and the
choice of those two is the load-bearing part:

- `AgentRenderParams::fromAgentSurface`, which only AgentRpc's `render`
  handler and `FinishElement`'s own isolate render set.  Every INTERNAL
  render in the file — the inventory's identity pass, `light_scene`'s solos,
  `query_object_at`, the reference comparison — leaves it false and is
  therefore structurally invisible to the gate.  A diagnostic that could be
  refused would be a diagnostic that stops working exactly when it is needed.
- `isolate` empty, which excludes looking at ONE part.  That is not judging
  the picture, it is what `FinishElement`'s render is, and it is the same
  exclusion `ApplyVisibilityCensus_` already makes.

### 6.2 The raw-material listing, and where it is read from

The prompt's vocabulary section is read from the **retained CST's
`standard_object` chunks**, not from the object manager, because what a
repeat has to write is a chunk NAME and the manager holds resolved pointers.
Each entry names the geometry, the materials it is worn with, and the objects
currently using it, most-used first; both the geometry list and the per-entry
user list are bounded, and any truncation is stated rather than shown as a
quietly short list.

Admissibility is checked against the LIVE managers (`GetGeometries()` /
`GetMaterials()`), which is what the derive would resolve against anyway, so
a name that passes cannot dangle later.

### 6.3 The worked example is built from the live scene

`light_scene`'s palette can afford invented names because it CREATES what it
references.  A repeat cannot: an illustrative `geometry fish` in a scene with
no `fish` would be copied and then rejected, spending the one repair retry on
the harness's own placeholder.  So the example names the scene's most-used
geometry and a material it is actually worn with, placed at that object's own
position plus a step sized from the world bounds, with a different
orientation and scale — and its object name is bumped until it collides with
nothing.

Test A82g lifts that example OUT OF THE SHIPPED PROMPT and pushes it through
the real validated insertion, reference rule included, so "the example
inserts" is proved rather than asserted (arc 81's A81h, applied here).

### 6.4 The three-arm cap interaction — the §3 question, answered

**No arrangement can starve a session**, for three independent reasons, all
pinned by A82e:

1. **The render arm fires at most ONCE per session**, which is stricter than
   its four siblings and deliberately so: a refused EDIT leaves a model able
   to look at its scene, while a refused RENDER leaves it blind, and a model
   that declines the redirection must still be able to see.  The consequence
   for the shared budget is that this arm consumes AT MOST ONE of the three
   refusals — ten consecutive renders produce exactly one.  **Without the
   one-shot this interaction is actively harmful**: a model that simply
   re-rendered would burn the whole cap and trip the give-up, silently
   disarming arc 80's delete ban and arc 81's light gate for the rest of the
   session.  That is what made the one-shot necessary rather than merely kind.
2. **The give-up is a GLOBAL release.**  The fourth refusable call of any
   kind proceeds and stops every arm intercepting, so three arms cannot
   compose into a deadlock.
3. **The failure direction is always "let through".**  An arm arriving after
   the budget is spent returns no clause, the call proceeds, and the give-up
   notice rides its own result — for the render arm, folded into the render
   payload, so a trajectory census sees the event rather than only a log line.

Also checked: the arm cannot refuse a render on a session that could not then
call `populate_scene`.  Both are gated on `BuildCapable()`, and
`populate_scene` is callable in every phase and with the protocol off.

### 6.5 No insert guard, and why that is a checked fact rather than an omission

`BuildElement` and `LightScene` each carry an `mIn...Insert` guard because
what they insert is exactly what some phase arm refuses.  `PopulateScene`
inserts `standard_object` and nothing else, and NO arm on the insert path
fires on an Object chunk: the build-plan gate,
`CheckComposePhaseForCreate_` and `CheckFirstGeometryThroughCleanRoom_` are
each entered only behind `ChunkTextCreatesGeometry_` (Geometry category
only), and the arc-81 arm only behind `ChunkTextCreatesLight_`.  Placing
objects is precisely what arc 80 says the compose phase is FOR.  The header
carries this enumeration so a future arm made to fire on Object creation
finds the note saying a guard became necessary.

### 6.6 One state the design did not name: nothing to repeat

A scene with no `standard_object` naming both a geometry and a material has
no raw material, and `populate_scene` returns ok:false saying so **without
calling the provider** — sending a prompt whose vocabulary list is empty
would spend real money to be told what the harness already knows.

### 6.7 The population fact on the inventory

One sentence appended to the inventory's own header, in its facts-only
register:

```
Those objects draw on 2 distinct geometries, 1 of which is used by more than
one object.
```

Distinctness is geometry POINTER identity (`IObject::GetGeometry`), so two
objects naming one geometry chunk are one geometry by construction and no
second lookup can disagree with the first.  An object whose geometry cannot
be read — a generator-synthesized instance name, an object kind that exposes
none — is counted as unread and stated in a second clause, never folded into
either figure.

### 6.8 Tests

`AgentChunkCrudTest` A82a–A82g: the happy path with the prompt composition,
the before/after counts and a prompt-hygiene scan proving no count or
exhortation reached the prompt; the hard contract (a geometry chunk, a
painter chunk, an unknown geometry, an unknown material) and the one retry
driven by the harness's own rejection text; the duplicate-name contract;
capability, spend cap and the nothing-to-repeat case; the COMPOSE render gate
with its one-shot, its lift by a successful pass, its lift by a FAILED pass,
the PIECES / isolate / internal-render seams and protocol-off; the three-arm
cap interaction in all three orders; and the wire shape including the gated
render as a model actually meets it.  `AgentProposeRenderTest` (b3): the
population fact on a four-object, two-geometry, one-repeated scene.
`SourceHygieneTest` gains an A82 pin block holding both model-facing surfaces
to the contract, the one-shot, the pieces seam and the before/after report.

---

## 7. RESULT — first live run (2026-08-12, N=1)

`evals/runs/imagine_s6_population`, undersea subject, gemini-3.6-flash.

| | objects | distinct geos | repeats | SDF parts |
|---|---|---|---|---|
| Fable benchmark | 47 | 18 | 38 (81%) | 131 |
| **this run** | **34** | 21 | **21 (61%)** | **104** |
| previous run (arc 81) | 16 | — | ~0 | 87 |
| arc 79 | 12 | — | 0 | 82 built / 2 kept |

Object count **doubled**; the reuse ratio went from near zero to 61% against
the frontier's 81%.  The gate fired once and `populate_scene` ran once — no
working around it.  Zero rejected repeats: every geometry and material it
named existed, so the inventory listing gave the builder what it needed.

The repeats it chose are the right ones — brain corals ×3, staghorn ×3,
anemones ×3, fish body and tail ×3 — distributed left and right along the
seabed rather than piled at the origin.  **Population is the largest
single-arc movement in this workstream since the clean room itself.**

### 7.1 The failure the count cannot see (§5 measurement 4)

Floating white bars hang in the water on the right and below the left
jellyfish: **orphaned repeats** — tentacle geometry placed without the bell
that belongs above it.  A repeat of a multi-part creature is only coherent if
its parts are repeated TOGETHER with a shared transform, and nothing in this
pass knows that `jellyfish_bell` and `jellyfish_tentacles` are one creature.
The inventory lists geometries, not assemblies.

This is the honest version of "do the repeats land sensibly": mostly yes, and
where they do not, the cause is structural rather than careless.  The fix is
not a better scatter — it is that the pass needs to repeat GROUPS.  Arc-78
attribution already knows which chunks belong to which element, which is
exactly the grouping this lacks; wiring that in is the obvious next slice.

### 7.2 An unforced regression worth watching

`hosek_wilkie_skylight` appeared for the first time ever — the second
never-used kind to show up since the palette was restructured.  But the AREA
LIGHT that appeared in arc 81's run did NOT: zero luminaire materials this
run, and nine lights of which eight are non-physical.

Two runs, two different outcomes, same build for the lighting surface.  At
N=1 each, that is variance, not a trend, and it is the clearest argument yet
for the repeats the supervisor recommended before this arc.  It also means
arc 81's "first area light" result must not be read as a settled behaviour
change.

## 8. CROSS-PROVIDER — gpt-5.6-terra, identical build (2026-08-12, N=1)

`evals/runs/imagine_s7_population_gpt`.  Same scenario, same commit, provider
swapped.

| | objects | repeats | parts | area lights | non-physical lights |
|---|---|---|---|---|---|
| benchmark | 47 | 38 (81%) | 131 | 0 | 8 |
| **gpt-5.6-terra** | **42** | 21 (50%) | 94 | **3** | 6 |
| gemini-3.6-flash | 34 | 21 (61%) | 104 | 0 | 9 |

**Every mechanism fired on both providers** — the gate, `populate_scene`,
`light_scene`, validated insertion, zero rejected repeats.  The stack is not
gemini-shaped.

**GPT complied with the lighting policy far better**: three area lights,
each a complete four-chunk chain, against gemini's zero.  It copied the
single worked example three times.  That is the copyability lever working
exactly as theorised — and it is now the strongest evidence for it, because
the same prompt produced 3 on one provider and 0 on the other while nothing
else in the palette changed.

**And it produced a worse picture.**  This is the arc's most useful result:

- The three emissive quads are **directly visible in frame as glowing white
  slabs** — left, right, and a large one behind the subject.  The worked
  example shows how to build an emitter; it says nothing about an emitter
  being a visible object in the shot.  A model that copies it faithfully gets
  a lit stage with the lights in the picture.
- The scene sits in a **black void** — no environment at all — so the fish
  repeats scatter into blackness rather than water.
- The seabed reads as a grey platform with cube blocks: a diorama on a stage,
  not a reef.

Luma stdev 103.2 against the benchmark's 29.3 — the highest number this
workstream has produced, and it measures the black-void-plus-white-panel
contrast, not quality.  **A metric moving hard in the "good" direction while
the picture gets worse.**  Precisely the reason every arc here pairs its
counts with an honest look.

Two things this changes:

1. The area-light example needs to say what the emitter IS in the frame — a
   window, a shaft, a glowing creature — or models will keep placing bare
   panels in shot.  That is a palette fix, not a new arc.
2. **Provider variance now dominates several single-run conclusions.**  Area
   lights: 1 (gemini/arc81), 0 (gemini/arc82), 3 (gpt/arc82) — on the same
   code.  No lighting-behaviour claim in arcs 81–82 should be treated as
   settled without repeats.

## 9. REPEATS — three gemini runs, identical build (2026-08-12)

`evals/runs/imagine_s8_repeats`, after the emitter-visibility fix.  The first
honest variance measurement in this workstream.

| run | turns | objects | repeats | reuse % | SDF parts | area lights | populate | light_scene |
|---|---|---|---|---|---|---|---|---|
| r1 | 68 | 23 | 15 | 65% | 82 | 0 | 1 | 1 |
| r2 | 40 | 26 | 16 | 62% | 67 | 0 | 1 | 1 |
| r3 | 48 | 27 | 18 | 67% | 66 | 2 | 1 | 1 |
| **mean** | 52 | **25.3** | 16.3 | **65%** | **71.7** | 0.7 | 1.0 | 1.0 |
| *single run s6* | 39 | *34* | *21* | *61%* | *104* | *0* | 1 | 1 |
| benchmark | — | 47 | 38 | 81% | 131 | 0 | — | — |

### 9.1 What is STABLE

- **The mechanisms fire every time.**  `populate_scene` and `light_scene` ran
  exactly once in all three runs; the gates are reliable, not lucky.
- **The reuse ratio is the solid result: 62 / 65 / 67%.**  Three runs inside
  five points, from a pre-population baseline of ~0.  Population is real.
- No emissive quad landed in frame as a bare panel in any run.

### 9.2 What is NOT stable, and the correction it forces

- **Object count 23–27 (mean 25.3).  The single run reported earlier was 34 —
  the top of the range, not the centre.**  "Object count doubled" was an
  overstatement of a real effect: 16 → ~25 is +56%, not +112%.
- ~~**SDF parts may be drifting down as prompts grow**~~ — **RETRACTED
  2026-08-12, see §10.**  Decomposing the total instead of repeating it showed
  the builder is stable and the metric was confounded.
- **Area lights 0 / 0 / 2 here, 3 on gpt, 1 in arc 81.**  Confirmed as
  variance-dominated.  No lighting-behaviour claim from arcs 81–82 survives.
- Picture quality varies far more than any count: r3 has 27 objects and reads
  as jellyfish-sticks over an empty teal field, while the earlier 34-object
  run was the best picture the workstream has made.

### 9.3 The lesson for this project's method

Four arcs of single-run conclusions were stacked on each other, and the first
variance check moved the headline by half.  **Counts here have a spread of
±20% or worse; effects smaller than that are unmeasurable at N=1 and should
not be reported as findings.**  The reuse ratio survived precisely because it
is a large, tight effect.  Run repeats before believing an arc, not after.

## 10. DECOMPOSITION BEATS REPETITION (2026-08-12)

The owner declined further repeats — *"I need more sound theories on what is
happening... I don't believe run to run the result can drop so badly"* — and
was right.  Decomposing the numbers already in hand explained both anomalies,
at no token cost.

### 10.1 The builder is stable; the METRIC was confounded

**Parts for the SAME element across runs** (the figure, apples to apples):

| run | s5 | s6 | s7 (gpt) | s8r1 | s8r2 | s8r3 |
|---|---|---|---|---|---|---|
| mermaid parts | 23 | 22 | 38 | 17 | 23 | 25 |

Gemini: 23, 22, 17, 23, 25 — mean 22, **no decline**.  The clean room is not
degrading, and the context-dilution-turning-on-us hypothesis is dead.

The scene TOTAL moved for two reasons that have nothing to do with builder
quality:

1. **Element-set composition.**  Runs planned 4 elements instead of 5, and
   terrain elements are cheap: `sea_floor` 6, `seabed` 10–11, one at 0.
2. **The metric cannot see primitive-built geometry.**  "SDF parts" counts
   `part` lines only, so an element built from a box or a plane scores ZERO
   while being perfectly good work.  s8r2's `sea_floor` at 0 parts is a flat
   seabed, not a failure.

**Total SDF parts is therefore a bad headline** — it multiplies a stable
per-element number by a variable element count and blinds itself to
primitives.  Parts for a NAMED element is the sound metric, and it is the one
this workstream started with (the wizard).

### 10.2 Area lights: cost, not taste

No area chain was ever rejected and no retry ever ran — the model simply does
not write one about 60% of the time.  But the lights-per-run figure is
strikingly stable:

| run | lights delivered | area chains | chunks written |
|---|---|---|---|
| s5 | 7 | 1 | 10 |
| s6 | 8 | 0 | 8 |
| s8r1 | 6 | 0 | 6 |
| s8r2 | 7 | 0 | 7 |
| s8r3 | 8 | 1 | 11 |

**Every run lands on 6–8 lights.**  That is the quantity the model holds
fixed.  And the policy asks it to express those lights in the most expensive
chunk shape available: an area light is FOUR chunks (painter, luminaire
material, geometry, object) where an omni is ONE.  Seven area lights would be
28 chunks against seven one-liners.

So adoption is governed by **chunk economy, not preference** — which explains
every observation: at most one area chain per run, gpt (which writes more
output per turn) managing three, and the ratio being insensitive to prose
about physics.

~~The fix is to make an area light cost one line via a scaffold.~~
**SUPERSEDED within the hour — see §10.4.  "Chunk economy" was the wrong
frame and the scaffold was the wrong fix: it treats typing cost, which is a
symptom, not the cause.**

### 10.3 Method

Two false conclusions in this arc were produced by repetition-without-
decomposition, and both were killed by arithmetic on data already collected.
**Before spending tokens on repeats, decompose the metric: is it a product?
Is it confounded by composition? Can it see all the ways the work can
succeed?**

### 10.4 Why "chunk economy" exists at all — and it is OUR bug (2026-08-12)

Owner: *"Why is chunk economy even a thing?  We aren't trying to save chunks,
especially if for more complex scenes, more chunks will be needed."*

Correct, and the question dissolves the previous section's fix.  Nothing in
the harness asks for economy: `ChatTextCompletionMaxTokens()` is **16384**,
and an eleven-chunk lighting answer is under a thousand.  The cap is not
binding and we imposed no budget.

The constraint is structural, and it is one we built:

| clean room | calls per run | output per run |
|---|---|---|
| `build_element` | **5** — one per element | ~90–100 SDF parts |
| `light_scene` | **1** — all lighting | 6–8 lights |
| `populate_scene` | **1** — all population | ~10–20 objects |

**A single completion yields roughly one response-worth of output whatever
you ask of it.**  Arc 79's entire finding was one fresh context per UNIT OF
WORK; construction is rich because every element gets its own completion.
Arcs 81 and 82 then set the unit to the whole CATEGORY, so lighting gets one
answer's worth of lights and population one answer's worth of repeats.  **The
bottleneck the clean room was invented to remove was reintroduced one level
down.**

That, not preference and not typing cost, is why the expensive form loses:
the model is fitting seven lights into one answer, and seven four-chunk
chains do not fit the shape of one answer the way seven one-liners do.  It
also explains gpt exactly — more output per response, so three area chains
and 42 objects from the same single calls.

**The fix is the unit, not the syntax.**  `light_scene` should run per
lighting intent (or be driven iteratively until it reports done) and
`populate_scene` per element rather than per scene.  The HARNESS must drive
that loop: the model may already call each up to four times
(`kLightSceneMaxPerSession` = 4) and calls each exactly once in every run
measured — voluntary re-use is zero here, as it has been for every voluntary
surface this project has shipped.

Predictions this makes, cheap to check on the next run and worth stating
before it: lighting called N times yields roughly N answer-fulls of lights;
area lights rise without any change to the palette prose; and per-element
population scales repeats with element count instead of flattening them into
one response.
