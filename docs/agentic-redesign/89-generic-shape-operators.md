# 89 — Generic shape operators: loft, skin, mirror, displaced detail

Status: DESIGN — slices A–D pending
Successor to: 85 (geometry-expressiveness candidates; C1/C2/C3/C6 shipped),
87 (recursive scene graph), 88 (procedural textures)
Companion evidence: the three 2026-08-19 live trajectories
(`20260819T200456Z` apothecary/gemini, `20260819T203939Z` dragons/gpt,
`20260819T231023Z` underwater/gpt)

## 1. Motivation — what the banked runs keep failing to say

Geometry expressiveness measurably works now: the dragon prompt on the
same model went sdf 24→10 while skeletons (6 chunks / 42 joints) and
superellipsoids (21 parts) took over; the underwater run authored 31
sweeps, 70 joints, 128 `point_scale` lines with zero parse failures.
Yet the *creatures and figures still read as low quality*.  Across all
three documents the failures are consistent, and they are operator
gaps, not category gaps:

1. **Everything has a circular cross-section.**  Skeleton bones are
   roundcones, sweeps default round, lathes are solids of revolution.
   A cat torso is a flattened ellipse; a snout tapers differently in
   two axes; hulls, shoes, seat rails, forearms are non-circular
   sections *varying along a spine*.  The vocabulary cannot state
   "flatter than wide, narrowing toward the head" — hence the
   sausage-mass cat.  C2's P1 (point_width is x-only) was this gap in
   miniature; the uniform `point_scale` patch never got the per-axis
   form.
2. **No surface can span between curves.**  The dragon document
   contains `dragon_left_wing_bones_skel` and
   `dragon_right_wing_bones_skel` — the model built wing BONES and
   stopped, because no chunk can stretch a membrane across them.
   Wings, fins, webbing, sails, leaves, tarps, awnings, lampshade
   panels: all "thin surface bounded by curves", all unbuildable.
3. **Bilateral symmetry is authored twice.**  Left and right wings are
   separate hand-built skeletons.  Instancing (`source`) exists but has
   no mirror, so symmetric halves drift and double the authoring cost.
4. **Surface detail does not (verifiably) compose onto the new
   builders.**  `displaced_geometry.base_geometry` is a
   `ValueKind::Reference` to any Geometry, but nobody has verified it
   against the meshes lathe/sweep/skin bake or against a
   skeleton-expanded SDF.  Scales, bark, hammered metal are one
   composition away — if it works.

Principle carried over from 85: **generic operators only.**  Nothing
below encodes dragon, cat, chair, or person.  (Deliberately NOT built,
same test as vegetation: faces/eyes as a feature, hair, cloth,
L-systems.)

## 2. Slice A — loft: per-station cross-section control in `sweep_geometry`

Extends the EXISTING chunk; no new chunk kind, no new adoption surface.

Grammar (all backward-compatible):

- `point_scale <s>` gains an optional second arity:
  `point_scale <sx> <sy>` — per-station anisotropic scale of the
  profile.  One-arg form stays uniform, exactly as shipped.
- A second profile: `profile2_point` / `profile2_circle` /
  `profile2_rect` (same trio, same mutual exclusion among themselves),
  plus per-station `point_morph <t>` (t in [0,1]) interpolating
  profile→profile2.  If profile2 is present with no `point_morph`
  lines, default a linear 0→1 ramp along the stations (the common
  taper).  Absent profile2, `point_morph` is a parse error.

This is deliberately two-profile loft, not N-profile: the 85 survey's
LLM-authoring concern is dead for station *parameters* (128
point_scale lines authored live) but station-indexed *profile blocks*
are exactly the indexed-topology shape the literature consensus warns
about.  Two profiles + morph + anisotropy covers torsos, snouts,
limbs, hulls, round-to-square furniture legs; a genuine 3-section form
is two chained sweeps.

