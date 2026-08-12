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

The three predictions from 82 §10.4 stand as this arc's falsifiers: N lighting
calls yield ~N answer-fulls of lights; area-light share rises with NO change
to palette prose; per-element population scales with element count.  If
area-light share does not move when the unit changes, cost was never the
mechanism and §1's principle is wrong for lighting.
