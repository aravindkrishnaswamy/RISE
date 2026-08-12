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
