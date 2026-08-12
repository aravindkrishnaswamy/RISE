# Composition — Stop Destroying Good Work (Arc 80 Design, 2026-08-12)

> **Status:** BUILDING.  User direction 2026-08-12: *"for the composition
> phase we need to not allow deletes and only placement of objects"* and
> *"the object map tooling should be built"*.  Successor to arc 79, whose
> §8.2 result is the entire reason this exists.

---

## 1. The failure, stated exactly

Arc 79's clean room works.  On the undersea subject its five builders
produced **82 SDF parts** and `place_element` converged in one call per
element.  Then the compose phase removed all 31 objects, re-inserted twice,
removed 18 more, and rebuilt with ellipsoids and cylinders: **2 of 82 parts
survived**.  Every operation applied cleanly.  It was deliberate.

The trigger is in the trajectory.  `query_object_at` returned *"no object at
this pixel"* on four of five probes, and the composed scene renders empty.
The model looked at its own scene, found nothing, and concluded — rationally,
on the evidence available to it — that its GEOMETRY was the problem.  So it
replaced rich forms with primitives it could reason about.

Its geometry was fine.  **Nothing in the harness could tell it so.**

That is the whole arc: a composition failure is currently paid for twice,
once in the bad composition and again in the good construction destroyed
trying to fix it.

## 2. Why the object map already existing does not help

`mode:"objectmap"` and `query_object_at` both shipped in the observe-toolkit
arc (2026-07-09), and objectmap's result already carries a legend of
`{name, colorHex, pixelCount}` — very nearly the exact fact the model needed.
It never asked for it.

This is the workstream's most reliable measurement: **every voluntary
consultation surface ever shipped here measures ~0 uses** (0/64 across arcs
76–77 — isolate, one-call form revision, per-part comparison).  A model deep
in a loaded context does not go looking for a diagnostic; it acts on what
arrives in front of it.

So arc 80 builds no new verb.  It moves an existing fact to where the model
will actually meet it.

## 2.1 "Where is everything" beats "what is at this pixel"

User, 2026-08-12: *"'where is everything' is a better tool for a model than
'what is at this pixel'."*  This is the design point of the arc, and it is
sharper than "the model needed more information".

`query_object_at` is an **inverse** query.  To use it you must already know
where to look, and its negative answer — *"no object at this pixel"* — is
information-free about where anything actually is.  Five such probes told the
model only that its guesses were wrong, and it read that as evidence about
its geometry.  A **forward** inventory answers the question actually being
asked: every object, and where each one is.

So the census is not a failure report that lists what went missing.  It is an
inventory of the whole scene, in which the zero-pixel objects are simply the
entries whose answer is unhappy.  Same code path, exposed both as the render
payload (where it will be met) and as an explicit verb (because the question
deserves a name).  `query_object_at` is untouched — it was never wrong, just
the wrong shape for this question.

## 3. Mechanism 1 — the visibility census (a payload fact)

Every full-scene beauty render result gains a compact inventory of **every**
object: its screen footprint, and where it is — in the frame for the ones
that landed, and in the world (plus which way it lies relative to the view)
for the ones that did not.  The unhappy entries carry a reason class:

- **behind the camera / outside the view frustum** — analytic, from the
  object's world bbox and the camera, no render needed (the pinhole
  projection math `ProjectedBBoxCoverage` already exists, along with its
  non-pinhole suppression contract, which is honoured rather than guessed
  around);
- **inside the frame but covered no pixels** — occluded, sub-pixel or
  transparent.  The census does not speculate about which; it states what is
  true.

The per-object tally reuses the existing objectmap identity pass at a small
fixed resolution — a single pass with no lighting — rather than re-rendering
beauty.  The payload is bounded: one compact line per object, the healthy
majority summarized in a large scene, every zero-pixel object named in full,
and any truncation stated rather than silent.

The same inventory is also exposed as an explicit verb, sharing ONE code path
with the payload — the question deserves a name, and the user asked for the
tooling.  But no load-bearing behaviour may depend on the verb being called:
this workstream's own measurements say it will not be.

FACTS ONLY: no advice, no suggested fix, and nothing described as "missing"
or "broken".  An object produced no pixels; that is the fact.  Advice would
both contaminate the behaviour being measured and violate the rule that every
clause in a model-facing payload must be true — the rule arc 79 §8.1 shows
the cost of breaking.

## 4. Mechanism 2 — COMPOSE places, it does not destroy

In the COMPOSE phase, removing a **form-bearing** chunk (geometry-category or
`standard_object`) is refused, naming `reopen_element`.  Lights, cameras,
film, rasterizers, materials and painters stay removable.

This is arc-78 §2.3's established rule — ATTRIBUTE EVERYTHING, REFUSE ONLY ON
FORM-BEARING CHUNKS — extended from creation to deletion, sharing the same
3-refusal cap and give-up so no model is stranded, and the same
`--agent-build-protocol=off` opt-out.

**A known and deliberate hole:** `reopen_element` is always available, and in
the arc-79 run the model used exactly that route to get back into PIECES and
re-insert.  The ban does not make destruction impossible; it makes it
deliberate and attributed.  Whether that is enough is a measured question,
not an assumption — see §6.

## 5. What this does NOT do

No new tools.  No automatic fixing of framing or placement.  No judgement of
composition quality.  No change to the builders or the local-frame contract.
The census reports; the model decides.

## 6. Measurement (pre-committed)

1. **HEADLINE: parts built vs parts surviving.**  Arc 79's mermaid run built
   82 and kept 2.  The ratio is the metric; anything near 1.0 is the win.
2. Does the composed scene render non-empty — and if it still renders empty,
   does the census now SAY so in terms the model can act on?
3. Compose-phase removal refusals: how many, and does the model route around
   them via `reopen_element` (the §4 hole) or change strategy?
4. Census overhead as a fraction of render wall time.
5. Whether the model's edits after a census actually address what the census
   reported — the only evidence that the fact landed.

Stop rules: survival ratio unmoved → the census is not the missing fact and
the destruction has another cause; the model routes around the ban via
`reopen_element` at the same rate → deletion was a symptom, not the lever,
and the ban should be reverted rather than tightened; census overhead
material on ordinary renders → make it conditional, not universal.

## 7. Open question inherited from arc 79

Why the arc-79 composed scene renders empty is still unpinned.  Ruled out:
parse errors, unresolved references, forward references, missing lights,
exotic materials, camera aim (the whole object cluster is inside the frustum
by arithmetic), oversized or displaced geometry, and the environment
background.  **The census is itself the instrument for this** — its first
real test is being pointed at that scene to see what it says.
