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
