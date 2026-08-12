# Clean-Room Construction — Local Frames and Validated Insertion (Arc 79 Design, 2026-08-11)

> **Status:** BUILT AND MEASURED (2026-08-11).  Shipped in `ef0beee3` +
> `560f2dad`; first live result in §7 — the wizard moved 4 → 10 SDF parts and
> the scene 20 → 48, at 2× the turn cost, with **composition now the binding
> constraint**.  Built on user direction *"nail the local-frame contract and
> validated insertion, then build it"*.  Successor to arc 78 (staged build
> protocol), which supplies the element windows and attribution this arc
> builds on.

---

## 1. The measurement this exists to exploit

Context volume, not knowledge, is the binding constraint on construction
richness.  Measured twice, both providers:

| condition | mean SDF parts |
|---|---|
| free text, short context | 14.7 (gemini) |
| tool call, short context | **18.0** (gemini) / 14.7 (gpt) |
| tool call + 60k of the agent's own skills | **7.7** (gemini) / 10.8 (gpt) |
| live agent session (~80k + images + history) | **1–2** |

A hand simulation (78 §5) then built the same five-element scene with one
FRESH minimal context per element: **67 SDF parts total, wizard 15** against
the live run's ~10 and 2.  Richness is recoverable simply by not asking for
it inside a loaded context.

The same simulation failed at composition, and its failure was structural,
not incidental: **each builder invented its own coordinate conventions**, the
glue was string concatenation with no validation (it silently dropped an
unbalanced chunk, every light, and the scene header), and the assembler was
handed a screenshot and a name list rather than an inventory.  Those three
gaps are exactly what this design closes.

## 2. The two contracts

### 2.1 The LOCAL-FRAME contract (what a builder must return)

A builder is told to construct one element **as if it stood alone**, and is
bound to a canonical frame so the orchestrator can place it afterwards:

1. **Origin at the element's base-centre.**  The element's lowest point sits
   at `y = 0`, horizontally centred on `x = 0, z = 0`.  Elements that will
   end up airborne (a flying dragon, a floating orb) are still authored
   base-at-origin; lift is placement, not construction.
2. **+Y is up. The element faces +Z** (toward a default camera at +Z).
3. **Height budget.**  The caller supplies a target height `H`; the element
   should occupy roughly `H` units in Y.  This is a request, not a gate — the
   harness MEASURES the realised bbox and reports it rather than enforcing it.
4. **Name prefix.**  Every chunk name begins `<element>_`.  ENFORCED (§2.2).
5. **Self-contained.**  The builder defines its own painters and materials
   and finishes with at least one `standard_object`.
6. **Forbidden:** cameras, lights, film, rasterizers, ground planes, and any
   world placement.  Those belong to the orchestrator and the compose phase.

Why base-at-origin rather than centre-at-origin: placement then reads as
"put the wizard at (x, 0, z)" with no height arithmetic, and the common case
— things standing on ground — needs no correction at all.

### 2.2 The VALIDATED-INSERTION contract (what the harness does with it)

Never string concatenation.  The returned text is split into chunks and
submitted through the SAME `InsertChunks` path any agent-authored chunk
takes, so the descriptor registry and the dry-run-guarded re-derive both
apply.  Then:

- **Prefix check** before insertion: any chunk whose `name` does not start
  `<element>_` is rejected.  Not silently renamed — renaming would break the
  internal references the builder just wrote.
- **Repair retry, capped at ONE.**  If insertion rejects anything, the
  builder is called a second time with the exact rejection text appended and
  asked to return the corrected set whole.  One retry, then stop.
- **Partial success is honest.**  Whatever landed, landed; the result names
  every chunk that landed and every one that was rejected, with its reason.
  No silent drops — the simulation's most damaging failure mode.
- **Attribution.**  Everything that lands is attributed to the active
  element by the arc-78 machinery, unchanged.
- **Bbox report.**  After insertion the harness computes the element's world
  bounding box (union over its `standard_object`s) and reports it.  This is
  the inventory the assembler needs and never had.

## 3. The two verbs

```
build_element { element, height, notes? }
```
Runs one fresh, minimal provider completion — grammar + the element's name,
its declared pieces from the build plan, its sketch outline if filed, the
height budget, the local-frame contract — through the session's OWN provider
and credentials (the `imagine_scene` host-mediated pattern, text instead of
image).  Validated-inserts the result.  Returns: chunks landed, chunks
rejected with reasons, whether a repair retry ran, the realised bbox, and the
part count.  Requires an active element (arc-78 PIECES phase).

```
place_element { element, position, scale?, orientation? }
```
Applies ONE rigid transform to every `standard_object` attributed to the
element — the operation the local frame exists to make possible, and the one
RISE's flat scene graph cannot express natively.  Composes with the existing
per-object params rather than replacing them.  Legal in PIECES and COMPOSE.

## 4. Where construction is forced through the clean room

