# Staged Build Protocol — Decompose, Focus, Compose (Arc 78 Design, 2026-08-11)

> **Status:** DESIGN — approved in intent by the user 2026-08-11: *"force the
> model to break down each element in the scene it needs to build into
> pieces, then force it to put attention on each piece and flesh it out.
> Then when everything is done have it do a pass to make sure arrangement
> and lighting make sense."*  Successor to arc 77 (imagination targets).

---

## 1. The evidence this is built on

The wizard probe (2026-08-11, recorded in 77 §16) settled what four days of
mechanisms could not:

- **Conception is not the gap.**  Asked in prose how to build a robed wizard
  from SDF primitives, gemini named the correct construction unprompted —
  flared hem, tapering body, sphere head, beard *"to break the circular
  profile"*, torus brim *"critical — it breaks the vertical conical
  silhouette"*, cone above it, sleeves.
- **Expression is not the gap.**  Handed that plan in prose, it wrote 11
  correct `part` lines first try, no feedback — and got the `capsule`
  parameter order right (a=radius, b=half-length), which the supervisor got
  WRONG twice and only fixed by reading `SDFGeometry.cpp`.
- **Yet in twelve full scene-building runs it produced a cone every time.**

The difference is ATTENTION BUDGET.  When the wizard is the whole request it
gets a competent wizard's worth of thought; when it is one of five to seven
parts competing with terrain, lighting, materials and camera, it gets one
call.  Nothing in the harness has ever made a single element the whole task.

Everything shipped in arcs 75–77 assumed the model needed information
(sketches), feedback (comparisons), visibility (isolate) or cheapness
(one-call revision).  It needs none of those.  It needs **serialization**.

## 2. The protocol

Three phases, enforced by the existing gate machinery.

```
PLAN     -> file_build_plan: elements, each broken into named pieces,
            each with a construction method and an outline sketch
PIECES   -> one ACTIVE ELEMENT at a time, in plan order.  Everything the
            model creates while an element is active is ATTRIBUTED to it.
            finish_element advances; reopen_element goes back.
COMPOSE  -> entered when the last element is finished.  Arrangement,
            lighting, camera.  Geometry CREATION is refused here (with an
            explicit reopen escape).
```

### 2.1 Why element-level windows, not piece-level

The wizard the model built well is ONE `sdf_geometry` with 11 parts.  A
piece-level window ("now build the hat") would forbid exactly the
whole-figure construction that worked.  So the **window is the element**;
the **piece list is the checklist** that makes "fleshed out" concrete and
gives the census something to score against.  Pieces are declared, not
individually gated.

### 2.2 Attribution solves the join problem by construction

G2's review established that a gate cannot know which part a chunk belongs
to (the chunk→part join), which is why the per-part comparison needed the
model to name the pairing at consultation time.  Phasing dissolves this:
**while element E is active, every chunk created belongs to E.**  No naming
convention, no volunteered field, no heuristic.  This is the single biggest
structural win of the design and it comes free.

### 2.3 What is refused, and what is not

| During an element window | |
|---|---|
| create/edit chunks (attributed to the active element) | ALLOWED |
| edit a **form-bearing** chunk attributed to a DIFFERENT element (geometry, `standard_object`) | REFUSED — names the active element and `finish_element` |
| light / camera / film / rasterizer / rasterizer-output / **material** / **painter** edits | ALLOWED (deliberately — see below) |
| renders, reads, sketch comparisons | ALLOWED |

| During COMPOSE | |
|---|---|
| arrangement (position/orientation/scale), lighting, camera, materials | ALLOWED |
| creating new geometry | REFUSED — names `reopen_element` |
| editing any element's chunks | ALLOWED (composition legitimately adjusts parts) |

**Lights are deliberately NOT refused during element windows.**  Over-refusal
was E1's review P1 and the failure mode to fear here: a model that cannot
light its own isolated part cannot see it in a beauty render.  The compose
phase re-examines lighting as its stated job; it does not need a monopoly.

