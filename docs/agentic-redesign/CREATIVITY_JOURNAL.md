# Getting Agents to Be Creative: A Week's Field Notes

> Consolidated journal of the creative-richness workstream, arcs 75–83
> (2026-08-06 → 2026-08-13).  Written as raw material for an essay.  Every
> number here is traceable to an arc doc (75–83 in this directory) and to a
> committed run under `evals/runs/`; where a claim was later retracted, the
> retraction is part of the story and is kept, because the retractions ARE
> the findings.
>
> The standing constraint on everything below: richness had to come from
> **harness mechanisms only** — never from quizzing the user about style.

---

## 0. The problem, in one image

Ask a scene-building agent for "a dreamy landscape with dragons and wizards,
psychedelic, creative" and you get a cone standing on a plane.  Ask a
frontier model at full attention — same renderer, same prompt — and you get
a 131-part reef with a mermaid, schooling fish, kelp, and god-rays.

The agent is not a weaker model.  In several of these experiments it was the
same model.  The week was about finding out where the creativity goes.

## 1. Six mechanisms that measured zero

The obvious hypotheses, each built properly, each measured honestly, each
dead:

| # | hypothesis | mechanism built | result |
|---|---|---|---|
| 1 | it lacks information | sketch/outline filing per part | no movement |
| 2 | it lacks feedback | render-vs-sketch IoU comparison | no movement |
| 3 | it can't see its work | `isolate` single-object renders | **0/64 voluntary uses** |
| 4 | revision is too costly | one-call form revision | 0 uses |
| 5 | attention is divided | element windows (serialization) | wizard stayed at 2 parts |
| 6 | it doesn't reason first | reasoning-before-code probe | control arm matched treatment |

Two durable laws fell out of the failures long before anything succeeded:

- **Advice ≈ 0 at any exposure.**  Exhortation, style tips, "be creative" —
  no measurable effect, ever, in any placement.
- **Voluntary tools go unused.**  Every consultation surface offered rather
  than forced measured ~0 uses (0/64 across two arcs).  A model deep in a
  task does not go looking for a diagnostic; it acts on what arrives in
  front of it.  Corollary: a fact must ride in the RESULT of a call the
  model already makes — a payload fact — or it may as well not exist.

## 2. The real constraint: context dilution

The seventh hypothesis was found by accident and confirmed by the cleanest
measurement of the week.  Identical request, identical model, identical
grammar — only the context volume varies:

| condition | mean SDF parts produced |
|---|---|
| bare request, short context | **18.0** (gemini) / 14.7 (gpt) |
| + 60k tokens of the agent's own skills | **7.7** / 10.8 |
| live agent session (~80k + images + history) | **1–2** |

The model *knows* how to build a wizard — asked in isolation it names the
flared hem, the beard "to break the circular profile," the torus brim, and
writes 11 correct SDF parts on the first try.  Inside a loaded session the
same model emits a cone.  **Context volume, not knowledge, is the binding
constraint on construction richness.**  Six mechanisms failed because they
were all aimed at other axes; when several well-built mechanisms fail on one
axis, the axis is wrong.

## 3. The clean room

The fix follows directly: build each element in a **fresh minimal context**.
The harness (which keeps the full session) briefs a separate builder
completion with only what that one element needs — grammar, the element's
name and pieces, a height budget, a coordinate contract — and validates the
returned chunks through the normal insertion path (balanced-brace extraction
that *reports* malformed input, a name-prefix check, exactly one repair
retry carrying the real rejection text, never a silent drop).

First live result: the wizard went from 4 SDF parts to **10** — with the
beard and staff no live session had ever built — and the scene from 20 to
**48**.  The per-element number then proved stable across every later run
(the figure element: 23, 22, 17, 23, 25 parts — mean 22, matching the
short-context measurement almost exactly).  The clean room doesn't make the
model smarter; it gives it back the attention the session was consuming.