Implementation cautions for the worker (verify, don't assume):
- Resample both profiles to a common N by arc length; align starting
  vertices (centroid-angle match) to avoid twist; RMF path framing
  unchanged.
- Caps must use the MORPHED end profiles.
- The conveniences expand to point lists before morphing, so all nine
  profile/profile2 combinations reduce to one code path.
- UVs stay arc-length parameterized; V continuity across morph.

Adoption: FREE.  Rides the already-gated sweep schema
(`construction == "sweep"`), the descriptor gloss, and the existing
worked example (which should gain `point_scale <sx> <sy>` in one line
rather than a second example — context-volume law).

## 3. Slice B — `skin_geometry`: a ruled/billowed surface between two rails

New mesh-baking chunk (RISE_API factory →
`AddPrebuiltTriangleMeshGeometry`, same pattern as lathe/sweep).

Grammar:
- `rail_a <x> <y> <z>` (repeatable, ≥2) and `rail_b …` — the two
  boundary polylines.  Counts may differ; arc-length resample to a
  common N.
- `n_len` / `n_across` tessellation counts.
- `billow <amt>` — inflate the interior along the local surface
  normal with a smooth (cosine) falloff to zero at both rails: sails,
  stretched wing membrane, water-filled tarp.  Default 0 = ruled.
- Open sheet.  Whether it bakes as a single-sided sheet relying on the
  renderer's two-sided handling, or as a thin closed slab, is decided
  by the WORKER against renderer reality (backface behaviour of the
  BSDFs actually used for membranes) — the doc mandates only: a
  membrane must light correctly from both sides.

Covers: wings, fins, webbing, sails, leaves, awnings, lampshade
panels.  This is deliberately the SIMPLEST spanning form — two rails.
Coons quads and subdivision cages stay excluded (indexed-topology
authoring risk).

Adoption (a new chunk needs the full playbook):
- Descriptor gloss = free reach.
- Gated schema + ONE worked example under `construction == "chain"` —
  creature membranes (wings/fins/webbing) are the dominant use and
  creatures declare chain; a wing element declaring `[chain, sweep]`
  gets bones and membrane in one prompt.  Measure the block size; if
  the chain gate grows past ~2× its current bytes, trim the example
  rather than adding a second gate.
- Census: add `skin_geometry` to the weight-0 `expressive_geometry`
  eval label (it is the non-anchored census label — extending it
  changes only the count series; `advanced_geometry` stays untouched).

## 4. Slice C — mirror

`mirror x|y|z` on `standard_object`: reflect the object (or the whole
`source`-cloned subtree) across the named local axis, composed in the
object's local frame before its own transform.

Engine cautions (the worker verifies each against code, not memory):
- Negative-determinant transform: winding/normal flip.  Prior art in
  tree: `Object::m_tangentFrameSign` and the `|det|^(2/3)` world-area
  scale (must become `|det|` where signed).
- TLAS/BVH refit with reflected bounds; `FinalizeTransformations`
  ordering per 87 (no transform stack).
- Interaction with instancing: `source` + `mirror` clones the subtree
  reflected — that is the whole payoff (author one wing, mirror the
  other).  `count_u` + `mirror` is legal and mirrors every repetition.

Adoption: descriptor gloss + one sentence in the scene-graph section
of `object-modeling-recipes` (the only skill both measured providers
read).  No new gate.

## 5. Slice D — displaced detail composition (audit, then gloss)

Verify `displaced_geometry { base_geometry <X> }` against every
builder output: lathe mesh, sweep mesh, skin mesh, skeleton-expanded
SDF, superellipsoid-bearing SDF.  For each: does it derive, does the
displacement painter see usable UVs (lathe/sweep bake real UVs; SDF
may need triplanar — document reality), does the render survive.  Fix
what breaks; then and only then update the descriptors and
`object-modeling-recipes` to say "displace the builder's output for
scales/bark/hammered metal".  Also fold `path_instances_geometry`
(rows-of-things-along-a-curve: spines, scale rows, rivets — shipped,
never adopted) into the same gloss pass.  Possibly zero new engine
code; the deliverable is verified composition + reach.

## 6. Object Graph / GUI compatibility (all slices)

The Object Graph tab (doc 88 S3) and the Material Graph derive nodes
and edges MECHANICALLY from descriptors: every cross-chunk pointer
declared `ValueKind::Reference` with correct `referenceCategories`
becomes an edge; params are node detail.  The rules for this doc:

1. Every new cross-chunk pointer MUST be a descriptor
   `ValueKind::Reference` — never a bare string — so both graph
   views, validation, and the reference graph see it without
   graph-side code.  (A: none.  B: none — rails are inline points.
   C: none new; `source` already draws.  D: `base_geometry` already a
   Reference — the graph already draws displaced→base edges.)
2. New chunk kinds must land in the graph as nodes automatically;
   each slice adds an assertion to the existing controller-level graph
   test (`ReadObjectGraph` on a document containing the new kind:
   node present, edges match the descriptor's references, no dangling
   port for well-formed scenes).
3. Outliner/tree: a mirrored `source` clone folds into its authoring
   chunk's row exactly like a counted clone (`ResolveTreeRowName`
   precedent from 87 step 4d); slice C asserts that.
4. The Qt shells remain UNCOMPILED on this machine (x64-windows
   triplet); Swift/AppKit side is the verified one.  Any Qt-side
   graph change ships flagged as uncompiled, per the standing debt.

## 7. Sequencing, discipline, measurement

Order: A → B → C → D.  A first: highest silhouette impact, zero
adoption cost.  Each slice follows the implementation-review-loop
(worker implements + red-proves; fresh adversarial review; fix every
P1; both build gates — make AND Xcode Deployment — plus
`./run_all_tests.sh`; `CstDeriveGoldenTest --generate` when a tracked
scene is added; stress scene per new capability under
`scenes/Tests/Geometry/`).

Measurement: re-run the three banked prompts (apothecary, dragons,
underwater) after A+B land.  Census: two-axis `point_scale` /
`point_morph` uses; `skin_geometry` chunks (a dragon element with
wings that carries membranes = B working); `mirror` uses; and the
existing declines-to-watch (`path_closed`, `source`/`count_u`).
Delivery is not compliance — census the document, never the note.