**Nor are materials and painters** (S1 fix-round, 2026-08-11 — supervisor
design call after the slice's first review).  The governing rule is
**ATTRIBUTE EVERYTHING (for measurement), REFUSE ONLY ON FORM-BEARING
CHUNKS**: a material or painter created inside an element window is still
recorded against that element, so the census and `finish_element` see it —
it simply stops being *refusable* from another window.  Two elements sharing
one material or painter is ordinary authoring, not an edge case (a skin
material on a head and a pair of hands; a fabric painter on a robe and a hat
trim), and refusing it would refuse the most common legitimate cross-element
reach there is.  What stays refusable is the form-bearing set — geometry
chunks and `standard_object` — which is exactly what the serialization this
arc tests is about.

### 2.4 Advancing, and never stranding

- `finish_element {}` — closes the active element, advances to the next, and
  returns a factual summary: what was attributed, which declared pieces have
  chunks whose names mention them, and **an isolate render of the element**.
  That last part is the payload-fact form the law endorses: a look the model
  did not have to ask for, arriving in the result of a call it just made.
  This is where "flesh it out" gets its feedback.
  *(2026-08-23 / 2026-08-24, as shipped: that render is a close-range 256px
  isolate, and there are now **two** of them composited side by side — a
  DRAFT panel for form, and a fixed-PT panel under a canonical studio light
  rig for MATERIALS, because draft shading is a crude, fixed-light,
  single-bounce approximation with no true light response (no shadows,
  reflection/refraction, or emission) and so cannot show specular rolloff,
  roughness, fresnel or transmission FULLY.  The rig replaces the scene's
  lights and environment for that one render and is restored after it; the
  document is untouched.  See `AgentSession::FinishElement` and
  `AgentRenderQuality::MaterialLook`.)*
- `reopen_element {name}` — legal from any phase, including COMPOSE.
  Re-enters that element's window.  Always available; never gated.
- The phase gate inherits G2's **3-refusal cap and give-up per transition**,
  so a model that cannot work the protocol is never stranded — it proceeds
  with a factual notice, and that notice is a census anchor.
- `--agent-build-protocol=off` disables the whole thing (mirrors
  `--agent-part-plan-gate`; both flags respected).

## 3. Schema: `file_build_plan` (supersedes `file_part_plan`)

```
file_build_plan { elements: [ {
    element:      string   (required)   e.g. "wizard"
    pieces:       [string] (required, >=1)  e.g. ["robe","hat","beard","staff"]
    construction: [enum]   (required, 1..2, no repeats; a bare string is also accepted)
                                       primitive|csg|sweep|lathe|chain|displaced|mesh
    outline:      "x y; ..." (required, >=3 pts)   the element's silhouette
    view:         front|side|top (optional)
    note:         string   (optional)
} ] }
```

`file_part_plan` is RENAMED, not duplicated — one gate, one artifact, one
census anchor lineage.  Its outline rasterization, target store, sketch echo
and per-part IoU comparison all carry over unchanged, keyed by element name.

## 4. Measurement (pre-committed before any run)

Census fingerprints, all from trajectory args:
1. Elements declared, and pieces per element (the decomposition depth).
2. Chunks attributed per element — is the wizard window actually spent on the
   wizard?
3. **HEADLINE: SDF part-count / chunk-count per figure element vs the
   thrice-measured baseline** (the wizard was 1 roundcone; the focused probe
   produced 11 parts).  Does a focus window move it?
4. Cross-element refusals (is the protocol biting, or is the model naturally
   serial already?).
5. Compose-phase edits — does the arrangement pass actually happen and change
   anything?
6. Give-up rate per transition; turn cost vs the arc-77 baseline.

Stop rules:
- Give-up in ≥3/9 runs → the protocol is too heavy; simplify to PLAN→COMPOSE
  with attribution but no per-element gating.
- Part-count per figure element unchanged vs baseline → serialization was not
  the constraint either, and the attention hypothesis dies with it; record
  the accept-the-deficit exit honestly.
- Turn cost >2× arc-77 with no quality gain → revert.
- Success: figure elements gain real internal structure (part counts moving
  toward the probe's 11) AND the compose pass produces arrangement edits.

## 5. Explicitly out of scope for v1

Piece-level gating; automatic quality judgement of a "fleshed out" element
(the piece checklist is declarative, never scored); reordering elements;
parallel element work; any numeric score anywhere (77 §15's lesson — an
attached fact selects the axis the model moves).
