# Clean-Room Construction — Local Frames and Validated Insertion (Arc 79 Design, 2026-08-11)

> **Status:** DESIGN, approved to build (user 2026-08-11: *"nail the
> local-frame contract and validated insertion, then build it"*).  Successor
> to arc 78 (staged build protocol), which supplies the element windows and
> attribution this arc builds on.

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

Stop rules: parts-per-element does not beat arc-78's → the clean room did not
survive productionisation, and the context-dilution finding is banked as
knowledge without a mechanism.  Rejection rate >50% after retry → the
local-frame contract is too hard to state; simplify it.  Elements still
interpenetrate after `place_element` → placement is not the missing piece
either, and composition needs its own arc.