Two contracts made it production-safe where a hand simulation had failed:
a **local-frame convention** (build as if standing alone: base-centre at
origin, +Y up, height as a *measured budget*, never a gate) and **validated
insertion** (the simulation's string-concatenation glue had silently eaten a
chunk, every light, and the scene header — "no silent drops" became the
contract's first clause).

## 4. Destruction, misdiagnosis, and the instrument that lied

The clean room's best early run built **82 parts — and the compose phase
deliberately deleted 80 of them**, replacing rich SDF forms with ellipsoids
and cylinders.  Why?  The model probed its scene with `query_object_at`,
got "no object at this pixel" five times, saw a render that looked empty,
and rationally concluded its geometry was broken.

It took days to learn that every part of that story was wrong:

- The render "looked empty" because **my own measurement harness was
  corrupting the scene**: a film-resize regex (`^(\s*width\s+)\d+` → 800)
  also matched `box_geometry`'s `width`, turning `width 0.8` into
  `width 800.8`.  Every box swallowed the camera.  Scenes built purely from
  SDFs were untouched — which made the corruption look exactly like a
  scene-class difference.  The owner opened the files in the GUI, said
  "these render fine," and the whole edifice collapsed in an hour.
- The two instruments had *disagreed in the evidence the whole time* — the
  object-identity inventory said 17 of 19 objects covered pixels while the
  beauty render beside it showed nothing — and the disagreement was
  rationalized ("the lighting must be drowned") instead of investigated
  ("one of these two is lying").

Lessons that transferred:

- **When something misbehaves only in your pipeline and not the user's,
  suspect the pipeline.**  A discrepancy between two observers of one
  artifact is evidence about the observers.
- **Scene mutation for measurement must be chunk-scoped or append-only.**
- **A false clause in a model-facing payload is catastrophic**, not
  cosmetic: a rejection message that mislabeled a quoting problem as a
  prefix problem ("does not begin with the required prefix" — untrue) once
  cost an entire session's mechanism, because the model cannot fix what the
  message misdescribes.

## 5. "Where is everything" beats "what is at this pixel"

The owner's diagnosis of the probe failure became a design principle.
`query_object_at` is an **inverse** query: you must already guess where to
look, and its negative answer carries no information about where anything
is.  Five "nothing here" answers told the model only that its guesses were
wrong — and it read them as evidence about its geometry.

The replacement is a **forward inventory** on every render result: every
object, its pixel footprint, where it sits in frame — and for the ones that
produced nothing, an analytic reason class (behind the camera / off-frame
left / in frame but covering no pixels).  It rides the render payload
because of the voluntary-tools law, and it reports facts with no advice
attached, because the mechanism being measured must not be contaminated by
coaching.

Its first use answered the question it was built for — by overturning it
(§4).  The instrument you build to explain a mystery may instead dissolve
it.

## 6. Composition: most of richness is repeats

Measuring the frontier benchmark's construction instead of assuming it:
**47 objects from only 18 distinct geometries — 38 of 47 are repeats** (fish
×14, motes ×5, rocks/kelp/jellyfish ×3 each), with no instancing machinery
at all; every repeat is a four-line `standard_object` reusing an existing
geometry.  The agent's gap was never only modelling — it builds one of each
thing and stops.

A `populate_scene` clean-room pass (may create ONLY `standard_object`s
naming existing geometry; one worked example **built from the live scene**,
because an invented example name would be copied and then rejected; and
deliberately NO target count, since object count is the metric) moved
object count from 16 to ~25 and the reuse ratio from ~0 to a rock-stable
62–67%.  Its visible failure was structural and honest: repeats of
*multi-part* creatures came apart (tentacles placed without their bell),
because the inventory lists geometries, not assemblies.

## 7. The lighting saga — five falsifications to one flip

The owner's physics policy: area lights (an object wearing a luminary
material) should light most scenes; ambient light never; point/spot/
directional only in special circumstances.  Getting a model to *live* that
policy consumed more falsified hypotheses than everything else combined:

```
mechanism                               physical sources per run
palette order + sole worked example ... 1 / 0 / 0 / 0 / 2   (of ~7)
one completion per light .............. 0 of 11
"source-first" vocabulary ............. 0 of 7
rect_light available .................. 1 of 10
shape_light + a price on zero-area .... 5 of 6   ← the flip
```

What each failure taught:

1. **Salience isn't preference.**  Putting the area light first with the
   only worked example moved almost nothing.
2. **Cost-as-room was wrong.**  Giving each light its own completion (so a
   four-chunk area light "fits") produced *zero* area lights — and exposed
   a deeper bug: the *planning* step, which had no palette, was naming
   instruments ("key spot light"), and the builder obediently implemented
   them.  **The kind was chosen where the options were invisible.**  A fact
   must be attached to the call that makes the decision, not the call that
   carries it out.
