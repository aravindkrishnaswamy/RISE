# Composition — Stop Destroying Good Work (Arc 80 Design, 2026-08-12)

> **Status:** BUILDING.  User direction 2026-08-12: *"for the composition
> phase we need to not allow deletes and only placement of objects"* and
> *"the object map tooling should be built"*.  Successor to arc 79, whose
> §8.2 result is the entire reason this exists.

---

> ## ⚠ CORRECTION (2026-08-12) — the "renders empty" finding was a MEASUREMENT-HARNESS BUG
>
> Every claim in this document that a scene "renders empty", "renders as a
> flat blue frame", or has "~3% contrast" is **FALSE and withdrawn**.
>
> The supervisor's scene-prep step rewrote film dimensions with the regex
> `^(\s*width\s+)\d+` → `800`.  `box_geometry` ALSO takes `width` / `height`,
> and the pattern matched only the integer part: **`width 0.8` became
> `width 800.8`.**  Every box in the scene inflated to ~800×600 world units and
> swallowed the camera, so the render was the inside of a giant box — a
> constant frame.  Scenes built purely from `sdf_geometry` (both dragon runs)
> were untouched, which is exactly why only the mermaid-family scenes appeared
> to fail, and why the false pattern looked so convincing.
>
> The project owner reported both scenes load and render correctly in the GUI;
> that is what exposed it.  Re-rendered with ONLY an output chunk appended and
> nothing else altered:
>
> | scene | luma stdev | span |
> |---|---|---|
> | Fable benchmark (frontier reference) | 29.3 | 116 |
> | arcs 80+81 mermaid | **40.1** | **176** |
> | arc-79 mermaid2 | **28.9** | **129** |
>
> Both agent scenes match or exceed the frontier benchmark's tonal range.
>
> **What survives:** everything measured from scene TEXT or the trajectory —
> part counts, parts built vs surviving, compose-phase removals, light counts,
> power ranges, light kinds, tool sequences.  Those never touched the corrupted
> copy.
>
> **What is void:** every conclusion drawn from a rendered frame — "the scene
> renders empty", "the lighting is drowned", and the causal story that the
> model destroyed its geometry *because* the render looked empty.  The
> destruction happened; the reason is now unexplained.
>

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

## 6.1 FIRST USE — the instrument overturned the diagnosis (2026-08-12)

Pointed at the arc-79 scene that "renders empty", before any live run:

```
SCENE INVENTORY -- 19 objects; 17 covered at least one pixel and 2 covered none
  seabed_sand_obj -- 1174 px, 17.0% of frame, centred 0.50 across / 0.85 down
  seabed_rock_obj --  337 px,  4.9% of frame, centred 0.51 across / 0.81 down
  corals_fan_obj  --   89 px,  1.3% of frame, centred 0.30 across / 0.69 down
  ...
```

**17 of 19 objects cover pixels, every one sensibly placed.**  Sampling the
beauty render at the seabed's own pixels gives `(39,111,151)`; empty sky
gives `(40,113,154)`.  Luma stdev 3.5 on a mean of 101 — **about 3% contrast
across the entire frame**.

So the geometry was correct, the placement was correct, and the picture is
one flat blue because the LIGHTING is drowned.  The model deleted 80 of 82
parts to fix a problem on an axis it never touched.  That is precisely the
attribution error this arc exists to prevent, and it is now demonstrated
rather than argued — §7's open question is answered, and the answer was never
geometry.

It also re-points the workstream: the next real constraint is **lighting**,
which is the arc the user queued on 2026-08-09 and which is still unstarted.

A candidate the census does NOT yet cover: it reports where things are, not
whether they are *distinguishable*.  A contrast or luma-spread fact would
have named this failure outright.  Recorded as a candidate, not built —
one mechanism at a time, measured.

## 7. Open question inherited from arc 79 — ANSWERED

Why the arc-79 composed scene renders empty is still unpinned.  Ruled out:
parse errors, unresolved references, forward references, missing lights,
exotic materials, camera aim (the whole object cluster is inside the frustum
by arithmetic), oversized or displaced geometry, and the environment
background.  **Answered by the census itself in §6.1: the scene is not empty.**  It
contains 17 visible objects at ~3% contrast, which is indistinguishable from
empty to the eye and to a model.  Everything ruled out above was ruled out
correctly; the remaining axis was the one never suspected.

## Postscript: the light-object exemption (2026-08-13)