Voluntary tools measure zero in this workstream (0/64 for every consultation
surface built in arcs 76–77).  So during an element window, **the FIRST
geometry chunk for an element must come from `build_element`**: a hand-
authored geometry chunk is refused while the element has no chunks yet,
naming `build_element`.  Once the element has content, hand authoring is
allowed — construction goes through the clean room, refinement stays in the
model's hands.  The refusal shares arc-78's 3-refusal cap and give-up, and
`--agent-build-protocol=off` disables it with everything else.

## 5. What this does NOT do

No parallel builders (one at a time, matching the element window).  No
builder-side rendering or self-correction — the builder is a single
completion, deliberately.  No cross-element reference (a builder cannot see
another element).  No change to the compose phase beyond the bbox inventory
now being available to it.

## 6. Measurement (pre-committed)

1. **HEADLINE: SDF parts per figure element** — live-session baseline 1–2,
   clean-room simulation 15.  Does the shipped path reach the simulation?
2. Rejection rate on first insertion, and repair-retry success rate — the
   validated-insertion contract's own health.
3. Realised bbox vs requested height (is the frame contract honoured?).
4. `place_element` usage, and whether elements still interpenetrate.
5. Turn cost and token cost vs arc-78.
6. Composition quality at the end — the honest visual judgement, since part
   count is a proxy and the simulation proved richness alone is not enough.

## 6.1 Build note — Gemini's tool schema is not JSON Schema (2026-08-11)

The first live S2 run died at HTTP 400 with **zero tool calls**:

```
Unknown name "exclusiveMinimum" at
  'tools[0].function_declarations[20].parameters.properties[1].value'
```

`build_element.height` and `place_element.scale` were declared
`"exclusiveMinimum": 0`.  Gemini's `functionDeclarations` is an OpenAPI-subset
proto, not full JSON Schema, and an unknown keyword is a hard request-level
rejection — not a warning, not an ignored field.  Every Gemini session was
broken by the commit, and nothing caught it because the other 25 tools use no
schema validation keywords at all; these two were the first in the table.

The keyword was removed rather than replaced: the real constraint is strictly
`> 0`, which `minimum` cannot express, and both values are already validated
server-side with a message that says so.  The constraint now lives in the
description text, which is where the model reads it anyway.

`TestGeminiSchemaKeywordDenylist` (AgentChatLoopTest T52) scans the REAL
`BuildRequest` body — not the source table — for the keyword class that
Gemini rejects, and was red-proved by reinjecting `exclusiveMinimum` and
confirming the test fails naming the keyword and the offending tool.

The general rule for anyone adding a tool: **the canonical `kToolDefs` schema
must satisfy the most restrictive provider on the roster, because it is shared
verbatim by all of them.**  Express constraints in prose and enforce them
server-side.

## 7. RESULT — first live run, gemini-3.6-flash, N=1 (2026-08-11)

`evals/runs/imagine_s2_gemini` against the arc-78 baseline
`evals/runs/imagine_s1_gemini`, same scenario, same model, same prompt.

### 7.1 The headline: construction richness moved