3. **The question selects the form of the answer** (the owner's insight).
   "What lighting jobs does this scene need?" is a cinematography question;
   its native vocabulary is key/fill/rim, and those words *refer to studio
   instruments* — zero-area lights.  "What in this world physically emits
   light?" has THINGS as answers: a bulb, a window, the sky, a glowing
   creature.  The enumeration became perfect ("bioluminescent jellyfish
   glowing in the water column") …
4. … **and the build still wrote omnis for the glowing jellyfish.**  The
   final mechanism: **the category of the task summons the category of the
   chunk.**  Asked for "lighting," the model writes lighting-category
   chunks, whatever they are.  Meanwhile the same model, *building* a
   jellyfish, freely gives it emissive materials — it lights the thing when
   the glow belongs to the thing.  No prompt inverts this.
5. So the fix moved into the **scene language**: `rect_light` and
   `shape_light` (sphere/ellipsoid/box/cylinder) — one-chunk physical
   lights, expanded at parse time into the painter/luminaire/geometry/object
   chain, sitting IN the lighting category so the model's reach lands on
   them — plus a **price on the zero-area path**: every omni/spot/
   directional request is refused once with physics facts and lands when
   re-issued verbatim.  Ambient is a wall (banned outright, no confirm);
   zero-area is a toll (occasionally right, so deliberate use costs one
   bounce).  The next run: **five of six sources physical, voluntarily,
   with the toll never firing** — the water surface as a downward rect
   panel, the creatures as glowing solids.

The distilled law: **availability for the right shapes, plus a price on the
wrong path, moves behaviour where information never did.**  And its
corollary: when four prompt-level mechanisms fail on the same question, the
fix probably doesn't live in the prompt.

## 8. The unit of work

A late structural insight that reframes everything above: **a single
completion yields roughly one response-worth of output, whatever you ask of
it.**  Construction was rich because each *element* got its own completion;
lighting and population were thin because each *category* got one.  The
harness had reintroduced, one level down, the exact bottleneck the clean
room removed.

The sizing rule: **one response must be one unit's work** — an element
(~20 parts), one lighting design, one population pass.  Get the unit wrong
in either direction and you pay: category-sized units starve the output;
over-fine units (one completion per light) multiply cost 7× while making
results *worse*, because they push decisions upstream into planning steps
that can't see the options.

And because voluntary re-use measures zero, **the harness must drive the
loop** — the model may call a pass four times and calls it once, every
single time.

## 9. Compulsion and refinement

Forced-first-use gates (the clean room, populate-before-first-render) are
what make the mechanisms fire at all — but compulsion built for scene
CONSTRUCTION becomes user-hostile the moment a human is tweaking a finished
scene ("delete the manta ray" must never be refused).  The resolution is a
one-way session mode: **compulsion belongs to the first build; availability
is forever.**  Gates exist only while an agent is building a scene that
started empty and dissolve at its first final answer; renderer *physics*
policy (the ambient wall, the zero-area toll) survives in both modes,
because it's a property of the renderer, not of the workflow.

The wild-run evidence says the balance is roughly right: in the owner's own
live session, the delete-ban stopped a 14-chunk deletion of a finished
turtle (arc-79's disaster, prevented in production), the toll extracted its
price on a light batch, the model routed around some compulsion via the
designed escape hatch — and still produced the best scene of the week,
including an emissive-SDF jellyfish nobody asked it for.

## 10. Measurement discipline (the meta-findings)

The week produced as many lessons about *measuring* agents as about
steering them:

- **Run repeats before believing an arc — or better, decompose first.**
  Single-run headlines moved by half under repeats ("object count doubled"
  was really +56%).  But decomposition beat repetition twice at zero cost:
  "parts are drifting down" died the moment the metric was split per
  element (a stable 22 × a variable element count, blind to primitives);
  "area lights are cost-limited" died the moment lights-per-run was seen to
  be the stable quantity.  Ask of every metric: is it a product of two
  numbers?  Is it confounded by composition?  Can it see every way the work
  can succeed?
- **Counts spread ±20% at N=1.**  Effects smaller than that are not
  findings.  The effects that survived (reuse ratio, the lighting flip,
  clean-room part counts) were all large and tight.
- **Pre-commit falsifiers, then honour them.**  The week's fastest progress
  came from predictions stated before runs — most of which failed, each
  failure eliminating a hypothesis family for good.
- **Red-prove your instruments.**  A guard test that cannot be made to fail
  is not a guard; a verification step that cannot distinguish "clean" from
  "didn't run" is not verification.  (Both happened.  Both are in the
  ledger.)
- **Never let a payload lie**, including by staleness — a stale claim in a
  model-facing description is the same defect as a false one.

## 11. The laws, in one place

1. Models act on facts that BLOCK or ARRIVE; advice ≈ 0 at any exposure.
2. Voluntary tools go unused (0/64); load-bearing facts ride result
   payloads of calls the model already makes.
3. An attached fact SELECTS the axis the model moves; attaching the wrong
   one steers rather than merely failing.
4. Context volume, not knowledge, binds construction richness; fresh
   minimal contexts recover it (18.0 → 7.7 → 1–2 parts as context grows).
5. One response is one unit's work; size the unit accordingly, and let the
   harness drive the loop.
6. A fact must be attached to the call that makes the decision, not the
   call that executes it.
7. The question asked selects the form of the answer (jobs → instruments;
   things → objects).
8. The category of the task summons the category of the chunk; no prompt
   inverts it — but the *language* can, by putting the desired physics
   inside the summoned category.
9. Availability for the right shapes plus a price on the wrong path moves
   behaviour where information doesn't.  Walls for never; tolls for rarely.
10. Compulsion belongs to the first build; availability is forever.
11. Most visual richness is repeats — placement, not modelling.
12. Examples move copying; whatever is drop-in-copyable is what will be
    built.  Curate examples as policy, and prove they parse.
13. When N well-built mechanisms fail on one axis, the axis is wrong.
14. When it only breaks in your pipeline, suspect the pipeline.

## 12. Open threads

- Environment/atmosphere and camera-framing construction steps (designed,
  not yet built) — framing is now the worst visible axis.
- Group-aware repeats (assemblies, not geometries) for population.
- The VCM misbehavior on the banked SDF-luminaire torture scene
  (`scenes/FeatureBased/VCM/vcm_sdf_luminaire_jellyfish.RISEscene`).
- Cross-provider depth: most post-arc-79 results are gemini-only, N=1–3.
- The essay's honest epigraph candidate, from the week's ledger: nearly
  everything that worked was designed by the human after the harness's
  measurements falsified the supervisor's mechanisms.  The measurements
  were the contribution; the ideas mostly weren't.