§4's Light-category exemption ("Lights, cameras, film, rasterizers, materials
and painters stay removable") is arc-78 §2.3's rule, stated in terms of
`ChunkCategory`.  It stopped covering the scene's actual lights once arc-81's
physics moved area lights into **Object**-category chunks — a
`standard_object` bound to a luminaire material, which is exactly what
`rect_light`, `shape_light` and `light_scene`'s builder produce (arc-81 §7.2).

**The trajectory that surfaced it** (live GUI, `20260813T231623Z-267620ce.jsonl`):
a model built 10 area lights via `light_scene` in COMPOSE, then tried
`remove_chunks` on 9 of them to redo the lighting.  Refused: "9 of the 9
chunks named are form-bearing."  A model that cannot re-light its own scene
cannot compose it — the exact failure §4 exists to prevent, on the one
category §4's classifier never anticipated.

**Two concrete routes, both closed:**

1. **The shadow trap.**  A `rect_light` / `shape_light` chunk persists in the
   CST as a Light-category chunk named `<name>` (parse-time sugar), but its
   derive ALSO creates an OBJECT of the same name in the live object manager.
   `TargetIsFormBearing_`'s live-manager fallback found that object and
   refused a chunk that is literally Light-category in the Document.
2. **The general form.**  `light_scene` and hand authors emit the 4-chunk
   expansion (painter / luminaire material / geometry / standard_object) as
   REAL Document chunks.  The object and geometry chunks are genuinely
   Object/Geometry category and, unattributed, were refused outright.

**The fix.**  `TargetIsFormBearing_` (`src/Library/Agent/AgentSession.cpp`)
now consults the live Document (a READ of the retained CST head, not a
re-parse) between the attribution check and the live-manager fallback.  For
an unattributed name the Document carries, the DOCUMENT KIND wins: a chunk
whose descriptor category is neither Geometry nor Object is never form-bearing
(closes the shadow trap outright — every plain light / material / painter
chunk, including rect_light/shape_light's own Light-category chunk, is
covered here); an Object-category chunk is form-bearing UNLESS it is a
**light-object**; a Geometry-category chunk is form-bearing UNLESS every
Document chunk that references it is itself a light-object Object chunk
(zero references stays form-bearing — a decided conservative bound, not an
oversight: the shared 3-refusal cap and `reopen_element` are the escape).