Per-element SDF `part` lines in the FINAL scene (not the builder's claim):

| element | arc-78 | arc-79 | |
|---|---|---|---|
| wizard | 4 (hat 2 + robe 2) | **10** (robe 3, beard 2, hat 2, staff 2, orb 1) | **2.5×** |
| dragon | 16 (one chain) | 22 | 1.4× |
| landscape | 4 | 9 | 2.3× |
| mist/magic | — | 7 | new |
| **scene total** | **20** | **48** | **2.4×** |

The wizard is the load-bearing number.  It sat at 1–4 parts across twelve
runs and three arcs while every information / feedback / visibility / cost
mechanism failed to move it.  A fresh minimal context moved it to 10, with
the beard and staff the focused probe named and no live run had ever built.
**Context dilution was a real constraint and the clean room relieves it.**

It did NOT reach the hand simulation's 15, and the dragon barely moved —
arc-78 already spent a 16-part chain there, so the headroom was in the
elements that were being skimped, not the one already getting attention.

### 7.2 What it cost, and what broke

- **Turn cost doubled**: 38 turns → 76; tool calls 37 → 75.  At arc-78's own
  stop-rule boundary (>2× with no quality gain → revert).  There IS a gain,
  so this is not a revert — but the margin is gone.
- **Composition regressed, and is now the binding constraint.**  The render
  crops both figures at the top edge with half the frame empty ground.
  `place_element` was called **24 times** — the most-used verb in the run,
  against 0/64 for every voluntary tool this workstream has shipped, so the
  verb is wanted — but it never converged: repeated re-placements of the same
  element with contradictory scales (wizard at 0.85, 0.45, 0.5, 0.45, 0.9,
  3.5, 1.0).  The model is placing blind and cannot tell when it is done.
- **Frame contract honoured only for grounded elements**: landscape 0.98×,
  wizard 1.24× — dragon 2.33×, mist 2.37×.  Both overshoots are the
  spread/airborne elements, whose Y extent is incidental to their form.  The
  budget reads as a scale hint for a standing figure and as nothing at all
  for a flying one.
- **Insertion rejection rate 2/5 on first attempt**, both repaired by the
  single retry — under the 50% stop rule, and the contract's honest-report
  behaviour worked exactly as designed (no silent drops, reasons named).

### 7.3 The defect this run exposed

`build_element`'s FIRST call returned **zero chunks**: its validated insertion
goes through `InsertChunks`, which inherits arc-77's precondition that a
session must have an imagined scene target before anything can be inserted.
The builder ran, produced geometry, and had all of it refused for a reason
that has nothing to do with the builder.  The model recovered (called
`imagine_scene`, retried) but paid a wasted provider call for it.  A
host-mediated builder should not be gated on a target the orchestrator is
free to create later — fix before the next measurement.

### 7.4 Verdict

The mechanism works on the axis it was aimed at and is worth keeping.  The
constraint has moved: **richness is no longer the limiter, arrangement is.**
Twenty-four non-converging placement calls and a camera that crops the
subject are not a construction problem, and no amount of further per-element
richness will fix them.  That is the next arc, and it is the same shape as
the lighting arc already queued.

N=1.  Repeats before any of this is treated as settled.

## 8. SECOND SUBJECT — the undersea mermaid prompt (2026-08-11, N=1 each)

Run on the verbatim prompt behind the held Fable benchmark
(`scenes/Benchmarks/dreamscape_coral_queens_hour.RISEscene`), so the agent
has a same-prompt frontier reference to be measured against.  Asked because
the dragon+wizard result was uninspiring and the scene itself was a suspect.

### 8.1 Attempt 1 was destroyed by a harness bug, not by the subject

`build_element` for "coral_reef" produced a complete four-piece reef — 17
chunks — and **lost every one**.  Two builder mistakes in sequence, and the
harness mishandled the second:

1. no `name` param at all, the intended name used as the chunk KEYWORD;
2. on the one repair retry, a real `name` — but QUOTED.
   `ChunkParamString_` returns raw token text, so the prefix check compared
   `"coral_reef_rock_painter` (leading quote) against `coral_reef_` and
   rejected all 17 as *"does not begin with the required prefix"*.

That message was false, and therefore unfixable from — which is why the
retry died too.  The element stayed empty, the clean-room gate then refused
hand-authoring three times, gave up as designed, and the model hand-authored
the rest of the run.  **One quoting technicality cost the entire mechanism
for that session.**  Fixed in `00dd86fd` (strip-and-disclose, a literal
syntax example in the builder prompt, and a builder-output excerpt on total
rejection — the completion text had been retained nowhere, so the failure
was undiagnosable after the fact).

### 8.2 Attempt 2: the clean room worked better than it ever has — and the compose phase demolished it

All five builders succeeded: **4 / 21 / 21 / 24 / 12 = 82 SDF parts built**,
against 48 for the whole dragon scene.  `place_element` was called exactly
**five times, once per element** — it converged, where the dragon run
thrashed through 24 calls.  On its own evidence the construction half of
this arc is working.

Then, in the compose phase, the model **removed all 31 objects and
re-inserted them — twice** — then removed 18 more and rebuilt with
ellipsoids, spheres and cylinders.  Final scene: **2 part lines, one
`sdf_geometry` survivor.**  80 of 82 parts destroyed by their own author.
Nothing failed to apply; every removal and insertion was clean and
deliberate.

The trigger is visible in the trajectory: `query_object_at` returned "no
object at this pixel" on four of five probes, and the composed scene renders
as an **empty blue frame**.  The model was looking at its own scene, finding
nothing, and rationally concluding its geometry was the problem — so it
replaced rich SDF forms with primitives it could reason about.  **The
harness let it destroy good work to fix a problem that was not in the work.**

Why the composed scene renders empty is NOT yet pinned, and that is the top
open question for the next arc.  What is ruled out: parse errors (none),
unresolved references (none), missing lights (five), exotic materials (plain
opaque PBR), camera aim (a pinhole at (0,-0.2,7) looking at the origin with
the entire object cluster inside ±3 units and inside the frustum by
arithmetic), and the environment background (off, still blue).  It needs the
object-map tooling, not more arithmetic.

### 8.3 The subject was not the problem — and the bar is now quantified

| | SDF parts | objects | geometry kinds |
|---|---|---|---|
| **Fable benchmark, same prompt, full context** | **131** | 47 | 5 |
| agent, dragon subject | 48 | 11 | 1 |
| agent, mermaid attempt 1 (clean room lost) | 34 | 12 | 3 |
| agent, mermaid attempt 2 (built 82, kept 2) | 2 | 19 | 5 |

The frontier reference is ~2.7× the best agent scene on parts and ~4× on
objects.  But the agent's BUILDERS produced 82 parts in attempt 2 — within
striking distance of 131 — and then threw them away.  The gap is no longer
mainly a construction gap.

**Conclusion: the scene was not too hard.  Composition is the whole
problem, and it is now doing active damage rather than merely underperforming.**

Stop rules: parts-per-element does not beat arc-78's → the clean room did not
survive productionisation, and the context-dilution finding is banked as
knowledge without a mechanism.  Rejection rate >50% after retry → the
local-frame contract is too hard to state; simplify it.  Elements still
interpenetrate after `place_element` → placement is not the missing piece
either, and composition needs its own arc.