**Light-object, the rule.**  A Document chunk is a light-object iff ALL of:
its descriptor category is Object and its kind is not `csg_object` (csg
composes other objects; it does not itself carry the light's geometry);
its `material` param names a Document chunk that the registry classifies as
a luminaire material (an `exitance` parameter on the descriptor — the SAME
rule `light_scene`'s admissibility check already uses, arc-81 §7.2); and
that luminaire chunk is EMISSIVE-ONLY — its own base-material param is
absent, `none`, or does not resolve to a real Material-category chunk.
`rect_light` / `shape_light` always pass a `none` base, so they always
qualify; a story object whose luminaire WRAPS a real surface material
(glowing skin over a base) fails the emissive-only clause and stays form.

**Boundary rationale.**  Story glow via `emissive` on ggx/pbr materials is
NOT luminaire-kind (those descriptors carry `emissive`, not `exitance`) — it
stays form, by construction, without any special case.  Attribution wins
unconditionally: a light-object built inside an element window is that
element's own form (rule 1 of `TargetIsFormBearing_`), so the light-object
exemption only ever applies to UNATTRIBUTED chunks — the cross-element gate
in `CheckElementWindowForEdit_` needed no change, and a comment there records
that this seam was checked, not skipped.  Under-refusal remains the correct
direction of error (zero-reference geometry, and any name the Document does
not carry at all), bounded by the existing 3-refusal cap and give-up.

Regression coverage: `tests/AgentChunkCrudTest.cpp` (the A80-series compose-
phase-remove tests), pinning both the shadow trap and the general 4-chunk
form, the conservative orphan-geometry bound, the story-glow and wrapped-
luminaire non-exemptions, and the cross-element seam.

### Fix round (2026-08-13): cross-category collisions, the reference scan, and the documented hole

A review pass on the postscript's implementation found two correctness bugs
in how it resolved names against the Document, one over-eager reference
scan, and confirmed a deliberate gap in what the exemption protects.  All
four are folded in here because they change what the rule above actually
means on a real (collision-bearing) scene, not just how it is implemented.

**Collision handling now matches the real remove resolver, exactly.**  RISE
chunk names are unique only PER CATEGORY — each category has its own
`GenericManager`, and the insert-time collision check is per-kind+name — so
`uniformcolor_painter { name Hero }` and `standard_object { name Hero }`
coexist legally.  `TargetIsFormBearing_`'s Document lookup used to resolve
the FIRST same-named chunk in document order, regardless of category: a
painter `Hero` declared before an object `Hero` made `remove_chunk("Hero",
kind="object")` sail through the gate reading the painter (non-form → not
refused) while the real removal deleted the OBJECT — the gate and the delete
had silently diverged.  The gate now resolves via
`RISE::Cst::DocFindByNameAnyRole`, the SAME kind-aware, ambiguity-aware
resolver `Job.cpp`'s `CstResolveRemoveTarget_` calls for the real remove,
passed the SAME `(name, kind)` the caller passed to the real remove — so the
two can no longer disagree.  When the resolver reports genuine AMBIGUITY
(more than one Document chunk shares the name, and `kind` does not narrow to
one), the gate is CONSERVATIVE rather than falling back to the live-manager
read: it classifies every same-named candidate, and the target is
form-bearing if ANY of them is a non-exempt Geometry/Object chunk — the real
remove is left to refuse the same ambiguity honestly (or act on its own
kind-narrowed match) once the gate has decided nothing here needs
protecting.  The same category-blindness existed one layer down: the
light-object classifier's two material lookups (the object's `material` →
the luminaire chunk, and the luminaire's own base `material` → the wrapped
material) also resolved the first same-named chunk of ANY category — a
non-Material chunk sharing either name could shadow the real material and
falsify the "wraps a real base material stays form" guarantee.  Both lookups
now require the resolved chunk's descriptor category to be Material.

**The light-object rule, restated precisely.**  Of the three Object-category
chunk kinds (`standard_object`, `csg_object`, `override_object`), only a
`standard_object` can ever be a light-object: `csg_object` is excluded by
rule 1 outright, and `override_object` declares no `material` param at all
(it only overrides an existing object's transform), so the classifier's
`material`-lookup clause can never fire for it.  The compose-phase refusal
text now says this plainly — "a standard_object is a light here when its
material is a luminaire material wrapping no other material, e.g. what
rect_light, shape_light and light_scene produce; a csg_object never
qualifies" — replacing an earlier "an object is a light here" phrasing that
was literally false for a `csg_object` carrying a bare luminaire (rule 1
already refused it; the text just failed to say so).

**The geometry-reference scan is now kind-restricted.**  The Geometry-clause
walk (does every Document chunk that references a geometry come from a
light-object?) used to compare EVERY param's joined value against the
geometry's name, on every chunk in the Document — so a rasterizer's
`oidn_quality auto` could make a geometry literally named `auto` look
"referenced" by a non-Object chunk, defeating the exemption on a pure text
coincidence (a spurious refusal, safe in direction but wrong in fact).  The
scan now counts a param only when the chunk's registry descriptor declares
that param `ValueKind::Reference` — the same metadata the parser itself uses
to know a value names another chunk.

**The documented hole (arbitrated decision), same class as `reopen_element`.**
An UNATTRIBUTED story object's `material` can be repointed at an existing
bare luminaire via an ordinary `propose_patch` edit — edits are never banned
in compose, only geometry CREATION and (now) non-light-object FORM removal
are — and the object then reads, by the live-document definition, as a
light-object: nothing protects it from `remove_chunk` on the next call.
This is ACCEPTED, not closed.  §4's `reopen_element` escape already makes
the point that this ban is about making destruction of form deliberate, not
about making it impossible; provenance bookkeeping (remembering that an
object was "really" built as story form even after its material no longer
says so) would be a second, heavier mechanism to defend a boundary the rule
already defines honestly by what the Document currently says.  The
live-document definition — "form-bearing iff its material currently makes
it form" — was chosen on purpose.  Pinned by a regression test that patches
a pre-existing object's material to a bare luminaire, then removes it, and
asserts success.

Regression coverage for this round: `tests/AgentChunkCrudTest.cpp`'s
`TestComposePhaseLightObjectExemption`, extended with the cross-category
collision pairs (both kind-given and kind-empty, and the mirror where the
collision must not itself break the exemption), the wrapped-luminaire base-
material collision, the reference-kind-restricted geometry case, the
`csg_object` exclusion, and the documented edit-then-remove hole.
